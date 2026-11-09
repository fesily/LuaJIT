/*
** Interpreter breakpoints (BC_INT3).
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_int3_c
#define LUA_CORE

#include "lj_obj.h"

#include "lj_err.h"
#include "lj_frame.h"
#include "lj_func.h"
#include "lj_dispatch.h"
#include "lj_gc.h"
#include "lj_int3.h"

#include "luajit.h"

#if LJ_HASPROFILE
#include "lj_jit.h"
#include "lj_trace.h"
#include "lj_profile.h"
#endif

/* A session owns up to LJ_INT3_MAX breakpoint requests of this VM. It shares
** the profiler mutual exclusion (at most one of profiler and INT3 is active)
** and leaves the JIT engine running: mounting a breakpoint flushes exactly
** those traces that execute or inline the armed prototype and excludes that
** prototype from JIT compilation, so every later execution of its bytecode is
** an interpreter dispatch.
**
** The canonical bytecode is never modified. Every prototype that carries a
** request owns one shadow copy of its complete bytecode array; only new
** instruction fetches read the shadow, where the armed slots hold BC_INT3
** with the original A/B/C/D operands. All hook, recorder and slow-path re-reads
** read the canonical array, so they need no translation and can never observe
** a trap.
**
** A request is accepted as pending and mounted at the next safe point: the
** prototype is flushed, kept out of the JIT and its canonical bytecode is
** copied into the shadow. Until then the request only holds a GC root, the
** reserved shadow memory and a forced C dispatch that leads to that safe
** point. A trap is permanent: every new fetch of the slot delivers one
** callback and then executes the original instruction from the canonical
** array until the request is disarmed by luaJIT_int3_cancel(), by
** luaJIT_int3_stop() or by a VM teardown. Re-arming an armed slot is a
** no-op, so a callback can keep trapping its own slot.
**
** Every request is a GC root (GCROOT_INT3+i), so a prototype that carries
** one can never be collected: the root is taken before the shadow is
** published and dropped only after the mapping and the shadow are gone.
** lj_int3_close() releases everything before the finalizer run and
** lj_gc_freeall() of a VM teardown.
**/

#if LJ_HASPROFILE

#define INT3_NONE	0	/* No session. */
#define INT3_ENABLED	1	/* Session accepts requests. */

#define INT3_PENDING	0	/* Request accepted, not in the shadow yet. */
#define INT3_MOUNTED	1	/* Request is planted in the published shadow. */

typedef struct Int3Entry {
  GCproto *pt;			/* Prototype of the request. */
  BCPos bcpos;			/* Bytecode index of the trapped slot. */
  uint8_t state;		/* INT3_PENDING/INT3_MOUNTED. */
} Int3Entry;

typedef struct Int3Owner {
  GCproto *pt;			/* Prototype sharing one shadow. */
  BCIns *shadow;		/* Reserved BCIns[sizebc] copy, or NULL. */
  MSize size;			/* Size of the reserved shadow in bytes. */
  int nreq;			/* Number of live requests of this owner. */
  uint8_t mounted;		/* pt->int3ofs is published. */
  uint8_t owns_nojit;		/* We set PROTO_NOJIT for this owner. */
} Int3Owner;

typedef struct Int3State {
  global_State *g;		/* VM that owns the session. */
  luaJIT_int3_callback cb;	/* Native read-only callback. */
  void *data;			/* Callback data. */
  int state;			/* INT3_NONE/INT3_ENABLED. */
  int count;			/* Number of live requests. */
  int npending;			/* Number of pending requests. */
  int delivering;		/* Callback delivery in progress. */
  Int3Entry entry[LJ_INT3_MAX];	/* Accepted requests. */
  Int3Owner owner[LJ_INT3_MAX];	/* Prototypes with a shadow. */
} Int3State;

static Int3State int3_state;

/* Index of the request for this slot, or -1. */
static int int3_find_slot(global_State *g, const BCIns *pc)
{
  Int3State *st = &int3_state;
  int i;
  if (st->g != g || st->count == 0) return -1;
  for (i = 0; i < LJ_INT3_MAX; i++) {
    GCproto *pt = st->entry[i].pt;
    if (pt != NULL && pc == &proto_bc(pt)[st->entry[i].bcpos]) return i;
  }
  return -1;
}

/* Index of the request for (pt, bcpos), or -1. */
static int int3_find_ins(Int3State *st, GCproto *pt, BCPos bcpos)
{
  int i;
  for (i = 0; i < LJ_INT3_MAX; i++)
    if (st->entry[i].pt == pt && st->entry[i].bcpos == bcpos) return i;
  return -1;
}

/* Index of the owner of this prototype, or -1. */
static int int3_find_owner(Int3State *st, GCproto *pt)
{
  int i;
  for (i = 0; i < LJ_INT3_MAX; i++)
    if (st->owner[i].pt == pt) return i;
  return -1;
}

/* Index of the owner of the canonical bytecode slot at pc, or -1. */
static int int3_owner_of_pc(Int3State *st, const BCIns *pc)
{
  uintptr_t p = (uintptr_t)pc;
  int i;
  for (i = 0; i < LJ_INT3_MAX; i++) {
    GCproto *pt = st->owner[i].pt;
    if (pt != NULL) {
      uintptr_t bc = (uintptr_t)proto_bc(pt);
      if (p >= bc && p < bc + pt->sizebc*(uintptr_t)sizeof(BCIns)) return i;
    }
  }
  return -1;
}

/* Non-zero if at least one request of this owner is not mounted yet. */
static int int3_owner_pending(Int3State *st, GCproto *pt)
{
  int i;
  for (i = 0; i < LJ_INT3_MAX; i++)
    if (st->entry[i].pt == pt && st->entry[i].state == INT3_PENDING) return 1;
  return 0;
}

/* Non-zero if lj_int3_hit() is delivering a callback for this VM. The
** delivery is masked as a VM event and switches the VM state to C, so those
** two are self-inflicted and must not be rejected by the checks below.
*/
static LJ_AINLINE int int3_delivering(global_State *g)
{
  Int3State *st = &int3_state;
  return st->delivering && st->g == g;
}

/* Non-zero for a bytecode slot that the JIT or the VM rewrites at run time:
** lazy I/J patches (FORL/ITERL/LOOP/FUNCF), root-trace start patches
** (FORL/ITERL/LOOP/ITERN/FUNCF and the whole instruction for RET), the
** despecialization of ITERN and ISNEXT and the FORI companion of a JFORL.
** A trap on such a slot would race with the patch, so these slots are never
** armed.
**
** BC_RETM is deliberately *not* listed even though bc_isret() includes
** it: it is a legitimate trap site (the interpreter executes and delivers
** it like any other opcode), and the tracer can never turn it into a
** trace-start slot, so the temporary unpatch in lj_trace_exit() can never
** write it into a JLOOP slot. That is guaranteed by trace_downrec()
** refusing RETM and by trace_stop() patching only RET/RET0/RET1/ITERN to
** BC_JLOOP. If either producer changes, RETM must move into the refused
** set.
*/
static int int3_mutable_op(BCOp op)
{
  switch (op) {
  case BC_ISNEXT: case BC_ITERN: case BC_ITERL: case BC_LOOP:
  case BC_FORI: case BC_FORL: case BC_FUNCF: case BC_FUNCV:
  case BC_RET: case BC_RET0: case BC_RET1:
  /* Already patched variants, e.g. when arming after a jit.off/on cycle. */
  case BC_JFORI: case BC_JFORL: case BC_JITERL: case BC_JLOOP:
  case BC_JFUNCF:
  case BC_IFORL: case BC_IITERL: case BC_ILOOP: case BC_IFUNCF:
  case BC_IFUNCV:
    return 1;
  default:
    return 0;
  }
}

/* Unsafe context flags, one bit per condition. Arming is allowed from a Lua
** debug hook and from the recorder/compiler, every other operation treats
** all of them as fatal.
*/
#define INT3_CTX_HOOK		0x01	/* Inside a Lua debug hook. */
#define INT3_CTX_HARD		0x02	/* GC hook, VM event or profiler. */
#define INT3_CTX_VMSTATE	0x04	/* Machine code, recording or compiling. */

/* Non-zero while the recorder, the optimizing compiler or the assembler is
** running. A VM event emitted from this code is its own bookkeeping and may
** accept a pending request. The state machine covers the events that are sent
** before the VM state switches to RECORD/OPT/ASM, e.g. the trace start event.
*/
static LJ_AINLINE int int3_recording(global_State *g)
{
#if LJ_HASJIT
  int vmst;
  if (G2J(g)->state != LJ_TRACE_IDLE) return 1;
  vmst = g->vmstate;
  return vmst == ~LJ_VMST_RECORD || vmst == ~LJ_VMST_OPT ||
	 vmst == ~LJ_VMST_ASM;
#else
  UNUSED(g);
  return 0;
#endif
}

/* Classify the context of the interrupting thread. Our own callback delivery
** masks the VM event it raises and switches the VM state to C, so those two
** are self-inflicted and must not be classified.
*/
static int int3_context(global_State *g)
{
  uint32_t hooks = g->hookmask & (HOOK_ACTIVE|HOOK_GC|HOOK_VMEVENT|HOOK_PROFILE);
  int ctx = 0;
  if (int3_delivering(g)) hooks &= ~(uint32_t)HOOK_VMEVENT;
  if (hooks & HOOK_ACTIVE) ctx |= INT3_CTX_HOOK;
  /* A GC hook sets HOOK_ACTIVE, too, so this is the hard subset. */
  if (hooks & (HOOK_GC|HOOK_VMEVENT|HOOK_PROFILE)) ctx |= INT3_CTX_HARD;
#if LJ_HASJIT
  {
    int vmst = g->vmstate;
    if (vmst == ~LJ_VMST_EXIT || vmst == ~LJ_VMST_RECORD ||
	vmst == ~LJ_VMST_OPT || vmst == ~LJ_VMST_ASM)
      ctx |= INT3_CTX_VMSTATE;  /* Machine code, recording or compiling. */
  }
#endif
  return ctx;
}

/* Reject unsafe contexts: GC, a foreign hook or VM event, a profiler
** callback, the recorder, the compiler or running machine code. Our own
** callback delivery is a safe context: it only allows one operation at a
** time and lj_int3_hit() re-establishes the VM context around it.
*/
static int int3_unsafe(global_State *g)
{
  return int3_context(g) != 0;
}

/* How arm() may treat its calling context. */
#define INT3_ARM_SAFE		0	/* Mount immediately if possible. */
#define INT3_ARM_PENDING	1	/* Register, mount at the next safe point. */
#define INT3_ARM_BUSY		2	/* Refuse. */

/* A request is only accepted from a place that can reach a safe point:
** the host, the interpreter, a Lua debug hook, the own callback, or the
** recorder/compiler (pending only, committed at the next dispatch).
*/
static int int3_arm_context(global_State *g)
{
  uint32_t hooks = g->hookmask & (HOOK_ACTIVE|HOOK_GC|HOOK_VMEVENT|HOOK_PROFILE);
  int recording = int3_recording(g);
  if (int3_delivering(g)) hooks &= ~(uint32_t)HOOK_VMEVENT;
  /* A GC hook sets HOOK_ACTIVE, too: test the hard bits first. */
  if (hooks & (HOOK_GC|HOOK_PROFILE)) return INT3_ARM_BUSY;
  if (hooks & HOOK_VMEVENT) {
    /* Only the recorder's own VM event is bookkeeping, never a foreign one. */
    return recording ? INT3_ARM_PENDING : INT3_ARM_BUSY;
  }
  if (recording) return INT3_ARM_PENDING;  /* No flush, no NOJIT change. */
  if (hooks & HOOK_ACTIVE) return INT3_ARM_PENDING;  /* Lua debug hook. */
  /* Outside a hook a VM state means machine code is running. */
  {
    int vmst = g->vmstate;
    if (vmst != ~LJ_VMST_INTERP && vmst != ~LJ_VMST_C)
      return INT3_ARM_BUSY;
  }
  return INT3_ARM_SAFE;
}

/* Non-zero if a pending request may be mounted right now: no hook work of a
** foreign event, no GC, no profiler, no recorder/compiler, no running trace.
*/
static int int3_commit_ready(global_State *g)
{
  if (int3_context(g) & (INT3_CTX_HARD|INT3_CTX_VMSTATE)) return 0;
#if LJ_HASJIT
  {
    jit_State *J = G2J(g);
    if (J->state != LJ_TRACE_IDLE) return 0;
    if (tvref(g->jit_base) != NULL) return 0;  /* Never on-trace. */
    if (J->patchpc != NULL) return 0;  /* Never mid-patch. */
  }
#endif
  return 1;
}

#if LJ_HASJIT

/* Non-zero if a trace executes, starts in or inlines the prototype.
**
** Two exact signals are used:
** 1. The recorder specializes every recorded Lua call on a GC constant: the
**    prototype (polymorphic closures) or the closure itself, see
**    rec_call_specialize(). Such a constant proves that this prototype was
**    recorded, i.e. inlined, even if it has no snapshot of its own. Only the
**    constant area is read (trace_isconstref(), the same refs jit.util.tracek
**    exposes), so no Lua code and no value materialisation is involved.
** 2. A snapshot PC inside the prototype proves that the bytecode was
**    recorded at that point (the innermost frame is this prototype there).
** The starting prototype covers the root trace and all its side traces.
*/
static int int3_trace_touches(GCtrace *T, GCproto *pt)
{
  GCobj *pto = obj2gco(pt);
  uintptr_t bc = (uintptr_t)proto_bc(pt), bce = bc + pt->sizebc*4;
  IRRef ref;
  SnapNo sn;
  if (gcref(T->startpt) == pto) return 1;
  for (ref = T->nk; trace_isconstref(T->nk, ref); ref++) {
    IRIns *ir = &T->ir[ref];
    if (ir->o == IR_KSLOT) ir = &T->ir[ir->op1];  /* Same as jit.util.tracek. */
    if (ir->o == IR_KGC) {
      GCobj *o = ir_kgc(ir);
      if (o == pto) return 1;
      if (o->gch.gct == ~LJ_TFUNC) {
	GCfunc *fn = gco2func(o);
	if (isluafunc(fn) && funcproto(fn) == pt) return 1;
      }
    }
  }
  for (sn = 0; sn < T->nsnap; sn++) {
    SnapShot *snap = &T->snap[sn];
    uintptr_t pc = (uintptr_t)snap_pc(&T->snapmap[snap->mapofs+snap->nent]);
    if (pc >= bc && pc < bce) return 1;
  }
  return 0;
}

/* Non-zero if the trace reaches the prototype directly or through the traces
** it links to. A link is a real execution edge: asm_tail_fixup() emits a
** direct jump to the linked trace's machine code, so clearing the metadata
** link would neither stop that code from running nor keep its trace alive.
*/
static int int3_trace_reaches(jit_State *J, GCtrace *T, GCproto *pt)
{
  MSize n = J->sizetrace;  /* Bound the walk: links form short chains. */
  while (T != NULL) {
    if (int3_trace_touches(T, pt)) return 1;
    if (T->link == 0 || T->link == T->traceno || n-- == 0) return 0;
    T = traceref(J, T->link);
  }
  return 0;
}

/* Flush all traces that execute or inline the prototype, and every trace that
** jumps into one of them through a compiled link. Only the root traces that
** can reach this bytecode are dropped, every other trace is kept.
*/
static void int3_flush_traces(jit_State *J, GCproto *pt)
{
  MSize i;
  for (i = 1; i < J->sizetrace; i++) {
    GCtrace *T = traceref(J, i);
    if (T != NULL && int3_trace_reaches(J, T, pt))
      lj_trace_flush(J, T->root ? T->root : (TraceNo)i);
  }
}

#endif

/* -- Shadow memory ------------------------------------------------------- */

/* Reserve the shadow of a prototype. This is a direct allocator call, so it
** cannot raise a Lua error from a read-only callback; the memory is accounted
** like any other VM allocation and never triggers a GC step.
*/
static BCIns *int3_reserve(global_State *g, MSize size)
{
  void *p = g->allocf(g->allocd, NULL, 0, size);
  if (p == NULL) return NULL;
  if (!checkptrGC(p)) {  /* Not representable as a GC pointer. */
    g->allocf(g->allocd, p, size, 0);
    return NULL;
  }
  g->gc.total = g->gc.total + (GCSize)size;
  return (BCIns *)p;
}

/* Free the reserved shadow of an owner. */
static void int3_release_shadow(global_State *g, Int3Owner *o)
{
  if (o->shadow != NULL) {
    lj_mem_free(g, o->shadow, o->size);
    o->shadow = NULL;
    o->size = 0;
  }
}

/* Drop the cached fetch bias if it belongs to this prototype. */
static void int3_cache_clear(global_State *g, GCproto *pt)
{
  if (mref(g->int3pc, const BCIns *) == (const BCIns *)proto_bc(pt)) {
    g->int3bias = 0;
    setmref(g->int3pc, NULL);
  }
}

/* Withdraw the published mapping of an owner and free everything it holds.
** The mapping goes first: no instruction fetch may use the shadow after it
** was released, and the JIT exclusion is the very last thing to go.
*/
static void int3_release_owner(global_State *g, Int3Owner *o)
{
  GCproto *pt = o->pt;
  int mounted = o->mounted;
  int owns_nojit = o->owns_nojit;
  int3_cache_clear(g, pt);
  if (mounted) pt->int3ofs = 0;  /* Withdraw the mapping first. */
  int3_release_shadow(g, o);  /* Then release the shadow it maps. */
  if (owns_nojit) {
    pt->flags &= ~PROTO_NOJIT;
#if LJ_HASJIT
    lj_trace_reenableproto(pt);  /* Unpatch lazy blacklisting. */
#endif
  }
  o->pt = NULL;
  o->nreq = 0;
  o->mounted = 0;
  o->owns_nojit = 0;
}

/* Remove one request. Restores the slot in the shadow if the owner survives,
** and releases the owner with the very last request.
*/
static void int3_remove_entry(global_State *g, int i)
{
  Int3State *st = &int3_state;
  Int3Entry *e = &st->entry[i];
  GCproto *pt = e->pt;
  if (pt == NULL) return;
  {
    int oi = int3_find_owner(st, pt);
    if (oi >= 0) {
      Int3Owner *o = &st->owner[oi];
      if (e->state == INT3_MOUNTED && o->shadow != NULL)
	o->shadow[e->bcpos] = proto_bc(pt)[e->bcpos];  /* Canonical slot. */
      if (e->state == INT3_PENDING && st->npending > 0) st->npending--;
      if (--o->nreq == 0) int3_release_owner(g, o);
    }
  }
  e->pt = NULL;
  e->bcpos = 0;
  e->state = 0;
  st->count--;
  setgcrefnull(g->gcroot[GCROOT_INT3+i]);
}

/* Remove every request. */
static void int3_cancel_all(global_State *g)
{
  int i;
  for (i = 0; i < LJ_INT3_MAX; i++)
    int3_remove_entry(g, i);
}

/* -- Mounting ------------------------------------------------------------ */

/* Mount every pending request of one owner. The first mount flushes the
** traces of the prototype, keeps it out of the JIT and copies the cleaned-up
** canonical bytecode into the shadow; a later request only plants its slot.
*/
static void int3_mount_owner(global_State *g, Int3Owner *o)
{
  Int3State *st = &int3_state;
  GCproto *pt = o->pt;
  int i;
  if (!o->mounted) {
#if LJ_HASJIT
    int3_flush_traces(G2J(g), pt);
#endif
    o->owns_nojit = !(pt->flags & PROTO_NOJIT);
    pt->flags |= PROTO_NOJIT;
    /* Copy after the flush: flushing may unput the canonical bytecode. */
    memcpy(o->shadow, proto_bc(pt), o->size);
  }
  for (i = 0; i < LJ_INT3_MAX; i++) {
    Int3Entry *e = &st->entry[i];
    if (e->pt == pt && e->state == INT3_PENDING) {
      setbc_op(&o->shadow[e->bcpos], BC_INT3);  /* Keep A/B/C/D. */
      e->state = INT3_MOUNTED;
      st->npending--;
    }
  }
  if (!o->mounted) {
    uintptr_t ofs = (uintptr_t)o->shadow - (uintptr_t)proto_bc(pt);
    o->mounted = 1;
    pt->int3ofs = ofs;  /* Publish the mapping last. */
    if (mref(g->int3pc, const BCIns *) == (const BCIns *)proto_bc(pt))
      g->int3bias = ofs;  /* The running frame is this prototype. */
  }
}

/* Mount every pending request if this is a safe point. */
void lj_int3_commit(lua_State *L)
{
  Int3State *st = &int3_state;
  global_State *g = G(L);
  int i;
  if (st->g != g || st->state != INT3_ENABLED || st->npending == 0) return;
  if (!int3_commit_ready(g)) return;
  for (i = 0; i < LJ_INT3_MAX; i++) {
    Int3Owner *o = &st->owner[i];
    if (o->pt != NULL && int3_owner_pending(st, o->pt))
      int3_mount_owner(g, o);
  }
  if (st->npending == 0)
    lj_dispatch_update(g, 0);  /* Drop the forced C dispatch. */
}

/* -- Requests ------------------------------------------------------------ */

/* Accept a request for (pt, bcpos). The caller has validated the session, the
** prototype and the slot. No bytecode is copied before the safe point.
*/
static int int3_arm_request(global_State *g, GCproto *pt, BCPos bcpos)
{
  Int3State *st = &int3_state;
  Int3Owner *o;
  int i, oi;
  /* Arming a slot that is already armed is a no-op. */
  if (int3_find_ins(st, pt, bcpos) >= 0) return LUAJIT_INT3_OK;
  for (i = 0; i < LJ_INT3_MAX; i++)
    if (st->entry[i].pt == NULL) break;
  if (i == LJ_INT3_MAX) return LUAJIT_INT3_FULL;
  oi = int3_find_owner(st, pt);
  if (oi < 0) {
    MSize size = (MSize)pt->sizebc * (MSize)sizeof(BCIns);
    BCIns *shadow;
    for (oi = 0; oi < LJ_INT3_MAX; oi++)
      if (st->owner[oi].pt == NULL) break;
    if (oi == LJ_INT3_MAX) return LUAJIT_INT3_FULL;
    shadow = int3_reserve(g, size);
    if (shadow == NULL) return LUAJIT_INT3_NOMEM;
    o = &st->owner[oi];
    memset(o, 0, sizeof(*o));
    o->pt = pt;
    o->shadow = shadow;
    o->size = size;
  } else {
    o = &st->owner[oi];
  }
  /* Root the prototype before anything can reach its shadow. */
  setgcref(g->gcroot[GCROOT_INT3+i], obj2gco(pt));
  st->entry[i].pt = pt;
  st->entry[i].bcpos = bcpos;
  st->entry[i].state = INT3_PENDING;
  o->nreq++;
  st->count++;
  if (st->npending++ == 0) {
    /* First pending request: install DISPMODE_INT3 now, so a safe point is
    ** reached even if this dispatch never returns to lj_dispatch_ins() or
    ** lj_dispatch_call() (e.g. a hook that unhooks itself, arms and raises).
    */
    lj_dispatch_update(g, 0);
  }
  return LUAJIT_INT3_OK;
}

int lj_int3_has_pending(global_State *g)
{
  Int3State *st = &int3_state;
  return st->g == g && st->npending > 0;
}

/* Despecialize a failed ISNEXT into JMP/ITERC. pc points one slot past the
** ISNEXT. Both the canonical bytecode and a published shadow are updated, so
** the shadow can never disagree about the instruction stream.
*/
BCIns *LJ_FASTCALL lj_int3_isnext(lua_State *L, const BCIns *pc)
{
  Int3State *st = &int3_state;
  global_State *g = G(L);
  BCIns *isnext = (BCIns *)pc - 1;
  BCIns *target = (BCIns *)pc + bc_j(*isnext);
  BCOp op;
  int oi = -1;
  if (st->g == g && st->count != 0) oi = int3_owner_of_pc(st, pc);
  setbc_op(isnext, BC_JMP);  /* Keep the jump and its operands. */
  op = bc_op(*target);
#if LJ_HASJIT
  if (op == BC_JLOOP) {  /* Restore the original instruction of the trace. */
    jit_State *J = G2J(g);
    MSize traceno = (MSize)bc_d(*target);
    GCtrace *T = traceno > 0 && traceno < J->sizetrace ?
		 traceref(J, traceno) : NULL;
    if (T != NULL) {
      *target = T->startins;
      op = bc_op(*target);
    }
  }
#else
  UNUSED(g);
#endif
  if (op == BC_ITERN) setbc_op(target, BC_ITERC);
  if (oi >= 0) {
    Int3Owner *o = &st->owner[oi];
    if (o->mounted) {
      BCIns *bc = proto_bc(o->pt);
      uintptr_t last = (uintptr_t)bc + o->pt->sizebc*(uintptr_t)sizeof(BCIns);
      if ((uintptr_t)isnext >= (uintptr_t)bc && (uintptr_t)isnext < last)
	o->shadow[isnext - bc] = *isnext;
      if ((uintptr_t)target >= (uintptr_t)bc && (uintptr_t)target < last)
	o->shadow[target - bc] = *target;
    }
  }
  return target;
}

int lj_int3_active(void)
{
  return int3_state.g != NULL;
}

/* A caller disabled JIT compilation for this prototype itself (jit.off). The
** request no longer owns the PROTO_NOJIT bit, so cancel/stop must keep it and
** must not re-enable compilation behind the caller's back.
*/
void lj_int3_nojit_release(global_State *g, GCproto *pt)
{
  Int3State *st = &int3_state;
  int oi;
  if (st->g != g || st->count == 0) return;
  oi = int3_find_owner(st, pt);
  if (oi >= 0 && st->owner[oi].mounted) st->owner[oi].owns_nojit = 0;
}

/* Non-zero if the JIT must not be enabled for this prototype (or for a
** subtree containing it): that would clear PROTO_NOJIT of a mounted owner
** and let a new trace inline the trapped bytecode. A pending request has not
** excluded the prototype yet, so it does not block.
*/
static int int3_proto_contains(GCproto *pt, GCproto *target)
{
  ptrdiff_t i;
  if (!(pt->flags & PROTO_CHILD)) return 0;
  for (i = -(ptrdiff_t)pt->sizekgc; i < 0; i++) {
    GCobj *o = proto_kgc(pt, i);
    if (o->gch.gct == ~LJ_TPROTO) {
      GCproto *cp = gco2pt(o);
      if (cp == target || int3_proto_contains(cp, target)) return 1;
    }
  }
  return 0;
}

int lj_int3_jit_on_blocked(global_State *g, GCproto *pt)
{
  Int3State *st = &int3_state;
  int i;
  if (st->g != g || st->count == 0) return 0;
  for (i = 0; i < LJ_INT3_MAX; i++) {
    Int3Owner *o = &st->owner[i];
    if (o->pt != NULL && o->mounted &&
	(pt == o->pt || int3_proto_contains(pt, o->pt)))
      return 1;
  }
  return 0;
}

void lj_int3_close(global_State *g)
{
  Int3State *st = &int3_state;
  if (st->g != g) return;
  int3_cancel_all(g);  /* No callback, no JIT state changes on teardown. */
  g->int3bias = 0;
  setmref(g->int3pc, NULL);
  st->cb = NULL;
  st->data = NULL;
  st->delivering = 0;
  st->state = INT3_NONE;
  st->g = NULL;
}

/* -- Interpreter dispatch ------------------------------------------------ */

/* Called from the VM when a shadow slot holds BC_INT3. PC points past the
** trapped slot. The request stays armed and the callback is invoked once per
** fetch, then the VM re-dispatches the original instruction. Returns that
** instruction: the callback may cancel the session, so neither the shadow nor
** the entry may be read after the delivery.
*/
LJ_FUNCA BCIns LJ_FASTCALL lj_int3_hit(lua_State *L, const BCIns *pc)
{
  ERRNO_SAVE
  Int3State *st = &int3_state;
  global_State *g = G(L);
  GCproto *pt;
  BCIns ins;
  BCPos bcpos;
  uint8_t mask;
  int vmstate, i;
  void *cf;
  i = int3_find_slot(g, pc-1);
  if (i < 0 || st->state != INT3_ENABLED ||
      st->entry[i].state != INT3_MOUNTED)
    lj_err_throw(L, LJ_ERR_BCFMT);  /* Stray trap: not a valid bytecode. */
  /* Copy the delivery out of the entry first: the callback may cancel or
  ** stop the session, which releases the shadow and the entry.
  */
  pt = st->entry[i].pt;
  bcpos = st->entry[i].bcpos;
  ins = *(pc-1);  /* The canonical array always holds the real instruction. */
  /* Establish a consistent read-only view of the original instruction. */
  cf = cframe_raw(L->cframe);
  setcframe_pc(cf, pc);
  L->top = L->base + lj_dispatch_topslot(pt, pc, cframe_multres_n(cf));
  /* Isolate the callback like a VM event: no hooks, no JIT, no reentry. */
  st->delivering = 1;
  mask = g->hookmask;
  vmstate = g->vmstate;
  g->hookmask = HOOK_VMEVENT;
  lj_dispatch_update(g, 0);
  setvmstate(g, C);  /* The callback is native code, not the interpreter. */
  st->cb(st->data, L, pt, (unsigned int)bcpos);
  g->vmstate = vmstate;
  g->hookmask = mask;
  lj_dispatch_update(g, 0);
  st->delivering = 0;
  ERRNO_RESTORE
  return ins;
}

/* -- Public breakpoint API ----------------------------------------------- */

LUA_API int luaJIT_int3_start(lua_State *L, luaJIT_int3_callback cb,
			      void *data)
{
  Int3State *st = &int3_state;
  global_State *g = G(L);
  if (cb == NULL) return LUAJIT_INT3_INVALID;
  if (st->g != NULL || st->delivering) return LUAJIT_INT3_BUSY;
  if (lj_profile_active()) return LUAJIT_INT3_BUSY;  /* Profiler in use. */
  if (int3_unsafe(g)) return LUAJIT_INT3_BUSY;
  /* The JIT engine is not touched: only mounting affects trace state. */
  st->g = g;
  st->cb = cb;
  st->data = data;
  st->state = INT3_ENABLED;
  st->count = 0;
  st->npending = 0;
  st->delivering = 0;
  memset(st->entry, 0, sizeof(st->entry));
  memset(st->owner, 0, sizeof(st->owner));
  return LUAJIT_INT3_OK;
}

/* Arm the breakpoint at bcpos of the Lua function at func_index.
**
** OK means the request was accepted, not that it is already mounted. A host
** call and the own callback mount it before returning; a Lua debug hook and
** the recorder/compiler leave it pending, and the C dispatch that leads to
** the next safe point mounts it there. The instruction the hook belongs to
** still executes from the canonical bytecode exactly once, because only new
** fetches read the shadow.
**
** Every other operation (start, cancel, stop) is still refused inside a hook,
** and the GC, profiler, foreign VM event and machine-code contexts are
** refused for arming as well.
*/
LUA_API int luaJIT_int3_arm(lua_State *L, int func_index, unsigned int bcpos)
{
  Int3State *st = &int3_state;
  global_State *g = G(L);
  GCfunc *fn;
  GCproto *pt;
  int ctx, res;
  if (st->g != g) return LUAJIT_INT3_DISABLED;
  if (st->state != INT3_ENABLED) return LUAJIT_INT3_DISABLED;
  ctx = int3_arm_context(g);
  if (ctx == INT3_ARM_BUSY) return LUAJIT_INT3_BUSY;
  if (lua_type(L, func_index) != LUA_TFUNCTION)
    return LUAJIT_INT3_INVALID;
  fn = (GCfunc *)lua_topointer(L, func_index);  /* GCfunc sits at GCobj. */
  if (fn == NULL || !isluafunc(fn)) return LUAJIT_INT3_INVALID;
  pt = funcproto(fn);
  if (bcpos >= pt->sizebc) return LUAJIT_INT3_INVALID;
  if (int3_mutable_op(bc_op(proto_bc(pt)[bcpos])))
    return LUAJIT_INT3_UNARMABLE;
  res = int3_arm_request(g, pt, (BCPos)bcpos);
  if (res == LUAJIT_INT3_OK && ctx == INT3_ARM_SAFE)
    lj_int3_commit(L);  /* Mount right away if this is a safe point. */
  return res;
}

LUA_API int luaJIT_int3_cancel(lua_State *L)
{
  Int3State *st = &int3_state;
  global_State *g = G(L);
  int pending;
  if (st->g == NULL) return LUAJIT_INT3_OK;  /* Nothing to cancel. */
  if (st->g != g) return LUAJIT_INT3_DISABLED;
  if (int3_unsafe(g)) return LUAJIT_INT3_BUSY;
  pending = st->npending;
  int3_cancel_all(g);  /* The prototypes become compilable again. */
  if (pending != 0) lj_dispatch_update(g, 0);
  return LUAJIT_INT3_OK;
}

LUA_API int luaJIT_int3_stop(lua_State *L)
{
  Int3State *st = &int3_state;
  global_State *g = G(L);
  int pending;
  if (st->g == NULL) return LUAJIT_INT3_OK;
  if (st->g != g) return LUAJIT_INT3_DISABLED;
  if (int3_unsafe(g)) return LUAJIT_INT3_BUSY;
  pending = st->npending;
  int3_cancel_all(g);
  if (pending != 0) lj_dispatch_update(g, 0);
  st->cb = NULL;
  st->data = NULL;
  st->state = INT3_NONE;
  st->g = NULL;
  return LUAJIT_INT3_OK;
}

#else  /* !LJ_HASPROFILE */

/* Profiling support is compiled out: no breakpoint can ever be armed and the
** interpreter reads the canonical bytecode, so the dispatch targets are
** unreachable. lj_int3_isnext() keeps the bytecode despecialization of the
** assembly ISNEXT handler correct for the GOT of the MIPS targets.
*/

int lj_int3_has_pending(global_State *g)
{
  UNUSED(g);
  return 0;
}

void lj_int3_commit(lua_State *L)
{
  UNUSED(L);
}

BCIns *LJ_FASTCALL lj_int3_isnext(lua_State *L, const BCIns *pc)
{
  BCIns *isnext = (BCIns *)pc - 1;
  BCIns *target = (BCIns *)pc + bc_j(*isnext);
  BCOp op;
  setbc_op(isnext, BC_JMP);
  op = bc_op(*target);
#if LJ_HASJIT
  if (op == BC_JLOOP) {
    jit_State *J = L2J(L);
    MSize traceno = (MSize)bc_d(*target);
    GCtrace *T = traceno > 0 && traceno < J->sizetrace ?
		 traceref(J, traceno) : NULL;
    if (T != NULL) {
      *target = T->startins;
      op = bc_op(*target);
    }
  }
#else
  UNUSED(L);
#endif
  if (op == BC_ITERN) setbc_op(target, BC_ITERC);
  return target;
}

int lj_int3_jit_on_blocked(global_State *g, GCproto *pt)
{
  UNUSED(g); UNUSED(pt);
  return 0;
}

int lj_int3_active(void)
{
  return 0;
}

void lj_int3_nojit_release(global_State *g, GCproto *pt)
{
  UNUSED(g); UNUSED(pt);
}

void lj_int3_close(global_State *g)
{
  UNUSED(g);
}

LJ_FUNCA BCIns LJ_FASTCALL lj_int3_hit(lua_State *L, const BCIns *pc)
{
  UNUSED(pc);
  lj_err_throw(L, LJ_ERR_BCFMT);
  return 0;
}

LUA_API int luaJIT_int3_start(lua_State *L, luaJIT_int3_callback cb,
			      void *data)
{
  UNUSED(L); UNUSED(cb); UNUSED(data);
  return LUAJIT_INT3_UNAVAILABLE;
}

LUA_API int luaJIT_int3_arm(lua_State *L, int func_index, unsigned int bcpos)
{
  UNUSED(L); UNUSED(func_index); UNUSED(bcpos);
  return LUAJIT_INT3_UNAVAILABLE;
}

LUA_API int luaJIT_int3_cancel(lua_State *L)
{
  UNUSED(L);
  return LUAJIT_INT3_UNAVAILABLE;
}

LUA_API int luaJIT_int3_stop(lua_State *L)
{
  UNUSED(L);
  return LUAJIT_INT3_UNAVAILABLE;
}

#endif

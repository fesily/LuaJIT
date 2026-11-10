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
#include "lj_gc.h"	/* lj_mem_newt/lj_mem_freet */
#include "lj_tab.h"
#include "lj_dispatch.h"
#include "lj_int3.h"

#include "luajit.h"

#if LJ_HASPROFILE
#include "lj_jit.h"
#include "lj_trace.h"
#include "lj_profile.h"
#endif

/* A session owns up to LJ_INT3_MAX breakpoints of this VM. It shares the
** profiler mutual exclusion (at most one of profiler and INT3 is active) and
** leaves the JIT engine running: mounting a breakpoint flushes exactly those
** traces that execute or inline the armed prototype and excludes that
** prototype from JIT compilation, so every later execution of its bytecode is
** an interpreter dispatch. No bytecode other than the armed slots is touched
** and no other trace is dropped.
**
** A request is accepted as pending and mounted at the next safe point: the
** prototype is flushed, kept out of the JIT and only then its bytecode slot is
** replaced by BC_INT3. Until then the request only holds a GC root and a
** forced C dispatch that leads to that safe point. Arming from a Lua debug
** hook, or from the recorder or the compiler, therefore never has to patch a
** slot behind the dispatcher, and the instruction that is already being
** dispatched still executes once with its original opcode.
**
** A breakpoint is permanent: the slot keeps the BC_INT3 opcode (with the
** original A/B/C/D operands) and keeps its entry, GC root and JIT exclusion
** until it is explicitly disarmed by luaJIT_int3_cancel(), by
** luaJIT_int3_stop() or by a VM teardown. Every execution of the slot
** delivers one callback; the handler then executes the shadowed instruction
** from the entry instead of from the bytecode. Re-arming an armed slot is a
** no-op, so a callback can keep trapping its own slot without any per-hit
** bookkeeping.
**
** Every request is a slot of the session's anchor table (the single GC root
** GCROOT_INT3, whose array part has one entry per request), so a prototype
** that carries a trap can never be collected: the slot is filled before the
** slot is patched and cleared only after it was restored, and lj_int3_close()
** does the same before the finalizer run and lj_gc_freeall() of a VM teardown.
** That is why no prototype-free hook is needed: apart from the armed
** breakpoints (anchored) and the delivery in progress (the executing
** function's prototype), this module holds no prototype pointer.
**
** All requests of one prototype share one owner, which holds the request
** count and the PROTO_NOJIT ownership, so the last request releases the
** exclusion no matter in which order the requests are disarmed.
**
** The session itself belongs to a VM: it is allocated from that VM's
** allocator on demand and hung off global_State (g->int3), so it can neither
** outlive its VM nor be inherited by a later one, and a VM that never starts
** a session pays one pointer. The only process-wide fact left is the session
** count, which is what keeps the profiler exclusion symmetric: the profiler
** owns its own process-wide state, so "an INT3 session is active somewhere"
** must be answerable without a VM.
**/

#if LJ_HASPROFILE

#define INT3_NONE	0	/* No session. */
#define INT3_ENABLED	1	/* Session accepts requests. */

#define INT3_PENDING	0	/* Request accepted, slot not patched yet. */
#define INT3_MOUNTED	1	/* Request is planted in the bytecode. */

typedef struct Int3Entry {
  GCproto *pt;			/* Prototype of the breakpoint. */
  BCPos bcpos;			/* Bytecode index of the trapped slot. */
  BCIns ins;			/* Original instruction, captured at mount. */
  uint8_t state;		/* INT3_PENDING/INT3_MOUNTED. */
} Int3Entry;

typedef struct Int3Owner {
  GCproto *pt;			/* Prototype owning the requests. */
  int nreq;			/* Number of live requests of this owner. */
  uint8_t mounted;		/* The prototype is flushed and excluded. */
  uint8_t owns_nojit;		/* We set PROTO_NOJIT for this owner. */
} Int3Owner;

struct Int3State {
  luaJIT_int3_callback cb;	/* Native read-only callback. */
  void *data;			/* Callback data. */
  int state;			/* INT3_NONE/INT3_ENABLED. */
  int count;			/* Number of live requests. */
  int nmax;			/* One past the highest used slot index. */
  int npending;			/* Number of pending requests. */
  int delivering;		/* Callback delivery in progress. */
  Int3Entry entry[LJ_INT3_MAX];	/* Accepted requests. */
  Int3Owner owner[LJ_INT3_MAX];	/* Prototypes with a request. */
  BCIns *slot[LJ_INT3_MAX];	/* Mounted slots: slot[i] != NULL <=> entry[i]. */
};

/* Number of VMs that own a session, i.e. 0 or 1 by policy. Only the profiler
** exclusion reads it: everything else goes through g->int3.
*/
static int int3_sessions;

/* Index of the breakpoint mounted on this slot, or -1.
**
** The slot addresses are kept in a parallel array: the scan then only
** compares pointers, without chasing the prototype of every entry. This runs
** for every dispatched instruction while a breakpoint session is armed and
** instruction dispatch is instrumented, so it is the one hot loop here. A
** pending request has not replaced its slot and leaves slot[i] NULL, so this
** is also the test for "the slot holds a trap".
*/
static int int3_find(global_State *g, const BCIns *pc)
{
  Int3State *st = g->int3;
  int i;
  if (st == NULL || st->count == 0) return -1;
  /* Only the used prefix is scanned: a few breakpoints cost a few compares. */
  for (i = 0; i < st->nmax; i++)
    if (st->slot[i] == pc) return i;
  return -1;
}

/* Index of the request for (pt, bcpos) in any state, or -1. */
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

/* Non-zero if at least one request of this owner is not mounted yet. */
static int int3_owner_pending(Int3State *st, GCproto *pt)
{
  int i;
  for (i = 0; i < LJ_INT3_MAX; i++)
    if (st->entry[i].pt == pt && st->entry[i].state == INT3_PENDING) return 1;
  return 0;
}

/* The session's anchor table: one array slot per request, so the prototype of
** every accepted request is reachable by the GC through the single root
** GCROOT_INT3. Created by luaJIT_int3_start(), released by
** luaJIT_int3_stop() and lj_int3_close() once the slots are cleared.
*/
static LJ_AINLINE GCtab *int3_roots(global_State *g)
{
  return tabref(g->gcroot[GCROOT_INT3]);
}

/* Fill (pt != NULL) or clear the anchor slot of request i. */
static void int3_root_set(global_State *g, int i, GCproto *pt)
{
  GCtab *t = int3_roots(g);
  TValue *tv;
  lj_assertG(t != NULL && t->asize > (uint32_t)i, "bad int3 anchor slot");
  tv = &tvref(t->array)[i];
  if (pt != NULL) {
    setgcVraw(tv, obj2gco(pt), LJ_TPROTO);
    /* The table lives across GC cycles, so the store needs a barrier. */
    if (LJ_UNLIKELY(isblack(obj2gco(t)))) lj_gc_barrierback(g, t);
  } else {
    setnilV(tv);  /* Dropping a reference needs no barrier. */
  }
}

/* Non-zero if lj_int3_hit() is delivering a callback for this VM. The
** delivery is masked as a VM event and switches the VM state to C, so those
** two are self-inflicted and must not be rejected by the checks below.
*/
static LJ_AINLINE int int3_delivering(global_State *g)
{
  Int3State *st = g->int3;
  return st != NULL && st->delivering != 0;
}

/* Non-zero for a bytecode slot that must never carry a breakpoint.
**
** Almost every opcode the JIT rewrites is fine now: mounting a request flushes
** the traces of its prototype and sets PROTO_NOJIT before the slot is patched,
** and the prototype then never records or traces again (enabling JIT
** compilation for it is refused), so trace_start()'s lazy I-patch, trace_stop(),
** trace_unpatch(), the temporary substitution in trace_ins() and
** blacklist_pc() cannot reach a mounted slot. Each of those writers asserts
** that the slot it is about to rewrite is not trapped, so the invariant cannot
** rot silently. What remains is rewritten by the interpreter itself:
**
** - BC_ISNEXT rewrites its own slot (BC_JMP) and its neighbour (ITERN ->
**   ITERC, or a JLOOP slot back to ITERC) when its checks fail, so both the
**   prep slot and the slot it points at are refused.
** - Function headers are refused in every form (IFUNCF/JFUNCF/IFUNCV/JFUNCV
**   are the patched variants of FUNCF/FUNCV): the frame and the vararg layout
**   are not set up when the header runs, so the delivery could not give the
**   callback a consistent view.
**
** BC_RETM is deliberately *not* listed: the tracer never rewrites it
** (trace_downrec() refuses RETM and trace_stop() patches only
** RET/RET0/RET1/ITERN to BC_JLOOP), so it is a trap site like any other
** opcode. The dispatched jumps (loop back edges, UCLO, ISNEXT's prep, for-prep)
** are armable; a jump slot that a comparison consumes is rejected separately by
** int3_paired_jump(), because the interpreter never dispatches it.
*/
static int int3_mutable_op(BCOp op)
{
  switch (op) {
  case BC_ISNEXT: case BC_ITERN:
  case BC_FUNCF: case BC_FUNCV:
  /* The patched forms of a function header, e.g. after a jit.off/on cycle or
  ** a trace that started at the header.
  */
  case BC_IFUNCF: case BC_JFUNCF: case BC_IFUNCV: case BC_JFUNCV:
    return 1;
  default:
    return 0;
  }
}

/* Non-zero for the jump slot that the preceding comparison or unary test
** consumes. The parser emits a JMP right after every instruction of the
** BC_ISLT..BC_ISF range (comparisons, ORDER OPR, then the unary tests): the
** comparison reads its branch target from the D field of that slot and either
** branches to it or skips the slot entirely (see the `add PC, 4` in
** build_ins()), so that JMP is never dispatched on its own and a trap there
** could never fire. The dispatched jumps (loop back edges, UCLO, ISNEXT's
** prep) are unaffected.
*/
static int int3_paired_jump(global_State *g, GCproto *pt, BCIns ins,
			    BCPos bcpos)
{
  BCOp prev;
  if (bc_op(ins) != BC_JMP || bcpos == 0) return 0;
  prev = bc_op(lj_int3_bcins(g, &proto_bc(pt)[bcpos-1]));
  return prev >= BC_ISLT && prev <= BC_ISF;
}

/* Unsafe context flags, one bit per condition. Arming is allowed from a Lua
** debug hook and from the recorder/compiler, every other operation treats all
** of them as fatal.
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
** callback delivery is a safe context: it only allows one operation at a time
** and lj_int3_hit() re-establishes the VM context around it.
*/
static int int3_unsafe(global_State *g)
{
  return int3_context(g) != 0;
}

/* How arm() may treat its calling context. */
#define INT3_ARM_SAFE		0	/* Mount immediately if possible. */
#define INT3_ARM_PENDING	1	/* Register, mount at the next safe point. */
#define INT3_ARM_BUSY		2	/* Refuse. */

/* A request is only accepted from a place that can reach a safe point: the
** host, the interpreter, a Lua debug hook, the own callback, or the
** recorder/compiler (pending only, mounted at the next dispatch).
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
  if (recording) return INT3_ARM_PENDING;  /* No flush, no bytecode write. */
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

/* -- Requests ------------------------------------------------------------ */

/* Drop the owner of a prototype. The bytecode slots are restored by
** int3_remove_entry() before the last request releases the owner, so this only
** undoes the JIT exclusion.
**
** Marking the prototype PROTO_NOJIT makes the recorder lazily blacklist its
** hot spots (IFUNCF/ILOOP patches). That blacklisting must not outlive the
** breakpoints for that prototype, so re-enable compilation the same way
** jit.on(func) does once the last breakpoint for it is gone.
*/
static void int3_release_owner(global_State *g, Int3Owner *o)
{
  GCproto *pt = o->pt;
  int mounted = o->mounted;
  int owns_nojit = o->owns_nojit;
  o->pt = NULL;
  o->nreq = 0;
  o->mounted = 0;
  o->owns_nojit = 0;
  if (mounted && owns_nojit) {
    pt->flags &= ~PROTO_NOJIT;
#if LJ_HASJIT
    lj_trace_reenableproto(pt);  /* Unpatch lazy blacklisting. */
#endif
  }
}

/* Remove one request: restore its slot if it was mounted, then release the
** owner with the very last request of the prototype.
*/
static void int3_remove_entry(global_State *g, int i)
{
  Int3State *st = g->int3;
  Int3Entry *e = &st->entry[i];
  GCproto *pt = e->pt;
  if (pt == NULL) return;
  if (e->state == INT3_MOUNTED) {
    *st->slot[i] = e->ins;  /* Restore the whole instruction. */
    st->slot[i] = NULL;
    while (st->nmax > 0 && st->slot[st->nmax-1] == NULL) st->nmax--;
  } else if (st->npending > 0) {
    st->npending--;
  }
  e->pt = NULL;
  e->bcpos = 0;
  e->ins = 0;
  e->state = 0;
  st->count--;
  int3_root_set(g, i, NULL);
  {
    int oi = int3_find_owner(st, pt);
    if (oi >= 0 && --st->owner[oi].nreq == 0)
      int3_release_owner(g, &st->owner[oi]);
  }
}

/* Remove every request. */
static void int3_cancel_all(global_State *g)
{
  int i;
  for (i = 0; i < LJ_INT3_MAX; i++)
    int3_remove_entry(g, i);
}

/* Accept a request for (pt, bcpos). The caller has validated the session, the
** prototype and the slot. Nothing is flushed and no bytecode is written before
** the safe point.
*/
static int int3_arm_request(global_State *g, GCproto *pt, BCPos bcpos)
{
  Int3State *st = g->int3;
  Int3Owner *o;
  int i, oi;
  /* Arming a slot that is already armed is a no-op. */
  if (int3_find_ins(st, pt, bcpos) >= 0) return LUAJIT_INT3_OK;
  for (i = 0; i < LJ_INT3_MAX; i++)
    if (st->entry[i].pt == NULL) break;
  if (i == LJ_INT3_MAX) return LUAJIT_INT3_FULL;
  oi = int3_find_owner(st, pt);
  if (oi < 0) {
    for (oi = 0; oi < LJ_INT3_MAX; oi++)
      if (st->owner[oi].pt == NULL) break;
    if (oi == LJ_INT3_MAX) return LUAJIT_INT3_FULL;
    o = &st->owner[oi];
    memset(o, 0, sizeof(*o));
    o->pt = pt;
  } else {
    o = &st->owner[oi];
  }
  /* Anchor the prototype before anything can patch its bytecode. */
  int3_root_set(g, i, pt);
  st->entry[i].pt = pt;
  st->entry[i].bcpos = bcpos;
  st->entry[i].ins = 0;
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
  Int3State *st = g->int3;
  return st != NULL && st->npending > 0;
}

/* -- Mounting ------------------------------------------------------------ */

/* Mount every pending request of one owner. The first mount flushes the traces
** of the prototype and keeps it out of the JIT; only then may the canonical
** slot be replaced, because flushing a trace unpatches bytecode and the
** captured instruction must be the final one.
*/
static void int3_mount_owner(global_State *g, Int3Owner *o)
{
  Int3State *st = g->int3;
  GCproto *pt = o->pt;
  int i;
  if (!o->mounted) {
#if LJ_HASJIT
    int3_flush_traces(G2J(g), pt);
#endif
    /* Flush before planting: unpatching a flushed trace must not overwrite
    ** the trap. Then keep the prototype out of the JIT for as long as it
    ** carries a breakpoint, so no trace can inline or cover its bytecode.
    */
    o->owns_nojit = !(pt->flags & PROTO_NOJIT);
    pt->flags |= PROTO_NOJIT;
    o->mounted = 1;
  }
  for (i = 0; i < LJ_INT3_MAX; i++) {
    Int3Entry *e = &st->entry[i];
    if (e->pt == pt && e->state == INT3_PENDING) {
      BCIns *slot = &proto_bc(pt)[e->bcpos];
      e->ins = *slot;  /* Capture the complete original instruction. */
      e->state = INT3_MOUNTED;
      st->npending--;
      st->slot[i] = slot;
      if (i + 1 > st->nmax) st->nmax = i + 1;
      setbc_op(slot, BC_INT3);  /* Keep A/B/C/D: they stay readable. */
    }
  }
}

/* Mount every pending request if this is a safe point. */
void lj_int3_commit(lua_State *L)
{
  Int3State *st = G(L)->int3;
  global_State *g = G(L);
  int i;
  if (st == NULL || st->state != INT3_ENABLED || st->npending == 0) return;
  if (!int3_commit_ready(g)) return;
  for (i = 0; i < LJ_INT3_MAX; i++) {
    Int3Owner *o = &st->owner[i];
    if (o->pt != NULL && int3_owner_pending(st, o->pt))
      int3_mount_owner(g, o);
  }
  if (st->npending == 0)
    lj_dispatch_update(g, 0);  /* Drop the forced C dispatch. */
}

/* -- Logical bytecode view ----------------------------------------------- */

BCIns lj_int3_bcins(global_State *g, const BCIns *pc)
{
  int i = int3_find(g, pc);
  return i >= 0 ? g->int3->entry[i].ins : *pc;
}

int lj_int3_pending(global_State *g, const BCIns *pc)
{
  return int3_find(g, pc) >= 0;
}

void lj_int3_bcwrite_view(global_State *g, const GCproto *pt, BCIns *bc)
{
  Int3State *st = g->int3;
  int i;
  if (st == NULL || st->count == 0) return;
  /* bc[] is a copy of proto_bc(pt)+1, so index bcpos-1 omits the header. */
  for (i = 0; i < LJ_INT3_MAX; i++) {
    Int3Entry *e = &st->entry[i];
    if (e->pt == pt && e->state == INT3_MOUNTED && e->bcpos != 0)
      bc[e->bcpos-1] = e->ins;
  }
}

int lj_int3_active(void)
{
  return int3_sessions != 0;
}

/* Non-zero if the JIT must not be enabled for this prototype (or for a
** subtree containing it): that would clear PROTO_NOJIT and let a new trace
** inline the trapped bytecode. Only a mounted owner has excluded it; a pending
** request has not touched the JIT yet.
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
  Int3State *st = g->int3;
  int i;
  if (st == NULL || st->count == 0) return 0;
  for (i = 0; i < LJ_INT3_MAX; i++) {
    Int3Owner *o = &st->owner[i];
    if (o->pt != NULL && o->mounted &&
	(pt == o->pt || int3_proto_contains(pt, o->pt)))
      return 1;
  }
  return 0;
}

/* A caller disabled JIT compilation for this prototype itself (jit.off). The
** request no longer owns the PROTO_NOJIT bit, so cancel/stop must keep it and
** must not re-enable compilation behind the caller's back.
*/
void lj_int3_nojit_release(global_State *g, GCproto *pt)
{
  Int3State *st = g->int3;
  int oi;
  if (st == NULL || st->count == 0) return;
  oi = int3_find_owner(st, pt);
  if (oi >= 0 && st->owner[oi].mounted) st->owner[oi].owns_nojit = 0;
}

void lj_int3_close(global_State *g)
{
  Int3State *st = g->int3;
  if (st == NULL) return;
  int3_cancel_all(g);  /* No callback, no JIT state changes on teardown. */
  /* Every anchor slot was cleared by the cancel above; release the table. */
  setgcrefnull(g->gcroot[GCROOT_INT3]);
  g->int3 = NULL;
  lj_mem_freet(g, st);
  int3_sessions--;
}

/* -- Interpreter dispatch ------------------------------------------------ */

/* Called from the VM when a physical BC_INT3 is executed. PC points past the
** trapped slot. The breakpoint stays armed and the callback is invoked once
** per execution, then the VM re-dispatches the shadowed instruction.
** Returns that instruction: the callback may disarm the slot, so the
** re-dispatched opcode must not be re-read from the bytecode.
*/
LJ_FUNCA BCIns LJ_FASTCALL lj_int3_hit(lua_State *L, const BCIns *pc)
{
  ERRNO_SAVE
  Int3State *st = G(L)->int3;
  global_State *g = G(L);
  GCproto *pt;
  BCIns ins;
  BCPos bcpos;
  const BCIns *oldpc;
  uint8_t mask;
  int vmstate, i;
  void *cf;
  i = int3_find(g, pc-1);
  if (i < 0 || st->state != INT3_ENABLED)
    lj_err_throw(L, LJ_ERR_BCFMT);  /* Stray trap: not a valid bytecode. */
  /* Copy the delivery out of the entry first: the callback may cancel or
  ** stop the session, which clears it. The instruction itself must still be
  ** executed exactly once after this delivery.
  */
  pt = st->entry[i].pt;
  bcpos = st->entry[i].bcpos;
  ins = st->entry[i].ins;
  /* Establish a consistent read-only view of the original instruction. The
  ** previous PC is saved: it is what lj_dispatch_ins() compares against when
  ** the instruction is re-dispatched, and leaving the current PC behind would
  ** look like a backward jump and fire a line event for an instruction that
  ** stays on the same line.
  */
  cf = cframe_raw(L->cframe);
  oldpc = cframe_pc(cf);
  setcframe_pc(cf, pc);
  L->top = L->base + lj_dispatch_topslot(g, pt, pc, cframe_multres_n(cf));
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
  /* Restore the previous PC: lj_dispatch_ins() of the re-dispatched
  ** instruction must see the same old PC as without a breakpoint.
  */
  setcframe_pc(cframe_raw(L->cframe), oldpc);
  ERRNO_RESTORE
  return ins;
}

LJ_FUNCA BCIns LJ_FASTCALL lj_int3_ins(lua_State *L, const BCIns *pc)
{
  return lj_int3_bcins(G(L), pc-1);
}

/* -- Public breakpoint API ----------------------------------------------- */

LUA_API int luaJIT_int3_start(lua_State *L, luaJIT_int3_callback cb,
			      void *data)
{
  global_State *g = G(L);
  Int3State *st;
  GCtab *roots;
  if (cb == NULL) return LUAJIT_INT3_INVALID;
  if (g->int3 != NULL || int3_sessions != 0) return LUAJIT_INT3_BUSY;
  if (lj_profile_active()) return LUAJIT_INT3_BUSY;  /* Profiler in use. */
  if (int3_unsafe(g)) return LUAJIT_INT3_BUSY;
  /* One anchor table per session: one array slot per request, mounted under
  ** the single root before a request can exist. NOBARRIER: the table is new
  ** (marked white) and the array part is cleared to nil.
  */
  roots = lj_tab_new(L, LJ_INT3_MAX, 0);
  lj_assertG(roots->asize >= LJ_INT3_MAX, "int3 anchor table too small");
  setgcref(g->gcroot[GCROOT_INT3], obj2gco(roots));
  /* The JIT engine is not touched: only mounting affects trace state. All
  ** checks are done, so a refused start leaves no state behind. Should the
  ** session allocation fail, the anchored table stays unused until the next
  ** start overwrites the root.
  */
  st = lj_mem_newt(L, sizeof(Int3State), Int3State);
  st->cb = cb;
  st->data = data;
  st->state = INT3_ENABLED;
  st->count = 0;
  st->nmax = 0;
  st->npending = 0;
  st->delivering = 0;
  memset(st->entry, 0, sizeof(st->entry));
  memset(st->owner, 0, sizeof(st->owner));
  memset(st->slot, 0, sizeof(st->slot));
  g->int3 = st;
  int3_sessions++;
  return LUAJIT_INT3_OK;
}

/* Arm the breakpoint at bcpos of the Lua function at func_index.
**
** OK means the request was accepted, not that the slot is already patched. A
** host call and the own callback mount it before returning; a Lua debug hook
** and the recorder/compiler leave it pending, and the C dispatch that leads to
** the next safe point mounts it there. The instruction the hook belongs to
** still executes from the real bytecode, because the slot is only replaced
** after that dispatch.
**
** Every other operation (start, cancel, stop) is still refused inside a hook,
** and the GC, profiler, foreign VM event and machine-code contexts are
** refused for arming as well.
*/
LUA_API int luaJIT_int3_arm(lua_State *L, int func_index, unsigned int bcpos)
{
  Int3State *st = G(L)->int3;
  global_State *g = G(L);
  GCfunc *fn;
  GCproto *pt;
  BCIns ins;
  int ctx;
  if (st == NULL || st->state != INT3_ENABLED) return LUAJIT_INT3_DISABLED;
  ctx = int3_arm_context(g);
  if (ctx == INT3_ARM_BUSY) return LUAJIT_INT3_BUSY;
  if (lua_type(L, func_index) != LUA_TFUNCTION)
    return LUAJIT_INT3_INVALID;
  fn = (GCfunc *)lua_topointer(L, func_index);  /* GCfunc sits at GCobj. */
  if (fn == NULL || !isluafunc(fn)) return LUAJIT_INT3_INVALID;
  pt = funcproto(fn);
  if (bcpos >= pt->sizebc) return LUAJIT_INT3_INVALID;
  ins = lj_int3_bcins(g, &proto_bc(pt)[bcpos]);
  if (int3_mutable_op(bc_op(ins)) ||
      int3_paired_jump(g, pt, ins, (BCPos)bcpos))
    return LUAJIT_INT3_UNARMABLE;
  {
    int res = int3_arm_request(g, pt, (BCPos)bcpos);
    if (res == LUAJIT_INT3_OK && ctx == INT3_ARM_SAFE)
      lj_int3_commit(L);  /* Mount right away if this is a safe point. */
    return res;
  }
}

LUA_API int luaJIT_int3_cancel(lua_State *L)
{
  global_State *g = G(L);
  Int3State *st = g->int3;
  int pending;
  if (st == NULL) return LUAJIT_INT3_OK;  /* Nothing to cancel. */
  if (int3_unsafe(g)) return LUAJIT_INT3_BUSY;
  pending = st->npending;
  int3_cancel_all(g);  /* The prototypes become compilable again. */
  if (pending != 0) lj_dispatch_update(g, 0);
  return LUAJIT_INT3_OK;
}

LUA_API int luaJIT_int3_stop(lua_State *L)
{
  global_State *g = G(L);
  Int3State *st = g->int3;
  int pending;
  if (st == NULL) return LUAJIT_INT3_OK;  /* No session in this VM. */
  if (int3_unsafe(g)) return LUAJIT_INT3_BUSY;
  pending = st->npending;
  int3_cancel_all(g);
  /* Every anchor slot was cleared by the cancel above; release the table. */
  setgcrefnull(g->gcroot[GCROOT_INT3]);
  if (pending != 0) lj_dispatch_update(g, 0);
  g->int3 = NULL;
  lj_mem_freet(g, st);
  int3_sessions--;
  return LUAJIT_INT3_OK;
}

#else  /* !LJ_HASPROFILE */

/* Profiling support is compiled out: no breakpoint can ever be armed, so the
** bytecode view is the real bytecode and the dispatch targets are
** unreachable.
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

BCIns lj_int3_bcins(global_State *g, const BCIns *pc)
{
  UNUSED(g);
  return *pc;
}

int lj_int3_pending(global_State *g, const BCIns *pc)
{
  UNUSED(g); UNUSED(pc);
  return 0;
}

void lj_int3_bcwrite_view(global_State *g, const GCproto *pt, BCIns *bc)
{
  UNUSED(g); UNUSED(pt); UNUSED(bc);
}

int lj_int3_jit_on_blocked(global_State *g, GCproto *pt)
{
  UNUSED(g); UNUSED(pt);
  return 0;
}

void lj_int3_nojit_release(global_State *g, GCproto *pt)
{
  UNUSED(g); UNUSED(pt);
}

int lj_int3_active(void)
{
  return 0;
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

LJ_FUNCA BCIns LJ_FASTCALL lj_int3_ins(lua_State *L, const BCIns *pc)
{
  UNUSED(L);
  return pc[-1];
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

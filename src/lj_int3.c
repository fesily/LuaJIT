/*
** One-shot interpreter interrupt (BC_INT3).
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_int3_c
#define LUA_CORE

#include "lj_obj.h"

#include "lj_err.h"
#include "lj_frame.h"
#include "lj_func.h"
#include "lj_dispatch.h"
#include "lj_int3.h"

#include "luajit.h"

#if LJ_HASPROFILE
#include "lj_jit.h"
#include "lj_trace.h"
#include "lj_profile.h"
#endif

/* A session is a single process-wide, one-shot interrupt. It shares the
** profiler mutual exclusion (at most one of profiler and INT3 is active) and
** leaves the JIT engine running: arming a request flushes exactly those
** traces that execute or inline the armed prototype and excludes that
** prototype from JIT compilation, so every later execution of its bytecode
** is an interpreter dispatch. No bytecode other than the armed slot is
** touched and no other trace is dropped.
**
** Up to LJ_INT3_MAX requests can be pending at the same time, in any mix of
** prototypes and bytecode slots (one entry per slot), so a debugger can keep
** breakpoints in several functions.
**
** Every pending prototype is a GC root (GCROOT_INT3+i), so a prototype that
** carries a trap can never be collected: the root is taken before the opcode
** is replaced and dropped only after it was restored, and lj_int3_close()
** does the same before the finalizer run and lj_gc_freeall() of a VM
** teardown. That is why no prototype-free hook is needed: apart from the
** pending requests (rooted) and the delivery in progress (the executing
** function's prototype), this module holds no prototype pointer.
**/

#if LJ_HASPROFILE

#define INT3_NONE	0	/* No session. */
#define INT3_ENABLED	1	/* Session accepts requests. */

typedef struct Int3Entry {
  GCproto *pt;			/* Prototype of the request. */
  BCPos bcpos;			/* Bytecode index of the trapped slot. */
  BCIns ins;			/* Latest original instruction of that slot. */
  int nojit;			/* We set PROTO_NOJIT for this request. */
  int planted;			/* The slot holds BC_INT3. */
  int skipped;			/* Boundary skipped once (self-referencing slot). */
} Int3Entry;

typedef struct Int3State {
  global_State *g;		/* VM that owns the session. */
  luaJIT_int3_callback cb;	/* Native read-only callback. */
  void *data;			/* Callback data. */
  int state;			/* INT3_NONE/INT3_ENABLED. */
  int count;			/* Number of pending requests. */
  int delivering;		/* Callback delivery in progress. */
  GCproto *last_pt;		/* Prototype of the request being delivered. */
  BCPos last_bcpos;		/* Bytecode index of the request being delivered. */
  Int3Entry entry[LJ_INT3_MAX];	/* Pending requests. */
} Int3State;

static Int3State int3_state;

/* Index of the request that traps this slot, or -1. */
static int int3_find(global_State *g, const BCIns *pc)
{
  Int3State *st = &int3_state;
  int i;
  if (st->g != g || st->count == 0) return -1;
  for (i = 0; i < LJ_INT3_MAX; i++) {
    GCproto *pt = st->entry[i].pt;
    if (pt != NULL && st->entry[i].planted &&
	pc == &proto_bc(pt)[st->entry[i].bcpos]) return i;
  }
  return -1;
}

/* Index of a pending request for this slot, planted or not, or -1. */
static int int3_find_any(global_State *g, const BCIns *pc)
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

/* Non-zero while a request waits for the next instruction boundary. */
static int int3_has_suspended(Int3State *st)
{
  int i;
  for (i = 0; i < LJ_INT3_MAX; i++)
    if (st->entry[i].pt != NULL && !st->entry[i].planted) return 1;
  return 0;
}

/* Non-zero while a request waits for the next instruction boundary. */
int lj_int3_stepping(void)
{
  Int3State *st = &int3_state;
  return st->g != NULL && int3_has_suspended(st);
}

/* Index of a pending request for this prototype, or -1. */
static int int3_find_proto(Int3State *st, GCproto *pt)
{
  int i;
  for (i = 0; i < LJ_INT3_MAX; i++)
    if (st->entry[i].pt == pt) return i;
  return -1;
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

/* Reject unsafe contexts: GC, a foreign hook or VM event, a profiler
** callback, the recorder, the compiler or running machine code. Our own
** callback delivery is a safe context: it only allows one operation at a
** time and lj_int3_hit() re-establishes the VM context around it.
*/
static int int3_unsafe(global_State *g)
{
  Int3State *st = &int3_state;
  uint32_t hooks = g->hookmask & (HOOK_ACTIVE|HOOK_GC|HOOK_VMEVENT|HOOK_PROFILE);
  if (int3_delivering(g)) hooks &= ~(uint32_t)HOOK_VMEVENT;
  /* HOOK_INT3 borrows HOOK_PROFILE; the profiler cannot own it here. */
  if (st->g == g && int3_has_suspended(st)) hooks &= ~(uint32_t)HOOK_PROFILE;
  if (hooks)
    return 1;
#if LJ_HASJIT
  {
    int vmst = g->vmstate;
    if (vmst == ~LJ_VMST_EXIT || vmst == ~LJ_VMST_RECORD ||
	vmst == ~LJ_VMST_OPT || vmst == ~LJ_VMST_ASM)
      return 1;  /* Running machine code, recording or compiling. */
  }
#endif
  return 0;
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

/* Flush all traces that execute or inline the prototype. Only the root
** traces that can reach this bytecode are dropped, every other trace is
** kept.
*/
static void int3_flush_traces(jit_State *J, GCproto *pt)
{
  MSize i;
  for (i = 1; i < J->sizetrace; i++) {
    GCtrace *T = traceref(J, i);
    if (T != NULL && int3_trace_touches(T, pt))
      lj_trace_flush(J, T->root ? T->root : (TraceNo)i);
  }
  /* Never resume a flushed trace through a stitched link. */
  for (i = 1; i < J->sizetrace; i++) {
    GCtrace *T = traceref(J, i);
    if (T != NULL && T->link != 0 && T->link != i) {
      GCtrace *L = traceref(J, T->link);
      if (L != NULL && int3_trace_touches(L, pt))
	T->link = 0;
    }
  }
}

#endif

/* Drop one pending request: restore the bytecode and undo the JIT exclusion.
**
** Marking the prototype PROTO_NOJIT makes the recorder lazily blacklist its
** hot spots (IFUNCF/ILOOP patches). That blacklisting must not outlive the
** requests for that prototype, so re-enable compilation the same way
** jit.on(func) does once the last request for it is gone.
*/
static void int3_cancel_entry(global_State *g, int i)
{
  Int3State *st = &int3_state;
  Int3Entry *e = &st->entry[i];
  GCproto *pt = e->pt;
  if (pt != NULL) {
    int nojit = e->nojit;
    if (e->planted) {  /* A suspended slot still holds the original ins. */
      BCIns *slot = &proto_bc(pt)[e->bcpos];
      *slot = e->ins;  /* Restore the latest original instruction. */
    }
    e->pt = NULL;
    e->bcpos = 0;
    e->ins = 0;
    e->nojit = 0;
    e->planted = 0;
    e->skipped = 0;
    st->count--;
    setgcrefnull(g->gcroot[GCROOT_INT3+i]);
    if (nojit) {
      int j = int3_find_proto(st, pt);
      if (j >= 0) {
	/* Pass the PROTO_NOJIT ownership to a surviving request, so the last
	** one releases the flag we set.
	*/
	st->entry[j].nojit = 1;
      } else {
#if LJ_HASJIT
	lj_trace_reenableproto(pt);  /* Unpatch lazy blacklisting. */
#endif
	pt->flags &= ~PROTO_NOJIT;
      }
    }
  }
}

/* Drop every pending request. */
static void int3_cancel_all(global_State *g)
{
  int i;
  for (i = 0; i < LJ_INT3_MAX; i++)
    int3_cancel_entry(g, i);
  if (g->hookmask & HOOK_INT3) {  /* Deferred planting is done. */
    g->hookmask &= ~HOOK_INT3;
    lj_dispatch_update(g, 0);
  }
}

/* Write BC_INT3 into a pending slot (the arm-time work is already done). */
static void int3_plant_entry(global_State *g, int i)
{
  Int3Entry *e = &int3_state.entry[i];
  UNUSED(g);
  setbc_op(&proto_bc(e->pt)[e->bcpos], BC_INT3);
  e->planted = 1;
  e->skipped = 0;
}

/* Plant the requests that a callback suspended, at this instruction boundary.
**
** Planting them from the callback would put BC_INT3 back into the slot while
** the instruction that delivered the interrupt is still being dispatched and
** executed: the hook trampolines and the metamethod/global slow paths read the
** physical opcode of that slot. While a request is suspended, HOOK_INT3 keeps
** instruction dispatch in lj_vm_inshook(), which calls this function through
** lj_dispatch_ins() before every instruction. The slot of the instruction that
** is about to run is skipped once (a slot that references itself); it is
** planted at the next boundary.
*/
void lj_int3_step(global_State *g, const BCIns *pc)
{
  Int3State *st = &int3_state;
  int i, pending = 0;
  if (st->g != g || st->count == 0) return;
  for (i = 0; i < LJ_INT3_MAX; i++) {
    Int3Entry *e = &st->entry[i];
    if (e->pt == NULL || e->planted) continue;
    if (&proto_bc(e->pt)[e->bcpos] == pc && !e->skipped) {
      e->skipped = 1;  /* The instruction about to run: plant after it. */
      pending = 1;
      continue;
    }
    int3_plant_entry(g, i);
  }
  for (i = 0; i < LJ_INT3_MAX; i++)
    if (st->entry[i].pt != NULL && !st->entry[i].planted) pending = 1;
  if (!pending && (g->hookmask & HOOK_INT3)) {
    g->hookmask &= ~HOOK_INT3;
    lj_dispatch_update(g, 0);
  }
}

/* Plant the trap for (pt, bcpos) and capture the shadowed instruction.
** The caller has validated the session, the context and the prototype.
*/
static int int3_arm_request(global_State *g, GCproto *pt, BCPos bcpos,
			    int defer)
{
  Int3State *st = &int3_state;
  BCIns *slot = &proto_bc(pt)[bcpos];
  Int3Entry *e;
  int i;
  /* Arming a slot that is already pending is a no-op. */
  if (int3_find_any(g, slot) >= 0) return LUAJIT_INT3_OK;
  for (i = 0; i < LJ_INT3_MAX; i++)
    if (st->entry[i].pt == NULL) break;
  if (i == LJ_INT3_MAX) return LUAJIT_INT3_FULL;
  e = &st->entry[i];
#if LJ_HASJIT
  {
    jit_State *J = G2J(g);
    if (J->state != LJ_TRACE_IDLE || tvref(g->jit_base) != NULL)
      return LUAJIT_INT3_BUSY;  /* Never flush while recording or on-trace. */
    /* Flush before planting: unpatching a flushed trace must not overwrite
    ** the trap. Then keep the prototype out of the JIT for the duration of
    ** the request, so no trace can inline or cover its bytecode.
    **
    ** Re-arming the request that is being delivered needs no flush: it was
    ** flushed on the way in and no Lua code ran in between, so nothing can
    ** have been recorded. This keeps a per-iteration breakpoint cheap.
    */
    if (!(int3_delivering(g) && st->last_pt == pt))
      int3_flush_traces(J, pt);
  }
#endif
  /* Root the prototype before mutating its bytecode. */
  setgcref(g->gcroot[GCROOT_INT3+i], obj2gco(pt));
  e->pt = pt;
  e->bcpos = bcpos;
  e->ins = *slot;  /* Capture the complete original instruction. */
  e->nojit = !(pt->flags & PROTO_NOJIT);
  e->planted = 0;
  e->skipped = 0;
  pt->flags |= PROTO_NOJIT;
  st->count++;
  if (defer) return LUAJIT_INT3_OK;  /* Planted at the next boundary. */
  int3_plant_entry(g, i);
  return LUAJIT_INT3_OK;
}

/* -- Logical bytecode view ----------------------------------------------- */

BCIns lj_int3_bcins(global_State *g, const BCIns *pc)
{
  int i = int3_find(g, pc);
  return i >= 0 ? int3_state.entry[i].ins : *pc;
}

int lj_int3_pending(global_State *g, const BCIns *pc)
{
  return int3_find(g, pc) >= 0;
}

void lj_int3_bcwrite_view(global_State *g, const GCproto *pt, BCIns *bc)
{
  Int3State *st = &int3_state;
  int i;
  if (st->g != g || st->count == 0) return;
  /* bc[] is a copy of proto_bc(pt)+1, so index bcpos-1 omits the header. */
  for (i = 0; i < LJ_INT3_MAX; i++) {
    Int3Entry *e = &st->entry[i];
    if (e->pt == pt && e->planted && e->bcpos != 0) bc[e->bcpos-1] = e->ins;
  }
}

int lj_int3_active(void)
{
  return int3_state.g != NULL;
}

/* Non-zero if the JIT must not be enabled for this prototype (or for a
** subtree containing it): that would clear PROTO_NOJIT and let a new trace
** inline the trapped bytecode.
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
    GCproto *armed = st->entry[i].pt;
    if (armed != NULL && (pt == armed || int3_proto_contains(pt, armed)))
      return 1;
  }
  return 0;
}

void lj_int3_close(global_State *g)
{
  Int3State *st = &int3_state;
  if (st->g != g) return;
  int3_cancel_all(g);  /* No callback, no JIT state changes on teardown. */
  st->cb = NULL;
  st->data = NULL;
  st->delivering = 0;
  if (g->hookmask & HOOK_INT3) {
    g->hookmask &= ~HOOK_INT3;
    lj_dispatch_update(g, 0);
  }
  st->last_pt = NULL;
  st->last_bcpos = 0;
  st->state = INT3_NONE;
  st->g = NULL;
}

/* -- Interpreter dispatch ------------------------------------------------ */

/* Called from the VM when a physical BC_INT3 is executed. PC points past the
** trapped slot. The original instruction is restored, the request consumed
** and the callback invoked, then the VM re-dispatches the original opcode.
** Returns that instruction: the callback may re-arm the same slot, so the
** re-dispatched opcode must not be re-read from the bytecode.
*/
LJ_FUNCA BCIns LJ_FASTCALL lj_int3_hit(lua_State *L, const BCIns *pc)
{
  Int3State *st = &int3_state;
  global_State *g = G(L);
  GCproto *pt;
  BCIns ins;
  BCPos bcpos;
  uint8_t mask;
  int vmstate, i;
  void *cf;
  i = int3_find(g, pc-1);
  if (i < 0 || st->state != INT3_ENABLED)
    lj_err_throw(L, LJ_ERR_BCFMT);  /* Stray trap: not a valid bytecode. */
  pt = st->entry[i].pt;
  bcpos = st->entry[i].bcpos;
  ins = st->entry[i].ins;
  /* Publish the delivery target for luaJIT_int3_rearm(), then restore the
  ** original instruction and consume that request. Other requests stay
  ** armed.
  */
  st->last_pt = pt;
  st->last_bcpos = bcpos;
  int3_cancel_entry(g, i);
  /* Establish a consistent read-only view of the original instruction. */
  cf = cframe_raw(L->cframe);
  setcframe_pc(cf, pc);
  if (bc_op(ins) == BC_FUNCF || bc_op(ins) == BC_FUNCV) {
    /* Function headers have not set up the frame (stack frame and vararg
    ** layout): never expose uninitialized slots to the callback.
    */
    L->top = L->base;
  } else {
    L->top = L->base + lj_dispatch_topslot(g, pt, pc, cframe_multres_n(cf));
  }
  /* Isolate the callback like a VM event: no hooks, no JIT, no reentry. */
  st->delivering = 1;
  mask = g->hookmask;
  vmstate = g->vmstate;
  g->hookmask = HOOK_VMEVENT;
  lj_dispatch_update(g, 0);
  setvmstate(g, C);  /* The callback is native code, not the interpreter. */
  st->cb(st->data, L, pt, (unsigned int)bcpos);
  g->vmstate = vmstate;
  if (int3_has_suspended(st)) mask |= HOOK_INT3;
  g->hookmask = mask;
  lj_dispatch_update(g, 0);
  st->delivering = 0;
  st->last_pt = NULL;
  st->last_bcpos = 0;
  /* When the VM handler delivered a function header and call hooks are on,
  ** the header is re-dispatched through lj_dispatch_call(). That dispatch has
  ** already run the call hook for this call, so the slot is marked and the
  ** request must not be delivered a second time there.
  */
  return ins;
}

/* -- Public interrupt API ------------------------------------------------ */

LUA_API int luaJIT_int3_start(lua_State *L, luaJIT_int3_callback cb,
			      void *data)
{
  Int3State *st = &int3_state;
  global_State *g = G(L);
  if (cb == NULL) return LUAJIT_INT3_INVALID;
  if (st->g != NULL || st->delivering) return LUAJIT_INT3_BUSY;
  if (lj_profile_active()) return LUAJIT_INT3_BUSY;  /* Profiler in use. */
  if (int3_unsafe(g)) return LUAJIT_INT3_BUSY;
  /* The JIT engine is not touched: only arming affects trace state. */
  st->g = g;
  st->cb = cb;
  st->data = data;
  st->state = INT3_ENABLED;
  st->count = 0;
  st->delivering = 0;
  st->last_pt = NULL;
  st->last_bcpos = 0;
  memset(st->entry, 0, sizeof(st->entry));
  return LUAJIT_INT3_OK;
}

LUA_API int luaJIT_int3_arm(lua_State *L, int func_index, unsigned int bcpos)
{
  Int3State *st = &int3_state;
  global_State *g = G(L);
  GCfunc *fn;
  GCproto *pt;
  if (st->g != g) return LUAJIT_INT3_DISABLED;
  if (st->state != INT3_ENABLED) return LUAJIT_INT3_DISABLED;
  if (int3_unsafe(g)) return LUAJIT_INT3_BUSY;
  if (lua_type(L, func_index) != LUA_TFUNCTION)
    return LUAJIT_INT3_INVALID;
  fn = (GCfunc *)lua_topointer(L, func_index);  /* GCfunc sits at GCobj. */
  if (fn == NULL || !isluafunc(fn)) return LUAJIT_INT3_INVALID;
  pt = funcproto(fn);
  if (bcpos >= pt->sizebc) return LUAJIT_INT3_INVALID;
  if (int3_mutable_op(bc_op(lj_int3_bcins(g, &proto_bc(pt)[bcpos]))))
    return LUAJIT_INT3_UNARMABLE;
  return int3_arm_request(g, pt, (BCPos)bcpos, 0);
}

/* Re-arm the request that is being delivered, i.e. keep trapping the same
** instruction. This is only valid inside the callback: it is what turns a
** single interrupt into a per-iteration breakpoint of a loop. It never
** disturbs the other pending requests.
*/
LUA_API int luaJIT_int3_rearm(lua_State *L)
{
  Int3State *st = &int3_state;
  global_State *g = G(L);
  if (!int3_delivering(g)) return LUAJIT_INT3_DISABLED;
  if (st->last_pt == NULL) return LUAJIT_INT3_INVALID;
  return int3_arm_request(g, st->last_pt, st->last_bcpos, 1);
}

LUA_API int luaJIT_int3_cancel(lua_State *L)
{
  Int3State *st = &int3_state;
  global_State *g = G(L);
  if (st->g == NULL) return LUAJIT_INT3_OK;  /* Nothing to cancel. */
  if (st->g != g) return LUAJIT_INT3_DISABLED;
  if (int3_unsafe(g)) return LUAJIT_INT3_BUSY;
  int3_cancel_all(g);  /* The prototypes become compilable again. */
  return LUAJIT_INT3_OK;
}

LUA_API int luaJIT_int3_stop(lua_State *L)
{
  Int3State *st = &int3_state;
  global_State *g = G(L);
  if (st->g == NULL) return LUAJIT_INT3_OK;
  if (st->g != g) return LUAJIT_INT3_DISABLED;
  if (int3_unsafe(g)) return LUAJIT_INT3_BUSY;
  int3_cancel_all(g);
  st->cb = NULL;
  st->data = NULL;
  st->state = INT3_NONE;
  st->g = NULL;
  return LUAJIT_INT3_OK;
}

#else  /* !LJ_HASPROFILE */

/* Profiling support is compiled out: no session can ever be armed, so the
** bytecode view is the real bytecode and the dispatch target is unreachable.
*/

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

int lj_int3_stepping(void)
{
  return 0;
}

void lj_int3_step(global_State *g, const BCIns *pc)
{
  UNUSED(g); UNUSED(pc);
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

LUA_API int luaJIT_int3_rearm(lua_State *L)
{
  UNUSED(L);
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

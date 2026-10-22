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
**/

#if LJ_HASPROFILE

#define INT3_NONE	0	/* No session. */
#define INT3_ENABLED	1	/* Session accepts requests. */

typedef struct Int3State {
  global_State *g;		/* VM that owns the session. */
  luaJIT_int3_callback cb;	/* Native read-only callback. */
  void *data;			/* Callback data. */
  int state;			/* INT3_NONE/INT3_ENABLED. */
  int nojit;			/* We set PROTO_NOJIT on the pending proto. */
  int delivering;		/* Callback delivery in progress. */
  GCproto *pt;			/* Prototype of the pending request. */
  BCPos bcpos;			/* Bytecode index of the pending slot. */
  BCIns ins;			/* Latest original instruction of that slot. */
} Int3State;

static Int3State int3_state;

#define INT3_SLOT(st)	(&proto_bc((st)->pt)[(st)->bcpos])

/* Match a bytecode slot against the pending request of a VM. */
static LJ_AINLINE int int3_pending(Int3State *st, global_State *g,
				   const BCIns *pc)
{
  return st->pt != NULL && st->g == g &&
	 pc == &proto_bc(st->pt)[st->bcpos];
}

/* Reject unsafe contexts: GC, any hook, VM event or profiler callback. */
static int int3_unsafe(global_State *g)
{
  if ((g->hookmask & (HOOK_ACTIVE|HOOK_GC|HOOK_VMEVENT|HOOK_PROFILE)))
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

/* Drop the pending request: restore the bytecode and undo the JIT exclusion.
**
** Marking the prototype PROTO_NOJIT makes the recorder lazily blacklist its
** hot spots (IFUNCF/ILOOP patches). That blacklisting must not outlive the
** request, so re-enable compilation the same way jit.on(func) does.
*/
static void int3_cancel_pending(global_State *g)
{
  Int3State *st = &int3_state;
  if (st->pt != NULL) {
    GCproto *pt = st->pt;
    int nojit = st->nojit;
    *INT3_SLOT(st) = st->ins;  /* Restore the latest original instruction. */
    st->pt = NULL;
    st->bcpos = 0;
    st->ins = 0;
    st->nojit = 0;
    setgcrefnull(g->gcroot[GCROOT_INT3]);
    if (nojit) {
#if LJ_HASJIT
      lj_trace_reenableproto(pt);  /* Unpatch lazy blacklisting. */
#endif
      pt->flags &= ~PROTO_NOJIT;
    }
  }
}

/* -- Logical bytecode view ----------------------------------------------- */

BCIns lj_int3_bcins(global_State *g, const BCIns *pc)
{
  Int3State *st = &int3_state;
  if (int3_pending(st, g, pc)) return st->ins;
  return *pc;
}

void lj_int3_setbc(global_State *g, const BCIns *pc, BCIns ins)
{
  Int3State *st = &int3_state;
  if (int3_pending(st, g, pc))
    st->ins = ins;  /* Keep the trap, update the shadowed instruction. */
  else
    *(BCIns *)(void *)pc = ins;
}

void lj_int3_setbc_op(global_State *g, const BCIns *pc, BCOp op)
{
  /* Read-modify-write through the logical view, so a pending trap keeps its
  ** physical opcode and only the shadowed instruction changes.
  */
  BCIns ins = lj_int3_bcins(g, pc);
  setbc_op(&ins, op);
  lj_int3_setbc(g, pc, ins);
}

BCPos lj_int3_bcpos(global_State *g, const GCproto *pt)
{
  Int3State *st = &int3_state;
  if (st->pt != NULL && st->g == g && st->pt == pt) return st->bcpos;
  return LJ_INT3_NOBCPOS;
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
  if (st->pt == NULL || st->g != g) return 0;
  return pt == st->pt || int3_proto_contains(pt, st->pt);
}

void lj_int3_close(global_State *g)
{
  Int3State *st = &int3_state;
  if (st->g != g) return;
  int3_cancel_pending(g);  /* No callback, no JIT state changes on teardown. */
  st->cb = NULL;
  st->data = NULL;
  st->delivering = 0;
  st->state = INT3_NONE;
  st->g = NULL;
}

/* -- Interpreter dispatch ------------------------------------------------ */

/* Called from the VM when a physical BC_INT3 is executed. PC points past the
** trapped slot. The original instruction is restored, the request consumed
** and the callback invoked, then the VM re-dispatches the original opcode.
*/
LJ_FUNCA void LJ_FASTCALL lj_int3_hit(lua_State *L, const BCIns *pc)
{
  Int3State *st = &int3_state;
  global_State *g = G(L);
  GCproto *pt;
  BCIns ins;
  BCPos bcpos;
  uint8_t mask;
  int vmstate;
  void *cf;
  if (!int3_pending(st, g, pc-1) || st->state != INT3_ENABLED)
    lj_err_throw(L, LJ_ERR_BCFMT);  /* Stray trap: not a valid bytecode. */
  pt = st->pt;
  bcpos = st->bcpos;
  ins = st->ins;
  /* Restore the original instruction and consume the request first. */
  int3_cancel_pending(g);
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
  setvmstate(g, C);
  st->cb(st->data, L, pt, (unsigned int)bcpos);
  g->vmstate = vmstate;
  g->hookmask = mask;
  lj_dispatch_update(g, 0);
  st->delivering = 0;
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
  st->delivering = 0;
  st->pt = NULL;
  st->bcpos = 0;
  st->ins = 0;
  st->nojit = 0;
  return LUAJIT_INT3_OK;
}

LUA_API int luaJIT_int3_arm(lua_State *L, int func_index, unsigned int bcpos)
{
  Int3State *st = &int3_state;
  global_State *g = G(L);
  GCfunc *fn;
  GCproto *pt;
  BCIns *slot;
  if (st->g != g) return LUAJIT_INT3_DISABLED;
  if (st->state != INT3_ENABLED) return LUAJIT_INT3_DISABLED;
  if (st->delivering || st->pt != NULL || int3_unsafe(g))
    return LUAJIT_INT3_BUSY;
  if (lua_type(L, func_index) != LUA_TFUNCTION)
    return LUAJIT_INT3_INVALID;
  fn = (GCfunc *)lua_topointer(L, func_index);  /* GCfunc sits at GCobj. */
  if (fn == NULL || !isluafunc(fn)) return LUAJIT_INT3_INVALID;
  pt = funcproto(fn);
  if (bcpos >= pt->sizebc) return LUAJIT_INT3_INVALID;
#if LJ_HASJIT
  {
    jit_State *J = G2J(g);
    if (J->state != LJ_TRACE_IDLE || tvref(g->jit_base) != NULL)
      return LUAJIT_INT3_BUSY;  /* Never flush while recording or on-trace. */
    /* Flush before planting: unpatching a flushed trace must not overwrite
    ** the trap. Then keep the prototype out of the JIT for the duration of
    ** the request, so no trace can inline or cover its bytecode.
    */
    int3_flush_traces(J, pt);
    st->nojit = !(pt->flags & PROTO_NOJIT);
    pt->flags |= PROTO_NOJIT;
  }
#endif
  slot = proto_bc(pt) + bcpos;
  /* Root the prototype before mutating its bytecode. */
  setgcref(g->gcroot[GCROOT_INT3], obj2gco(pt));
  st->pt = pt;
  st->bcpos = (BCPos)bcpos;
  st->ins = *slot;  /* Capture the complete original instruction. */
  setbc_op(slot, BC_INT3);
  return LUAJIT_INT3_OK;
}

LUA_API int luaJIT_int3_cancel(lua_State *L)
{
  Int3State *st = &int3_state;
  global_State *g = G(L);
  if (st->g == NULL) return LUAJIT_INT3_OK;  /* Nothing to cancel. */
  if (st->g != g) return LUAJIT_INT3_DISABLED;
  if (st->delivering) return LUAJIT_INT3_BUSY;
  if (int3_unsafe(g)) return LUAJIT_INT3_BUSY;
  int3_cancel_pending(g);  /* The prototype becomes compilable again. */
  return LUAJIT_INT3_OK;
}

LUA_API int luaJIT_int3_stop(lua_State *L)
{
  Int3State *st = &int3_state;
  global_State *g = G(L);
  if (st->g == NULL) return LUAJIT_INT3_OK;
  if (st->g != g) return LUAJIT_INT3_DISABLED;
  if (st->delivering) return LUAJIT_INT3_BUSY;
  if (int3_unsafe(g)) return LUAJIT_INT3_BUSY;
  int3_cancel_pending(g);
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

void lj_int3_setbc(global_State *g, const BCIns *pc, BCIns ins)
{
  UNUSED(g);
  *(BCIns *)(void *)pc = ins;
}

void lj_int3_setbc_op(global_State *g, const BCIns *pc, BCOp op)
{
  UNUSED(g);
  setbc_op((BCIns *)(void *)pc, op);
}

BCPos lj_int3_bcpos(global_State *g, const GCproto *pt)
{
  UNUSED(g); UNUSED(pt);
  return LJ_INT3_NOBCPOS;
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

void lj_int3_close(global_State *g)
{
  UNUSED(g);
}

LJ_FUNCA void LJ_FASTCALL lj_int3_hit(lua_State *L, const BCIns *pc)
{
  UNUSED(pc);
  lj_err_throw(L, LJ_ERR_BCFMT);
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

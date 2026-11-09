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
** leaves the JIT engine running: arming a breakpoint flushes exactly those
** traces that execute or inline the armed prototype and excludes that
** prototype from JIT compilation, so every later execution of its bytecode is
** an interpreter dispatch. No bytecode other than the armed slots is touched
** and no other trace is dropped.
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
** Every armed prototype is a GC root (GCROOT_INT3+i), so a prototype that
** carries a trap can never be collected: the root is taken before the opcode
** is replaced and dropped only after it was restored, and lj_int3_close()
** does the same before the finalizer run and lj_gc_freeall() of a VM
** teardown. That is why no prototype-free hook is needed: apart from the
** armed breakpoints (rooted) and the delivery in progress (the executing
** function's prototype), this module holds no prototype pointer.
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

typedef struct Int3Entry {
  GCproto *pt;			/* Prototype of the breakpoint. */
  BCPos bcpos;			/* Bytecode index of the trapped slot. */
  BCIns ins;			/* Shadowed original instruction of that slot. */
  int nojit;			/* We set PROTO_NOJIT for this request. */
} Int3Entry;

struct Int3State {
  luaJIT_int3_callback cb;	/* Native read-only callback. */
  void *data;			/* Callback data. */
  int state;			/* INT3_NONE/INT3_ENABLED. */
  int count;			/* Number of armed breakpoints. */
  int nmax;			/* One past the highest used slot index. */
  int delivering;		/* Callback delivery in progress. */
  Int3Entry entry[LJ_INT3_MAX];	/* Armed breakpoints. */
  BCIns *slot[LJ_INT3_MAX];	/* Their slots, slot[i] != NULL <=> entry[i]. */
};

/* Number of VMs that own a session, i.e. 0 or 1 by policy. Only the profiler
** exclusion reads it: everything else goes through g->int3.
*/
static int int3_sessions;

/* Index of the breakpoint armed on this slot, or -1.
**
** The slot addresses are kept in a parallel array: the scan then only
** compares pointers, without chasing the prototype of every entry. This runs
** for every dispatched instruction while a breakpoint session is armed and
** instruction dispatch is instrumented, so it is the one hot loop here.
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

/* Index of a breakpoint armed on this prototype, or -1. */
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
  Int3State *st = g->int3;
  return st != NULL && st->delivering != 0;
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

/* Non-zero for the jump slot that the preceding comparison or unary test
** consumes. The parser emits a JMP right after every instruction of the
** BC_ISLT..BC_ISF range (comparisons, ORDER OPR, then the unary tests): the
** comparison reads its branch target from the D field of that slot and either
** branches to it or skips the slot entirely (see the `add PC, 4` in
** build_ins()), so the JMP is never dispatched on its own and a trap there
** could never fire. Such a slot is not a breakpoint site; the dispatched
** jumps (loop back edges, UCLO, ISNEXT) are unaffected.
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
** debug hook, every other operation treats all of them as fatal.
*/
#define INT3_CTX_HOOK		0x01	/* Inside a Lua debug hook. */
#define INT3_CTX_HARD		0x02	/* GC hook, VM event or profiler. */
#define INT3_CTX_VMSTATE	0x04	/* Machine code, recording or compiling. */

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

/* Disarm one breakpoint: restore the bytecode and undo the JIT exclusion.
**
** Marking the prototype PROTO_NOJIT makes the recorder lazily blacklist its
** hot spots (IFUNCF/ILOOP patches). That blacklisting must not outlive the
** breakpoints for that prototype, so re-enable compilation the same way
** jit.on(func) does once the last breakpoint for it is gone.
*/
static void int3_cancel_entry(global_State *g, int i)
{
  Int3State *st = g->int3;
  Int3Entry *e = &st->entry[i];
  GCproto *pt = e->pt;
  if (pt != NULL) {
    int nojit = e->nojit;
    *st->slot[i] = e->ins;  /* Restore the whole instruction. */
    st->slot[i] = NULL;
    while (st->nmax > 0 && st->slot[st->nmax-1] == NULL) st->nmax--;
    e->pt = NULL;
    e->bcpos = 0;
    e->ins = 0;
    e->nojit = 0;
    st->count--;
    setgcrefnull(g->gcroot[GCROOT_INT3+i]);
    if (nojit) {
      int j = int3_find_proto(st, pt);
      if (j >= 0) {
	/* Pass the PROTO_NOJIT ownership to a surviving breakpoint, so the
	** last one releases the flag we set.
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

/* Disarm every breakpoint. */
static void int3_cancel_all(global_State *g)
{
  int i;
  for (i = 0; i < LJ_INT3_MAX; i++)
    int3_cancel_entry(g, i);
}

/* Arm the breakpoint for (pt, bcpos) and capture the shadowed instruction.
** The caller has validated the session, the context and the prototype.
*/
static int int3_arm_request(global_State *g, GCproto *pt, BCPos bcpos)
{
  Int3State *st = g->int3;
  BCIns *slot = &proto_bc(pt)[bcpos];
  Int3Entry *e;
  int i;
  /* Arming a slot that is already armed is a no-op. */
  if (int3_find(g, slot) >= 0) return LUAJIT_INT3_OK;
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
    ** the trap. Then keep the prototype out of the JIT for as long as it
    ** carries a breakpoint, so no trace can inline or cover its bytecode.
    **
    ** A prototype that already carries one is done: its traces were flushed
    ** when its first breakpoint was armed and PROTO_NOJIT keeps every later
    ** trace off that bytecode, so nothing can have been recorded since.
    */
    if (int3_find_proto(st, pt) < 0)
      int3_flush_traces(J, pt);
  }
#endif
  /* Root the prototype before mutating its bytecode. */
  setgcref(g->gcroot[GCROOT_INT3+i], obj2gco(pt));
  e->pt = pt;
  e->bcpos = bcpos;
  e->ins = *slot;  /* Capture the complete original instruction. */
  st->slot[i] = slot;
  if (i + 1 > st->nmax) st->nmax = i + 1;
  e->nojit = !(pt->flags & PROTO_NOJIT);
  pt->flags |= PROTO_NOJIT;
  st->count++;
  setbc_op(slot, BC_INT3);  /* Keep A/B/C/D: they stay readable. */
  return LUAJIT_INT3_OK;
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
    if (e->pt == pt && e->bcpos != 0) bc[e->bcpos-1] = e->ins;
  }
}

int lj_int3_active(void)
{
  return int3_sessions != 0;
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
  Int3State *st = g->int3;
  int i;
  if (st == NULL || st->count == 0) return 0;
  for (i = 0; i < LJ_INT3_MAX; i++) {
    GCproto *armed = st->entry[i].pt;
    if (armed != NULL && (pt == armed || int3_proto_contains(pt, armed)))
      return 1;
  }
  return 0;
}

void lj_int3_close(global_State *g)
{
  Int3State *st = g->int3;
  if (st == NULL) return;
  int3_cancel_all(g);  /* No callback, no JIT state changes on teardown. */
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
  if (cb == NULL) return LUAJIT_INT3_INVALID;
  if (g->int3 != NULL || int3_sessions != 0) return LUAJIT_INT3_BUSY;
  if (lj_profile_active()) return LUAJIT_INT3_BUSY;  /* Profiler in use. */
  if (int3_unsafe(g)) return LUAJIT_INT3_BUSY;
  /* The JIT engine is not touched: only arming affects trace state. All
  ** checks are done, so a refused start leaves no state behind.
  */
  st = lj_mem_newt(L, sizeof(Int3State), Int3State);
  st->cb = cb;
  st->data = data;
  st->state = INT3_ENABLED;
  st->count = 0;
  st->nmax = 0;
  st->delivering = 0;
  memset(st->entry, 0, sizeof(st->entry));
  memset(st->slot, 0, sizeof(st->slot));
  g->int3 = st;
  int3_sessions++;
  return LUAJIT_INT3_OK;
}

/* Arm the breakpoint at bcpos of the Lua function at func_index.
**
** The trap is planted immediately, also from a Lua debug hook: the hook runs
** inside the instruction dispatch, but the continuation that re-dispatches
** the instruction it interrupted takes the shadowed instruction from this
** module instead of re-reading the slot (lj_int3_ins), so the instruction the
** hook belongs to is executed exactly once and the breakpoint fires from the
** next real dispatch on.
**
** Every other operation (start, cancel, stop) is still refused inside a hook,
** and the GC, VM event, profiler, recorder and machine-code contexts are
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
  ctx = int3_context(g);
  /* A GC hook is both HOOK_ACTIVE and HOOK_GC: test the hard bits first. */
  if (ctx & INT3_CTX_HARD) return LUAJIT_INT3_BUSY;
  /* A Lua hook is only ever entered from interpreter dispatch, never from
  ** machine code, so there the VM state is the recorder's transient
  ** bookkeeping and no trace is running. Outside a hook it does mean that
  ** machine code is executing, that the recorder is active or that a trace
  ** is being compiled.
  */
  if (!(ctx & INT3_CTX_HOOK) && (ctx & INT3_CTX_VMSTATE))
    return LUAJIT_INT3_BUSY;
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
  return int3_arm_request(g, pt, (BCPos)bcpos);
}

LUA_API int luaJIT_int3_cancel(lua_State *L)
{
  global_State *g = G(L);
  if (g->int3 == NULL) return LUAJIT_INT3_OK;  /* Nothing to cancel. */
  if (int3_unsafe(g)) return LUAJIT_INT3_BUSY;
  int3_cancel_all(g);  /* The prototypes become compilable again. */
  return LUAJIT_INT3_OK;
}

LUA_API int luaJIT_int3_stop(lua_State *L)
{
  global_State *g = G(L);
  Int3State *st = g->int3;
  if (st == NULL) return LUAJIT_INT3_OK;  /* No session in this VM. */
  if (int3_unsafe(g)) return LUAJIT_INT3_BUSY;
  int3_cancel_all(g);
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

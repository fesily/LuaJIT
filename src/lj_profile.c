/*
** Low-overhead profiling.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_profile_c
#define LUA_CORE

#include "lj_obj.h"

#include "lj_err.h"
#include "lj_frame.h"
#include "lj_func.h"
#include "lj_dispatch.h"
#include "lj_profile.h"

#include "luajit.h"

#if LJ_HASPROFILE

#include "lj_buf.h"
#include "lj_debug.h"
#if LJ_HASJIT
#include "lj_jit.h"
#include "lj_trace.h"
#endif

#if LJ_PROFILE_SIGPROF

#include <sys/time.h>
#include <signal.h>
#define profile_lock(ps)	UNUSED(ps)
#define profile_unlock(ps)	UNUSED(ps)

#elif LJ_PROFILE_PTHREAD

#include <pthread.h>
#include <time.h>
#if LJ_TARGET_PS3
#include <sys/timer.h>
#endif
#define profile_lock(ps)	pthread_mutex_lock(&ps->lock)
#define profile_unlock(ps)	pthread_mutex_unlock(&ps->lock)

#elif LJ_PROFILE_WTHREAD

#define WIN32_LEAN_AND_MEAN
#if LJ_TARGET_XBOX360
#include <xtl.h>
#include <xbox.h>
#else
#include <windows.h>
#endif
typedef unsigned int (WINAPI *WMM_TPFUNC)(unsigned int);
#define profile_lock(ps)	EnterCriticalSection(&ps->lock)
#define profile_unlock(ps)	LeaveCriticalSection(&ps->lock)

#endif

/* Profiler state. */
typedef struct ProfileState {
  global_State *g;		/* VM state that started the profiler. */
  luaJIT_profile_callback cb;	/* Profiler callback. */
  void *data;			/* Profiler callback data. */
  SBuf sb;			/* String buffer for stack dumps. */
  int interval;			/* Sample interval in milliseconds. */
  int samples;			/* Number of samples for next callback. */
  int vmstate;			/* VM state when profile timer triggered. */
#if LJ_PROFILE_SIGPROF
  struct sigaction oldsa;	/* Previous SIGPROF state. */
#elif LJ_PROFILE_PTHREAD
  pthread_mutex_t lock;		/* g->hookmask update lock. */
  pthread_t thread;		/* Timer thread. */
  int abort;			/* Abort timer thread. */
#elif LJ_PROFILE_WTHREAD
#if LJ_TARGET_WINDOWS
  HINSTANCE wmm;		/* WinMM library handle. */
  WMM_TPFUNC wmm_tbp;		/* WinMM timeBeginPeriod function. */
  WMM_TPFUNC wmm_tep;		/* WinMM timeEndPeriod function. */
#endif
  CRITICAL_SECTION lock;	/* g->hookmask update lock. */
  HANDLE thread;		/* Timer thread. */
  int abort;			/* Abort timer thread. */
#endif
} ProfileState;

/* Sadly, we have to use a static profiler state.
**
** The SIGPROF variant needs a static pointer to the global state, anyway.
** And it would be hard to extend for multiple threads. You can still use
** multiple VMs in multiple threads, but only profile one at a time.
*/
static ProfileState profile_state;

/* Default sample interval in milliseconds. */
#define LJ_PROFILE_INTERVAL_DEFAULT	10

/* -- Profiler/hook interaction ------------------------------------------- */

#if !LJ_PROFILE_SIGPROF
void LJ_FASTCALL lj_profile_hook_enter(global_State *g)
{
  ProfileState *ps = &profile_state;
  if (ps->g) {
    profile_lock(ps);
    hook_enter(g);
    profile_unlock(ps);
  } else {
    hook_enter(g);
  }
}

void LJ_FASTCALL lj_profile_hook_leave(global_State *g)
{
  ProfileState *ps = &profile_state;
  if (ps->g) {
    profile_lock(ps);
    hook_leave(g);
    profile_unlock(ps);
  } else {
    hook_leave(g);
  }
}

int lj_profile_lock(void)
{
  ProfileState *ps = &profile_state;
  if (ps->g) {
    profile_lock(ps);
    return 1;
  } else {
    return 0;
  }
}

void lj_profile_unlock(void)
{
  ProfileState *ps = &profile_state;
  profile_unlock(ps);
}
#endif

/* -- Profile callbacks --------------------------------------------------- */

/* Callback from profile hook (HOOK_PROFILE already cleared). */
void LJ_FASTCALL lj_profile_interpreter(lua_State *L)
{
  ProfileState *ps = &profile_state;
  global_State *g = G(L);
  uint8_t mask;
  profile_lock(ps);
  mask = (g->hookmask & ~HOOK_PROFILE);
  if (!(mask & HOOK_VMEVENT)) {
    int samples = ps->samples;
    ps->samples = 0;
    g->hookmask = HOOK_VMEVENT;
    lj_dispatch_update(g, 1);
    profile_unlock(ps);
    ps->cb(ps->data, L, samples, ps->vmstate);  /* Invoke user callback. */
    profile_lock(ps);
    mask |= (g->hookmask & HOOK_PROFILE);
  }
  g->hookmask = mask;
  lj_dispatch_update(g, 1);
  profile_unlock(ps);
}

/* Trigger profile hook. Asynchronous call from OS-specific profile timer. */
static void profile_trigger(ProfileState *ps)
{
  global_State *g = ps->g;
  uint8_t mask;
  profile_lock(ps);
  ps->samples++;  /* Always increment number of samples. */
  mask = g->hookmask;
  if (!(mask & (HOOK_PROFILE|HOOK_VMEVENT|HOOK_GC))) {  /* Set profile hook. */
    int st = g->vmstate;
    ps->vmstate = st >= 0 ? 'N' :
		  st == ~LJ_VMST_INTERP ? 'I' :
		  st == ~LJ_VMST_C ? 'C' :
		  st == ~LJ_VMST_GC ? 'G' : 'J';
    g->hookmask = (mask | HOOK_PROFILE);
    lj_dispatch_update(g, 1);
  }
  profile_unlock(ps);
}

/* -- OS-specific profile timer handling ---------------------------------- */

#if LJ_PROFILE_SIGPROF

/* SIGPROF handler. */
static void profile_signal(int sig)
{
  UNUSED(sig);
  profile_trigger(&profile_state);
}

/* Start profiling timer. */
static void profile_timer_start(ProfileState *ps)
{
  int interval = ps->interval;
  struct itimerval tm;
  struct sigaction sa;
  tm.it_value.tv_sec = tm.it_interval.tv_sec = interval / 1000;
  tm.it_value.tv_usec = tm.it_interval.tv_usec = (interval % 1000) * 1000;
  setitimer(ITIMER_PROF, &tm, NULL);
#if LJ_TARGET_QNX
  sa.sa_flags = 0;
#else
  sa.sa_flags = SA_RESTART;
#endif
  sa.sa_handler = profile_signal;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGPROF, &sa, &ps->oldsa);
}

/* Stop profiling timer. */
static void profile_timer_stop(ProfileState *ps)
{
  struct itimerval tm;
  tm.it_value.tv_sec = tm.it_interval.tv_sec = 0;
  tm.it_value.tv_usec = tm.it_interval.tv_usec = 0;
  setitimer(ITIMER_PROF, &tm, NULL);
  sigaction(SIGPROF, &ps->oldsa, NULL);
}

#elif LJ_PROFILE_PTHREAD

/* POSIX timer thread. */
static void *profile_thread(ProfileState *ps)
{
  int interval = ps->interval;
#if !LJ_TARGET_PS3
  struct timespec ts;
  ts.tv_sec = interval / 1000;
  ts.tv_nsec = (interval % 1000) * 1000000;
#endif
  while (1) {
#if LJ_TARGET_PS3
    sys_timer_usleep(interval * 1000);
#else
    nanosleep(&ts, NULL);
#endif
    if (ps->abort) break;
    profile_trigger(ps);
  }
  return NULL;
}

/* Start profiling timer thread. */
static void profile_timer_start(ProfileState *ps)
{
  pthread_mutex_init(&ps->lock, 0);
  ps->abort = 0;
  pthread_create(&ps->thread, NULL, (void *(*)(void *))profile_thread, ps);
}

/* Stop profiling timer thread. */
static void profile_timer_stop(ProfileState *ps)
{
  ps->abort = 1;
  pthread_join(ps->thread, NULL);
  pthread_mutex_destroy(&ps->lock);
}

#elif LJ_PROFILE_WTHREAD

/* Windows timer thread. */
static DWORD WINAPI profile_thread(void *psx)
{
  ProfileState *ps = (ProfileState *)psx;
  int interval = ps->interval;
#if LJ_TARGET_WINDOWS && !LJ_TARGET_UWP
  ps->wmm_tbp(interval);
#endif
  while (1) {
    Sleep(interval);
    if (ps->abort) break;
    profile_trigger(ps);
  }
#if LJ_TARGET_WINDOWS && !LJ_TARGET_UWP
  ps->wmm_tep(interval);
#endif
  return 0;
}

/* Start profiling timer thread. */
static void profile_timer_start(ProfileState *ps)
{
#if LJ_TARGET_WINDOWS && !LJ_TARGET_UWP
  if (!ps->wmm) {  /* Load WinMM library on-demand. */
    ps->wmm = LJ_WIN_LOADLIBA("winmm.dll");
    if (ps->wmm) {
      ps->wmm_tbp = (WMM_TPFUNC)GetProcAddress(ps->wmm, "timeBeginPeriod");
      ps->wmm_tep = (WMM_TPFUNC)GetProcAddress(ps->wmm, "timeEndPeriod");
      if (!ps->wmm_tbp || !ps->wmm_tep) {
	ps->wmm = NULL;
	return;
      }
    }
  }
#endif
  InitializeCriticalSection(&ps->lock);
  ps->abort = 0;
  ps->thread = CreateThread(NULL, 0, profile_thread, ps, 0, NULL);
}

/* Stop profiling timer thread. */
static void profile_timer_stop(ProfileState *ps)
{
  ps->abort = 1;
  WaitForSingleObject(ps->thread, INFINITE);
  DeleteCriticalSection(&ps->lock);
}

#endif

/* -- Public profiling API ------------------------------------------------ */

/* Start profiling. */
LUA_API void luaJIT_profile_start(lua_State *L, const char *mode,
				  luaJIT_profile_callback cb, void *data)
{
  ProfileState *ps = &profile_state;
  int interval = LJ_PROFILE_INTERVAL_DEFAULT;
  /* Never disturb an interrupt session, in this or in any other VM. */
  if (lj_int3_active()) return;
  while (*mode) {
    int m = *mode++;
    switch (m) {
    case 'i':
      interval = 0;
      while (*mode >= '0' && *mode <= '9')
	interval = interval * 10 + (*mode++ - '0');
      if (interval <= 0) interval = 1;
      break;
#if LJ_HASJIT
    case 'l': case 'f':
      L2J(L)->prof_mode = m;
      lj_trace_flushall(L);
      break;
#endif
    default:  /* Ignore unknown mode chars. */
      break;
    }
  }
  if (ps->g) {
    luaJIT_profile_stop(L);
    if (ps->g) return;  /* Profiler in use by another VM. */
  }
  ps->g = G(L);
  ps->interval = interval;
  ps->cb = cb;
  ps->data = data;
  ps->samples = 0;
  lj_buf_init(L, &ps->sb);
  profile_timer_start(ps);
}

/* Stop profiling. */
LUA_API void luaJIT_profile_stop(lua_State *L)
{
  ProfileState *ps = &profile_state;
  global_State *g = ps->g;
  if (G(L) == g) {  /* Only stop profiler if started by this VM. */
    ps->g = NULL;
    profile_timer_stop(ps);
    g->hookmask &= ~HOOK_PROFILE;
    lj_dispatch_update(g, 0);
#if LJ_HASJIT
    G2J(g)->prof_mode = 0;
    lj_trace_flushall(L);
#endif
    lj_buf_free(g, &ps->sb);
    ps->sb.w = ps->sb.e = NULL;
  }
}

/* Return a compact stack dump. */
LUA_API const char *luaJIT_profile_dumpstack(lua_State *L, const char *fmt,
					     int depth, size_t *len)
{
  ProfileState *ps = &profile_state;
  SBuf *sb = &ps->sb;
  setsbufL(sb, L);
  lj_buf_reset(sb);
  lj_debug_dumpstack(L, sb, fmt, depth);
  *len = (size_t)sbuflen(sb);
  return sb->b;
}

#endif

/* -- One-shot interpreter interrupt (BC_INT3) ---------------------------- */

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
  Int3State *st = &int3_state;
  if (int3_pending(st, g, pc))
    setbc_op(&st->ins, op);  /* Keep the trap, update the shadowed opcode. */
  else
    setbc_op((BCIns *)(void *)pc, op);
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
  if (profile_state.g != NULL) return LUAJIT_INT3_BUSY;  /* Profiler in use. */
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

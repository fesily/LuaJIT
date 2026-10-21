/*
** Low-overhead profiling.
** Copyright (C) 2005-2017 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_profile_c
#define LUA_CORE

#include "lj_obj.h"

#if LJ_HASPROFILE

#include "lj_buf.h"
#include "lj_frame.h"
#include "lj_debug.h"
#include "lj_dispatch.h"
#if LJ_HASJIT
#include "lj_jit.h"
#include "lj_trace.h"
#endif
#include "lj_profile.h"
#include "lj_profile_timer.h"
#include "lj_mixprof.h"
#include "lj_symtab.h"

#include "luajit.h"

/* Profiler state. */
typedef struct ProfileState {
  global_State *g;		/* VM state that started the profiler. */
  luaJIT_profile_callback cb;	/* Profiler callback. */
  void *data;			/* Profiler callback data. */
  SBuf sb;			/* String buffer for stack dumps. */
  int samples;			/* Number of samples for next callback. */
  int vmstate;			/* VM state when profile timer triggered. */
#if LJ_HASMIXPROF
  int host_enabled;		/* 'h' mode: capture host callchains. */
#endif
  lj_profile_timer timer;	/* Profiling timer */
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
#endif

/* Lock the profiler state for lj_dispatch_update() (see lj_dispatch.c). */
int lj_profile_lock(void)
{
  ProfileState *ps = &profile_state;
  if (ps->g) {
    profile_lock(ps);
    return 1;  /* The lock was taken, the caller must release it. */
  }
  return 0;
}

void lj_profile_unlock(void)
{
  ProfileState *ps = &profile_state;
  profile_unlock(ps);
}

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
#if LJ_HASMIXPROF
    if (ps->host_enabled)
      lj_mixprof_capture(L);
#endif
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
		  /* Stubs for profiler hooks. */
		  st == ~LJ_VMST_LFUNC ? 'I' :
		  st == ~LJ_VMST_FFUNC ? 'I' :
		  st == ~LJ_VMST_CFUNC ? 'C' :
		  st == ~LJ_VMST_GC ? 'G' : 'J';
    g->hookmask = (mask | HOOK_PROFILE);
    lj_dispatch_update(g, 1);
  }
  profile_unlock(ps);
}

#if LJ_PROFILE_SIGPROF

static void profile_handler(int sig, siginfo_t *info, void *ctx)
{
  UNUSED(sig);
  UNUSED(info);
  UNUSED(ctx);
  profile_trigger(&profile_state);
}

#else

static void profile_handler()
{
  profile_trigger(&profile_state);
}

#endif

/* -- Public profiling API ------------------------------------------------ */

/* Start profiling. */
LUA_API void luaJIT_profile_start(lua_State *L, const char *mode,
				  luaJIT_profile_callback cb, void *data)
{
  ProfileState *ps = &profile_state;
  int interval = LJ_PROFILE_INTERVAL_DEFAULT;
#if LJ_HASMIXPROF
  int host_requested = 0;
#endif
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
#if LJ_HASMIXPROF
    case 'h':  /* Capture host (native) callchains. */
      host_requested = 1;
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
  ps->cb = cb;
  ps->data = data;
  ps->samples = 0;
#if LJ_HASMIXPROF
  ps->host_enabled = host_requested;
  lj_mixprof_reset_capture();
#endif
  lj_buf_init(L, &ps->sb);
  ps->timer.opt.interval_msec = interval;
  ps->timer.opt.handler = profile_handler;
  lj_profile_timer_start(&ps->timer);
}

/* Stop profiling. */
LUA_API void luaJIT_profile_stop(lua_State *L)
{
  ProfileState *ps = &profile_state;
  global_State *g = ps->g;
  if (G(L) == g) {  /* Only stop profiler if started by this VM. */
    lj_profile_timer_stop(&ps->timer);
#if LJ_HASMIXPROF
    ps->host_enabled = 0;
    lj_mixprof_reset_capture();
#endif
    g->hookmask &= ~HOOK_PROFILE;
    lj_dispatch_update(g, 1);
#if LJ_HASJIT
    G2J(g)->prof_mode = 0;
    lj_trace_flushall(L);
#endif
    lj_buf_free(g, &ps->sb);
    ps->sb.b = NULL;
    ps->sb.e = NULL;
    ps->g = NULL;
  }
}

/* Adapter: the public callback takes a const void * address. */
typedef struct {
  luaJIT_symtab_callback cb;
  void *data;
} SymtabCb;

static void profile_symtab_cb(void *ctx, uintptr_t addr, const char *name,
			      uint32_t size)
{
  SymtabCb *c = (SymtabCb *)ctx;
  UNUSED(size);
  c->cb(c->data, (const void *)addr, name);
}

/* Walk the C function symbols of the loaded modules. */
LUA_API void luaJIT_profile_symtab(luaJIT_symtab_callback cb, void *data)
{
  SymtabCb c;
  if (cb == NULL)
    return;
  c.cb = cb;
  c.data = data;
  lj_symtab_cfuncs(profile_symtab_cb, &c);
}

/* Copy the host (native) callchain of the most recent sample. */
LUA_API int luaJIT_profile_hoststack(lua_State *L, const void **buf,
				     int maxdepth)
{
  return lj_mixprof_hoststack(L, buf, maxdepth);
}

/* Return the merged Lua + host callchain of the most recent sample. */
LUA_API const char *luaJIT_profile_mixedstack(lua_State *L, int depth,
					      size_t *len)
{
  return lj_mixprof_mixedstack(L, depth, len);
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

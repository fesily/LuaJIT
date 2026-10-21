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
#include "lj_strfmt.h"
#include "lj_dispatch.h"
#if LJ_HASJIT
#include "lj_jit.h"
#include "lj_trace.h"
#include "lj_vm.h"
#endif
#include "lj_profile.h"
#include "lj_profile_timer.h"
#include "lj_symtab.h"

#include "luajit.h"

/*
** Host (native) callchain capture. The profiler samples at VM safepoints:
** the callback already runs on the profiled thread, so its stack is read
** right there, with no need to stop any thread. The frames therefore start
** at the VM dispatch entry, not at the instruction that was executing when
** the timer ticked.
*/
#if LJ_TARGET_WINDOWS
#define LJ_PROFILE_HOSTSTACK	1
#else
#define LJ_PROFILE_HOSTSTACK	0
#endif

/*
** One frame of the merged callchain. A C function that appears on the Lua
** stack is not emitted as such: the host frames it belongs to are spliced in
** its place, so the merged chain alternates between the two stacks the same
** way the program does.
*/
typedef struct {
  uint8_t kind;		/* LJ_MIX_* */
  uint8_t ffid;
  BCLine line;
  const GCproto *pt;	/* LJ_MIX_LUA */
  const void *addr;	/* LJ_MIX_C / LJ_MIX_HOST */
} LjProfileFrame;

enum { LJ_MIX_LUA, LJ_MIX_FFUNC, LJ_MIX_C, LJ_MIX_HOST };

#define LJ_PROFILE_MIXED_MAX	96

/* Profiler state. */
typedef struct ProfileState {
  global_State *g;		/* VM state that started the profiler. */
  luaJIT_profile_callback cb;	/* Profiler callback. */
  void *data;			/* Profiler callback data. */
  SBuf sb;			/* String buffer for stack dumps. */
  int samples;			/* Number of samples for next callback. */
  int vmstate;			/* VM state when profile timer triggered. */
#if LJ_PROFILE_HOSTSTACK
  int host_enabled;		/* 'h' mode: capture host callchains. */
  void *host_frames[LUAJIT_PROFILE_HOSTSTACK_MAX];
  int host_depth;
  LjProfileFrame mixed[LJ_PROFILE_MIXED_MAX];	/* Merged Lua + host chain. */
  int mixed_depth;
  uintptr_t vm_func[2];	/* PE functions that cover the VM assembly blob. */
  int vm_funcs;
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

#if LJ_PROFILE_HOSTSTACK
/*
** Frames of the profiler itself: the profiler hook trampoline, the profiler
** dispatch and the interpreter entry. They are skipped, so the first frame is
** the code that handed control to the VM.
*/
#define LJ_PROFILE_HOSTSTACK_SKIP	4

/* Start address of the PE function containing pc, 0 if it has no unwind info. */
static uintptr_t profile_func_begin(const void *pc)
{
  DWORD64 base = 0;
  PRUNTIME_FUNCTION rf =
    RtlLookupFunctionEntry((DWORD64)(uintptr_t)pc, &base, NULL);
  return rf != NULL ? (uintptr_t)(base + rf->BeginAddress) : 0;
}

/* Is this function the VM assembly blob (bytecode handlers, interpreter)? */
static int profile_is_vm(const ProfileState *ps, uintptr_t func)
{
  int i;
  if (func == 0)
    return 0;
  for (i = 0; i < ps->vm_funcs; i++)
    if (func == ps->vm_func[i])
      return 1;
  return 0;
}

/* Collect the Lua callchain of the profiled thread, innermost first. */
static int profile_capture_lua(lua_State *L, LjProfileFrame *out, int max)
{
  int level = 0, n = 0, size;

  for (;;) {
    cTValue *frame = lj_debug_frame(L, level, &size);
    GCfunc *fn;
    if (frame == NULL || n >= max)
      break;
    fn = frame_func(frame);
    if (isluafunc(fn)) {
      GCproto *pt = funcproto(fn);
      out[n].kind = LJ_MIX_LUA;
      out[n].pt = pt;
      out[n].line = pt->firstline;
    } else if (iscfunc(fn)) {
      out[n].kind = LJ_MIX_C;
      out[n].addr = (const void *)fn->c.f;
    } else {
      out[n].kind = LJ_MIX_FFUNC;
      out[n].ffid = fn->c.ffid;
    }
    n++;
    level++;
  }
  return n;
}

/*
** Capture both stacks of the current (profiled) thread at the safepoint and
** merge them. The join points are structural, no symbol names are involved:
** a C function on the Lua stack is matched to the host frame that lies in the
** same function, which is found through the PE unwind info of that frame, and
** the host frames above it (callees of that function) are emitted in its
** place.
*/
static LJ_NOINLINE void profile_capture_mixed(ProfileState *ps, lua_State *L)
{
  void *host[LUAJIT_PROFILE_HOSTSTACK_MAX];
  uintptr_t hfunc[LUAJIT_PROFILE_HOSTSTACK_MAX];
  LjProfileFrame lua[LUAJIT_PROFILE_HOSTSTACK_MAX];
  USHORT nhost, i;
  int nlua, n = 0, hi = 0, k;

  nhost = RtlCaptureStackBackTrace(LJ_PROFILE_HOSTSTACK_SKIP,
				   LUAJIT_PROFILE_HOSTSTACK_MAX, host, NULL);
  if (nhost > LUAJIT_PROFILE_HOSTSTACK_MAX)
    nhost = LUAJIT_PROFILE_HOSTSTACK_MAX;
  for (i = 0; i < nhost; i++)
    hfunc[i] = profile_func_begin(host[i]);  /* 0 if it has no unwind info */

  for (i = 0; i < nhost; i++)
    ps->host_frames[i] = (void *)host[i];
  ps->host_depth = (int)nhost;

  nlua = profile_capture_lua(L, lua, LUAJIT_PROFILE_HOSTSTACK_MAX);

  for (k = 0; k < nlua && n < LJ_PROFILE_MIXED_MAX; k++) {
    LjProfileFrame f = lua[k];
    if (f.kind == LJ_MIX_C && f.addr != NULL) {
      int j = hi;
      while (j < (int)nhost && hfunc[j] != (uintptr_t)f.addr)
	j++;
      if (j < (int)nhost) {
	/* The frames above this C function are its callees: splice them in. */
	while (hi <= j && n < LJ_PROFILE_MIXED_MAX) {
	  ps->mixed[n].kind = LJ_MIX_HOST;
	  ps->mixed[n].addr = host[hi];
	  n++;
	  hi++;
	}
	/* Skip the bytecode handler that called this C function. */
	while (hi < (int)nhost && profile_is_vm(ps, hfunc[hi]))
	  hi++;
	continue;  /* The C function itself came from the host chain. */
      }
      /* No matching host frame: keep the marker instead of losing it. */
    } else if (f.kind == LJ_MIX_FFUNC) {
      /* A fast function has no C entry point: the host frames of its
      ** implementation sit above the bytecode handler that invoked it, and
      ** the marker itself is the outer boundary of those frames.
      */
      while (hi < (int)nhost && !profile_is_vm(ps, hfunc[hi]) &&
	     n < LJ_PROFILE_MIXED_MAX) {
	ps->mixed[n].kind = LJ_MIX_HOST;
	ps->mixed[n].addr = host[hi];
	n++;
	hi++;
      }
      while (hi < (int)nhost && profile_is_vm(ps, hfunc[hi]))
	hi++;
      if (n < LJ_PROFILE_MIXED_MAX)
	ps->mixed[n++] = f;
      continue;
    }
    ps->mixed[n++] = f;
  }
  while (hi < (int)nhost && n < LJ_PROFILE_MIXED_MAX) {
    if (!profile_is_vm(ps, hfunc[hi])) {
      ps->mixed[n].kind = LJ_MIX_HOST;
      ps->mixed[n].addr = host[hi];
      n++;
    }
    hi++;
  }
  ps->mixed_depth = n;
}
#endif

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
#if LJ_PROFILE_HOSTSTACK
    if (ps->host_enabled)
      profile_capture_mixed(ps, L);
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
#if LJ_PROFILE_HOSTSTACK
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
#if LJ_PROFILE_HOSTSTACK
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
#if LJ_PROFILE_HOSTSTACK
  ps->host_enabled = host_requested;
  ps->host_depth = 0;
  ps->mixed_depth = 0;
  ps->vm_funcs = 0;
  if (host_requested) {
    ps->vm_func[ps->vm_funcs++] = profile_func_begin((const void *)lj_vm_asm_begin);
    {
      uintptr_t f = profile_func_begin((const void *)lj_vm_profhook);
      if (f != 0 && f != ps->vm_func[0])
	ps->vm_func[ps->vm_funcs++] = f;
    }
  }
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
#if LJ_PROFILE_HOSTSTACK
    ps->host_enabled = 0;
    ps->host_depth = 0;
    ps->mixed_depth = 0;
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

static void profile_symtab_cb(void *ctx, uintptr_t addr, const char *name)
{
  SymtabCb *c = (SymtabCb *)ctx;
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
#if LJ_PROFILE_HOSTSTACK
  ProfileState *ps = &profile_state;
  int depth, i;
  UNUSED(L);
  if (ps->g == NULL || !ps->host_enabled || buf == NULL || maxdepth <= 0)
    return 0;
  depth = ps->host_depth;
  if (depth > maxdepth) depth = maxdepth;
  for (i = 0; i < depth; i++)
    buf[i] = ps->host_frames[i];
  return depth;
#else
  UNUSED(L);
  UNUSED(buf);
  UNUSED(maxdepth);
  return 0;
#endif
}

#if LJ_PROFILE_HOSTSTACK
/* Format the merged callchain, using the same notation as dumpstack. */
static void profile_format_mixed(SBuf *sb, int depth)
{
  ProfileState *ps = &profile_state;
  int lim = ps->mixed_depth;
  int i;
  if (depth > 0 && depth < lim)
    lim = depth;
  for (i = 0; i < lim; i++) {
    const LjProfileFrame *f = &ps->mixed[i];
    if (i != 0)
      lj_buf_putb(sb, ';');
    switch (f->kind) {
    case LJ_MIX_LUA:
      if (lj_debug_putchunkname(sb, (GCproto *)f->pt, 1)) {
	lj_buf_putb(sb, ':');
	lj_strfmt_putint(sb, f->line);
      }
      break;
    case LJ_MIX_FFUNC:
      lj_buf_putmem(sb, "[builtin#", 9);
      lj_strfmt_putint(sb, f->ffid);
      lj_buf_putb(sb, ']');
      break;
    default:
      lj_buf_putb(sb, '@');
      lj_strfmt_putptr(sb, (void *)f->addr);
      break;
    }
  }
}
#endif

/* Return the merged Lua + host callchain of the most recent sample. */
LUA_API const char *luaJIT_profile_mixedstack(lua_State *L, int depth,
					      size_t *len)
{
#if LJ_PROFILE_HOSTSTACK
  ProfileState *ps = &profile_state;
  SBuf *sb = &ps->sb;
  if (ps->g == NULL || !ps->host_enabled) {
    *len = 0;
    return "";
  }
  setsbufL(sb, L);
  lj_buf_reset(sb);
  profile_format_mixed(sb, depth);
  *len = (size_t)sbuflen(sb);
  return sb->b;
#else
  UNUSED(L);
  UNUSED(depth);
  *len = 0;
  return "";
#endif
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

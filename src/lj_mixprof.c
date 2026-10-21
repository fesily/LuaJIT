/*
** Mixed (Lua + host) callchain sampling on top of the profile hook.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
**
** The profiler samples at VM safepoints: the sample handler already runs on
** the profiled thread, so its native stack is read right there, with no need
** to stop any thread. This module turns that into a self-contained sampler:
** the handler is a C function registered through the ordinary profile
** interface (luaJIT_profile_start), so nothing crosses into Lua per sample;
** the samples are aggregated as formatted callchains and a Lua table is built
** only when the caller asks for a report.
*/

#define lj_mixprof_c
#define LUA_CORE

#include "lj_obj.h"

#if LJ_HASPROFILE

#include "lj_buf.h"
#include "lj_debug.h"
#include "lj_strfmt.h"
#include "lj_gc.h"
#include "lj_vm.h"
#include "lj_mixprof.h"

#include "luajit.h"

#if LJ_HASMIXPROF

#include "lj_frame.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

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
} MixFrame;

enum { LJ_MIX_LUA, LJ_MIX_FFUNC, LJ_MIX_C, LJ_MIX_HOST };

/* One collected sample: a formatted callchain and the number of ticks. */
typedef struct {
  uint32_t off;		/* Offset of the chain in the arena. */
  uint32_t len;		/* Length of the chain, without the terminator. */
  uint32_t count;	/* Number of ticks this chain was observed. */
} MixRecord;

typedef struct MixProfState {
  lua_State *L;		/* Profiled VM, NULL if no run has been prepared. */
  int active;		/* Collecting samples. */
  int capturing;	/* The last capture is valid ('h' mode is on). */
  int depth;		/* Frames kept per sample. */
  /* Last captured sample, read by the public hoststack/mixedstack API. */
  void *host[LUAJIT_PROFILE_HOSTSTACK_MAX];
  int nhost;
  MixFrame mixed[LJMIXPROF_MAX_FRAMES];
  int nmixed;
  uintptr_t vm_func[2];	/* PE functions that cover the VM assembly blob. */
  int vm_funcs;
  /* Collected samples. */
  MixRecord *recs;
  uint32_t nrecs, caprecs;
  char *arena;
  size_t used, cap;
  uint32_t ticks;	/* Total number of ticks. */
  SBuf sb;		/* Formatting buffer. */
} MixProfState;

static MixProfState mixprof;

/* -- Merging ------------------------------------------------------------- */

/* Start address of the PE function containing pc, 0 if it has no unwind info. */
static uintptr_t mixprof_func_begin(const void *pc)
{
  DWORD64 base = 0;
  PRUNTIME_FUNCTION rf =
    RtlLookupFunctionEntry((DWORD64)(uintptr_t)pc, &base, NULL);
  return rf != NULL ? (uintptr_t)(base + rf->BeginAddress) : 0;
}

/* Is this function the VM assembly blob (bytecode handlers, interpreter)? */
static int mixprof_is_vm(const MixProfState *mp, uintptr_t func)
{
  int i;
  if (func == 0)
    return 0;
  for (i = 0; i < mp->vm_funcs; i++)
    if (func == mp->vm_func[i])
      return 1;
  return 0;
}

/* Remember which PE functions make up the VM assembly. */
static void mixprof_vm_functions(MixProfState *mp)
{
  mp->vm_funcs = 0;
  mp->vm_func[mp->vm_funcs++] = mixprof_func_begin((const void *)lj_vm_asm_begin);
  {
    uintptr_t f = mixprof_func_begin((const void *)lj_vm_profhook);
    if (f != 0 && f != mp->vm_func[0])
      mp->vm_func[mp->vm_funcs++] = f;
  }
}

/* Collect the Lua callchain of the profiled thread, innermost first. */
static int mixprof_capture_lua(lua_State *L, MixFrame *out, int max)
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

/* Frames of the sampler itself: hook trampoline, dispatch and entry. */
#define MIXPROF_HOST_SKIP	4

/*
** Read both stacks of the current (profiled) thread at the safepoint and
** merge them. The join points are structural, no symbol names are involved:
** a C function on the Lua stack is matched to the host frame that lies in the
** same function, which is found through the PE unwind info of that frame, and
** the host frames above it (callees of that function) are emitted in its
** place.
*/
LJ_NOINLINE void lj_mixprof_capture(lua_State *L)
{
  MixProfState *mp = &mixprof;
  void *host[LUAJIT_PROFILE_HOSTSTACK_MAX];
  uintptr_t hfunc[LUAJIT_PROFILE_HOSTSTACK_MAX];
  MixFrame lua[LUAJIT_PROFILE_HOSTSTACK_MAX];
  USHORT nhost, i;
  int nlua, n = 0, hi = 0, k;

  nhost = RtlCaptureStackBackTrace(MIXPROF_HOST_SKIP,
				   LUAJIT_PROFILE_HOSTSTACK_MAX, host, NULL);
  if (nhost > LUAJIT_PROFILE_HOSTSTACK_MAX)
    nhost = LUAJIT_PROFILE_HOSTSTACK_MAX;
  for (i = 0; i < nhost; i++)
    hfunc[i] = mixprof_func_begin(host[i]);  /* 0 if it has no unwind info */

  for (i = 0; i < nhost; i++)
    mp->host[i] = (void *)host[i];
  mp->nhost = (int)nhost;
  mp->capturing = 1;

  nlua = mixprof_capture_lua(L, lua, LUAJIT_PROFILE_HOSTSTACK_MAX);

  for (k = 0; k < nlua && n < LJMIXPROF_MAX_FRAMES; k++) {
    MixFrame f = lua[k];
    if (f.kind == LJ_MIX_C && f.addr != NULL) {
      int j = hi;
      while (j < (int)nhost && hfunc[j] != (uintptr_t)f.addr)
	j++;
      if (j < (int)nhost) {
	/* The frames above this C function are its callees: splice them in. */
	while (hi <= j && n < LJMIXPROF_MAX_FRAMES) {
	  mp->mixed[n].kind = LJ_MIX_HOST;
	  mp->mixed[n].addr = host[hi];
	  n++;
	  hi++;
	}
	/* Skip the bytecode handler that called this C function. */
	while (hi < (int)nhost && mixprof_is_vm(mp, hfunc[hi]))
	  hi++;
	continue;  /* The C function itself came from the host chain. */
      }
      /* No matching host frame: keep the marker instead of losing it. */
    } else if (f.kind == LJ_MIX_FFUNC) {
      /* A fast function has no C entry point: the host frames of its
      ** implementation sit above the bytecode handler that invoked it, and
      ** the marker itself is the outer boundary of those frames.
      */
      while (hi < (int)nhost && !mixprof_is_vm(mp, hfunc[hi]) &&
	     n < LJMIXPROF_MAX_FRAMES) {
	mp->mixed[n].kind = LJ_MIX_HOST;
	mp->mixed[n].addr = host[hi];
	n++;
	hi++;
      }
      while (hi < (int)nhost && mixprof_is_vm(mp, hfunc[hi]))
	hi++;
      if (n < LJMIXPROF_MAX_FRAMES)
	mp->mixed[n++] = f;
      continue;
    }
    mp->mixed[n++] = f;
  }
  while (hi < (int)nhost && n < LJMIXPROF_MAX_FRAMES) {
    if (!mixprof_is_vm(mp, hfunc[hi])) {
      mp->mixed[n].kind = LJ_MIX_HOST;
      mp->mixed[n].addr = host[hi];
      n++;
    }
    hi++;
  }
  mp->nmixed = n;
}

void lj_mixprof_reset_capture(void)
{
  MixProfState *mp = &mixprof;
  mp->capturing = 0;
  mp->nhost = 0;
  mp->nmixed = 0;
}

/* -- Formatting ---------------------------------------------------------- */

/* Format one merged callchain, using the same notation as dumpstack. */
static void mixprof_format(SBuf *sb, const MixFrame *frames, int n, int depth)
{
  int lim = n, i;
  if (depth > 0 && depth < lim)
    lim = depth;
  for (i = 0; i < lim; i++) {
    const MixFrame *f = &frames[i];
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

/* -- Sample collection --------------------------------------------------- */

/* Append one formatted chain to the arena and count its ticks. */
static void mixprof_store(MixProfState *mp, const char *s, size_t len,
			  uint32_t ticks)
{
  lua_State *L = mp->L;
  size_t need = mp->used + len;

  if (mp->nrecs == mp->caprecs) {
    uint32_t cap = mp->caprecs ? mp->caprecs * 2 : 64;
    /* Out of memory is a Lua error, raised through the normal path. */
    mp->recs = (MixRecord *)lj_mem_realloc(L, mp->recs,
					   (GCSize)(mp->caprecs * sizeof(MixRecord)),
					   (GCSize)(cap * sizeof(MixRecord)));
    mp->caprecs = cap;
  }
  if (need > mp->cap) {
    size_t cap = mp->cap ? mp->cap : 4096;
    while (cap < need)
      cap *= 2;
    mp->arena = (char *)lj_mem_realloc(L, mp->arena, (GCSize)mp->cap, (GCSize)cap);
    mp->cap = cap;
  }
  memcpy(mp->arena + mp->used, s, len);
  mp->recs[mp->nrecs].off = (uint32_t)mp->used;
  mp->recs[mp->nrecs].len = (uint32_t)len;
  mp->recs[mp->nrecs].count = ticks;
  mp->nrecs++;
  mp->used = need;
  mp->ticks += ticks;
}

/* Format the last capture and store it (sample handler, no Lua code). */
static void mixprof_collect(MixProfState *mp, uint32_t ticks)
{
  SBuf *sb = &mp->sb;
  if (!mp->capturing)
    return;
  setsbufL(sb, mp->L);
  lj_buf_reset(sb);
  mixprof_format(sb, mp->mixed, mp->nmixed, mp->depth);
  mixprof_store(mp, sb->b, (size_t)sbuflen(sb), ticks);
}

/*
** Sample handler. The profile interface calls it on the profiled thread at
** the safepoint, with the host stacks already captured by lj_mixprof_capture;
** `samples` is the number of ticks that elapsed since the previous call.
*/
static void LJ_FASTCALL mixprof_cb(void *data, lua_State *L, int samples,
				   int vmstate)
{
  MixProfState *mp = &mixprof;
  UNUSED(data);
  UNUSED(vmstate);
  if (!mp->active || mp->L != L || samples <= 0)
    return;
  mixprof_collect(mp, (uint32_t)samples);
}

/* -- Public module API --------------------------------------------------- */

/* Format "hi<interval>" without dragging in stdio. */
static void mixprof_mode(char *buf, int interval)
{
  char digits[8];
  int n = 0, i = 0;
  buf[n++] = 'h';
  buf[n++] = 'i';
  do {
    digits[sizeof(digits) - 1 - i++] = (char)('0' + interval % 10);
    interval /= 10;
  } while (interval != 0 && i < (int)sizeof(digits));
  while (i > 0)
    buf[n++] = digits[sizeof(digits) - i--];
  buf[n] = '\0';
}

/* Free the collected samples and the formatting buffer. */
static void mixprof_purge(MixProfState *mp)
{
  if (mp->L != NULL) {
    global_State *g = G(mp->L);
    if (mp->recs != NULL)
      lj_mem_free(g, mp->recs, mp->caprecs * sizeof(MixRecord));
    if (mp->arena != NULL)
      lj_mem_free(g, mp->arena, mp->cap);
    if (mp->sb.b != NULL)
      lj_buf_free(g, &mp->sb);
    mp->sb.b = mp->sb.w = mp->sb.e = NULL;
  }
  mp->recs = NULL;
  mp->nrecs = mp->caprecs = 0;
  mp->arena = NULL;
  mp->used = mp->cap = 0;
  mp->ticks = 0;
}

void lj_mixprof_start(lua_State *L, int interval, int depth)
{
  MixProfState *mp = &mixprof;
  char mode[16];

  lj_mixprof_stop(L);
  mixprof_purge(mp);
  lj_mixprof_reset_capture();

  if (depth <= 0)
    depth = 32;
  if (depth > LJMIXPROF_MAX_FRAMES)
    depth = LJMIXPROF_MAX_FRAMES;
  if (interval <= 0)
    interval = 10;
  mp->L = L;
  mp->depth = depth;
  mp->active = 1;
  mixprof_vm_functions(mp);
  lj_buf_init(L, &mp->sb);

  /*
  ** The ordinary profile interface does the sampling: 'h' makes the hook
  ** capture the host stacks at every safepoint, 'iN' sets the interval. The
  ** callback is a C function, so no Lua code runs per sample.
  */
  mixprof_mode(mode, interval);
  luaJIT_profile_start(L, mode, mixprof_cb, NULL);
}

void lj_mixprof_stop(lua_State *L)
{
  MixProfState *mp = &mixprof;
  if (mp->active && mp->L == L) {
    luaJIT_profile_stop(L);
    mp->active = 0;
  }
  lj_mixprof_reset_capture();
}

int lj_mixprof_active(void)
{
  return mixprof.active;
}

/* Order two collected samples by their callchain. */
static MixProfState *mixprof_sort_state;

static int mixprof_cmp(const void *a, const void *b)
{
  const MixRecord *x = (const MixRecord *)a;
  const MixRecord *y = (const MixRecord *)b;
  const char *arena = mixprof_sort_state->arena;
  uint32_t len = x->len < y->len ? x->len : y->len;
  int r = memcmp(arena + x->off, arena + y->off, len);
  if (r != 0)
    return r;
  return x->len < y->len ? -1 : (x->len > y->len);
}

int lj_mixprof_report(lua_State *L)
{
  MixProfState *mp = &mixprof;
  uint32_t i, n = 0;

  if (mp->nrecs > 1) {
    mixprof_sort_state = mp;
    qsort(mp->recs, mp->nrecs, sizeof(MixRecord), mixprof_cmp);
    mixprof_sort_state = NULL;
  }
  lua_createtable(L, 0, 0);
  for (i = 0; i < mp->nrecs; i = n) {
    const MixRecord *first = &mp->recs[i];
    uint32_t count = 0;
    for (n = i; n < mp->nrecs; n++) {
      const MixRecord *r = &mp->recs[n];
      if (r->len != first->len ||
	  memcmp(mp->arena + r->off, mp->arena + first->off, r->len) != 0)
	break;
      count += r->count;
    }
    lua_pushlstring(L, mp->arena + first->off, first->len);
    lua_pushinteger(L, (lua_Integer)count);
    lua_rawset(L, -3);
  }
  return (int)mp->ticks;
}

/* -- Accessors behind luaJIT_profile_hoststack/mixedstack ---------------- */

int lj_mixprof_hoststack(lua_State *L, const void **buf, int maxdepth)
{
  MixProfState *mp = &mixprof;
  int depth, i;
  UNUSED(L);
  if (!mp->capturing || buf == NULL || maxdepth <= 0)
    return 0;
  depth = mp->nhost;
  if (depth > maxdepth)
    depth = maxdepth;
  for (i = 0; i < depth; i++)
    buf[i] = mp->host[i];
  return depth;
}

const char *lj_mixprof_mixedstack(lua_State *L, int depth, size_t *len)
{
  MixProfState *mp = &mixprof;
  SBuf *sb;
  if (!mp->capturing) {
    *len = 0;
    return "";
  }
  if (mp->sb.b == NULL)
    lj_buf_init(L, &mp->sb);
  sb = &mp->sb;
  setsbufL(sb, L);
  lj_buf_reset(sb);
  mixprof_format(sb, mp->mixed, mp->nmixed, depth);
  *len = (size_t)sbuflen(sb);
  return sb->b;
}

#else /* !LJ_HASMIXPROF */

/* Mixed sampling needs the native unwinder of the Windows port. */

void lj_mixprof_start(lua_State *L, int interval, int depth)
{
  UNUSED(L); UNUSED(interval); UNUSED(depth);
}

void lj_mixprof_stop(lua_State *L)
{
  UNUSED(L);
}

int lj_mixprof_active(void)
{
  return 0;
}

int lj_mixprof_report(lua_State *L)
{
  lua_createtable(L, 0, 0);
  return 0;
}

void lj_mixprof_capture(lua_State *L)
{
  UNUSED(L);
}

void lj_mixprof_reset_capture(void)
{
}

int lj_mixprof_hoststack(lua_State *L, const void **buf, int maxdepth)
{
  UNUSED(L); UNUSED(buf); UNUSED(maxdepth);
  return 0;
}

const char *lj_mixprof_mixedstack(lua_State *L, int depth, size_t *len)
{
  UNUSED(L); UNUSED(depth);
  *len = 0;
  return "";
}

#endif /* LJ_HASMIXPROF */

#endif /* LJ_HASPROFILE */

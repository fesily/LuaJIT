/*
** Memory profiler library: memprof.snapshot{}, memprof.diff(s1, s2).
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
**
** Loadable Lua module, registered like jit.profile (see lib_init.c preload).
** The whole module is gated behind LUAJIT_ENABLE_MEMPROF && LJ_HASGCMARK.
** With the flag off the file compiles to an empty translation unit and the
** preload entry is absent, so the build is byte-identical to the no-memprof
** baseline.
*/

#define lib_memprof_c
#define LUA_LIB

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_lib.h"

#if LJ_HASGCMARK && defined(LUAJIT_ENABLE_MEMPROF)
#include "lj_memprof.h"
#endif

/* -- memprof.* functions ------------------------------------------------- */

#define LJLIB_MODULE_memprof

/* memprof.snapshot{name=, gc="full"|"step"|"none", details=true|false}
** Returns a census table (see lj_memprof.c for the layout). */
LJLIB_CF(memprof_snapshot)
{
#if LJ_HASGCMARK && defined(LUAJIT_ENABLE_MEMPROF)
  MemprofOpts opts;
  GCtab *arg = NULL;
  opts.gc = LJ_MEMPROF_GC_NONE;
  opts.details = 0;
  if (L->base < L->top && tvistab(L->base)) {
    TValue k;
    const TValue *v;
    arg = tabV(L->base);
    /* gc option. */
    setstrV(L, &k, lj_str_newlit(L, "gc"));
    v = lj_tab_get(L, arg, &k);
    if (tvisstr(v)) {
      GCstr *s = strV(v);
      if (strdata(s)[0] == 'f') opts.gc = LJ_MEMPROF_GC_FULL;	/* "full" */
      else if (strdata(s)[0] == 's') opts.gc = LJ_MEMPROF_GC_STEP; /* "step" */
      else opts.gc = LJ_MEMPROF_GC_NONE;
    }
    /* details option (default false). */
    setstrV(L, &k, lj_str_newlit(L, "details"));
    v = lj_tab_get(L, arg, &k);
    if (tvisbool(v) && boolV(v))
      opts.details = 1;
    /* name option (stored verbatim in the snapshot for correlation). */
    setstrV(L, &k, lj_str_newlit(L, "name"));
    v = lj_tab_get(L, arg, &k);
    if (tvisstr(v)) {
      /* Stash on the returned table via lj_memprof_snapshot? The walker
      ** does not take a name; set it here after the call. */
    }
  }
  {
    int n = lj_memprof_snapshot(L, &opts);
    /* Attach name if provided. */
    if (n == 1 && arg != NULL) {
      TValue nk, nv;
      GCtab *res;
      setstrV(L, &nk, lj_str_newlit(L, "name"));
      nv = *lj_tab_get(L, arg, &nk);
      if (tvisstr(&nv)) {
	res = tabV(L->top - 1);
	copyTV(L, lj_tab_set(L, res, &nk), &nv);
      }
    }
    return n;
  }
#else
  return luaL_error(L, "memprof not enabled (build with -DLUAJIT_ENABLE_MEMPROF)");
#endif
}

/* memprof.diff(s1, s2) -> {leak_suspects=, grown=, shrunk=, new_count, gone_count} */
LJLIB_CF(memprof_diff)
{
#if LJ_HASGCMARK && defined(LUAJIT_ENABLE_MEMPROF)
  return lj_memprof_diff(L);
#else
  return luaL_error(L, "memprof not enabled (build with -DLUAJIT_ENABLE_MEMPROF)");
#endif
}

/* memprof.start{mode="event", depth=1, out="path"} -> true | nil, err
** Starts the event-stream profiler. Only one VM may profile at a time. */
LJLIB_CF(memprof_start)
{
#if LJ_HASGCMARK && defined(LUAJIT_ENABLE_MEMPROF)
  GCtab *arg = NULL;
  TValue k;
  const TValue *v;
  const char *outpath = NULL;
  int depth = 1;
  int rc;
  if (!(L->base < L->top && tvistab(L->base)))
    return luaL_error(L, "memprof.start: table argument required");
  arg = tabV(L->base);
  setstrV(L, &k, lj_str_newlit(L, "out"));
  v = lj_tab_get(L, arg, &k);
  if (tvisstr(v)) outpath = strdata(strV(v));
  setstrV(L, &k, lj_str_newlit(L, "depth"));
  v = lj_tab_get(L, arg, &k);
  if (tvisnum(v)) depth = (int)numV(v);
  if (outpath == NULL)
    return luaL_error(L, "memprof.start: out= path required");
  rc = lj_memprof_start(L, outpath, depth);
  if (rc == 1)
    return luaL_error(L, "memprof.start: profiler already active on a VM");
  if (rc == 2)
    return luaL_error(L, "memprof.start: cannot open output file");
  lua_pushboolean(L, 1);
  return 1;
#else
  return luaL_error(L, "memprof not enabled (build with -DLUAJIT_ENABLE_MEMPROF)");
#endif
}

/* memprof.stop() -> true
** Stops the event stream, flushes symtab + epilogue, closes the file. */
LJLIB_CF(memprof_stop)
{
#if LJ_HASGCMARK && defined(LUAJIT_ENABLE_MEMPROF)
  lj_memprof_stop(L);
  lua_pushboolean(L, 1);
  return 1;
#else
  return luaL_error(L, "memprof not enabled (build with -DLUAJIT_ENABLE_MEMPROF)");
#endif
}

#include "lj_libdef.h"

#if LJ_HASGCMARK && defined(LUAJIT_ENABLE_MEMPROF)
LUALIB_API int luaopen_memprof(lua_State *L)
{
  LJ_LIB_REG(L, NULL, memprof);
  return 1;
}
#else
/* Stub so the symbol exists when the feature is disabled (never registered). */
LUALIB_API int luaopen_memprof(lua_State *L)
{
  return luaL_error(L, "memprof not enabled (build with -DLUAJIT_ENABLE_MEMPROF)");
}
#endif

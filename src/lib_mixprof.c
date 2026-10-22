/*
** Mixed (Lua + host) callchain profiler: the Lua interface.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
**
** The sampling itself runs in C (see lj_mixprof.c): this module only starts
** and stops it and asks for the aggregated report, so no Lua callback is
** involved while profiling.
*/

#define lib_mixprof_c
#define LUA_LIB

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_lib.h"
#include "lj_mixprof.h"

#include "lualib.h"

#if LJ_HASPROFILE

#define LJLIB_MODULE_mixprof

/* mixprof.start(depth, interval) */
LJLIB_CF(mixprof_start)
{
  int depth = lj_lib_optint(L, 1, 32);
  int interval = lj_lib_optint(L, 2, 10);
  lj_mixprof_start(L, interval, depth);
  return 0;
}

/* mixprof.stop() */
LJLIB_CF(mixprof_stop)
{
  lj_mixprof_stop(L);
  return 0;
}

/* chains, spans, ticks = mixprof.report() */
LJLIB_CF(mixprof_report)
{
  int ticks = lj_mixprof_report(L);
  lua_pushinteger(L, ticks);
  return 3;
}

/* info = mixprof.info() */
LJLIB_CF(mixprof_info)
{
  return lj_mixprof_info(L);
}

/* mixprof.active() */
LJLIB_CF(mixprof_active)
{
  lua_pushboolean(L, lj_mixprof_active());
  return 1;
}

#include "lj_libdef.h"

LUALIB_API int luaopen_mixprof(lua_State *L)
{
  LJ_LIB_REG(L, LUA_JITLIBNAME ".mixprof", mixprof);
  return 1;
}

#endif

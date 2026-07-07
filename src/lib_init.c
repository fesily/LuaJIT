/*
** Library initialization.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
**
** Major parts taken verbatim from the Lua interpreter.
** Copyright (C) 1994-2008 Lua.org, PUC-Rio. See Copyright Notice in lua.h
*/

#define lib_init_c
#define LUA_LIB

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_arch.h"

#include <stdlib.h>
#include <string.h>

#if LJ_HASGCMARK && defined(LUAJIT_ENABLE_MEMPROF)
LUALIB_API int luaopen_memprof(lua_State *L);
#define LUA_MEMPROFLIBNAME	"memprof"
#endif

static luaL_Reg lj_lib_load[] = {
  { "",			luaopen_base },
  { LUA_LOADLIBNAME,	luaopen_package },
  { LUA_TABLIBNAME,	luaopen_table },
  { LUA_IOLIBNAME,	luaopen_io },
  { LUA_OSLIBNAME,	luaopen_os },
  { LUA_STRLIBNAME,	luaopen_string },
  { LUA_MATHLIBNAME,	luaopen_math },
  { LUA_DBLIBNAME,	luaopen_debug },
  { LUA_BITLIBNAME,	luaopen_bit },
  { LUA_JITLIBNAME,	luaopen_jit },
  { NULL,		NULL }
};

static const luaL_Reg lj_lib_preload[] = {
#if LJ_HASFFI
  { LUA_FFILIBNAME,	luaopen_ffi },
#endif
#if LJ_HASGCMARK && defined(LUAJIT_ENABLE_MEMPROF)
  { LUA_MEMPROFLIBNAME,	luaopen_memprof },
#endif
  { NULL,		NULL }
};

#ifdef DO_LUA_INIT
static void handle_luainit(lua_State *L)
{
  const char *init = getenv(LUA_INIT);
  if (init == NULL)
    return;
  if (init[0] == '@')
    (luaL_loadfile(L, init+1) || lua_pcall(L, 0, 0, 0));
  else
    (luaL_loadstring(L, init) || lua_pcall(L, 0, 0, 0));
}
#endif

LUALIB_API void luaL_openlibs(lua_State *L)
{
  const luaL_Reg *lib;
  for (lib = lj_lib_load; lib->func; lib++) {
    lua_pushcfunction(L, lib->func);
    lua_pushstring(L, lib->name);
    lua_call(L, 1, 0);
  }
  luaL_findtable(L, LUA_REGISTRYINDEX, "_PRELOAD",
		 sizeof(lj_lib_preload)/sizeof(lj_lib_preload[0])-1);
  for (lib = lj_lib_preload; lib->func; lib++) {
    lua_pushcfunction(L, lib->func);
    lua_setfield(L, -2, lib->name);
  }
  lua_pop(L, 1);
#ifdef LJ_DS
  const char* dump_fix = ""
#if LUA_COMPAT_MATH
 "math.mod = math.fmod\n"
#endif
;
  (luaL_loadstring(L, dump_fix) || lua_pcall(L, 0, 0, 0));
#endif

#if DO_LUA_INIT
  handle_luainit(L);
#endif
}

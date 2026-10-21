/*
** Miscellaneous public C API extensions: the platform and Lua profiler.
**
** This is the sysprof subset of the tarantool misc library, ported to the
** v2.1 tree.
*/

#define lj_mapi_c
#define LUA_CORE

#include "lua.h"
#include "lmisclib.h"

#include "lj_obj.h"
#include "lj_sysprof.h"

LUAMISC_API int luaM_sysprof_set_writer(luam_Sysprof_writer writer)
{
  return lj_sysprof_set_writer(writer);
}

LUAMISC_API int luaM_sysprof_set_on_stop(luam_Sysprof_on_stop on_stop)
{
  return lj_sysprof_set_on_stop(on_stop);
}

LUAMISC_API int luaM_sysprof_set_backtracer(luam_Sysprof_backtracer backtracer)
{
  return lj_sysprof_set_backtracer(backtracer);
}

LUAMISC_API int luaM_sysprof_start(lua_State *L,
				   const struct luam_Sysprof_Options *opt)
{
  return lj_sysprof_start(L, opt);
}

LUAMISC_API int luaM_sysprof_stop(lua_State *L)
{
  return lj_sysprof_stop(L);
}

LUAMISC_API int luaM_sysprof_report(struct luam_Sysprof_Counters *counters)
{
  return lj_sysprof_report(counters);
}

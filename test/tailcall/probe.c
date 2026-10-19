#include "lua.h"
#include "lauxlib.h"

static unsigned calls;

static int values(lua_State *L)
{
  int i, n = luaL_checkint(L, 1);
  luaL_argcheck(L, n >= 0 && n <= 600, 1, "result count out of range");
  if (!lua_checkstack(L, n+8)) return luaL_error(L, "cannot grow result stack");
  calls++;
  for (i = 1; i <= n; i++) {
    if (i % 3 == 0) lua_pushnil(L);
    else lua_pushinteger(L, n*1000+i);
  }
  return n;
}

static int objects(lua_State *L)
{
  int i, n = luaL_checkint(L, 1);
  luaL_argcheck(L, n >= 0 && n <= 600, 1, "result count out of range");
  if (!lua_checkstack(L, n+8)) return luaL_error(L, "cannot grow result stack");
  calls++;
  for (i = 1; i <= n; i++) {
    lua_newtable(L);
    lua_pushinteger(L, i);
    lua_setfield(L, -2, "index");
  }
  lua_gc(L, LUA_GCSTEP, 1);
  return n;
}

static int meta_values(lua_State *L)
{
  lua_remove(L, 1);
  return values(L);
}

static int invoke(lua_State *L)
{
  luaL_checktype(L, 1, LUA_TFUNCTION);
  lua_settop(L, 2);
  calls++;
  lua_pushvalue(L, 1);
  lua_pushvalue(L, 2);
  lua_call(L, 1, LUA_MULTRET);
  return lua_gettop(L)-2;
}

static int yield_values(lua_State *L)
{
  return lua_yield(L, values(L));
}

static int fail(lua_State *L)
{
  calls++;
  return luaL_error(L, "tail-call probe error");
}

static int count(lua_State *L)
{
  lua_pushnumber(L, calls);
  return 1;
}

static int visible(lua_State *L)
{
  lua_Debug ar;
  int level, target = 1;
  if (lua_istable(L, 1)) {
    lua_getfield(L, 1, "target");
    target = lua_gettop(L);
  }
  luaL_checktype(L, target, LUA_TFUNCTION);
  calls++;
  for (level = 0; lua_getstack(L, level, &ar); level++) {
    lua_getinfo(L, "f", &ar);
    if (lua_rawequal(L, target, -1)) {
      lua_pushboolean(L, 1);
      return 1;
    }
    lua_pop(L, 1);
  }
  lua_pushboolean(L, 0);
  return 1;
}

#ifdef _WIN32
__declspec(dllexport)
#endif
int luaopen_tailcallprobe(lua_State *L)
{
  static const luaL_Reg funcs[] = {
    {"values", values}, {"objects", objects}, {"invoke", invoke},
    {"yield_values", yield_values}, {"fail", fail}, {"count", count},
    {"visible", visible},
    {"meta_values", meta_values},
    {NULL, NULL}
  };
  luaL_register(L, "tailcallprobe", funcs);
  return 1;
}

#include "lua.h"
#include "lauxlib.h"

static int values(lua_State *L)
{
  int i, n = luaL_checkint(L, 1);
  luaL_argcheck(L, n >= 0 && n <= 600, 1, "result count out of range");
  if (!lua_checkstack(L, n + 8))
    return luaL_error(L, "cannot grow result stack");
  for (i = 1; i <= n; i++) {
    if (i % 3 == 0)
      lua_pushnil(L);
    else
      lua_pushinteger(L, n * 1000 + i);
  }
  return n;
}

static int callback(lua_State *L)
{
  luaL_checktype(L, 1, LUA_TFUNCTION);
  lua_pushvalue(L, 1);
  lua_call(L, 0, 0);
  lua_pushinteger(L, 7);
  return 1;
}

static int fail(lua_State *L)
{
  return luaL_error(L, "retm evaluation error");
}

static int yield_values(lua_State *L)
{
  int n = values(L);
  return lua_yield(L, n);
}

#ifdef _WIN32
__declspec(dllexport)
#endif
int luaopen_retmprobe(lua_State *L)
{
  static const luaL_Reg funcs[] = {
    {"values", values}, {"callback", callback},
    {"fail", fail}, {"yield_values", yield_values}, {NULL, NULL}
  };
  luaL_register(L, "retmprobe", funcs);
  return 1;
}

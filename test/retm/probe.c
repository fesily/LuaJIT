/* Internal assertion entry points are hidden in the VM DLL. */
#undef LUA_USE_ASSERT
#undef LUA_USE_APICHECK
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_obj.h"
#include "lj_jit.h"
#include "lj_dispatch.h"

static unsigned calls;

#ifdef _WIN32
__declspec(dllexport)
#endif
double retm_weighted(double a, double b, double c, double d,
  double e, double f, double g, double h, double i, double j, double k, double l,
  double m, double n, double o, double p)
{
  return a+b+c+d+e+f+g+h+i+j+k+l+m+n+o+p;
}

static int values(lua_State *L)
{
  int i, n = luaL_checkint(L, 1);
  int typed = lua_toboolean(L, 2);
  luaL_argcheck(L, n >= 0 && n <= 600, 1, "result count out of range");
  if (!lua_checkstack(L, n+8)) return luaL_error(L, "cannot grow result stack");
  calls++;
  for (i = 1; i <= n; i++) {
    if (i % 3 == 0) lua_pushnil(L);
    else if (typed && i == 1) lua_pushliteral(L, "changed");
    else lua_pushinteger(L, n*1000+i);
  }
  return n;
}

static int callback(lua_State *L)
{
  luaL_checktype(L, 1, LUA_TFUNCTION);
  calls++;
  lua_pushvalue(L, 1);
  lua_call(L, 0, 0);
  lua_pushinteger(L, 7);
  return 1;
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

static int yield_values(lua_State *L)
{
  return lua_yield(L, values(L));
}

static void field(lua_State *L, const char *key, unsigned value)
{
  lua_pushnumber(L, value);
  lua_setfield(L, -2, key);
}

static int stats(lua_State *L)
{
  jit_State *J = G2J(G(L));
  TraceNo n;
  unsigned roots = 0, sides = 0, retfv = 0, linked = 0, interpreted = 0;
  lua_newtable(L);
  field(L, "calls", calls);
  field(L, "entries", J->retm_entries);
  field(L, "returns", J->retm_returns);
  field(L, "fallbacks", J->retm_fallbacks);
  field(L, "preexits", J->retm_preexits);
  field(L, "postexits", J->retm_postexits);
  field(L, "cframe", LUA_COMPAT_TAILCALL_CFRAME);
  field(L, "count", LUA_COMPAT_TAILCALL_COUNT);
  for (n = 1; n < J->sizetrace; n++) {
    GCtrace *T = traceref(J, n);
    IRRef ref;
    if (!T || !T->stitch_entry) continue;
    if (T->root) sides++;
    else {
      roots++;
      luaL_argcheck(L, bc_op(T->startins) == BC_RETM, 1, "wrong entry bytecode");
    }
    if (T->retm_retref && T->link) linked++;
    if (!T->retm_retref && T->linktype == LJ_TRLINK_RETURN) interpreted++;
    for (ref = REF_FIRST; ref < T->nins; ref++)
      if (T->ir[ref].o == IR_RETFV) retfv++;
  }
  field(L, "roots", roots);
  field(L, "sides", sides);
  field(L, "retfv", retfv);
  field(L, "linked", linked);
  field(L, "interpreted", interpreted);
  return 1;
}

#ifdef _WIN32
__declspec(dllexport)
#endif
int luaopen_retmprobe(lua_State *L);

static int other_state(lua_State *L)
{
  lua_State *S = luaL_newstate();
  int status;
  unsigned returns;
  if (!S) return luaL_error(L, "cannot create independent state");
  luaL_openlibs(S);
  luaopen_retmprobe(S);
  lua_settop(S, 0);
  status = luaL_dostring(S,
    "local p=require('retmprobe'); jit.opt.start('hotloop=3','minstitch=0');"
    "local function f(n) return p.values(n) end;"
    "for i=1,1200 do local a=f(4); assert(a==4001) end");
  returns = G2J(G(S))->retm_returns;
  if (status) {
    lua_pushstring(L, lua_tostring(S, -1));
    lua_close(S);
    return lua_error(L);
  }
  lua_close(S);
  lua_pushinteger(L, returns);
  return 1;
}

#ifdef _WIN32
__declspec(dllexport)
#endif
int luaopen_retmprobe(lua_State *L)
{
  static const luaL_Reg funcs[] = {
    {"values", values}, {"callback", callback}, {"invoke", invoke},
    {"yield_values", yield_values},
    {"stats", stats}, {"other_state", other_state}, {"objects", objects},
    {NULL, NULL}
  };
  luaL_register(L, "retmprobe", funcs);
  return 1;
}

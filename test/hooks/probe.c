/* Native hook checks the lua_Debug passed by the VM and leaves stack scratch. */
#include "lua.h"
#include "lauxlib.h"

typedef struct HookRun {
  int logref;
  int callbackref;
  int events;
  int calls;
  int returns;
  int tails;
} HookRun;

static char context_key;

static void context_table(lua_State *L)
{
  lua_pushlightuserdata(L, &context_key);
  lua_rawget(L, LUA_REGISTRYINDEX);
  if (lua_isnil(L, -1)) {
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushlightuserdata(L, &context_key);
    lua_pushvalue(L, -2);
    lua_rawset(L, LUA_REGISTRYINDEX);
  }
}

static HookRun *context_get(lua_State *L)
{
  HookRun *run;
  context_table(L);
  lua_pushlightuserdata(L, L);
  lua_rawget(L, -2);
  run = (HookRun *)lua_touserdata(L, -1);
  lua_pop(L, 2);
  return run;
}

static void context_set(lua_State *L, HookRun *run)
{
  context_table(L);
  lua_pushlightuserdata(L, L);
  if (run) lua_pushlightuserdata(L, run); else lua_pushnil(L);
  lua_rawset(L, -3);
  lua_pop(L, 1);
}

static void native_hook(lua_State *L, lua_Debug *ar)
{
  HookRun *run = context_get(L);
  lua_Debug parent;
  int log, record, event = ar->event;
  if (!run) return;
  if (!lua_checkstack(L, 600)) luaL_error(L, "hook stack growth failed");
  lua_rawgeti(L, LUA_REGISTRYINDEX, run->logref);
  log = lua_gettop(L);
  lua_newtable(L);
  record = lua_gettop(L);
  lua_pushinteger(L, event);
  lua_setfield(L, record, "event");
  if (!lua_getinfo(L, "Sf", ar)) luaL_error(L, "hook debug info unavailable");
  lua_setfield(L, record, "func");
  lua_pushstring(L, ar->what);
  lua_setfield(L, record, "what");
  if (event == LUA_HOOKTAILRET && lua_getlocal(L, ar, 1) != NULL)
    luaL_error(L, "virtual tail frame exposes a local");
  if (lua_getstack(L, 1, &parent)) {
    lua_getinfo(L, "f", &parent);
    lua_setfield(L, record, "parent");
  }
  lua_rawseti(L, log, ++run->events);
  if (run->callbackref != LUA_NOREF) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, run->callbackref);
    lua_pushinteger(L, event);
    lua_call(L, 1, 0);
  }
  lua_pushliteral(L, "hook scratch");
  lua_pushnil(L);
}

static int run_hook(lua_State *L)
{
  HookRun run, *previous;
  lua_Hook oldhook = lua_gethook(L);
  int oldmask = lua_gethookmask(L), oldcount = lua_gethookcount(L);
  int status;
  luaL_checktype(L, 1, LUA_TFUNCTION);
  run.events = 0;
  run.callbackref = LUA_NOREF;
  if (lua_isfunction(L, 2)) {
    lua_pushvalue(L, 2);
    run.callbackref = luaL_ref(L, LUA_REGISTRYINDEX);
  }
  lua_newtable(L);
  run.logref = luaL_ref(L, LUA_REGISTRYINDEX);
  previous = context_get(L);
  context_set(L, &run);
  lua_sethook(L, native_hook, LUA_MASKCALL|LUA_MASKRET, 0);
  lua_pushvalue(L, 1);
  status = lua_pcall(L, 0, 1, 0);
  lua_sethook(L, oldhook, oldmask, oldcount);
  context_set(L, previous);
  lua_pushboolean(L, status == 0);
  lua_insert(L, -2);
  lua_rawgeti(L, LUA_REGISTRYINDEX, run.logref);
  luaL_unref(L, LUA_REGISTRYINDEX, run.logref);
  if (run.callbackref != LUA_NOREF)
    luaL_unref(L, LUA_REGISTRYINDEX, run.callbackref);
  return 3;
}

static void count_hook(lua_State *L, lua_Debug *ar)
{
  HookRun *run = context_get(L);
  if (!run) return;
  if (ar->event == LUA_HOOKCALL) run->calls++;
  else if (ar->event == LUA_HOOKRET) run->returns++;
  else if (ar->event == LUA_HOOKTAILRET) run->tails++;
}

/* Keep million-event tests bounded in memory and avoid compiling the hook. */
static int count_hooks(lua_State *L)
{
  HookRun run, *previous;
  lua_State *target = lua_isnoneornil(L, 3) ? L : lua_tothread(L, 3);
  lua_Hook oldhook;
  int oldmask, oldcount;
  int mask = luaL_optint(L, 2, LUA_MASKCALL|LUA_MASKRET);
  int status;
  luaL_checktype(L, 1, LUA_TFUNCTION);
  if (!target) return luaL_argerror(L, 3, "thread expected");
  oldhook = lua_gethook(target);
  oldmask = lua_gethookmask(target);
  oldcount = lua_gethookcount(target);
  run.calls = run.returns = run.tails = 0;
  previous = context_get(target);
  context_set(target, &run);
  lua_sethook(target, count_hook, mask, 0);
  lua_pushvalue(L, 1);
  status = lua_pcall(L, 0, 1, 0);
  lua_sethook(target, oldhook, oldmask, oldcount);
  context_set(target, previous);
  lua_pushboolean(L, status == 0);
  lua_insert(L, -2);
  lua_pushinteger(L, run.calls);
  lua_pushinteger(L, run.returns);
  lua_pushinteger(L, run.tails);
  return 5;
}

static int get_frame(lua_State *L)
{
  lua_Debug ar;
  if (!lua_getstack(L, luaL_checkint(L, 1), &ar)) {
    lua_pushnil(L);
    return 1;
  }
  if (!lua_getinfo(L, "Sf", &ar)) return luaL_error(L, "frame info unavailable");
  lua_pushstring(L, ar.what);
  lua_insert(L, -2);
  return 2;
}

#ifdef _WIN32
__declspec(dllexport)
#endif
int luaopen_hookprobe(lua_State *L)
{
  lua_newtable(L);
  lua_pushcfunction(L, run_hook);
  lua_setfield(L, -2, "run");
  lua_pushcfunction(L, count_hooks);
  lua_setfield(L, -2, "count");
  lua_pushcfunction(L, get_frame);
  lua_setfield(L, -2, "frame");
  return 1;
}

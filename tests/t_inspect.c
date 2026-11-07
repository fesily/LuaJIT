/*
** Callback stack inspection and coexistence with debug.sethook.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#include <stdlib.h>

#include "test.h"

static int hookcalls;

static void linehook(lua_State *L, lua_Debug *ar)
{
  (void)ar; (void)L;
  hookcalls++;
}

static void yieldhook(lua_State *L, lua_Debug *ar)
{
  (void)ar;
  lua_yield(L, 0);
}

/* The hook may walk the suspended stack with the non-allocating API. */
static void check_inspection(void)
{
  lua_State *L = luaL_newstate();
  lua_State *co;
  test_open(L);
  luaJIT_setcoroutinehook(L, test_cb_inspect, TEST_DATA_A);
  test_reset();

  co = test_newco(L, "body");
  lua_pushinteger(co, 1);
  lua_pushinteger(co, 2);
  TEST_CHECK(lua_resume(co, 2) == LUA_YIELD && lua_tointeger(co, -1) == 3);
  lua_pushinteger(co, 4);
  TEST_CHECK(lua_resume(co, 1) == LUA_YIELD);
  lua_pushinteger(co, 5);
  TEST_CHECK(lua_resume(co, 1) == LUA_OK);
  TEST_CHECK(test_inspect_calls() == 3);	/* One LEAVE per activation. */
  TEST_CHECK(test_inspect_frames() >= 3);	/* Frames were walkable. */

  /* Nested case: the Lua fast path gives a real parent thread. */
  co = test_newco(L, "nestedbody");
  lua_pushstring(co, "a");
  lua_pushstring(co, "b");
  TEST_CHECK(lua_resume(co, 2) == LUA_YIELD);
  TEST_CHECK(lua_resume(co, 1) == LUA_OK);
  TEST_CHECK(test_inspect_calls() == 7);
  TEST_CHECK(test_inspect_frames() >= 7);

  luaJIT_setcoroutinehook(L, NULL, NULL);
  lua_close(L);
}

/* debug.sethook keeps working with and without the coroutine hook. */
static void check_sethook(void)
{
  lua_State *L = luaL_newstate();
  lua_State *co;
  test_open(L);
  luaJIT_setcoroutinehook(L, test_cb, TEST_DATA_A);

  hookcalls = 0;
  test_reset();
  co = test_newco(L, "hotbody");
  lua_pushinteger(co, 3);
  lua_sethook(co, linehook, LUA_MASKLINE, 0);
  TEST_CHECK(lua_resume(co, 1) == LUA_YIELD);
  TEST_CHECK(hookcalls > 0);
  TEST_CHECK(test_nev == 2);
  lua_sethook(co, NULL, 0, 0);
  TEST_CHECK(lua_resume(co, 0) == LUA_OK);
  TEST_CHECK(test_nev == 4);

  /* A Lua-level hook that yields from a line event. */
  test_reset();
  co = lua_newthread(L);
  luaL_loadstring(co,
    "local s = 0 for i = 1, 3 do s = s + i end return s");
  lua_sethook(co, yieldhook, LUA_MASKLINE, 0);
  TEST_CHECK(lua_resume(co, 0) == LUA_YIELD);
  TEST_CHECK(test_nev == 2);
  TEST_CHECK(test_evlog[1].status == LUA_YIELD);
  lua_sethook(co, NULL, 0, 0);
  TEST_CHECK(lua_resume(co, 0) == LUA_OK);
  TEST_CHECK(lua_tointeger(co, -1) == 6);
  TEST_CHECK(test_nev == 4);
  TEST_CHECK(test_evlog[3].status == LUA_OK);

  /* debug.sethook() from Lua, with a resume inside the hooked region. */
  test_reset();
  co = lua_newthread(L);
  luaL_loadstring(co,
    "local n = 0\n"
    "debug.sethook(function() n = n + 1 end, '', 10)\n"
    "for i = 1, 100 do n = n + 1 end\n"
    "debug.sethook()\n"
    "assert(coroutine.resume(coroutine.create(function() return 1 end)))\n"
    "return n");
  TEST_CHECK(lua_resume(co, 0) == LUA_OK);
  TEST_CHECK(lua_tointeger(co, -1) > 10);
  TEST_CHECK(test_nev == 4);	/* Outer chunk + one inner activation. */

  luaJIT_setcoroutinehook(L, NULL, NULL);
  lua_close(L);
}

void test_case_inspect(void)
{
  check_inspection();
  check_sethook();
}

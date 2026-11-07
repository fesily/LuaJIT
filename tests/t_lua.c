/*
** Lua-level coroutine.resume()/coroutine.wrap(): events and results.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#include <stdlib.h>

#include "test.h"

static void check_resume_events(void)
{
  lua_State *L = luaL_newstate();
  lua_State *co;
  int i;
  test_open(L);
  luaJIT_setcoroutinehook(L, test_cb, TEST_DATA_A);
  test_reset();

  co = lua_newthread(L);
  luaL_loadstring(co,
    "local me = curth()\n"
    "local c = coroutine.create(body)\n"
    "local ok, v = coroutine.resume(c, 5, 6) assert(ok and v == 11)\n"
    "local ok2, a, b = coroutine.resume(c, 'x')\n"
    "assert(ok2 and a == 30 and b == 'x')\n"
    "local ok3, r1, r2, r3, r4, r5, r6 = coroutine.resume(c, 'y')\n"
    "assert(ok3 and r1 == 'done' and r2 == 5 and r3 == 6)\n"
    "assert(r4 == 'x' and r5 == 'y' and r6 == 42)\n"
    "return me");
  TEST_STATUS(lua_resume(co, 0) == LUA_OK, co);
  TEST_CHECK(lua_tothread(co, 1) == co);

  /* Outer C resume plus three inner activations. */
  TEST_CHECK(test_nev == 8);
  TEST_CHECK(test_evlog[0].event == LUAJIT_CORO_ENTER);
  TEST_CHECK(test_evlog[0].from == NULL && test_evlog[0].to == co);
  for (i = 1; i <= 5; i += 2) {
    /* The parent is the resuming Lua thread, not derived from cur_L. */
    TEST_CHECK(test_evlog[i].event == LUAJIT_CORO_ENTER);
    TEST_CHECK(test_evlog[i].from == co);
    TEST_CHECK(test_evlog[i].to != NULL && test_evlog[i].to != co);
    TEST_CHECK(test_evlog[i].status == LUA_OK);
    TEST_CHECK(test_evlog[i+1].event == LUAJIT_CORO_LEAVE);
    TEST_CHECK(test_evlog[i+1].from == test_evlog[i].to);
    TEST_CHECK(test_evlog[i+1].to == co);
  }
  TEST_CHECK(test_evlog[2].status == LUA_YIELD);
  TEST_CHECK(test_evlog[4].status == LUA_YIELD);
  TEST_CHECK(test_evlog[6].status == LUA_OK);
  TEST_CHECK(test_evlog[7].event == LUAJIT_CORO_LEAVE);
  TEST_CHECK(test_evlog[7].status == LUA_OK);

  lua_close(L);
}

static void check_resume_errors(void)
{
  lua_State *L = luaL_newstate();
  lua_State *co;
  test_open(L);
  luaJIT_setcoroutinehook(L, test_cb, TEST_DATA_A);
  test_reset();

  /* The success flag must be false and the error object must be preserved. */
  co = lua_newthread(L);
  luaL_loadstring(co,
    "obj = {tag = 'E'}\n"
    "local ok1, e1 = coroutine.resume(coroutine.create(function() error('boom', 0) end))\n"
    "local ok2, e2 = coroutine.resume(coroutine.create(function() error(obj, 0) end))\n"
    "local c3 = coroutine.create(function() coroutine.yield(7) return 8 end)\n"
    "local ok3, v3 = coroutine.resume(c3)\n"
    "local ok4, v4 = coroutine.resume(c3)\n"
    "return ok1, e1, ok2, e2, obj, ok3, v3, ok4, v4");
  TEST_STATUS(lua_resume(co, 0) == LUA_OK, co);
  TEST_CHECK(lua_toboolean(co, 1) == 0);
  TEST_CHECK(lua_isstring(co, 2));
  TEST_CHECK(strstr(lua_tostring(co, 2), "boom") != NULL);
  TEST_CHECK(lua_toboolean(co, 3) == 0);
  TEST_CHECK(lua_rawequal(co, 4, 5));
  TEST_CHECK(lua_toboolean(co, 6) == 1 && lua_tointeger(co, 7) == 7);
  TEST_CHECK(lua_toboolean(co, 8) == 1 && lua_tointeger(co, 9) == 8);
  TEST_CHECK(test_nev == 2 + 2*4);	/* Outer chunk + four activations. */
  TEST_CHECK(test_evlog[2].event == LUAJIT_CORO_LEAVE);
  TEST_CHECK(test_evlog[2].status == LUA_ERRRUN);

  lua_close(L);
}

static void check_wrap(void)
{
  lua_State *L = luaL_newstate();
  lua_State *co;
  test_open(L);
  luaJIT_setcoroutinehook(L, test_cb, TEST_DATA_A);

  /* Values, zero results and the final return value. */
  test_reset();
  co = lua_newthread(L);
  luaL_loadstring(co,
    "local w = coroutine.wrap(function(...)\n"
    "  coroutine.yield(...) return 'wr' end)\n"
    "local a, b, c = w(1, nil, 3)\n"
    "assert(a == 1 and b == nil and c == 3)\n"
    "assert(w() == 'wr')\n"
    "local e = coroutine.wrap(function() coroutine.yield() end)\n"
    "e() assert(select('#', e()) == 0)\n"
    "return 1");
  TEST_STATUS(lua_resume(co, 0) == LUA_OK, co);
  TEST_CHECK(test_nev == 2 + 2*4);
  TEST_CHECK(test_evlog[2].status == LUA_YIELD);
  TEST_CHECK(test_evlog[4].status == LUA_OK);

  /* Error rethrow keeps the error object. */
  test_reset();
  co = lua_newthread(L);
  luaL_loadstring(co,
    "obj = {}\n"
    "local w = coroutine.wrap(function() coroutine.yield()\n"
    "  error(obj, 0) end)\n"
    "w()\n"
    "local ok, e = pcall(w)\n"
    "assert(ok == false and e == obj)\n"
    "local ws = coroutine.wrap(function() error('wrap-err', 0) end)\n"
    "local ok2, e2 = pcall(ws)\n"
    "assert(ok2 == false and type(e2) == 'string')\n"
    "assert(e2:find('wrap-err', 1, true) ~= nil)\n"
    "return 1");
  TEST_STATUS(lua_resume(co, 0) == LUA_OK, co);
  TEST_CHECK(test_nev == 2 + 2*3);
  TEST_CHECK(test_evlog[4].status == LUA_ERRRUN);	/* First wrap error. */
  TEST_CHECK(test_evlog[6].status == LUA_ERRRUN);	/* Second wrap error. */

  lua_close(L);
}

static void check_rejected(void)
{
  lua_State *L = luaL_newstate();
  lua_State *co;
  test_open(L);
  luaJIT_setcoroutinehook(L, test_cb, TEST_DATA_A);
  test_reset();

  co = lua_newthread(L);
  luaL_loadstring(co,
    "local me = curth()\n"
    "local c = coroutine.create(function() return 1 end)\n"
    "assert(coroutine.resume(me) == false)\n"
    "assert(coroutine.resume(c) == true)\n"
    "assert(coroutine.resume(c) == false)\n"
    "assert(pcall(coroutine.resume, 42) == false)\n"
    "return 1");
  TEST_STATUS(lua_resume(co, 0) == LUA_OK, co);
  /* Only the accepted resume is a switch: outer + one inner activation. */
  TEST_CHECK(test_nev == 4);
  TEST_CHECK(test_evlog[2].event == LUAJIT_CORO_LEAVE);
  TEST_CHECK(test_evlog[2].status == LUA_OK);

  lua_close(L);
}

static void check_nested(void)
{
  lua_State *L = luaL_newstate();
  lua_State *co;
  test_open(L);
  luaJIT_setcoroutinehook(L, test_cb, TEST_DATA_A);

  test_reset();
  co = test_newco(L, "nestedbody");
  lua_pushstring(co, "a");
  lua_pushstring(co, "b");
  TEST_CHECK(lua_resume(co, 2) == LUA_YIELD);
  /* ENTER(outer), ENTER(inner), LEAVE(inner), LEAVE(outer). */
  TEST_CHECK(test_nev == 4);
  TEST_CHECK(test_evlog[0].to == co && test_evlog[0].from == NULL);
  TEST_CHECK(test_evlog[1].event == LUAJIT_CORO_ENTER);
  TEST_CHECK(test_evlog[1].from == co);
  TEST_CHECK(test_evlog[2].event == LUAJIT_CORO_LEAVE);
  TEST_CHECK(test_evlog[2].from == test_evlog[1].to);
  TEST_CHECK(test_evlog[2].to == co && test_evlog[2].status == LUA_YIELD);
  TEST_CHECK(test_evlog[3].from == co && test_evlog[3].to == NULL);
  TEST_CHECK(test_evlog[3].status == LUA_YIELD);

  test_reset();
  TEST_CHECK(lua_resume(co, 1) == LUA_OK);
  TEST_CHECK(lua_gettop(co) == 3);
  TEST_CHECK(test_nev == 4);
  TEST_CHECK(test_evlog[2].status == LUA_OK);
  TEST_CHECK(test_evlog[3].event == LUAJIT_CORO_LEAVE);
  TEST_CHECK(test_evlog[3].status == LUA_OK);

  lua_close(L);
}

static void check_registration_midflight(void)
{
  lua_State *L = luaL_newstate();
  lua_State *co;
  test_open(L);

  /* Fast path entered while unregistered: no boundary on its return. */
  luaJIT_setcoroutinehook(L, NULL, NULL);
  test_reset();
  co = lua_newthread(L);
  luaL_loadstring(co,
    "local c = coroutine.create(function() register_from_c(1) return 1 end)\n"
    "return coroutine.resume(c)");
  TEST_CHECK(lua_resume(co, 0) == LUA_OK);
  TEST_CHECK(test_nev == 1);	/* Only the C API LEAVE after re-registering. */
  TEST_CHECK(test_evlog[0].event == LUAJIT_CORO_LEAVE);
  TEST_CHECK(test_evlog[0].data == TEST_DATA_B);

  /* The next activation is instrumented from the start. */
  test_reset();
  co = lua_newthread(L);
  luaL_loadstring(co,
    "local c = coroutine.create(function() return 1 end)\n"
    "local ok, v = coroutine.resume(c)\n"
    "assert(ok and v == 1)\n"
    "return 1");
  TEST_CHECK(lua_resume(co, 0) == LUA_OK);
  TEST_CHECK(test_nev == 4);
  TEST_CHECK(test_evlog[0].event == LUAJIT_CORO_ENTER);
  TEST_CHECK(test_evlog[3].event == LUAJIT_CORO_LEAVE);
  TEST_CHECK(test_evlog[3].status == LUA_OK);

  luaJIT_setcoroutinehook(L, NULL, NULL);
  lua_close(L);
}

void test_case_lua(void)
{
  check_resume_events();
  check_resume_errors();
  check_wrap();
  check_rejected();
  check_nested();
  check_registration_midflight();
}

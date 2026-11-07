/*
** Many resumes, large stacks, GC rooting and the JIT modes.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#include <stdio.h>
#include <stdlib.h>

#include "test.h"

#define STRESS_ITERS	200000

/* A coroutine that is resumed many times must not grow its stack. */
static void check_many_resumes(void)
{
  lua_State *L = luaL_newstate();
  lua_State *co;
  char buf[256];
  long size0;
  int i;

  test_open(L);
  luaJIT_setcoroutinehook(L, test_cb, TEST_DATA_A);

  /* C API driver. */
  test_reset();
  co = test_newco(L, "spinbody");
  lua_pushinteger(co, STRESS_ITERS);	/* Yields, then the return. */
  size0 = test_stacksize(co);
  for (i = 0; i < STRESS_ITERS; i++) {
    int status = lua_resume(co, i == 0 ? 1 : 0);
    if (status != LUA_YIELD) { TEST_CHECK(status == LUA_YIELD); break; }
  }
  TEST_CHECK(lua_resume(co, 0) == LUA_OK);
  TEST_CHECK(strcmp(lua_tostring(co, -1), "spin-done") == 0);
  TEST_CHECK(test_stacksize(co) == size0);
  TEST_CHECK(test_nev == 2*(STRESS_ITERS+1));

  /* Lua fast path driver. */
  test_reset();
  snprintf(buf, sizeof(buf),
    "local c = coroutine.create(spinbody)\n"
    "assert(coroutine.resume(c, %d))\n"
    "for i = 1, %d do assert(coroutine.resume(c)) end\n"
    "return 1", STRESS_ITERS, STRESS_ITERS);
  co = lua_newthread(L);
  luaL_loadstring(co, buf);
  TEST_CHECK(lua_resume(co, 0) == LUA_OK);
  /* Yields, the return and the outer chunk. */
  TEST_CHECK(test_nev == 2*(STRESS_ITERS+2));

  /* coroutine.wrap driver. */
  test_reset();
  snprintf(buf, sizeof(buf),
    "local w = coroutine.wrap(function()\n"
    "  for i = 1, %d do coroutine.yield(i) end end)\n"
    "for i = 1, %d do assert(w()) end\n"
    "w()\n"
    "return 1", STRESS_ITERS, STRESS_ITERS);
  co = lua_newthread(L);
  luaL_loadstring(co, buf);
  TEST_CHECK(lua_resume(co, 0) == LUA_OK);
  TEST_CHECK(test_nev == 2*(STRESS_ITERS+2));

  luaJIT_setcoroutinehook(L, NULL, NULL);
  lua_close(L);
}

/* Large argument and result sets force stack relocation on both threads. */
static void check_big_results(void)
{
  lua_State *L = luaL_newstate();
  lua_State *co;
  int n = 800;
  test_open(L);
  luaJIT_setcoroutinehook(L, test_cb, TEST_DATA_A);

  test_reset();
  co = test_newco(L, "bigbody");
  lua_pushinteger(co, n);
  TEST_CHECK(lua_resume(co, 1) == LUA_YIELD);
  TEST_CHECK(lua_gettop(co) == n);
  TEST_CHECK(lua_tointeger(co, 1) == 2 && lua_tointeger(co, n) == 2*n);
  /* Results stay rooted across a collection between activations. */
  lua_gc(L, LUA_GCCOLLECT, 0);
  TEST_CHECK(lua_resume(co, 0) == LUA_OK);
  TEST_CHECK(lua_gettop(co) == n);
  TEST_CHECK(strcmp(lua_tostring(co, n), "x800") == 0);

  /* Many arguments through the Lua fast path. */
  test_reset();
  co = lua_newthread(L);
  luaL_loadstring(co,
    "local c = coroutine.create(manyargs) local args = {}\n"
    "for i = 1, 600 do args[i] = i end\n"
    "local ok, cnt, v = coroutine.resume(c, 600, unpack(args))\n"
    "assert(ok and cnt == 600 and v == 600)\n"
    "local ok2, n2, first = coroutine.resume(c) assert(ok2)\n"
    "assert(n2 == 600 and first == 1)\n"
    "return 1");
  TEST_CHECK(lua_resume(co, 0) == LUA_OK);
  TEST_CHECK(test_nev == 6);
  TEST_CHECK(test_evlog[2].status == LUA_YIELD);

  lua_close(L);
}

/* Garbage created by the coroutine must survive across yields. */
static void check_gc_rooting(void)
{
  lua_State *L = luaL_newstate();
  lua_State *co;
  test_open(L);
  luaJIT_setcoroutinehook(L, test_cb, TEST_DATA_A);

  test_reset();
  co = test_newco(L, "gcbody");
  lua_pushinteger(co, 3);
  TEST_CHECK(lua_resume(co, 1) == LUA_YIELD && lua_tointeger(co, -1) == 1);
  lua_gc(L, LUA_GCCOLLECT, 0);
  TEST_CHECK(lua_resume(co, 0) == LUA_YIELD && lua_tointeger(co, -1) == 2);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCCOLLECT, 0);
  TEST_CHECK(lua_resume(co, 0) == LUA_YIELD && lua_tointeger(co, -1) == 3);
  TEST_CHECK(lua_resume(co, 0) == LUA_OK);
  TEST_CHECK(strcmp(lua_tostring(co, -1), "gc-done") == 0);
  TEST_CHECK(test_nev == 8);

  lua_close(L);
}

/* The gate must work with the JIT on and off, and for compiled traces. */
static void check_jit_modes(void)
{
  lua_State *L = luaL_newstate();
  lua_State *co;
  int mode;
  test_open(L);

  for (mode = 0; mode < 2; mode++) {
    int n = mode ? 2000 : 200;
    luaJIT_setmode(L, 0, LUAJIT_MODE_ENGINE |
		   (mode ? LUAJIT_MODE_ON : LUAJIT_MODE_OFF));
    luaJIT_setcoroutinehook(L, test_cb, TEST_DATA_A);
    test_reset();
    co = test_newco(L, "hotbody");
    lua_pushinteger(co, n);
    TEST_CHECK(lua_resume(co, 1) == LUA_YIELD);
    TEST_CHECK(lua_tointeger(co, -1) == test_modsum(n, 7));
    TEST_CHECK(lua_resume(co, 0) == LUA_OK);
    TEST_CHECK(lua_tointeger(co, -1) ==
	       test_modsum(n, 7) + test_modsum(n, 11));
    TEST_CHECK(test_nev == 4);
    TEST_CHECK(test_evlog[3].status == LUA_OK);
  }

  luaJIT_setmode(L, 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_ON);
  test_reset();
  co = lua_newthread(L);
  luaL_loadstring(co,
    "local c = coroutine.create(hotbody)\n"
    "local ok, s = coroutine.resume(c, 5000)\n"
    "local ok2, r = coroutine.resume(c)\n"
    "return ok, s, ok2, r");
  TEST_CHECK(lua_resume(co, 0) == LUA_OK);
  TEST_CHECK(lua_tointeger(co, 2) == test_modsum(5000, 7));
  TEST_CHECK(lua_tointeger(co, 4) ==
	     test_modsum(5000, 7) + test_modsum(5000, 11));
  TEST_CHECK(test_nev == 6);
  TEST_CHECK(test_evlog[5].event == LUAJIT_CORO_LEAVE);
  TEST_CHECK(test_evlog[5].status == LUA_OK);

  luaJIT_setcoroutinehook(L, NULL, NULL);
  lua_close(L);
}

void test_case_stress(void)
{
  check_many_resumes();
  check_big_results();
  check_gc_rooting();
  check_jit_modes();
}

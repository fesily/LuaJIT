/*
** Coroutine behaviour without a hook: must be unaffected by the extension.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#include <stdlib.h>

#include "test.h"

/* The coroutine fast path is only instrumented when a hook is registered, so
** this case covers the uninstrumented path (or the upstream code, when the
** extension is compiled out).
*/

static void check_lua_resume(void)
{
  lua_State *L = luaL_newstate();
  test_open(L);

  /* Plain resume/wrap, yield/return values, result counts. */
  TEST_DOSTRING(L,
    "local c = coroutine.create(body)\n"
    "local ok, v = coroutine.resume(c, 3, 4)\n"
    "assert(ok == true and v == 7)\n"
    "local ok2, a, b = coroutine.resume(c, 10)\n"
    "assert(ok2 == true and a == 12 and b == 10)\n"
    "local ok3, r1, r2, r3, r4, r5, r6 = coroutine.resume(c, 20)\n"
    "assert(ok3 == true and r1 == 'done' and r6 == 42)\n"
    "assert(coroutine.resume(c) == false)\n"
    "local w = coroutine.wrap(function(...) coroutine.yield(...) return 'wr' end)\n"
    "assert(select('#', w(1, nil, 3)) == 3)\n"
    "assert(w() == 'wr')\n"
    "local e = coroutine.wrap(function() coroutine.yield() end)\n"
    "e() assert(select('#', e()) == 0)\n"
    "return 1");

  /* Success flag and error object identity for a raising coroutine. */
  TEST_DOSTRING(L,
    "obj = {tag = 'E'}\n"
    "local ok1, e1 = coroutine.resume(coroutine.create(function() error('boom', 0) end))\n"
    "assert(ok1 == false and type(e1) == 'string')\n"
    "local ok2, e2 = coroutine.resume(coroutine.create(function() error(obj, 0) end))\n"
    "assert(ok2 == false and e2 == obj)\n"
    "local ok3, e3 = coroutine.resume(coroutine.create(function() return trap[1] end))\n"
    "assert(ok3 == false)\n"
    "local w = coroutine.wrap(function() error('wrap-err', 0) end)\n"
    "local ok4, e4 = pcall(w)\n"
    "assert(ok4 == false and type(e4) == 'string')\n"
    "local ok5, e5 = pcall(coroutine.wrap(function() error(obj, 0) end))\n"
    "assert(ok5 == false and e5 == obj)\n"
    "return 1");

  /* C yields: the yielded values go out, the resumed C function returns the
  ** resume arguments (none here).
  */
  TEST_DOSTRING(L,
    "local c = coroutine.create(function()\n"
    "  local a = cyield1() local b = cyield2() return a, b end)\n"
    "local ok, v = coroutine.resume(c)\n"
    "assert(ok and v == 'cyield')\n"
    "local ok2, v2 = coroutine.resume(c)\n"
    "assert(ok2 and v2 == 7)\n"
    "local ok3, a, b = coroutine.resume(c)\n"
    "assert(ok3 and a == nil and b == nil)\n"
    "return 1");

  /* Rejected resumes. coroutine.running() is nil on the main thread (5.1
  ** semantics), so this has to run inside a coroutine.
  */
  {
    lua_State *co = lua_newthread(L);
    luaL_loadstring(co,
      "local me = curth()\n"
      "local c = coroutine.create(function() return 1 end)\n"
      "assert(coroutine.resume(me) == false)\n"
      "assert(coroutine.resume(c) == true)\n"
      "assert(coroutine.resume(c) == false)\n"
      "assert(pcall(coroutine.resume, 42) == false)\n"
      "return 1");
    TEST_STATUS(lua_resume(co, 0) == LUA_OK, co);
  }

  /* Nested resumes. */
  TEST_DOSTRING(L,
    "local c = coroutine.create(nestedbody)\n"
    "local ok, v = coroutine.resume(c, 'a', 'b')\n"
    "assert(ok and v == 'inner:true:in:ner')\n"
    "local ok2, tag, ok3, v2 = coroutine.resume(c)\n"
    "assert(ok2 and tag == 'outer' and ok3 == true and v2 == 'inner-ret')\n"
    "return 1");

  lua_close(L);
}

static void check_c_api(void)
{
  lua_State *L = luaL_newstate();
  lua_State *co;
  test_open(L);

  co = test_newco(L, "body");
  lua_pushinteger(co, 3);
  lua_pushinteger(co, 4);
  TEST_CHECK(lua_resume(co, 2) == LUA_YIELD);
  TEST_CHECK(lua_gettop(co) == 1 && lua_tointeger(co, -1) == 7);
  lua_pushinteger(co, 10);
  TEST_CHECK(lua_resume(co, 1) == LUA_YIELD);
  TEST_CHECK(lua_gettop(co) == 2);
  TEST_CHECK(lua_tointeger(co, -2) == 12 && lua_tointeger(co, -1) == 10);
  lua_pushinteger(co, 20);
  TEST_CHECK(lua_resume(co, 1) == LUA_OK);
  TEST_CHECK(lua_gettop(co) == 6);
  TEST_CHECK(strcmp(lua_tostring(co, 1), "done") == 0);
  TEST_CHECK(lua_tointeger(co, 6) == 42);
  /* Dead coroutine. */
  TEST_CHECK(lua_resume(co, 0) == LUA_ERRRUN);

  /* Zero arguments and zero results. */
  co = test_newco(L, "zerobody");
  TEST_CHECK(lua_resume(co, 0) == LUA_YIELD && lua_gettop(co) == 0);
  TEST_CHECK(lua_resume(co, 0) == LUA_OK && lua_gettop(co) == 0);

  /* Error status and error object. */
  co = test_newco(L, "errbody");
  TEST_CHECK(lua_resume(co, 0) == LUA_YIELD);
  TEST_CHECK(lua_resume(co, 0) == LUA_ERRRUN);
  TEST_CHECK(strcmp(lua_tostring(co, -1), "boom") == 0);

  lua_close(L);
}

#if TEST_HAS_COROHOOK
/* With the extension compiled in but never registered, behaviour must be
** identical and no notification may be recorded.
*/
static void check_unregistered(void)
{
  lua_State *L = luaL_newstate();
  lua_State *co;
  test_open(L);
  test_reset();
  co = test_newco(L, "body");
  lua_pushinteger(co, 1);
  lua_pushinteger(co, 2);
  TEST_CHECK(lua_resume(co, 2) == LUA_YIELD && lua_tointeger(co, -1) == 3);
  TEST_CHECK(lua_resume(co, 0) == LUA_YIELD);
  TEST_CHECK(lua_resume(co, 0) == LUA_OK && lua_gettop(co) == 6);
  TEST_CHECK(test_nev == 0);
  TEST_DOSTRING(L,
    "local c = coroutine.create(body)\n"
    "local ok, v = coroutine.resume(c, 5, 6)\n"
    "assert(ok and v == 11)\n"
    "assert(coroutine.resume(c, 'x'))\n"
    "assert(coroutine.resume(c, 'y'))\n"
    "return 1");
  TEST_CHECK(test_nev == 0);
  lua_close(L);
}
#endif

void test_case_plain(void)
{
  check_lua_resume();
  check_c_api();
#if TEST_HAS_COROHOOK
  check_unregistered();
#endif
}

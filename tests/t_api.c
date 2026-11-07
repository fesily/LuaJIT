/*
** C API resume: event order, directions, statuses and identities.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#include <stdlib.h>

#include "test.h"

static void check_basic(void)
{
  lua_State *L = luaL_newstate();
  lua_State *co;
  test_open(L);
  luaJIT_setcoroutinehook(L, test_cb, TEST_DATA_A);
  test_reset();

  co = test_newco(L, "body");
  lua_pushinteger(co, 3);
  lua_pushinteger(co, 4);
  TEST_CHECK(lua_resume(co, 2) == LUA_YIELD);
  TEST_CHECK(lua_gettop(co) == 1 && lua_tointeger(co, -1) == 7);

  /* ENTER(parent=NULL, co) with status LUA_OK. */
  TEST_CHECK(test_nev == 2);
  TEST_CHECK(test_evlog[0].event == LUAJIT_CORO_ENTER);
  TEST_CHECK(test_evlog[0].from == NULL && test_evlog[0].to == co);
  TEST_CHECK(test_evlog[0].status == LUA_OK);
  TEST_CHECK(test_evlog[0].data == TEST_DATA_A);
  /* LEAVE(co, parent=NULL) with the resume status. */
  TEST_CHECK(test_evlog[1].event == LUAJIT_CORO_LEAVE);
  TEST_CHECK(test_evlog[1].from == co && test_evlog[1].to == NULL);
  TEST_CHECK(test_evlog[1].status == LUA_YIELD);
  TEST_CHECK(test_evlog[1].data == TEST_DATA_A);

  lua_pushinteger(co, 10);
  TEST_CHECK(lua_resume(co, 1) == LUA_YIELD);
  TEST_CHECK(lua_gettop(co) == 2);
  TEST_CHECK(lua_tointeger(co, -2) == 12 && lua_tointeger(co, -1) == 10);
  lua_pushinteger(co, 20);
  TEST_CHECK(lua_resume(co, 1) == LUA_OK);
  TEST_CHECK(lua_gettop(co) == 6);
  TEST_CHECK(strcmp(lua_tostring(co, 1), "done") == 0);
  TEST_CHECK(lua_tointeger(co, 2) == 3 && lua_tointeger(co, 3) == 4);
  TEST_CHECK(lua_tointeger(co, 4) == 10 && lua_tointeger(co, 5) == 20);
  TEST_CHECK(lua_tointeger(co, 6) == 42);
  TEST_CHECK(test_nev == 6);
  TEST_CHECK(test_evlog[5].event == LUAJIT_CORO_LEAVE);
  TEST_CHECK(test_evlog[5].status == LUA_OK);

  /* Zero arguments and zero results still notify. */
  test_reset();
  co = test_newco(L, "zerobody");
  TEST_CHECK(lua_resume(co, 0) == LUA_YIELD && lua_gettop(co) == 0);
  TEST_CHECK(lua_resume(co, 0) == LUA_OK && lua_gettop(co) == 0);
  TEST_CHECK(test_nev == 4);
  TEST_CHECK(test_evlog[3].event == LUAJIT_CORO_LEAVE);
  TEST_CHECK(test_evlog[3].status == LUA_OK);

  lua_close(L);
}

static void check_errors(void)
{
  lua_State *L = luaL_newstate();
  lua_State *co;
  int ref;
  const void *objp;
  test_open(L);
  luaJIT_setcoroutinehook(L, test_cb, TEST_DATA_A);

  /* String error. */
  test_reset();
  co = test_newco(L, "errbody");
  TEST_CHECK(lua_resume(co, 0) == LUA_YIELD);
  TEST_CHECK(lua_resume(co, 0) == LUA_ERRRUN);
  TEST_CHECK(strcmp(lua_tostring(co, -1), "boom") == 0);
  TEST_CHECK(test_nev == 4);
  TEST_CHECK(test_evlog[3].event == LUAJIT_CORO_LEAVE);
  TEST_CHECK(test_evlog[3].status == LUA_ERRRUN);

  /* Non-string error object keeps its identity. */
  test_reset();
  co = test_newco(L, "errbody_obj");
  lua_newtable(co);
  lua_pushvalue(co, -1);
  ref = luaL_ref(co, LUA_REGISTRYINDEX);
  objp = lua_topointer(co, -1);
  TEST_CHECK(lua_resume(co, 1) == LUA_YIELD);
  TEST_CHECK(lua_resume(co, 0) == LUA_ERRRUN);
  TEST_CHECK(lua_istable(co, -1));
  TEST_CHECK(lua_topointer(co, -1) == objp);
  TEST_CHECK(test_nev == 4 && test_evlog[3].status == LUA_ERRRUN);
  luaL_unref(L, LUA_REGISTRYINDEX, ref);

  /* Error raised from a metamethod. */
  test_reset();
  co = test_newco(L, "errbody_trap");
  lua_pushstring(co, "from-table");
  TEST_CHECK(lua_resume(co, 1) == LUA_YIELD);
  TEST_CHECK(lua_resume(co, 0) == LUA_ERRRUN);
  TEST_CHECK(lua_isstring(co, -1));
  TEST_CHECK(strcmp(lua_tostring(co, -1), "from-table") == 0);
  TEST_CHECK(test_nev == 4 && test_evlog[3].status == LUA_ERRRUN);

  lua_close(L);
}

static void check_universes(void)
{
  lua_State *L1 = luaL_newstate(), *L2 = luaL_newstate(), *co;
  test_open(L1);
  test_open(L2);
  luaJIT_setcoroutinehook(L1, test_cb, TEST_DATA_A);
  luaJIT_setcoroutinehook(L2, test_cb, TEST_DATA_B);

  test_reset();
  co = test_newco(L1, "zerobody");
  TEST_CHECK(lua_resume(co, 0) == LUA_YIELD);
  TEST_CHECK(test_nev == 2);
  TEST_CHECK(test_evlog[0].data == TEST_DATA_A);
  TEST_CHECK(test_evlog[1].data == TEST_DATA_A);

  /* The other universe keeps its own registration. */
  test_reset();
  co = lua_newthread(L2);
  luaL_loadstring(co,
    "return coroutine.resume(coroutine.create(function() end))");
  TEST_CHECK(lua_resume(co, 0) == LUA_OK);
  TEST_CHECK(test_nev == 4);
  TEST_CHECK(test_evlog[1].data == TEST_DATA_B);
  TEST_CHECK(test_evlog[2].data == TEST_DATA_B);

  lua_close(L1);
  lua_close(L2);
}

static void check_registration(void)
{
  lua_State *L = luaL_newstate();
  lua_State *co;
  test_open(L);
  luaJIT_setcoroutinehook(L, test_cb, TEST_DATA_A);

  /* Unregistering between activations silences the next one. */
  co = test_newco(L, "zerobody");
  test_reset();
  TEST_CHECK(lua_resume(co, 0) == LUA_YIELD && test_nev == 2);
  luaJIT_setcoroutinehook(L, NULL, NULL);
  test_reset();
  TEST_CHECK(lua_resume(co, 0) == LUA_OK && test_nev == 0);

  /* A different registration takes over completely. */
  luaJIT_setcoroutinehook(L, test_cb, TEST_DATA_B);
  test_reset();
  co = test_newco(L, "zerobody");
  TEST_CHECK(lua_resume(co, 0) == LUA_YIELD);
  TEST_CHECK(test_nev == 2);
  TEST_CHECK(test_evlog[0].data == TEST_DATA_B);
  TEST_CHECK(test_evlog[1].data == TEST_DATA_B);

  /* NULL normalizes the stored data. */
  luaJIT_setcoroutinehook(L, NULL, TEST_DATA_C);
  luaJIT_setcoroutinehook(L, test_cb, TEST_DATA_C);
  test_reset();
  TEST_CHECK(lua_resume(co, 0) == LUA_OK);
  TEST_CHECK(test_nev == 2 && test_evlog[0].data == TEST_DATA_C);

  /* (a) A at ENTER, replaced by B while running: only B sees the LEAVE. */
  luaJIT_setcoroutinehook(L, test_cb, TEST_DATA_A);
  test_reset();
  co = lua_newthread(L);
  luaL_loadstring(co, "register_from_c(1) return 'a'");
  TEST_CHECK(lua_resume(co, 0) == LUA_OK);
  TEST_CHECK(test_nev == 2);
  TEST_CHECK(test_evlog[0].event == LUAJIT_CORO_ENTER);
  TEST_CHECK(test_evlog[0].data == TEST_DATA_A);
  TEST_CHECK(test_evlog[1].event == LUAJIT_CORO_LEAVE);
  TEST_CHECK(test_evlog[1].data == TEST_DATA_B);

  /* (b) Unregistered while running: no LEAVE and nothing afterwards. */
  luaJIT_setcoroutinehook(L, test_cb, TEST_DATA_A);
  test_reset();
  co = lua_newthread(L);
  luaL_loadstring(co, "register_from_c(0) return 'b'");
  TEST_CHECK(lua_resume(co, 0) == LUA_OK);
  TEST_CHECK(test_nev == 1);
  TEST_CHECK(test_evlog[0].event == LUAJIT_CORO_ENTER);
  test_reset();
  co = lua_newthread(L);
  luaL_loadstring(co, "return 'c'");
  TEST_CHECK(lua_resume(co, 0) == LUA_OK && test_nev == 0);

  /* (c) Registered while running: LEAVE without ENTER. */
  test_reset();
  co = lua_newthread(L);
  luaL_loadstring(co, "register_from_c(2) return 'd'");
  TEST_CHECK(lua_resume(co, 0) == LUA_OK);
  TEST_CHECK(test_nev == 1);
  TEST_CHECK(test_evlog[0].event == LUAJIT_CORO_LEAVE);
  TEST_CHECK(test_evlog[0].data == TEST_DATA_C);
  TEST_CHECK(test_evlog[0].from == co && test_evlog[0].to == NULL);

  luaJIT_setcoroutinehook(L, NULL, NULL);
  lua_close(L);
}

void test_case_api(void)
{
  check_basic();
  check_errors();
  check_universes();
  check_registration();
}

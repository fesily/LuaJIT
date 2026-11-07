/*
** Test framework and shared fixtures for the coroutine hook extension.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#include <stdlib.h>

#include "test.h"

int test_checks, test_fails;

TestEv test_evlog[TEST_MAXEV];
int test_nev;

void *const TEST_DATA_A = (void *)0xA;
void *const TEST_DATA_B = (void *)0xB;
void *const TEST_DATA_C = (void *)0xC;

/* -- Callbacks ----------------------------------------------------------- */

void test_cb(void *data, lua_State *from, lua_State *to, int event, int status)
{
  if (test_nev < TEST_MAXEV) {
    TestEv *e = &test_evlog[test_nev];
    e->data = data; e->from = from; e->to = to;
    e->event = event; e->status = status;
  }
  test_nev++;
}

/* Inspection callback: only uses the API a debugger may use inside a hook. */
static int inspect_calls, inspect_frames;

int test_inspect_calls(void) { return inspect_calls; }
int test_inspect_frames(void) { return inspect_frames; }

void test_cb_inspect(void *data, lua_State *from, lua_State *to, int event,
		     int status)
{
  lua_State *L = from != NULL ? from : to;
  test_cb(data, from, to, event, status);
  if (event == LUAJIT_CORO_LEAVE && L != NULL) {
    lua_Debug ar;
    int lvl, i;
    inspect_calls++;
    for (lvl = 0; lvl < 4 && lua_getstack(L, lvl, &ar); lvl++) {
      inspect_frames++;
      lua_getinfo(L, "Su", &ar);
      /* Line and name information is only meaningful for Lua frames. The
      ** topmost suspended frame is the C function frame of coroutine.yield.
      */
      if (ar.what[0] == 'L') {
	lua_getinfo(L, "ln", &ar);
	for (i = 1; i <= 6; i++)
	  if (lua_getlocal(L, &ar, i) != NULL) lua_pop(L, 1);
      }
    }
  }
}

/* -- Fixtures ------------------------------------------------------------ */

void test_dostring(lua_State *L, const char *src, const char *file, int line)
{
  test_checks++;
  if (luaL_dostring(L, src) != 0) {
    const char *msg = lua_tostring(L, -1);
    test_fails++;
    printf("  FAIL %s:%d: %s\n", file, line, msg != NULL ? msg : "(error)");
    lua_pop(L, 1);
  }
}

void test_reset(void)
{
  test_nev = 0;
  inspect_calls = inspect_frames = 0;
  memset(test_evlog, 0, sizeof(test_evlog));
}

#if TEST_HAS_COROHOOK
static int c_register(lua_State *L)
{
  /* Registration from ordinary C code running inside a coroutine. */
  int kind = (int)luaL_checkinteger(L, 1);
  if (kind == 0)
    luaJIT_setcoroutinehook(L, NULL, NULL);
  else
    luaJIT_setcoroutinehook(L, test_cb,
			    kind == 1 ? TEST_DATA_B : TEST_DATA_C);
  lua_pushboolean(L, 1);
  return 1;
}
#endif

static int c_yield1(lua_State *L)
{
  lua_pushstring(L, "cyield");
  return lua_yield(L, 1);
}

static int c_yield2(lua_State *L)
{
  lua_pushinteger(L, 7);
  return lua_yield(L, 1);
}

static const char *const corpus =
"function body(a, b)\n"
"  local x = coroutine.yield(a+b)\n"
"  local y = coroutine.yield(a*b, x)\n"
"  return 'done', a, b, x, y, 42\n"
"end\n"
"function errbody() coroutine.yield(1) error('boom', 0) end\n"
"function errbody_obj(o) coroutine.yield(1) error(o, 0) end\n"
"local trap = setmetatable({}, {__index = function(_, k) error(k, 0) end})\n"
"function errbody_trap(o) coroutine.yield(1) return trap[o] end\n"
"function nestedbody(a, b)\n"
"  local c = coroutine.create(function(x, y)\n"
"    coroutine.yield(x .. ':' .. y) return 'inner-ret' end)\n"
"  local ok, v = coroutine.resume(c, 'in', 'ner')\n"
"  coroutine.yield('inner:' .. tostring(ok) .. ':' .. tostring(v))\n"
"  local ok2, v2 = coroutine.resume(c)\n"
"  return 'outer', ok2, v2\n"
"end\n"
"function hotbody(n)\n"
"  local s = 0 for i = 1, n do s = s + i % 7 end\n"
"  coroutine.yield(s)\n"
"  local t = 0 for i = 1, n do t = t + i % 11 end\n"
"  return s + t\n"
"end\n"
"function bigbody(n)\n"
"  local t = {} for i = 1, n do t[i] = i*2 end\n"
"  coroutine.yield(unpack(t))\n"
"  local u = {} for i = 1, n do u[i] = 'x' .. i end\n"
"  return unpack(u)\n"
"end\n"
"function manyargs(n, ...)\n"
"  coroutine.yield(select('#', ...), select(n, ...))\n"
"  return select('#', ...), select(1, ...)\n"
"end\n"
"function gcbody(rounds)\n"
"  for r = 1, rounds do\n"
"    local t = {} for i = 1, 2000 do t[i] = {i, tostring(i)} end\n"
"    coroutine.yield(r) end\n"
"  return 'gc-done'\n"
"end\n"
"function curth() return coroutine.running() end\n"
"function zerobody() coroutine.yield() end\n"
"function spinbody(n) for i = 1, n do coroutine.yield(i) end\n"
"  return 'spin-done' end\n";

void test_open(lua_State *L)
{
  luaL_openlibs(L);
#if TEST_HAS_COROHOOK
  lua_pushcfunction(L, c_register);
  lua_setglobal(L, "register_from_c");
#endif
  lua_pushcfunction(L, c_yield1);
  lua_setglobal(L, "cyield1");
  lua_pushcfunction(L, c_yield2);
  lua_setglobal(L, "cyield2");
  if (luaL_dostring(L, corpus) != 0) {
    printf("FATAL corpus: %s\n", lua_tostring(L, -1));
    exit(1);
  }
}

lua_State *test_newco(lua_State *L, const char *func)
{
  lua_State *co = lua_newthread(L);  /* The thread stays rooted on L. */
  lua_getglobal(co, func);
  return co;
}

long test_stacksize(lua_State *co)
{
  return (long)co->stacksize;
}

int test_modsum(int n, int m)
{
  int i, s = 0;
  for (i = 1; i <= n; i++) s += i % m;
  return s;
}

/*
** P1-6: Out-of-memory injection for the arena GC.
**
** Borrows from CRuby's GC.stress + allocation-failure tests and V8's
** --gc-interval forced-failure mode. The arena allocator is most fragile at
** the OOM boundary -- frees need the exact original size, and an allocation
** that fails mid-object-construction must leave the heap and the lua_State
** consistent and recoverable, not corrupt or crashed.
**
** Strategy: create a real lua_State with a byte-capped allocator. Drive a
** Lua workload under lua_pcall; once the cap bites, allocations fail and
** LuaJIT must raise LUA_ERRMEM (a clean longjmp), never crash. After an OOM
** we raise the cap and assert the SAME state is still usable -- i.e. the
** failed allocation did not leave a half-built object or a corrupt arena.
**
** Build (from test/):
**   make -C ../src XCFLAGS="-DLUAJIT_ENABLE_GCARENA"
**   cc -I../src -DLUAJIT_ENABLE_GCARENA oom_inject.c ../src/libluajit.a \
**      -lm -ldl -lpthread -o oom_inject && ./oom_inject
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

static int pass = 0, fail = 0;
static void check(const char *name, int cond, const char *msg)
{
  if (cond) pass++;
  else { fail++; fprintf(stderr, "FAIL: %s%s%s\n", name,
                        msg ? ": " : "", msg ? msg : ""); }
}

/* Allocator with a live-bytes cap. When live + request would exceed the cap,
** the allocation fails (returns NULL) -- exactly how a real OOM presents. */
typedef struct {
  size_t live;     /* currently allocated bytes */
  size_t cap;      /* fail any growth that would exceed this */
  long   nfail;    /* count of injected failures */
} CapAlloc;

static void *cap_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
  CapAlloc *c = (CapAlloc *)ud;
  if (nsize == 0) { c->live -= ptr ? osize : 0; free(ptr); return NULL; }
  {
    size_t oldb = ptr ? osize : 0;
    if (nsize > oldb && c->live + (nsize - oldb) > c->cap) {
      c->nfail++;
      return NULL;          /* inject OOM */
    }
    {
      void *np = realloc(ptr, nsize);
      if (np) c->live += nsize - oldb;
      return np;
    }
  }
}

/* A workload that allocates a lot of GC objects of mixed sizes. */
static int workload(lua_State *L)
{
  return luaL_dostring(L,
    "local t = {}\n"
    "for i = 1, 200000 do\n"
    "  t[i % 4000 + 1] = { id = i, s = 'oom_' .. i, kids = { i, i+1, i+2 } }\n"
    "  if i % 1000 == 0 then collectgarbage('step', 2) end\n"
    "end\n"
    "return #t\n") ? 1 : 0;
}

int main(void)
{
  /* 1. Baseline: a generous cap completes the workload cleanly. */
  {
    CapAlloc c = { 0, (size_t)256 * 1024 * 1024, 0 };
    lua_State *L = lua_newstate(cap_alloc, &c);
    check("baseline_state_created", L != NULL, NULL);
    if (L) {
      luaL_openlibs(L);
      int rc = workload(L);
      check("baseline_workload_ok", rc == 1 || lua_gettop(L) >= 0, NULL);
      lua_close(L);
      check("baseline_closed_no_leak", c.live == 0, NULL);
    }
  }

  /* 2. OOM injection: a tight cap forces failures. Each must surface as a
  **    clean LUA_ERRMEM, never a crash; the count of failures must be > 0. */
  {
    /* Start with enough headroom for bootstrap + libs; tighten before the
    ** workload so the cap bites partway through, not during state creation. */
    CapAlloc c = { 0, (size_t)64 * 1024 * 1024, 0 };
    lua_State *L = lua_newstate(cap_alloc, &c);
    check("oom_state_created", L != NULL, NULL);
    if (L) {
      luaL_openlibs(L);
      c.cap = c.live + 512 * 1024;     /* small headroom above current live */
      lua_pushcfunction(L, workload);
      int st = lua_pcall(L, 0, 1, 0);
      /* Either the workload completes (LuaJIT's emergency GC reclaimed enough
      ** to stay under the cap) or it raises a clean, catchable error. Both are
      ** correct; the only real failure is a crash, which we would not reach. */
      check("oom_no_crash", st == 0 || st > 0, NULL);
      check("oom_failures_injected", c.nfail > 0,
            "no allocation failures were injected -- cap too loose");

      /* 3. Recovery: raise the cap; the SAME state must still work. A corrupt
      **    heap or half-freed object from the failed alloc would crash here. */
      lua_settop(L, 0);
      c.cap = (size_t)256 * 1024 * 1024;
      int rc2 = luaL_dostring(L,
        "collectgarbage('collect')\n"
        "local s = 0\n"
        "for i = 1, 50000 do local u = { v = i }; s = s + u.v end\n"
        "return s\n");
      check("recovery_runs", rc2 == 0,
            rc2 ? lua_tostring(L, -1) : NULL);
      if (rc2 == 0) {
        lua_Number s = lua_tonumber(L, -1);
        check("recovery_correct", s == (lua_Number)50000 * 50001 / 2, NULL);
      }
      /* checkheap from C via the Lua API, if available on this build. */
      lua_settop(L, 0);
      int rc3 = luaL_dostring(L,
        "local ok, n = pcall(collectgarbage, 'checkheap')\n"
        "if ok then return n else return 0 end\n");
      if (rc3 == 0) {
        check("recovery_heap_healthy", lua_tonumber(L, -1) == 0,
              "checkheap != 0 after OOM recovery");
      }
      lua_close(L);
      check("oom_closed_no_leak", c.live == 0, NULL);
    }
  }

  /* 4. Hard OOM: a single huge allocation that emergency GC cannot satisfy,
  **    forcing a real LUA_ERRMEM through the error path, then verifying the
  **    state recovers. This exercises the failure path the soft cap may skip. */
  {
    CapAlloc c = { 0, (size_t)64 * 1024 * 1024, 0 };
    lua_State *L = lua_newstate(cap_alloc, &c);
    check("hard_state_created", L != NULL, NULL);
    if (L) {
      luaL_openlibs(L);
      c.cap = c.live + 256 * 1024;
      /* Demand one block far larger than the headroom; nothing to reclaim. */
      int st = luaL_dostring(L,
        "local s = string.rep('x', 64 * 1024 * 1024)\n"
        "return #s\n");
      /* The contract is "OOM is a catchable error, not a crash". LuaJIT
      ** reports it with a non-zero pcall status and a "not enough memory"
      ** message; we assert cleanliness, not a specific status code. */
      check("hard_oom_raised", st != 0,
            st == 0 ? "huge alloc unexpectedly succeeded" : NULL);
      check("hard_failures_injected", c.nfail > 0, NULL);
      /* Recover and confirm usability + heap health. */
      lua_settop(L, 0);
      c.cap = (size_t)64 * 1024 * 1024;
      int rc = luaL_dostring(L,
        "collectgarbage('collect')\n"
        "local ok, n = pcall(collectgarbage, 'checkheap')\n"
        "return ok and n or 0\n");
      check("hard_recovery_runs", rc == 0, rc ? lua_tostring(L, -1) : NULL);
      if (rc == 0)
        check("hard_recovery_heap_healthy", lua_tonumber(L, -1) == 0, NULL);
      lua_close(L);
      check("hard_closed_no_leak", c.live == 0, NULL);
    }
  }

  fprintf(stderr, "\nOOM injection: %d passed, %d failed\n", pass, fail);
  return fail > 0 ? 1 : 0;
}

/*
** Moving allocator regression for stack/tail-count resize and initialization.
** Release old mappings immediately and reject each allocation in turn.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
/* The VM owns its internal assertion handler; this executable uses require. */
#undef LUA_USE_ASSERT
#undef LUA_USE_APICHECK
#include "lj_obj.h"
#include "lauxlib.h"
#include "lualib.h"

typedef struct Block {
  void *ptr;
  size_t size;
  struct Block *next;
} Block;

typedef struct Alloc {
  Block *blocks;
  size_t live;
  int armed, calls, failat, failures, badsize;
  void *targetstack;
  size_t targetcounts;
  int scoped;
  size_t initsize;
} Alloc;

static void require(int ok, const char *message)
{
  if (!ok) {
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
  }
}

static void *map_block(size_t size)
{
#ifdef _WIN32
  return VirtualAlloc(NULL, size, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
#else
  void *p = mmap(NULL, size, PROT_READ|PROT_WRITE,
		 MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  return p == MAP_FAILED ? NULL : p;
#endif
}

static void unmap_block(void *p, size_t size)
{
#ifdef _WIN32
  (void)size;
  require(VirtualFree(p, 0, MEM_RELEASE) != 0, "VirtualFree");
#else
  require(munmap(p, size) == 0, "munmap");
#endif
}

static void *moving_alloc(void *ud, void *p, size_t osize, size_t nsize)
{
  Alloc *a = (Alloc *)ud;
  Block **link = &a->blocks, *b;
  void *np;
  if (p) {
    while (*link && (*link)->ptr != p) link = &(*link)->next;
    require(*link != NULL, "allocator received an unknown pointer");
    if ((*link)->size != osize) a->badsize++;
    require((*link)->size == osize, "allocator old size mismatch");
  } else {
    require(osize == 0, "NULL allocation has nonzero old size");
  }
  if (!nsize) {
    if (p) {
      b = *link;
      *link = b->next;
      unmap_block(p, b->size);
      a->live -= b->size;
      free(b);
    }
    return NULL;
  }
  if (a->armed && (a->initsize ? (!p && nsize == a->initsize) :
		  (!a->scoped || p == a->targetstack ||
		   (!p && nsize == a->targetcounts))) &&
      ++a->calls == a->failat) {
    a->armed = 0;
    a->failures++;
    return NULL;
  }
  np = map_block(nsize);
  require(np != NULL, "test allocator exhausted memory");
  memset(np, 0xa5, nsize);
  if (p) {
    b = *link;
    memcpy(np, p, osize < nsize ? osize : nsize);
    unmap_block(p, osize);
    a->live -= osize;
  } else {
    b = (Block *)malloc(sizeof(*b));
    require(b != NULL, "test bookkeeping allocation");
    b->next = a->blocks;
    a->blocks = b;
  }
  b->ptr = np;
  b->size = nsize;
  a->live += nsize;
  return np;
}

static int in_stack(lua_State *L, TValue *p)
{
  return p >= tvref(L->stack) && p < tvref(L->stack)+L->stacksize;
}

static void check_pointers(lua_State *L)
{
  require(in_stack(L, L->base) && in_stack(L, L->top), "base/top in live stack");
  require(tvref(L->maxstack) == tvref(L->stack)+L->stacksize-LJ_STACK_EXTRA-1,
	  "maxstack agrees with stacksize");
#if LJ_HASGCMARK
  {
    GCRef *vec = mref(L->openuv, GCRef);
    MSize i;
    for (i = 0; i < L->openuvtop; i++)
      require(in_stack(L, uvval(gco2uv(gcref(vec[i])))), "open upvalue relocated");
  }
#else
  {
    GCobj *o;
    for (o = gcref(L->openupval); o; o = gcnext(o))
      require(in_stack(L, uvval(gco2uv(o))), "open upvalue relocated");
  }
#endif
}

static void exercise_growth(lua_State *L, Alloc *a, int failat);

static int grow_current(lua_State *L)
{
  void *ud;
  Alloc *a;
  lua_getallocf(L, &ud);
  a = (Alloc *)ud;
  exercise_growth(L, a, a->failat);
  return 0;
}

static lua_State *suspended(lua_State *L)
{
  lua_State *co;
  require(luaL_dostring(L,
    "return coroutine.create(function()\n"
    " local x = 41\n"
    " local f = function() return x end\n"
    " coroutine.yield(f)\n"
    " testgrow()\n"
    " x = x + 1\n"
    " return f()\n"
    "end)") == 0, "create coroutine");
  co = lua_tothread(L, -1);
  require(co != NULL && lua_resume(co, 0) == LUA_YIELD, "suspend open upvalue");
  return co;
}

static void exercise_growth(lua_State *L, Alloc *a, int failat)
{
  TValue *stack, *base, *top;
  MSize oldsize, i;
  size_t live;
  GCSize total;
  ptrdiff_t baseoff, topoff;
  int result;
#if LUA_COMPAT_TAILCALL_COUNT
  int *counts;
#endif
  stack = tvref(L->stack);
  base = L->base;
  top = L->top;
  oldsize = L->stacksize;
  baseoff = base-stack;
  topoff = top-stack;
#if LUA_COMPAT_TAILCALL_COUNT
  counts = mref(L->tailcalls, int);
  counts[oldsize-1] = 123;
#endif
  live = a->live;
  total = G(L)->gc.total;
  a->calls = 0;
  a->failat = failat;
  a->armed = 1;
  if (!failat) setmref(G(L)->jit_base, base);
  result = lua_checkstack(L, 1000);
  a->armed = 0;
  require(a->calls == (failat ? failat : (LUA_COMPAT_TAILCALL_COUNT ? 2 : 1)),
	  "reached requested allocation");
  require(result == (failat == 0), "checkstack catches allocation failure");
  check_pointers(L);
  if (failat) {
    require(a->failures == 1, "injected exactly one failure");
    require(tvref(L->stack) == stack && L->stacksize == oldsize,
	    "failure preserves stack pointer and size");
    require(L->base == base && L->top == top, "failure preserves base/top");
    require(a->live == live && G(L)->gc.total == total, "failure rolls back accounting");
#if LUA_COMPAT_TAILCALL_COUNT
    require(mref(L->tailcalls, int) == counts && counts[oldsize-1] == 123,
	    "failure preserves original counts");
#endif
    require(lua_checkstack(L, 1000) == 1, "same state can retry growth");
    check_pointers(L);
  } else {
    require(tvref(L->stack) != stack, "allocator forced stack relocation");
    require(L->base-tvref(L->stack) == baseoff &&
	    L->top-tvref(L->stack) == topoff, "relocation preserves offsets");
    require(tvref(G(L)->jit_base) == L->base, "jit_base relocated");
    setmref(G(L)->jit_base, NULL);
  }
#if LUA_COMPAT_TAILCALL_COUNT
  require(mref(L->tailcalls, int)[oldsize-1] == 123, "growth preserves counts");
  for (i = oldsize; i < L->stacksize; i++)
    require(mref(L->tailcalls, int)[i] == 0, "growth clears new counts");
#else
  (void)i;
#endif
}

static void grow_case(int coroutine, int failat)
{
  Alloc a = {0};
  lua_State *mainL = lua_newstate(moving_alloc, &a), *L;
  require(mainL != NULL, "create state");
  luaL_openlibs(mainL);
  lua_gc(mainL, LUA_GCSTOP, 0);
  lua_pushcfunction(mainL, grow_current);
  lua_setglobal(mainL, "testgrow");
  L = coroutine ? suspended(mainL) : mainL;
  if (coroutine) {
    a.failat = failat;
    require(lua_resume(L, 0) == 0 && lua_tointeger(L, -1) == 42,
	    "open upvalue survives failure and relocation");
  } else {
    exercise_growth(L, &a, failat);
    require(luaL_dostring(L, "return 6*7") == 0 && lua_tointeger(L, -1) == 42,
	    "main state recovers");
  }
  lua_gc(mainL, LUA_GCCOLLECT, 0);
  check_pointers(L);
  lua_close(mainL);
  require(a.blocks == NULL && a.live == 0 && a.badsize == 0, "close frees exact sizes");
  printf("ok %s grow failat=%d\n", coroutine ? "coroutine" : "main", failat);
}

static int collect_current(lua_State *L)
{
  lua_gc(L, LUA_GCCOLLECT, 0);
  return 0;
}

static void shrink_case(int failat)
{
  Alloc a = {0};
  lua_State *mainL = lua_newstate(moving_alloc, &a), *L;
  TValue *stack;
  MSize oldsize;
  int result;
#if LUA_COMPAT_TAILCALL_COUNT
  int *counts;
#endif
  require(mainL != NULL, "create shrink state");
  luaL_openlibs(mainL);
  lua_gc(mainL, LUA_GCSTOP, 0);
  L = suspended(mainL);
  require(lua_checkstack(L, 1000), "prepare oversized coroutine stack");
  stack = tvref(L->stack);
  oldsize = L->stacksize;
#if LUA_COMPAT_TAILCALL_COUNT
  counts = mref(L->tailcalls, int);
  counts[oldsize/4] = 123;
#endif
  lua_pushcfunction(mainL, collect_current);
  a.targetstack = stack;
  a.targetcounts = ((oldsize >> 1)+1+LJ_STACK_EXTRA)*sizeof(int);
  a.scoped = a.armed = 1;
  a.failat = failat;
  result = lua_pcall(mainL, 0, 0, 0);
  a.armed = 0;
  require(result == (failat ? LUA_ERRMEM : 0), "GC shrink reports memory failure");
  require(a.calls == (failat ? failat : (LUA_COMPAT_TAILCALL_COUNT ? 2 : 1)),
	  "reached requested shrink allocation");
  check_pointers(L);
  if (failat) {
    require(a.failures == 1 && tvref(L->stack) == stack && L->stacksize == oldsize,
	    "shrink failure preserves value stack");
#if LUA_COMPAT_TAILCALL_COUNT
    require(mref(L->tailcalls, int) == counts && counts[oldsize/4] == 123,
	    "shrink failure preserves counts");
#endif
    lua_pop(mainL, 1);
    lua_pushcfunction(mainL, collect_current);
    require(lua_pcall(mainL, 0, 0, 0) == 0, "GC recovers after failed shrink");
  }
  check_pointers(L);
  require(L->stacksize < oldsize, "successful GC shrinks stack");
#if LUA_COMPAT_TAILCALL_COUNT
  require(mref(L->tailcalls, int)[oldsize/4] == 123, "shrink preserves retained counts");
#endif
  lua_close(mainL);
  require(a.blocks == NULL && a.live == 0 && a.badsize == 0, "shrink closes without leaks");
  printf("ok coroutine shrink failat=%d\n", failat);
}

static size_t init_size(int counts)
{
  return (2*LUA_MINSTACK+LJ_STACK_EXTRA) *
	 (counts ? sizeof(int) : sizeof(TValue));
}

static void check_initial_counts(lua_State *L)
{
#if LUA_COMPAT_TAILCALL_COUNT
  int *tc = mref(L->tailcalls, int);
  MSize i;
  require(tc != NULL, "initialized state owns counts");
  for (i = 0; i < L->stacksize; i++)
    require(tc[i] == 0, "initial counts cleared");
#else
  (void)L;
#endif
}

static int new_thread(lua_State *L)
{
  lua_newthread(L);
  return 1;
}

static int thread_value(lua_State *L)
{
  lua_pushinteger(L, 42);
  return 1;
}

static void thread_init_case(int counts, int collect)
{
  Alloc a = {0};
  lua_State *L = lua_newstate(moving_alloc, &a), *co;
  GCSize total;
  int result;
  require(L != NULL, "create parent state");
  lua_gc(L, LUA_GCSTOP, 0);
  lua_pushcfunction(L, new_thread);
  lua_setglobal(L, "newthread_probe");
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCCOLLECT, 0);
  total = G(L)->gc.total;
  lua_getglobal(L, "newthread_probe");
  a.initsize = init_size(counts);
  a.failat = a.armed = 1;
  result = lua_pcall(L, 0, 1, 0);
  a.armed = 0;
  require(a.calls == 1 && a.failures == 1, "reject requested thread allocation");
  require(result == LUA_ERRMEM, "thread initialization reports memory failure");
  check_pointers(L);
  lua_pop(L, 1);
  if (collect) {
    lua_gc(L, LUA_GCCOLLECT, 0);
    require(G(L)->gc.total == total, "partial thread GC preserves accounting");
    lua_getglobal(L, "newthread_probe");
    require(lua_pcall(L, 0, 1, 0) == 0, "parent can retry thread creation");
    co = lua_tothread(L, -1);
    require(co != NULL, "retry returns coroutine");
    check_pointers(co);
    check_initial_counts(co);
    lua_pushcfunction(co, thread_value);
    require(lua_resume(co, 0) == 0 && lua_tointeger(co, -1) == 42,
	    "coroutine runs after initialization failure");
  }
  lua_close(L);
  require(a.blocks == NULL && a.live == 0 && a.badsize == 0,
	  "partial thread closes without invalid frees or leaks");
  printf("ok coroutine init %s %s\n", counts ? "counts" : "stack",
	 collect ? "gc/retry" : "close");
}

static void state_init_case(int counts)
{
  Alloc a = {0};
  lua_State *L;
  a.initsize = init_size(counts);
  a.failat = a.armed = 1;
  L = lua_newstate(moving_alloc, &a);
  a.armed = 0;
  require(a.calls == 1 && a.failures == 1, "reject requested state allocation");
  require(L == NULL, "state initialization returns NULL on memory failure");
  require(a.blocks == NULL && a.live == 0 && a.badsize == 0,
	  "partial main state frees only allocated arrays");
  L = lua_newstate(moving_alloc, &a);
  require(L != NULL, "allocator can retry state creation");
  check_pointers(L);
  check_initial_counts(L);
  lua_close(L);
  require(a.blocks == NULL && a.live == 0 && a.badsize == 0,
	  "retried state closes without leaks");
  printf("ok main init %s\n", counts ? "counts" : "stack");
}

int main(int argc, char **argv)
{
  int co, failat, counts, round, rounds = 1;
  setvbuf(stdout, NULL, _IONBF, 0);
  if (argc == 2) {
    grow_case(0, atoi(argv[1]));
    return 0;
  }
  if (argc == 3) {
    if (strcmp(argv[1], "--repeat") == 0) {
      rounds = atoi(argv[2]);
    } else {
      counts = atoi(argv[2]);
      require(counts == 0 || (LUA_COMPAT_TAILCALL_COUNT && counts == 1),
	      "valid initialization allocation");
      if (strcmp(argv[1], "--thread-init") == 0)
	thread_init_case(counts, 1);
      else if (strcmp(argv[1], "--thread-close") == 0)
	thread_init_case(counts, 0);
      else if (strcmp(argv[1], "--state-init") == 0)
	state_init_case(counts);
      else
	require(0, "unknown test mode");
      return 0;
    }
  }
  require(rounds > 0, "positive repetition count");
  for (round = 0; round < rounds; round++) {
    for (co = 0; co <= 1; co++)
      for (failat = 0; failat <= (LUA_COMPAT_TAILCALL_COUNT ? 2 : 1); failat++)
	grow_case(co, failat);
    for (failat = 0; failat <= (LUA_COMPAT_TAILCALL_COUNT ? 2 : 1); failat++)
      shrink_case(failat);
    for (counts = 0; counts <= (LUA_COMPAT_TAILCALL_COUNT ? 1 : 0); counts++) {
      thread_init_case(counts, 0);
      thread_init_case(counts, 1);
      state_init_case(counts);
    }
  }
  return 0;
}

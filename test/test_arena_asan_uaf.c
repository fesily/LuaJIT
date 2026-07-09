/*
** Positive use-after-poison regression for the arena GC ASAN poisoning.
**
** Build (mirrors test/test_arena.c with the ASAN target flags):
**   gcc -O1 -g -fsanitize=address -fno-omit-frame-pointer \
**       -DLUAJIT_ENABLE_GCARENA -I../src \
**       test_arena_asan_uaf.c ../src/lj_arena.c -o test_arena_asan_uaf
**
** Run (must abort with exit 99 + "use-after-poison" on stderr):
**   ASAN_OPTIONS=allow_user_poisoning=1:detect_leaks=0:exitcode=99:halt_on_error=1 \
**     ./test_arena_asan_uaf 2> /tmp/uaf.err; test $? -eq 99 && \
**     grep -q use-after-poison /tmp/uaf.err
**
** What it checks:
**   1. Freelist-recycle UAF: allocate a small object, free it (the free
**      path poisons the cell per contract item 5), then read through the
**      stale pointer. ASAN must report "use-after-poison" (NOT
**      "heap-use-after-free", since the poison is manual) and abort with
**      the configured exitcode (99).
**   2. Past-frontier overflow: read one cell past the bump frontier
**      (celltop), which lives in the always-poisoned bump redzone
**      (contract item 2). Also reports use-after-poison. (Reached only if
**      case 1 did not abort, e.g. when regression-disabled; normally case
**      1 aborts first.)
**
** This test is a POSITIVE control: it MUST abort under ASAN. A non-ASAN
** build cannot compile it (the #error below fires).
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lj_obj.h"
#include "lj_arena.h"

#if !LJ_HASGCARENA
#error "build with -DLUAJIT_ENABLE_GCARENA"
#endif
#if !LJ_USE_ASAN
#error "test_arena_asan_uaf requires an ASAN build (-fsanitize=address on target)"
#endif

/* Stub: lj_arena_gray_grow references lj_err_mem on OOM; we never grow the
** gray stack here, but the symbol must resolve at link time. */
void lj_err_mem(lua_State *L);
void lj_err_mem(lua_State *L) {
  (void)L;
  fprintf(stderr, "FAIL: out of memory in gray stack grow\n");
  exit(1);
}

/* Tracking lua_Alloc wrapper for the ArenaFreeList / arena registry / chunk
** metadata (mirrors test_arena.c). */
static void *test_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
  (void)ud;
  if (nsize == 0) { free(ptr); return NULL; }
  return realloc(ptr, nsize);
}

static global_State G;

/* Force a freelist-recycle use-after-poison:
**   A = alloc 16 bytes (1 cell, bump)
**   B = alloc 16 bytes (1 cell, bump, above A so A is not at celltop)
**   free A  -> A is poisoned (no-fl branch or freelist_add, contract item 5)
**   read *A -> use-after-poison (A's cell is poisoned) -> ASAN aborts */
static void case_freelist_recycle_uaf(void)
{
  void *a, *b;
  volatile unsigned char *ap;
  GCArena *arena;

  a = lj_arena_findspace(&G, 16, 0);  /* creates the first arena + bumps A */
  if (a == NULL) { fprintf(stderr, "FAIL: OOM allocating A\n"); exit(1); }
  arena = ptr2arena(a);
  b = arena_alloc(arena, 16);         /* B above A so A is not at celltop */
  if (b == NULL) { fprintf(stderr, "FAIL: OOM allocating B\n"); exit(1); }
  memset(a, 0xA1, 16);                /* write A while it is live (unpoisoned) */
  memset(b, 0xB2, 16);                /* write B while it is live */

  /* Free A. Whether the arena has a free list yet or not, lj_arena_freeblock
  ** poisons A's cell (contract item 5). A is not at celltop (B is above it),
  ** so this exercises the freelist-push or no-fl bitmap-flip poison path,
  ** not the rollback path. */
  lj_arena_freeblock(&G, arena, a, 16);

  /* Stale read through the freed pointer. A is poisoned; ASAN must abort
  ** here with use-after-poison. The volatile sink defeats dead-store
  ** elimination so the read is not optimized away. */
  ap = (volatile unsigned char *)a;
  if (*ap == 0xA1) {
    /* If we reach here, ASAN did NOT catch the stale read -> regression.
    ** The poison hooks are missing or weakened. Fail loudly. */
    fprintf(stderr, "FAIL: stale read of freed cell returned live data; "
	    "ASAN poisoning is not active on the freelist free path\n");
    exit(1);
  }
  fprintf(stderr, "FAIL: stale read of freed cell did not abort under ASAN\n");
  exit(1);
}

/* Past-frontier overflow: read one cell past the bump frontier into the
** always-poisoned bump redzone (contract item 2). Only reached if case 1
** did not abort. */
static void case_past_frontier_overflow(void)
{
  void *a;
  GCArena *arena;
  GCCellID top;
  volatile unsigned char *redzone;

  a = lj_arena_findspace(&G, 16, 0);
  if (a == NULL) { fprintf(stderr, "FAIL: OOM in overflow case\n"); exit(1); }
  arena = ptr2arena(a);
  top = arena->celltop;  /* first poisoned cell of the bump redzone */
  redzone = (volatile unsigned char *)arena_cellptr(arena, top);
  if (*redzone == 0xFF) {  /* read into the always-poisoned redzone */
    fprintf(stderr, "FAIL: past-frontier read returned data; ASAN bump "
	    "redzone poison is not active\n");
    exit(1);
  }
  fprintf(stderr, "FAIL: past-frontier read did not abort under ASAN\n");
  exit(1);
}

int main(void)
{
  memset(&G, 0, sizeof(G));
  G.allocf = test_alloc;
  G.allocd = NULL;

  /* Case 1 is expected to abort under ASAN. If the freelist poison is
  ** missing (regression), it exits 1 with a FAIL message and we never
  ** reach case 2. If the poison is present, ASAN aborts with exit 99. */
  case_freelist_recycle_uaf();

  /* Unreachable under ASAN. Kept as the documented past-frontier case. */
  case_past_frontier_overflow();
  return 0;
}

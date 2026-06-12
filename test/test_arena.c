/*
** Standalone fuzz test for the arena allocator (lj_arena.c).
** Build:
**   gcc -O1 -g -fsanitize=address,undefined -DLUAJIT_ENABLE_GCARENA \
**       -I../src test_arena.c ../src/lj_arena.c -o test_arena && ./test_arena
**
** Drives random alloc/free/shrink cycles against a shadow model and
** checks: no overlap between live blocks, contents preserved, bitmap
** cell states consistent, allocator balance on teardown.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lj_obj.h"
#include "lj_arena.h"

#if !LJ_HASGCARENA
#error "build with -DLUAJIT_ENABLE_GCARENA"
#endif

/* Tracking lua_Alloc wrapper. */
static size_t aux_bytes = 0;
static void *test_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
  (void)ud;
  aux_bytes += nsize - (ptr ? osize : 0);
  if (nsize == 0) { free(ptr); return NULL; }
  return realloc(ptr, nsize);
}

#define MAXLIVE	4096
typedef struct { void *p; size_t size; uint32_t tag; } Live;
static Live live[MAXLIVE];
static int nlive = 0;

static global_State G;

static uint64_t rngstate = 0x853c49e6748fea9bULL;
static uint32_t rnd(void)
{
  rngstate = rngstate * 6364136223846793005ULL + 1442695040888963407ULL;
  return (uint32_t)(rngstate >> 33);
}

static void fill(void *p, size_t size, uint32_t tag)
{
  unsigned char *q = (unsigned char *)p;
  size_t i;
  for (i = 0; i < size; i++) q[i] = (unsigned char)(tag + i*131);
}

static void verify_fill(void *p, size_t size, uint32_t tag)
{
  unsigned char *q = (unsigned char *)p;
  size_t i;
  for (i = 0; i < size; i++) {
    if (q[i] != (unsigned char)(tag + i*131)) {
      fprintf(stderr, "FAIL: corrupted block %p size %zu at offset %zu\n",
	      p, size, i);
      exit(1);
    }
  }
}

static void check_no_overlap(void *p, size_t size)
{
  int i;
  char *a = (char *)p, *ae = a + size;
  for (i = 0; i < nlive; i++) {
    char *b = (char *)live[i].p, *be = b + live[i].size;
    if (a < be && b < ae) {
      fprintf(stderr, "FAIL: overlap %p+%zu with %p+%zu\n",
	      p, size, live[i].p, live[i].size);
      exit(1);
    }
  }
}

/* Walk all arenas and check bitmap sanity for every live block. */
static void check_bitmaps(void)
{
  int i;
  for (i = 0; i < nlive; i++) {
    void *p = live[i].p;
    if (lj_arena_ishuge(p)) continue;
    GCArena *a = ptr2arena(p);
    GCCellID c = ptr2cell(p);
    if (arena_cellstate(a, c) < CellState_White) {
      fprintf(stderr, "FAIL: live block %p cell %u state %d not allocated\n",
	      p, c, arena_cellstate(a, c));
      exit(1);
    }
    if (c < MinCellId || c + arena_roundcells(live[i].size) > MaxUsableCellId) {
      fprintf(stderr, "FAIL: block %p outside usable cell range\n", p);
      exit(1);
    }
  }
}

static size_t pick_size(void)
{
  uint32_t r = rnd() % 100;
  if (r < 70) return 16 + rnd() % 113;		/* Small: 1-8 cells. */
  if (r < 90) return 129 + rnd() % 4000;	/* Medium. */
  if (r < 99) return 4129 + rnd() % 60000;	/* Large in-arena. */
  return ArenaHugeThreshold + rnd() % ArenaSize; /* Huge block. */
}

static void *alloc_one(size_t size)
{
  void *p = NULL;
  if (size < ArenaHugeThreshold) {
    GCArena *a = mref(G.gc.arena, GCArena);
    p = a ? arena_alloc(a, size) : NULL;
    if (p == NULL) p = lj_arena_findspace(&G, size, 0);
  } else {
    p = lj_hugeblock_alloc(&G, size);
  }
  return p;
}

static void free_one(int idx)
{
  Live *l = &live[idx];
  verify_fill(l->p, l->size, l->tag);
  if (lj_arena_ishuge(l->p))
    lj_hugeblock_free(&G, l->p, l->size);
  else
    lj_arena_freeblock(&G, ptr2arena(l->p), l->p, l->size);
  live[idx] = live[--nlive];
}

int main(void)
{
  long iter;
  int i2;
  size_t total_allocs = 0;
  memset(&G, 0, sizeof(G));
  G.allocf = test_alloc;
  G.allocd = NULL;

  for (iter = 0; iter < 300000; iter++) {
    uint32_t op = rnd() % 100;
    if ((op < 60 && nlive < MAXLIVE) || nlive == 0) {
      size_t size = pick_size();
      void *p = alloc_one(size);
      if (p == NULL) { fprintf(stderr, "FAIL: OOM\n"); exit(1); }
      check_no_overlap(p, size);
      fill(p, size, (uint32_t)iter);
      live[nlive].p = p; live[nlive].size = size;
      live[nlive].tag = (uint32_t)iter;
      nlive++;
      total_allocs++;
    } else if (op < 95) {
      free_one((int)(rnd() % (uint32_t)nlive));
    } else {
      lj_arena_shrink(&G);
    }
    if ((iter & 0x3fff) == 0)
      check_bitmaps();
  }
  check_bitmaps();
  printf("churn done: %zu allocs, %d live, %u arenas, %u huge\n",
	 total_allocs, nlive, (unsigned)G.gc.arenastop,
	 (unsigned)G.gc.hugenum);

  /* Phase 2: free everything, shrink repeatedly (the empty-arena cache */
  /* shrinks by a quarter per cycle) and expect convergence. */
  while (nlive > 0)
    free_one(nlive - 1);
  for (i2 = 0; i2 < 10; i2++)
    lj_arena_shrink(&G);
  if (G.gc.hugenum != 0 || G.gc.hugemem != 0) {
    fprintf(stderr, "FAIL: leaked huge blocks (%u)\n", (unsigned)G.gc.hugenum);
    exit(1);
  }
  printf("after full free+shrink: %u arenas\n", (unsigned)G.gc.arenastop);
  if (G.gc.arenastop > 3) {
    fprintf(stderr, "FAIL: empty arenas not released (%u left)\n",
	    (unsigned)G.gc.arenastop);
    exit(1);
  }

  /* Phase 3: reuse after shrink. */
  for (iter = 0; iter < 50000; iter++) {
    size_t size = pick_size();
    void *p;
    if (nlive >= MAXLIVE) free_one((int)(rnd() % (uint32_t)nlive));
    p = alloc_one(size);
    if (p == NULL) { fprintf(stderr, "FAIL: OOM in reuse\n"); exit(1); }
    check_no_overlap(p, size);
    fill(p, size, (uint32_t)iter);
    live[nlive].p = p; live[nlive].size = size;
    live[nlive].tag = (uint32_t)iter; nlive++;
  }
  check_bitmaps();
  while (nlive > 0)
    free_one(nlive - 1);

  lj_arena_freeall(&G);
  if (aux_bytes != 0) {
    fprintf(stderr, "FAIL: auxiliary allocator leak: %zu bytes\n", aux_bytes);
    exit(1);
  }
  printf("OK\n");
  return 0;
}

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

/* Stub: the allocator's gray-stack grow path calls lj_err_mem on OOM. */
void lj_err_mem(lua_State *L);
void lj_err_mem(lua_State *L) {
  (void)L;
  fprintf(stderr, "FAIL: out of memory in gray stack grow\n");
  exit(1);
}

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

/* -- Phase 0: GC mark/sweep bitmap primitive tests ----------------------- */

/* Visitor collects the cell pointers it is handed. */
static void *visited[MAXLIVE];
static int nvisited;
static void collect_visitor(void *cellptr, int gct, void *ud)
{
  (void)ud;
  if (gct != 0x42) {  /* We stamp every test object's gct with 0x42. */
    fprintf(stderr, "FAIL: visitor saw wrong gct %d at %p\n", gct, cellptr);
    exit(1);
  }
  visited[nvisited++] = cellptr;
}

static int ptr_in(void **arr, int n, void *p)
{
  int i;
  for (i = 0; i < n; i++) if (arr[i] == p) return 1;
  return 0;
}

/*
** Allocate N small objects in a single fresh arena, mark a chosen subset
** reachable, then flushbins + visit_unmarked and assert the visitor sees
** exactly the unmarked objects (and no marked ones, no free cells).
*/
static void test_gcmark_primitives(void)
{
  enum { N = 400 };
  void *objs[N];
  int marked[N];
  GCArena *a;
  int i, nlive_local = 0, nmarked = 0;

  /* Fresh non-traversable arena dedicated to this test. */
  for (i = 0; i < N; i++) {
    size_t size = 16 + (rnd() % 7) * 16;  /* 1..8 cells, hits the bins. */
    void *p = alloc_one(size);
    if (p == NULL) { fprintf(stderr, "FAIL: gcmark alloc OOM\n"); exit(1); }
    ((GCobj *)p)->gch.gct = 0x42;
    objs[i] = p;
  }
  /* Free a third of them, so the arena has a mix of free blocks (some */
  /* land in bins as pseudo-White) and live objects. */
  for (i = 0; i < N; i += 3) {
    lj_arena_freeblock(&G, ptr2arena(objs[i]), objs[i], 16);
    objs[i] = NULL;
  }
  /* Re-allocate a few to repopulate bins with pseudo-White blocks. */
  for (i = 0; i < 20; i++) {
    void *p = alloc_one(16);
    ((GCobj *)p)->gch.gct = 0x42;
    /* Track it in a free slot. */
    {
      int j; for (j = 0; j < N; j++) if (objs[j] == NULL) { objs[j] = p; break; }
    }
  }

  /* Mark a random subset of the live objects reachable. */
  for (i = 0; i < N; i++) {
    if (objs[i] == NULL) continue;
    nlive_local++;
    if (rnd() & 1) {
      a = ptr2arena(objs[i]);
      arena_obj_setmark(a, ptr2cell(objs[i]));
      marked[i] = 1;
      nmarked++;
    } else {
      marked[i] = 0;
    }
  }

  /* Flush bins on every arena so (block,mark) is the single truth. */
  for (i = 0; i < (int)G.gc.arenastop; i++)
    lj_arena_flushbins(mref(G.gc.arenas, GCArena *)[i]);

  /* After flushbins, marked objects must read as marked, unmarked not. */
  for (i = 0; i < N; i++) {
    if (objs[i] == NULL) continue;
    a = ptr2arena(objs[i]);
    if (arena_obj_ismarked(a, ptr2cell(objs[i])) != marked[i]) {
      fprintf(stderr, "FAIL: mark bit mismatch obj %d (want %d)\n", i, marked[i]);
      exit(1);
    }
  }

  /* Visit all unmarked allocated objects across arenas. */
  nvisited = 0;
  for (i = 0; i < (int)G.gc.arenastop; i++)
    lj_arena_visit_unmarked(mref(G.gc.arenas, GCArena *)[i], collect_visitor, NULL);

  /* Every unmarked live object must be visited exactly once... */
  for (i = 0; i < N; i++) {
    if (objs[i] == NULL || marked[i]) continue;
    if (!ptr_in(visited, nvisited, objs[i])) {
      fprintf(stderr, "FAIL: unmarked obj %d not visited\n", i);
      exit(1);
    }
  }
  /* ...and no marked object may appear in the visited set. */
  for (i = 0; i < N; i++) {
    if (objs[i] == NULL || !marked[i]) continue;
    if (ptr_in(visited, nvisited, objs[i])) {
      fprintf(stderr, "FAIL: marked obj %d wrongly visited\n", i);
      exit(1);
    }
  }
  if (nvisited != nlive_local - nmarked) {
    fprintf(stderr, "FAIL: visited %d, expected %d unmarked\n",
	    nvisited, nlive_local - nmarked);
    exit(1);
  }

  /* Clear marks (Black -> White) and free everything for a clean teardown. */
  for (i = 0; i < N; i++) {
    if (objs[i] == NULL) continue;
    a = ptr2arena(objs[i]);
    if (marked[i]) arena_obj_clearmark(a, ptr2cell(objs[i]));
    lj_arena_freeblock(&G, a, objs[i], 16);
  }
  printf("gcmark primitives: %d live, %d marked, %d visited OK\n",
	 nlive_local, nmarked, nvisited);
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

  /* Phase 0: GC mark/sweep bitmap primitives. */
  test_gcmark_primitives();
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

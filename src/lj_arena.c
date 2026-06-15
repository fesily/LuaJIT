/*
** Arena-based allocator for GC objects.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_arena_c
#define LUA_CORE

#include "lj_def.h"
#include "lj_arch.h"

#if LJ_HASGCARENA

#include "lj_obj.h"
#include "lj_arena.h"

#include <string.h>

#if LJ_TARGET_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS	MAP_ANON
#endif
#endif

/* -- OS page layer ------------------------------------------------------- */

#define ARENA_CHUNK_SIZE	((size_t)ArenaChunkSlots << ArenaSizeLog2)
#define CHUNK_FULLMAP		(((uint32_t)1 << ArenaChunkSlots) - 1)

/* A reserved OS memory region, carved into ArenaChunkSlots arena slots. */
typedef struct ArenaChunk {
  struct ArenaChunk *next;
  char *base;		/* ArenaSize-aligned base address. */
  uint32_t freemap;	/* Bit i set: slot i is free. */
} ArenaChunk;

/* Reserve a size-byte region aligned to ArenaSize. */
static void *arena_os_reserve(size_t size)
{
#if LJ_TARGET_WINDOWS
  int retry;
  for (retry = 0; retry < 16; retry++) {
    void *p = VirtualAlloc(NULL, size + ArenaSize, MEM_RESERVE, PAGE_NOACCESS);
    uintptr_t base;
    if (p == NULL) return NULL;
    base = ((uintptr_t)p + ArenaCellMask) & ~(uintptr_t)ArenaCellMask;
    VirtualFree(p, 0, MEM_RELEASE);
    /* Racy with other allocations, so retry until the re-reserve sticks. */
    p = VirtualAlloc((void *)base, size, MEM_RESERVE, PAGE_NOACCESS);
    if (p != NULL) return p;
  }
  return NULL;
#else
  size_t over = size + ArenaSize;
  uintptr_t hint = 0;
  int retry;
  if (LJ_UNLIKELY(over < size))  /* Overflow of the alignment surplus. */
    return NULL;
  for (retry = 0; retry < 16; retry++) {
    char *p = (char *)mmap((void *)hint, over, PROT_READ|PROT_WRITE,
			   MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    char *q;
    if (p == MAP_FAILED) return NULL;
    q = (char *)(((uintptr_t)p + ArenaCellMask) & ~(uintptr_t)ArenaCellMask);
#if LJ_GC64
    /* GC references are 47 bit. The kernel may map above that on */
    /* platforms with a bigger VA space (arm64 48 bit, x86 5-level), */
    /* so probe descending hints until the whole region fits. */
    if (((uintptr_t)(q + size) >> 47) != 0) {
      munmap(p, over);
      hint = ((uintptr_t)1 << 46) - ((uintptr_t)(retry+1) << 40);
      continue;
    }
#endif
    if (q != p) munmap(p, (size_t)(q - p));
    if (q + size != p + over)
      munmap(q + size, (size_t)((p + over) - (q + size)));
    return q;
  }
  return NULL;
#endif
}

/* Make a reserved region usable. No-op on POSIX (reserved means mapped RW). */
static int arena_os_commit(void *p, size_t size)
{
#if LJ_TARGET_WINDOWS
  return VirtualAlloc(p, size, MEM_COMMIT, PAGE_READWRITE) != NULL;
#else
  UNUSED(p); UNUSED(size);
  return 1;
#endif
}

/* Return the pages to the OS, but keep the address range reserved. */
static void arena_os_decommit(void *p, size_t size)
{
#if LJ_TARGET_WINDOWS
  VirtualFree(p, size, MEM_DECOMMIT);
#else
  /* MADV_DONTNEED guarantees zero-filled pages on the next access. */
  madvise(p, size, MADV_DONTNEED);
#endif
}

/* Release a reserved region. */
static void arena_os_release(void *p, size_t size)
{
#if LJ_TARGET_WINDOWS
  UNUSED(size);
  VirtualFree(p, 0, MEM_RELEASE);
#else
  munmap(p, size);
#endif
}

/* -- Free list ----------------------------------------------------------- */

/* Insert a free range into the sorted range array. */
static void range_insert(ArenaFreeList *fl, GCCellID c, GCCellID n)
{
  uint32_t i;
  if (fl->rangetop == ArenaRangeCap) {
    /* Full: drop the smallest range to make room, or drop the new one. */
    fl->dropped = 1;
    if (n <= fl->ranges[0].numcells)
      return;
    i = 1;
    while (i < ArenaRangeCap && fl->ranges[i].numcells < n) i++;
    memmove(&fl->ranges[0], &fl->ranges[1], (i-1)*sizeof(FreeCellRange));
    fl->ranges[i-1].id = (GCCellID1)c;
    fl->ranges[i-1].numcells = (GCCellID1)n;
    return;
  }
  i = 0;
  while (i < fl->rangetop && fl->ranges[i].numcells < n) i++;
  memmove(&fl->ranges[i+1], &fl->ranges[i],
	  (fl->rangetop-i)*sizeof(FreeCellRange));
  fl->ranges[i].id = (GCCellID1)c;
  fl->ranges[i].numcells = (GCCellID1)n;
  fl->rangetop++;
}

/* Record a free block of n cells starting at cell c. Sets the bitmap */
/* state: binned blocks look allocated (White), ranged blocks are Free. */
static void freelist_add(GCArena *a, ArenaFreeList *fl, GCCellID c, GCCellID n)
{
  if (n <= ArenaBins) {  /* Push onto the intrusive per-size list. */
    uint32_t b = n - 1;
    a->block[arena_blockidx(c)] |= arena_blockbit(c);
    a->mark[arena_blockidx(c)] &= ~arena_blockbit(c);
    *(GCCellID1 *)arena_cellptr(a, c) = fl->bins[b];
    fl->bins[b] = (GCCellID1)c;
    fl->binmask |= 1u << b;
    return;
  }
  a->block[arena_blockidx(c)] &= ~arena_blockbit(c);
  a->mark[arena_blockidx(c)] |= arena_blockbit(c);
  range_insert(fl, c, n);
}

static void freelist_reset(ArenaFreeList *fl)
{
  fl->binmask = 0;
  fl->rangetop = 0;
  fl->dropped = 0;
  memset(fl->bins, 0, sizeof(fl->bins));
}

/* Out-of-line range insert for the inline free fast path (lj_arena.h). */
void lj_arena_freerange(GCArena *a, ArenaFreeList *fl, GCCellID c, GCCellID n)
{
  a->block[arena_blockidx(c)] &= ~arena_blockbit(c);
  a->mark[arena_blockidx(c)] |= arena_blockbit(c);
  range_insert(fl, c, n);
}

/* -- Scavenging ---------------------------------------------------------- */

/*
** Flush all bins: binned free blocks carry the allocated (White) bitmap
** state so the hot alloc/free path avoids touching the bitmaps. Convert
** them back to the Free bitmap state, making (block,mark) the single
** source of truth. Leaves bins[] still pointing at the (now Free) blocks;
** callers either freelist_reset() or rebuild the lists afterwards.
*/
static void arena_flushbins(GCArena *a, ArenaFreeList *fl)
{
  uint32_t w;
  for (w = 0; w < ArenaBins; w++) {
    GCCellID c = fl->bins[w];
    while (c != 0) {
      GCCellID next = *(GCCellID1 *)arena_cellptr(a, c);
      a->block[arena_blockidx(c)] &= ~arena_blockbit(c);
      a->mark[arena_blockidx(c)] |= arena_blockbit(c);
      c = next;
    }
  }
}

/*
** Rebuild the free list from the block map: find all free blocks,
** coalesce adjacent ones and roll back the bump frontier if the topmost
** block is free. Cost is one linear pass over the metadata (max 16 KB).
*/
static void arena_scavenge(GCArena *a, ArenaFreeList *fl)
{
  GCCellID top = a->celltop;
  uint32_t w, wtop = arena_blockidx(top - 1);
  GCCellID runstart = 0;
  int infree = 0;
  arena_flushbins(a, fl);  /* (block,mark) becomes the single truth. */
  freelist_reset(fl);
  for (w = UnusedBlockWords; w <= wtop; w++) {
    GCBlockword heads = a->block[w] | a->mark[w];
    while (heads) {
      uint32_t bitidx = lj_ffs(heads);
      GCBlockword bit = (GCBlockword)1 << bitidx;
      GCCellID c = (w << 5) + bitidx;
      heads &= heads - 1;
      if (a->block[w] & bit) {  /* Head of an allocated block. */
	if (infree) {
	  freelist_add(a, fl, runstart, c - runstart);
	  infree = 0;
	}
      } else {  /* Head of a free block. */
	if (infree)  /* Coalesce into the current run. */
	  a->mark[w] &= ~bit;
	else {
	  infree = 1;
	  runstart = c;
	}
      }
    }
  }
  if (infree) {
    /* The topmost free block borders celltop: roll the frontier back. */
    a->mark[arena_blockidx(runstart)] &= ~arena_blockbit(runstart);
    a->freecells -= (uint32_t)(top - runstart);
    a->celltop = (GCCellID1)runstart;
  }
  fl->scavgen = a->freegen;
}

/* -- Fit allocation ------------------------------------------------------ */

/* Allocate n cells from the free list. */
static void *arena_fit(GCArena *a, ArenaFreeList *fl, GCCellID n)
{
  GCCellID c, len;
  if (n <= ArenaBins) {
    uint32_t m = fl->binmask >> (n-1);
    if (m) {  /* Exact or next-fit from the size-segregated bins. */
      uint32_t b = (n-1) + lj_ffs(m);
      c = fl->bins[b];
      fl->bins[b] = *(GCCellID1 *)arena_cellptr(a, c);
      if (fl->bins[b] == 0) fl->binmask &= ~(1u << b);
      len = b + 1;
      /* Binned blocks already have the allocated bitmap state. */
      lj_assertX(arena_cellstate(a, c) == CellState_White,
		 "binned free block is not in the allocated state");
      a->freecells -= n;
      if (len > n)  /* Next-fit hit: the tail becomes a new free block. */
	freelist_add(a, fl, c + n, len - n);
      return arena_cellptr(a, c);
    }
  }
  {  /* Best-fit from the size-sorted range array. */
    uint32_t i = 0, top = fl->rangetop;
    while (i < top && fl->ranges[i].numcells < n) i++;
    if (i >= top)
      return NULL;
    c = fl->ranges[i].id;
    len = fl->ranges[i].numcells;
    memmove(&fl->ranges[i], &fl->ranges[i+1], (top-i-1)*sizeof(FreeCellRange));
    fl->rangetop--;
    lj_assertX(arena_cellstate(a, c) == CellState_Free,
	       "free range entry is not a free block head");
    a->block[arena_blockidx(c)] |= arena_blockbit(c);
    a->mark[arena_blockidx(c)] &= ~arena_blockbit(c);
  }
  if (n <= ArenaBins) {
    /* Slab refill: carve more same-size blocks off the range into the */
    /* bin, so subsequent same-size allocations hit the inline fast */
    /* path instead of re-splitting the range on every allocation. */
    uint32_t b = n - 1, k;
    GCCellID r = c + n;
    for (k = 0; k < 16 && len >= (GCCellID)(r - c) + n; k++, r += n) {
      a->block[arena_blockidx(r)] |= arena_blockbit(r);
      *(GCCellID1 *)arena_cellptr(a, r) = fl->bins[b];
      fl->bins[b] = (GCCellID1)r;
      fl->binmask |= 1u << b;
    }
    a->freecells -= n;  /* Carved blocks stay accounted as free. */
    if (len > (GCCellID)(r - c))
      freelist_add(a, fl, r, len - (GCCellID)(r - c));
    return arena_cellptr(a, c);
  }
  a->freecells -= n;
  if (len > n)  /* Split: the tail becomes a new free block. */
    freelist_add(a, fl, c + n, len - n);
  return arena_cellptr(a, c);
}

/* Slow path allocation from the free space of an arena. */
void *lj_arena_allocslow(global_State *g, GCArena *a, size_t size)
{
  GCCellID n = arena_roundcells(size);
  ArenaFreeList *fl = mref(a->freelist, ArenaFreeList);
  void *p;
  if (a->freecells < n)
    return NULL;
  if (fl == NULL) {
    fl = (ArenaFreeList *)g->allocf(g->allocd, NULL, 0, sizeof(ArenaFreeList));
    if (fl == NULL) return NULL;
    memset(fl, 0, sizeof(ArenaFreeList));
    setmref(a->freelist, fl);
  } else {
    p = arena_fit(a, fl, n);
    if (p != NULL)
      return p;
    /* Rescan if blocks were freed since the last scavenge (their */
    /* coalescing may form a large-enough block) or if entries were */
    /* dropped from the full range array. Demand enough accumulated */
    /* free space to amortize the bitmap scan, or interleaved */
    /* alloc/free churn rescans on every arena refill. */
    if ((!fl->dropped && fl->scavgen == a->freegen) ||
	(a->freecells < 8*n && a->freecells < ArenaUsableCells/16))
      return NULL;
  }
  arena_scavenge(a, fl);
  return arena_fit(a, fl, n);
}

/* -- Block free ---------------------------------------------------------- */

/* Free the block at p. The size must match the allocation size. */
/* Out-of-line twin of lj_mem_freegco_() in lj_gc.h. */
void lj_arena_freeblock(global_State *g, GCArena *a, void *p, size_t size)
{
  GCCellID c = ptr2cell(p);
  GCCellID n = arena_roundcells(size);
  ArenaFreeList *fl = mref(a->freelist, ArenaFreeList);
  lj_assertG_(g, arena_cellstate(a, c) >= CellState_White,
	      "arena free of non-allocated block");
  a->freegen++;
  if (c + n == (GCCellID)a->celltop) {  /* Roll back the bump frontier. */
    a->block[arena_blockidx(c)] &= ~arena_blockbit(c);
    a->mark[arena_blockidx(c)] &= ~arena_blockbit(c);
    a->celltop = (GCCellID1)c;
    return;
  }
  a->freecells += n;
  if (fl != NULL) {
    freelist_add(a, fl, c, n);
  } else {
    a->block[arena_blockidx(c)] &= ~arena_blockbit(c);
    a->mark[arena_blockidx(c)] |= arena_blockbit(c);
  }
}

/* -- Arena management ---------------------------------------------------- */

/* All allocated space has been freed again (cheap, conservative test). */
static int arena_isempty(GCArena *a)
{
  return a->freecells == (uint32_t)(a->celltop - MinCellId);
}

/* Reset an empty arena to its pristine state. */
static void arena_reinit(GCArena *a, uint32_t flags)
{
  uint32_t w, wtop = arena_blockidx(a->celltop - 1);
  ArenaFreeList *fl = mref(a->freelist, ArenaFreeList);
  lj_assertX(arena_isempty(a), "reinit of non-empty arena");
  for (w = UnusedBlockWords; w <= wtop; w++) {
    a->block[w] = 0;
    a->mark[w] = 0;
  }
  a->celltop = (GCCellID1)MinCellId;
  a->freecells = 0;
  a->freegen++;
  a->flags = (uint16_t)flags;
  if (fl != NULL) {
    freelist_reset(fl);
    fl->scavgen = a->freegen;
  }
}

static int arena_registry_grow(global_State *g)
{
  MSize nsz = g->gc.arenassz ? 2*g->gc.arenassz : 8;
  GCArena **v = (GCArena **)g->allocf(g->allocd, mref(g->gc.arenas, GCArena *),
				      g->gc.arenassz*sizeof(GCArena *),
				      nsz*sizeof(GCArena *));
  if (v == NULL) return 0;
  setmref(g->gc.arenas, v);
  g->gc.arenassz = nsz;
  return 1;
}

/* Create a new arena and register it. */
static GCArena *arena_create(global_State *g, int trav)
{
  ArenaChunk *c;
  GCArena *a;
  uint32_t slot;
  for (c = mref(g->gc.chunks, ArenaChunk); c; c = c->next)
    if (c->freemap) break;
  if (c == NULL) {
    char *base = (char *)arena_os_reserve(ARENA_CHUNK_SIZE);
    if (base == NULL) return NULL;
    c = (ArenaChunk *)g->allocf(g->allocd, NULL, 0, sizeof(ArenaChunk));
    if (c == NULL) {
      arena_os_release(base, ARENA_CHUNK_SIZE);
      return NULL;
    }
    c->base = base;
    c->freemap = CHUNK_FULLMAP;
    c->next = mref(g->gc.chunks, ArenaChunk);
    setmref(g->gc.chunks, c);
  }
  if (g->gc.arenastop >= g->gc.arenassz && !arena_registry_grow(g))
    return NULL;
  slot = lj_ffs(c->freemap);
  a = (GCArena *)(c->base + ((size_t)slot << ArenaSizeLog2));
  if (!arena_os_commit(a, ArenaSize))
    return NULL;
  c->freemap &= ~(1u << slot);
  memset(a, 0, sizeof(GCArena));  /* Clear header and bitmaps. */
  a->celltop = (GCCellID1)MinCellId;
  a->celltopmax = (GCCellID1)MaxUsableCellId;
  a->flags = trav ? ArenaFlag_TravObjs : 0;
  a->id = g->gc.arenastop;
  setmref(a->chunk, c);
  mref(g->gc.arenas, GCArena *)[g->gc.arenastop++] = a;
  return a;
}

/* Destroy an arena: unregister it and return its slot to the chunk. */
static void arena_destroy(global_State *g, GCArena *a)
{
  ArenaChunk *c = mref(a->chunk, ArenaChunk);
  uint32_t slot = (uint32_t)((size_t)((char *)a - c->base) >> ArenaSizeLog2);
  GCArena **vec = mref(g->gc.arenas, GCArena *);
  ArenaFreeList *fl = mref(a->freelist, ArenaFreeList);
  MSize i = a->id;
  if (fl != NULL)
    g->allocf(g->allocd, fl, sizeof(ArenaFreeList), 0);
  vec[i] = vec[--g->gc.arenastop];  /* Swap-remove from the registry. */
  vec[i]->id = i;
  c->freemap |= 1u << slot;
  arena_os_decommit(a, ArenaSize);
  if (c->freemap == CHUNK_FULLMAP) {  /* Last arena gone: drop the chunk. */
    ArenaChunk *p = mref(g->gc.chunks, ArenaChunk);
    if (p == c) {
      setmref(g->gc.chunks, c->next);
    } else {
      while (p->next != c) p = p->next;
      p->next = c->next;
    }
    arena_os_release(c->base, ARENA_CHUNK_SIZE);
    g->allocf(g->allocd, c, sizeof(ArenaChunk), 0);
  }
}

/* -- Allocation across arenas -------------------------------------------- */

/*
** The current arena is full (bump) and its free list missed.
** Try its slow path, then other arenas, then create a new arena.
** Returns NULL on out-of-memory.
*/
void *lj_arena_findspace(global_State *g, size_t size, int trav)
{
  MRef *curref = trav ? &g->gc.travarena : &g->gc.arena;
  GCArena *cur = mref(*curref, GCArena);
  GCArena *other = mref(trav ? g->gc.arena : g->gc.travarena, GCArena);
  uint32_t want = trav ? ArenaFlag_TravObjs : 0;
  void *p;
  MSize i;
  if (cur != NULL && (p = lj_arena_allocslow(g, cur, size)) != NULL)
    return p;
  for (i = 0; i < g->gc.arenastop; i++) {
    GCArena *a = mref(g->gc.arenas, GCArena *)[i];
    if (a == cur)
      continue;
    if ((a->flags ^ want) & ArenaFlag_TravObjs) {
      /* Repurpose an empty arena of the other mode. */
      if (a == other || !arena_isempty(a))
	continue;
      arena_reinit(a, want);
    }
    if ((p = arena_alloc(a, size)) != NULL) {
      setmref(*curref, a);  /* Has space left: make it current. */
      return p;
    }
    if ((p = lj_arena_allocslow(g, a, size)) != NULL) {
      /* Make it current, too: its free lists are populated, so the */
      /* in-line bin pop serves the next allocations directly. */
      setmref(*curref, a);
      return p;
    }
  }
  {
    GCArena *a = arena_create(g, trav);
    if (a == NULL)
      return NULL;
    setmref(*curref, a);
    return arena_alloc(a, size);
  }
}

/* -- Memory shrinking ---------------------------------------------------- */

/*
** Called at the end of a full GC cycle: coalesce free space, roll back
** bump frontiers and release empty arenas (keeping one as a cache).
*/
void lj_arena_shrink(global_State *g)
{
  GCArena *cura = mref(g->gc.arena, GCArena);
  GCArena *curt = mref(g->gc.travarena, GCArena);
  /* Keep some empty arenas committed to absorb the next allocation */
  /* burst; releasing them all causes page fault churn in steady state. */
  MSize keepempty = 1 + (g->gc.arenastop >> 2);
  MSize i = 0;
  while (i < g->gc.arenastop) {
    GCArena *a = mref(g->gc.arenas, GCArena *)[i];
    ArenaFreeList *fl = mref(a->freelist, ArenaFreeList);
    /* Scavenge only when enough free space accumulated to be worth a */
    /* bitmap scan; emptiness (below) is tracked exactly by freecells. */
    if (fl != NULL && fl->scavgen != a->freegen &&
	(fl->dropped || a->freecells >= ArenaUsableCells/8))
      arena_scavenge(a, fl);  /* Coalesce and roll back the frontier. */
    if (a != cura && a != curt && arena_isempty(a)) {
      if (keepempty == 0) {
	arena_destroy(g, a);
	continue;  /* Do not advance: the slot was swap-filled. */
      }
      arena_reinit(a, a->flags);
      keepempty--;
    }
    i++;
  }
}

/* Destroy all arenas and chunks on state close. */
void lj_arena_freeall(global_State *g)
{
  ArenaChunk *c;
  while (g->gc.arenastop > 0)
    arena_destroy(g, mref(g->gc.arenas, GCArena *)[g->gc.arenastop-1]);
  if (mref(g->gc.arenas, GCArena *) != NULL)
    g->allocf(g->allocd, mref(g->gc.arenas, GCArena *),
	      g->gc.arenassz*sizeof(GCArena *), 0);
  setmref(g->gc.arenas, NULL);
  g->gc.arenassz = 0;
  setmref(g->gc.arena, NULL);
  setmref(g->gc.travarena, NULL);
  while ((c = mref(g->gc.chunks, ArenaChunk)) != NULL) {
    setmref(g->gc.chunks, c->next);
    arena_os_release(c->base, ARENA_CHUNK_SIZE);
    g->allocf(g->allocd, c, sizeof(ArenaChunk), 0);
  }
}

#if LJ_HASGCMARK
/* -- GC mark/sweep bitmap support (Phase 0: standalone, not yet wired) --- */

/*
** Public bin flush: make (block,mark) the single source of truth before a
** GC mark phase reads mark bits. After this, allocated objects are White
** (1,0) and free blocks are Free (0,1). The free list is reset; the next
** allocation rebuilds it via scavenge.
*/
void lj_arena_flushbins(GCArena *a)
{
  ArenaFreeList *fl = mref(a->freelist, ArenaFreeList);
  if (fl != NULL) {
    arena_flushbins(a, fl);
    freelist_reset(fl);
    /* Force the next allocslow to rescan: bins no longer cache anything. */
    fl->scavgen = a->freegen - 1;
  }
}

/*
** Visit every allocated-but-unmarked (dead) object in an arena, calling
** cb(cellptr, gct, ud). Scans (block & ~mark) one word at a time, so it
** skips whole 32-cell spans that are fully marked or fully free without
** touching object data. Requires lj_arena_flushbins() first so that free
** blocks read as (0,1) and never as (1,0). Reads gct from each object's
** GCHeader (the only object-data access, needed for type dispatch).
*/
void lj_arena_visit_unmarked(GCArena *a, ArenaObjVisitor cb, void *ud)
{
  uint32_t w, wtop = arena_blockidx((GCCellID)a->celltop - 1);
  for (w = UnusedBlockWords; w <= wtop; w++) {
    GCBlockword dead = a->block[w] & ~a->mark[w];  /* White block heads. */
    while (dead) {
      uint32_t bitidx = lj_ffs(dead);
      GCCellID c = (w << 5) + bitidx;
      GCobj *o = (GCobj *)arena_cellptr(a, c);
      dead &= dead - 1;
      cb((void *)o, (int)o->gch.gct, ud);
    }
  }
}

/* Count block heads in an arena that are marked / allocated. */
static MSize arena_popcount(GCArena *a, int wantmarked)
{
  uint32_t w, wtop = arena_blockidx((GCCellID)a->celltop - 1);
  MSize n = 0;
  for (w = UnusedBlockWords; w <= wtop; w++) {
    GCBlockword bits = wantmarked ? (a->block[w] & a->mark[w]) : a->block[w];
    n += (MSize)__builtin_popcount(bits);
  }
  return n;
}

/*
** Prepare every arena for a fresh GC mark cycle: flush bins so the block
** map is authoritative, then clear all GC mark bits (so nothing reads as
** reachable until the collector marks it). After this an allocated object
** is White=(block=1,mark=0) and free space is (block=0,mark=0/1).
*/
void lj_arena_gcprepare(global_State *g)
{
  MSize i;
  for (i = 0; i < g->gc.arenastop; i++) {
    GCArena *a = mref(g->gc.arenas, GCArena *)[i];
    ArenaFreeList *fl = mref(a->freelist, ArenaFreeList);
    uint32_t w, wtop = arena_blockidx((GCCellID)a->celltop - 1);
    if (fl != NULL) {
      arena_flushbins(a, fl);
      freelist_reset(fl);
      fl->scavgen = a->freegen - 1;  /* Force rescan on next allocslow. */
    }
    /* Clear GC mark bits for allocated cells; free blocks keep mark=0 too. */
    for (w = UnusedBlockWords; w <= wtop; w++)
      a->mark[w] = 0;
  }
}

/*
** Initialize arenas for a mark-driven GC cycle. Flush bins so (block,mark)
** is authoritative, then clear mark bits only for allocated cells:
**   mark[w] &= ~block[w]
** Allocated objects become White=(1,0), ready for the mark phase to set
** their mark bits. Free blocks preserve Free=(0,1) state, so the allocator
** can continue servicing mutator allocations during incremental marking.
*/
void lj_arena_gc_markinit(global_State *g)
{
  MSize i;
  for (i = 0; i < g->gc.arenastop; i++) {
    GCArena *a = mref(g->gc.arenas, GCArena *)[i];
    ArenaFreeList *fl = mref(a->freelist, ArenaFreeList);
    uint32_t w, wtop = arena_blockidx((GCCellID)a->celltop - 1);
    if (fl != NULL) {
      arena_flushbins(a, fl);
      freelist_reset(fl);
      fl->scavgen = a->freegen - 1;
    }
    for (w = UnusedBlockWords; w <= wtop; w++)
      a->mark[w] &= ~a->block[w];
  }
}

MSize lj_arena_count_marked(global_State *g)
{
  MSize i, n = 0;
  for (i = 0; i < g->gc.arenastop; i++)
    n += arena_popcount(mref(g->gc.arenas, GCArena *)[i], 1);
  return n;
}

MSize lj_arena_count_allocated(global_State *g)
{
  MSize i, n = 0;
  for (i = 0; i < g->gc.arenastop; i++)
    n += arena_popcount(mref(g->gc.arenas, GCArena *)[i], 0);
  return n;
}
#endif

/* -- Huge blocks --------------------------------------------------------- */

/*
** Huge blocks are arena-aligned and arena-size-multiple OS allocations.
** No header and no metadata: the (rounded) size is reconstructed from
** the object size passed to free, so they are distinguished from arena
** objects purely by their alignment.
*/

void *lj_hugeblock_alloc(global_State *g, size_t size)
{
  size_t rsz = (size + ArenaCellMask) & ~(size_t)ArenaCellMask;
  void *p;
  if (LJ_UNLIKELY(rsz < size))  /* Overflow of the size rounding. */
    return NULL;
  p = arena_os_reserve(rsz);
  if (p == NULL)
    return NULL;
  if (!arena_os_commit(p, rsz)) {
    arena_os_release(p, rsz);
    return NULL;
  }
  lj_assertG_(g, lj_arena_ishuge(p), "huge block is not arena-aligned");
  g->gc.hugemem += (GCSize)rsz;
  g->gc.hugenum++;
  return p;
}

void lj_hugeblock_free(global_State *g, void *p, size_t size)
{
  size_t rsz = (size + ArenaCellMask) & ~(size_t)ArenaCellMask;
  lj_assertG_(g, g->gc.hugenum > 0, "huge block underflow");
  arena_os_release(p, rsz);
  g->gc.hugemem -= (GCSize)rsz;
  g->gc.hugenum--;
}

#endif

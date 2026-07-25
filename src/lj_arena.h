/*
** Arena-based allocator for GC objects.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
**
** Design based on the LuaJIT 3.0 new GC proposal (arenas, cells,
** block/mark bitmaps) and the gc/arenagc prototype branch.
**
** This implements only the allocator side: bump allocation, a
** segregated-fit free-list allocator and on-demand scavenging of the
** block map. Objects of at least half an arena are allocated as huge
** blocks, directly from the OS. The mark bitmap is reserved for a future
** arena-aware GC; the current linked-list GC is unaffected and frees
** objects through lj_mem_freegco().
*/

#ifndef _LJ_ARENA_H
#define _LJ_ARENA_H

#include "lj_obj.h"

#if LJ_HASGCARENA

/* -- Arena layout constants ---------------------------------------------- */

enum {
  ArenaSizeLog2 = 20,			/* 1 MB arenas. */
  ArenaSize = 1 << ArenaSizeLog2,
  ArenaCellMask = ArenaSize - 1,
  CellSizeLog2 = 4,			/* 16 byte cells. */
  CellSize = 1 << CellSizeLog2,
  ArenaMetadataSize = ArenaSize / 64,	/* 16 KB = 1.5% overhead. */

  MinCellId = ArenaMetadataSize / CellSize,	/* 1024. */
  MaxCellId = ArenaSize / CellSize,		/* 65536. */
  /* Top two cell ids are sacrificed: celltop fields are uint16_t. */
  MaxUsableCellId = MaxCellId - 2,
  ArenaUsableCells = MaxUsableCellId - MinCellId,

  BlocksetBits = 32,
  BlocksetMask = BlocksetBits - 1,
  MaxBlockWord = MaxCellId / BlocksetBits,	/* 2048 words = 8 KB/map. */
  /* Bitmap words covering the metadata cells double as the arena header. */
  UnusedBlockWords = MinCellId / BlocksetBits,	/* 32 words = 128 bytes. */

  /* Objects of at least this size are allocated as huge blocks. */
  ArenaHugeThreshold = ArenaSize >> 1,

  /* Segregated-fit allocator parameters. */
  ArenaBins = 8,			/* Bins for 1..8 cell free blocks. */
  ArenaRangeCap = 64,			/* Entries in the sorted range array. */

  /* Number of arena slots per reserved OS memory chunk. */
#if LJ_64
  ArenaChunkSlots = 16			/* 16 MB chunks. */
#else
  ArenaChunkSlots = 4			/* 4 MB chunks. */
#endif
};

/*
** Cell state encoding, derived from the first cell of each block:
**
**   +========+=======+======+
**   | State  | Block | Mark |
**   +========+=======+======+
**   | Extent |   0   |  0   |  extension of the preceding block
**   | Free   |   0   |  1   |  head of a free block
**   | White  |   1   |  0   |  allocated (unmarked) object
**   | Black  |   1   |  1   |  allocated marked object (future GC)
**   +========+=======+======+
*/
typedef enum CellState {
  CellState_Extent = 0,
  CellState_Free = 1,
  CellState_White = 2,
  CellState_Black = 3
} CellState;

typedef uint32_t GCBlockword;
typedef uint32_t GCCellID;
typedef uint16_t GCCellID1;

/* Arena flags. */
enum {
  ArenaFlag_TravObjs = 0x01,	/* Arena holds traversable objects. */
  ArenaFlag_PODOnly  = 0x02,	/* Arena holds only POD types (closures,
				** protos): no external backing, no globals,
				** no finalizers. Always set together with
				** ArenaFlag_TravObjs (POD objects are still
				** traversable). Enables word-parallel sweep. */
  ArenaFlag_InGrayHeap = 0x04,	/* Arena currently has an entry in the gray
				** priority heap (grayastack). Dedup guard:
				** arena_gray_push -> notify skips re-inserting
				** an arena already queued, so a 1-wide mark
				** frontier oscillating empty<->nonempty in one
				** arena does not churn the heap. Set on heap
				** insert, cleared on stale-root eviction and at
				** every wholesale grayastop=0 reset. Invariant:
				** set <=> the arena's index is in grayastack. */
  ArenaFlag_UdataOnly = 0x08,	/* Arena holds only userdata. Set together with
				** ArenaFlag_TravObjs (userdata are traversable).
				** Enables separateudata bitmap-scan enumeration. */
  ArenaFlag_CdataVOnly = 0x10	/* Arena holds only VLA/over-aligned cdata
				** (cdatav). Cell base is a GCcdataVar; GCobj is
				** at base + GCcdataVar.offset. NOT set with
				** ArenaFlag_TravObjs: cdata are collectable but
				** opaque leaves -- gc_mark does not traverse into
				** them, matching the NonTrav class where VLA cdata
				** live today. */
};

/*
** Arena allocation classes. The current arena per class is held in a
** dedicated GCState pointer (arena / travarena / podarena / udatarena /
** cdatavarena). The class determines the arena flags and which current
** pointer is updated.
*/
enum {
  ArenaClass_NonTrav = 0,	/* Non-traversable: strings, VLA cdata. */
  ArenaClass_Trav    = 1,	/* Traversable, mixed (tables, threads, ...). */
  ArenaClass_POD     = 2,	/* Traversable, POD-only (closures, protos). */
  ArenaClass_Udata   = 3,	/* Userdata-only traversable (GCudata). */
  ArenaClass_CdataV  = 4	/* VLA/over-aligned cdata only (GCcdataVar). */
};

/* A free block range in the sorted range array. */
typedef struct FreeCellRange {
  GCCellID1 id;			/* First cell of the free block. */
  GCCellID1 numcells;		/* Number of cells in the free block. */
} FreeCellRange;

/*
** Free list for the segregated-fit allocator. Lazily allocated.
**
** Small free blocks (<= ArenaBins cells) are kept in intrusive per-size
** lists: the first two bytes of a free block store the cell id of the
** next free block of that size (0 terminates; cell 0 is metadata and
** never a block). Binned blocks keep their *allocated* bitmap state, so
** the hot free/alloc cycle does not touch the bitmaps at all; only
** scavenging flushes the bins back into the block map. Larger blocks
** are flipped to the free bitmap state and tracked in a small sorted
** array; inserts into a full array drop the smallest entry, which the
** next scavenge rediscovers.
**
** Invariants:
**   block in a bin    <=> bitmap state White (looks allocated)
**   block in ranges    => bitmap state Free
**   bitmap state Free  => found by the next scavenge
*/
typedef struct ArenaFreeList {
  uint32_t binmask;		/* Which bins are non-empty. */
  uint32_t scavgen;		/* arena->freegen at the last scavenge. */
  uint32_t rangetop;		/* Number of entries in ranges[]. */
  uint8_t dropped;		/* Entries were dropped; a rescan may help. */
  uint8_t unused[3];
  GCCellID1 bins[ArenaBins];	/* Heads of intrusive free block lists. */
  FreeCellRange ranges[ArenaRangeCap];	/* Sorted by numcells, ascending. */
} ArenaFreeList;

/*
** Arena header, overlaid on the bitmap words that correspond to the
** metadata cells (cell ids 0..MinCellId-1 never hold objects, so the
** first UnusedBlockWords words of each bitmap are free for other use).
** The data area starts at cell MinCellId, directly after the two bitmaps.
*/
typedef union GCArena {
  struct {
    union {
      struct {	/* Overlaps mark[0..UnusedBlockWords-1]. */
	GCCellID1 celltop;	/* Bump allocator position (cell id). */
	GCCellID1 celltopmax;	/* Bump allocator limit. */
	uint16_t flags;		/* ArenaFlag_*. */
	uint16_t unused1;
 	MSize id;		/* Index in the arena registry. */
 	uint32_t freecells;	/* Total cells in free blocks. */
 	uint32_t freegen;	/* Incremented on every block free. */
 	uint32_t swept_gen;	/* Last sweep epoch (g->gc.epoch); other = (!= epoch). */
 	MRef freelist;		/* ArenaFreeList *, lazily allocated. */
 	MRef chunk;		/* ArenaChunk this arena was carved from. */
 	/* Per-arena gray stack for mark propagation (LJ_HASGCMARK). */
  	MRef greytop;		/* GCCellID1 *, next free slot. */
  	MRef greybase;		/* GCCellID1 *, buffer start. */
  	MRef greyend;		/* GCCellID1 *, buffer end (overflow check). */
       };
      GCBlockword mark[MaxBlockWord];
    };
    GCBlockword block[MaxBlockWord];
  };
} GCArena;

LJ_STATIC_ASSERT(sizeof(GCArena) == ArenaMetadataSize);

/* -- Address and bitmap primitives --------------------------------------- */

#define ptr2arena(p) \
  ((GCArena *)((uintptr_t)(p) & ~(uintptr_t)ArenaCellMask))
#define ptr2cell(p) \
  ((GCCellID)(((uintptr_t)(p) & ArenaCellMask) >> CellSizeLog2))
#define arena_cellptr(a, c) \
  ((void *)((char *)(a) + ((size_t)(c) << CellSizeLog2)))

#include "lj_asan.h"  /* ASAN poison/linkword helpers (no-op without ASAN). */

/* Huge blocks are arena-aligned; objects inside arenas never are, */
/* because the first MinCellId cells hold the arena metadata. */
#define lj_arena_ishuge(p)	(((uintptr_t)(p) & ArenaCellMask) == 0)

#define arena_blockidx(c)	((c) >> 5)
#define arena_blockbit(c)	(((GCBlockword)1) << ((c) & BlocksetMask))

#define arena_roundcells(size) \
  ((GCCellID)(((size) + (CellSize-1)) >> CellSizeLog2))

/* Open upvalues are arena-allocated (lj_mem_newagco). Guard that a GCupval
** stays a compact in-arena allocation (<= 4 cells) now that the former
** uvhead DLL prev/next fields are dead union members -- a future bloat of
** GCupval must not silently push it past a small cell count. */
LJ_STATIC_ASSERT(arena_roundcells(sizeof(GCupval)) <= 4);

static LJ_AINLINE CellState arena_cellstate(GCArena *a, GCCellID c)
{
  uint32_t shift = c & BlocksetMask;
  uint32_t b = (a->block[arena_blockidx(c)] >> shift) & 1u;
  uint32_t m = (a->mark[arena_blockidx(c)] >> shift) & 1u;
  return (CellState)((b << 1) | m);
}

#if LJ_HASGCMARK
/* -- GC mark bitmap primitives ------------------------------------------- */
/*
** These operate on the arena mark[] bitmap as the GC "reachable" bit:
** after lj_arena_flushbins() the (block,mark) pair is clean (allocated
** objects are White=(1,0), free blocks are Free=(0,1)), so setting the
** mark bit of an allocated block turns it Black=(1,1). They are defined
** for Phase 0 with independent unit tests but are not yet wired into the
** collector (that happens in Phase M/S).
*/

/* Mark an allocated object cell as reachable (White -> Black). */
static LJ_AINLINE void arena_obj_setmark(GCArena *a, GCCellID c)
{
  a->mark[arena_blockidx(c)] |= arena_blockbit(c);
}

/* Is the allocated object cell marked reachable? */
static LJ_AINLINE int arena_obj_ismarked(GCArena *a, GCCellID c)
{
  return (a->mark[arena_blockidx(c)] & arena_blockbit(c)) != 0;
}

/* Clear an object's mark bit (Black -> White), e.g. at end of sweep. */
static LJ_AINLINE void arena_obj_clearmark(GCArena *a, GCCellID c)
{
  a->mark[arena_blockidx(c)] &= ~arena_blockbit(c);
}

/*
** Shadow-mark an object as reachable, given any interior/object pointer.
** Skips huge blocks (no bitmap) and is a no-op for them; the caller must
** not pass non-arena objects (mainthread/strempty) -- those are filtered
** out by lj_gc_shadowmark() in lj_gc.c. Used by the Phase M shadow-verify
** pass while the header color is still authoritative.
*/
static LJ_AINLINE void arena_obj_shadowmark(void *o)
{
  if (!lj_arena_ishuge(o))
    arena_obj_setmark(ptr2arena(o), ptr2cell(o));
}
#endif

#if LJ_HASGCMARK
/* -- Per-arena gray stack ------------------------------------------------- */

/* Arena epoch: current(a) = (a->swept_gen == g->gc.epoch). Nursery arenas
** (born or swept this cycle) are current; dead test (isdead) is false for
** them. other(a) = !current(a); only other arenas have freeable objects. */
static LJ_AINLINE int arena_is_current(global_State *g, GCArena *a)
{
  return a->swept_gen == g->gc.epoch;
}

enum {
  ArenaGrayInitSize = 256	/* Initial gray stack capacity (entries). */
};

LJ_FUNC GCCellID1 *lj_arena_gray_grow(global_State *g, GCArena *a);
LJ_FUNC void lj_gc_grayarena_notify(global_State *g, MSize idx);

static LJ_AINLINE void arena_gray_reset(GCArena *a)
{
  setmref(a->greytop, mref(a->greybase, GCCellID1));
}

static LJ_AINLINE int arena_gray_empty(GCArena *a)
{
  return mref(a->greytop, GCCellID1) <= mref(a->greybase, GCCellID1);
}

static LJ_AINLINE void arena_gray_push(global_State *g, GCArena *a, GCCellID1 cellid)
{
  GCCellID1 *top = mref(a->greytop, GCCellID1);
  if (LJ_UNLIKELY(top == NULL || top >= mref(a->greyend, GCCellID1)))
    top = lj_arena_gray_grow(g, a);
  *top = cellid;
  setmref(a->greytop, top + 1);
  if (LJ_UNLIKELY(top == mref(a->greybase, GCCellID1)))
    lj_gc_grayarena_notify(g, (MSize)a->id);
}

static LJ_AINLINE GCCellID1 arena_gray_pop(GCArena *a)
{
  GCCellID1 *top = mref(a->greytop, GCCellID1);
  top--;
  setmref(a->greytop, top);
  return *top;
}
#endif

static LJ_AINLINE void *arena_alloc(GCArena *a, size_t size)
{
  GCCellID ncells = arena_roundcells(size);
  GCCellID c = a->celltop;
  if (LJ_UNLIKELY(c + ncells > (GCCellID)a->celltopmax)) {
    /* Exact-fit pop from the intrusive free lists, to keep the hot */
    /* alloc/free cycle of small objects out of the slow path once the */
    /* arena fills up. Binned blocks keep the allocated bitmap state, */
    /* so this path does not touch the bitmaps at all. */
    ArenaFreeList *fl = mref(a->freelist, ArenaFreeList);
    if (fl != NULL && ncells <= ArenaBins &&
	(fl->binmask & (1u << (ncells-1)))) {
      uint32_t b = ncells - 1;
      c = fl->bins[b];
      fl->bins[b] = arena_linkword_get(a, c);  /* poisoned free cell head */
      if (fl->bins[b] == 0) fl->binmask &= ~(1u << b);
      a->freecells -= ncells;
      /* Contract item 4: unpoison the mutator-owned block. */
      lj_asan_unpoison(arena_cellptr(a, c), (size_t)ncells << CellSizeLog2);
      return arena_cellptr(a, c);
    }
    return NULL;
  }
  a->celltop = (GCCellID1)(c + ncells);
  a->block[arena_blockidx(c)] |= arena_blockbit(c);
  /* Contract item 4: unpoison the freshly bumped mutator-owned block. */
  lj_asan_unpoison(arena_cellptr(a, c), (size_t)ncells << CellSizeLog2);
  return arena_cellptr(a, c);
}

/* -- Arena allocator API ------------------------------------------------- */

LJ_FUNC void *lj_arena_allocslow(global_State *g, GCArena *a, size_t size);
LJ_FUNC void lj_arena_freeblock(global_State *g, GCArena *a, void *p,
				size_t size);
LJ_FUNC void lj_arena_freerange(GCArena *a, ArenaFreeList *fl, GCCellID c,
				GCCellID n);
LJ_FUNC void *lj_arena_findspace(global_State *g, size_t size, int cls);
LJ_FUNC void lj_arena_shrink(global_State *g);
LJ_FUNC void lj_arena_freeall(global_State *g);

#if LJ_HASGCMARK
/*
** Flush all free-list bins back into the block map, so the (block,mark)
** pair becomes the single source of truth (allocated=White, free=Free).
** Must be called on each arena before a GC mark phase reads mark bits.
** Phase 0: implemented and unit-tested; not yet called by the collector.
*/
LJ_FUNC void lj_arena_flushbins(GCArena *a);

/*
** Visit each marked (reachable) or each unmarked (dead) allocated object
** in an arena, calling cb(cellptr, gct, ud) for matches. Used by the
** Phase S bitmap sweep to locate dead objects fast. Phase 0: skeleton
** with unit tests over a constructed bitmap; not yet wired into sweep.
*/
typedef void (*ArenaObjVisitor)(void *cellptr, int gct, void *ud);
LJ_FUNC void lj_arena_visit_unmarked(GCArena *a, ArenaObjVisitor cb, void *ud);

/*
** Phase M shadow-verify support. lj_arena_gcprepare() flushes bins and
** clears all GC mark bits across every arena, ready for a fresh mark
** cycle.
*/
LJ_FUNC void lj_arena_gcprepare(global_State *g);
LJ_FUNC void lj_arena_gc_markinit(global_State *g);
LJ_FUNC void lj_arena_gray_free(global_State *g, GCArena *a);
/* Word-parallel sweep of one POD-only arena. Returns cells freed. */
LJ_FUNC GCCellID lj_arena_podsweep(global_State *g, GCArena *a);
#endif

LJ_FUNC void *lj_hugeblock_alloc(global_State *g, size_t size);
LJ_FUNC void lj_hugeblock_free(global_State *g, void *p, size_t size);
LJ_FUNC void lj_hugeset_free(global_State *g);
/* Hugeset slot encoding. A slot is EMPTY (0), TOMB (1), or a live arena-aligned
** address with up to two flag bits OR'd into the low bits: MARK (bit 1),
** CDATAV (bit 2). The address bits survive PTRMASK; EMPTY/TOMB
** have no address bits. Huge blocks are ArenaSize (1MB = 2^20)-aligned, so
** their low 20 bits are zero -- bits 0-2 are free for the TOMB/MARK/CDATAV
** flags; bits 3-19 remain free for future slot tags. */
#define HUGESET_MARK	((uintptr_t)2)	/* Bit 1: reachable this GC cycle. */
/* Bit 2: slot base is a GCcdataVar prefix; the GCobj is at
** base + GCcdataVar.offset (a VLA cdata whose block went huge). The GC must
** translate base->cd at every consumer; mark/color authority stays keyed on
** the base address (the slot). */
#define HUGESET_CDATAV	((uintptr_t)4)
/* T3 removed HUGESET_SWEPT (bit 3): marks are now authoritative through the
** rebuild window, so a restart sees survivor MARK set and skips the dead-free
** branch without a separate processed-skip tag. Bit 3 is free. */
#define HUGESET_PTRMASK	(~(uintptr_t)15)	/* Strip TOMB|MARK|CDATAV to recover addr. */
#define hugeset_slot_addr(u)	((GCobj *)((u) & HUGESET_PTRMASK))
#define hugeset_slot_live(u)	(((u) & HUGESET_PTRMASK) != 0)
/* Return the GCobj carried by a hugeset slot. For a CDATAV slot the stored
** address is the block BASE (a GCcdataVar prefix); the actual GCobj (GCcdata)
** lives at base + GCcdataVar.offset. For every other slot the stored address
** IS the GCobj. Use this at any consumer that reads gct, recolors, or
** dispatches a freefunc on the object; keep using hugeset_slot_addr + the
** slot value for mark/COLOR authority (hugeset_find/huge_obj_* key on base). */
static LJ_AINLINE GCobj *hugeset_slot_obj(uintptr_t u)
{
  GCobj *base = hugeset_slot_addr(u);
#if LJ_HASFFI
  if (u & HUGESET_CDATAV)
    return (GCobj *)((char *)base + ((GCcdataVar *)base)->offset);
#endif
  return base;
}
/* Huge-object mark bit, stored in the hugeset slot (bit 1). The argument is the
** huge object's base address; the object must be registered and non-string. */
LJ_FUNC void huge_obj_setmark(global_State *g, void *p);
LJ_FUNC int huge_obj_ismarked(global_State *g, void *p);
LJ_FUNC void huge_obj_clearmark(global_State *g, void *p);
/* Mark a registered huge block base as carrying a VLA cdata prefix (CDATAV).
** Called by lj_cdata_newv after a huge allocation so the GC can recover the
** GCobj (cd = base + GCcdataVar.offset) when scanning the hugeset. */
LJ_FUNC void lj_huge_set_cdatav(global_State *g, void *p);

#endif

#endif

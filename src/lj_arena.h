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
  ArenaFlag_TravObjs = 0x01	/* Arena holds traversable objects. */
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

/* Huge blocks are arena-aligned; objects inside arenas never are, */
/* because the first MinCellId cells hold the arena metadata. */
#define lj_arena_ishuge(p)	(((uintptr_t)(p) & ArenaCellMask) == 0)

#define arena_blockidx(c)	((c) >> 5)
#define arena_blockbit(c)	(((GCBlockword)1) << ((c) & BlocksetMask))

#define arena_roundcells(size) \
  ((GCCellID)(((size) + (CellSize-1)) >> CellSizeLog2))

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
      fl->bins[b] = *(GCCellID1 *)arena_cellptr(a, c);
      if (fl->bins[b] == 0) fl->binmask &= ~(1u << b);
      a->freecells -= ncells;
      return arena_cellptr(a, c);
    }
    return NULL;
  }
  a->celltop = (GCCellID1)(c + ncells);
  a->block[arena_blockidx(c)] |= arena_blockbit(c);
  return arena_cellptr(a, c);
}

/* -- Arena allocator API ------------------------------------------------- */

LJ_FUNC void *lj_arena_allocslow(global_State *g, GCArena *a, size_t size);
LJ_FUNC void lj_arena_freeblock(global_State *g, GCArena *a, void *p,
				size_t size);
LJ_FUNC void lj_arena_freerange(GCArena *a, ArenaFreeList *fl, GCCellID c,
				GCCellID n);
LJ_FUNC void *lj_arena_findspace(global_State *g, size_t size, int trav);
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
#endif

LJ_FUNC void *lj_hugeblock_alloc(global_State *g, size_t size);
LJ_FUNC void lj_hugeblock_free(global_State *g, void *p, size_t size);

#endif

#endif

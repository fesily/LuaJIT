/*
** Garbage collector.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_GC_H
#define _LJ_GC_H

#include "lj_obj.h"
#if LJ_HASGCMARK
#include "lj_arena.h"
#endif

/* Garbage collector states. Order matters. */
enum {
  GCSpause, GCSpropagate, GCSatomic, GCSsweepstring, GCSsweep, GCSfinalize
};

/* Bitmasks for marked field of GCobj. */
#define LJ_GC_WHITE0	0x01
#if !LJ_HASGCMARK
#define LJ_GC_WHITE1	0x02
#define LJ_GC_BLACK	0x04	/* Classic tri-color header bit; unused under bitmap GC. */
#endif
#define LJ_GC_FINALIZED	0x08
#define LJ_GC_WEAKKEY	0x08
#define LJ_GC_WEAKVAL	0x10
#define LJ_GC_CDATA_FIN	0x10
#define LJ_GC_FIXED	0x20
#define LJ_GC_SFIXED	0x40
#if LJ_HASGCMARK
#define LJ_GC_GRAY	0x01	/* Inline gray bit (reuses WHITE0 slot). */

/* gcmarkflags bits in GCState. */
#define GCF_BITMAPSWEEP	0x01	/* Bitmap sweep active for this GC cycle. */
#define GCF_MARKALLOC	0x02	/* Allocate-black: mark new arena objects. */
/* T4 removed GCF_DEADAUTH (0x04): marks are now authoritative through the
** ENTIRE sweep+rebuild window (T3 stopped mid-yield MARK teardown, T1 keeps
** finalized-cdata MARK set), so the gc_obj_is* readers read raw marks under
** GCF_BITMAPSWEEP directly -- no separate death-authority gate needed. */
#endif

#if LJ_HASGCMARK
/* Bitmap GC: no header white bit. WHITES/COLORS are empty so any residual
** mask-out of the white bits compiles to a no-op; 0x02 is a free slot. */
#define LJ_GC_WHITES	0
#else
#define LJ_GC_WHITES	(LJ_GC_WHITE0 | LJ_GC_WHITE1)
#endif
#if LJ_HASGCMARK
#define LJ_GC_COLORS	0	/* No header white; BLACK lives in arena bitmap. */
#else
#define LJ_GC_COLORS	(LJ_GC_WHITES | LJ_GC_BLACK)
#endif
#define LJ_GC_WEAK	(LJ_GC_WEAKKEY | LJ_GC_WEAKVAL)

/*
** Color test/set macros.
**
** With LJ_HASGCMARK the next-gen collector will keep the white/black
** "reachable" state in the arena mark bitmap and an inline gray bit, so
** these macros become the single seam through which all modules touch
** object color. Phase 0 keeps the implementation byte-for-byte identical
** to the classic header-based tri-color scheme (zero behavior change);
** Phase M swaps the LJ_HASGCMARK branch to the bitmap representation
** without touching the many call sites across the tree.
*/
#if LJ_HASGCMARK
/* Bitmap GC: the header has no white bit and BLACK is arena-bitmap-resident, so
** raw header color tests are meaningless. They are POISONED: any expansion is a
** compile error naming the bitmap-aware replacement. Liveness/color MUST go
** through gc_obj_iswhite / gc_obj_isblack / gc_obj_isdead. isgray stays valid:
** the inline GRAY frontier bit is still header-resident. */
#define iswhite(x)	LJ_GC_POISON_iswhite__use_gc_obj_iswhite
#define isblack(x)	LJ_GC_POISON_isblack__use_gc_obj_isblack
#define isgray(x)	((x)->gch.marked & LJ_GC_GRAY)
#else
#define iswhite(x)	((x)->gch.marked & LJ_GC_WHITES)
#define isblack(x)	((x)->gch.marked & LJ_GC_BLACK)
#define isgray(x)	(!((x)->gch.marked & (LJ_GC_BLACK|LJ_GC_WHITES)))
#endif
#define tviswhite(x)	(tvisgcv(x) && iswhite(gcV(x)))
#if LJ_HASGCMARK
/* Bitmap GC: no currentwhite field, no atomic white flip, no header white bit.
** Liveness for arena/huge objects is the mark bitmap / hugeset slot; the two
** FIXED|SFIXED roots (mainthread, strempty) are constant-live. isdead is always
** false (gc_obj_isdead is the real seam). curwhite/otherwhite have no meaning
** here and are POISONED so any accidental expansion fails to compile. */
#define otherwhite(g)	LJ_GC_POISON_otherwhite__no_white_under_bitmap_gc
#define isdead(g, v)	(0)
#define curwhite(g)	LJ_GC_POISON_curwhite__no_white_under_bitmap_gc
#else
#define otherwhite(g)	((g)->gc.currentwhite ^ LJ_GC_WHITES)
#define isdead(g, v)	((v)->gch.marked & otherwhite(g) & LJ_GC_WHITES)
#define curwhite(g)	((g)->gc.currentwhite & LJ_GC_WHITES)
#endif
#if LJ_HASGCMARK
/* Bitmap GC: liveness is the arena mark / hugeset slot; the header carries no
** white bit (0x02 is free). A fresh object is light-gray (GRAY only). */
#define newwhite(g, x)	(obj2gco(x)->gch.marked = (uint8_t)LJ_GC_GRAY)
#else
#define newwhite(g, x)	(obj2gco(x)->gch.marked = (uint8_t)curwhite(g))
#endif
#if LJ_HASGCMARK
/* Pure white = clear the inline GRAY frontier bit. The header has no BLACK
** bit under bitmap GC (liveness lives in the arena mark / hugeset slot). */
#define makewhite(g, x) \
  ((void)(g), (x)->gch.marked &= (uint8_t)~LJ_GC_GRAY)
#else
#define makewhite(g, x) \
  ((x)->gch.marked = ((x)->gch.marked & (uint8_t)~LJ_GC_COLORS) | curwhite(g))
#endif
#if LJ_HASGCMARK
#define flipwhite(x)	((x)->gch.marked ^= LJ_GC_GRAY)
#else
#define flipwhite(x)	((x)->gch.marked ^= LJ_GC_WHITES)
#endif
#if LJ_HASGCMARK
#define black2gray(x)	((x)->gch.marked |= LJ_GC_GRAY)
#else
#define black2gray(x)	((x)->gch.marked &= (uint8_t)~LJ_GC_BLACK)
#endif
#define fixstring(s)	((s)->marked |= LJ_GC_FIXED)
#define markfinalized(x)	((x)->gch.marked |= LJ_GC_FINALIZED)

#if LJ_HASGCMARK
/* Arena-aware color seam. Phase 1 keeps the legacy header color bits coherent
** with the arena mark bitmap; later phases can make the bitmap authoritative
** for arena objects without changing call sites again.
**
** VLA cdata layout: [GCcdataVar][GCcdata cd][payload], allocated as one block
** at base p. The GCobj is cd = p + GCcdataVar.offset, INTERIOR to the block.
** The arena/huge address classification and the mark bitmap / hugeset slot key
** on the BLOCK BASE p = memcdatav(cd), not on cd (a huge VLA block is registered
** by p; ishuge(cd) is false because cd is not arena-aligned). gc_obj_key returns
** that base for a VLA cdata and o for every other object; header reads (gct,
** marked) and recolor macros (makewhite/flipwhite) still use o, which carries a
** valid GCcdata header. This unifies small (in-arena) and huge VLA cdata: both
** key on the cell base / hugeset base, matching the MARKALLOC mark site. */
static LJ_AINLINE void *gc_obj_key(GCobj *o)
{
#if LJ_HASFFI
  if (o->gch.gct == ~LJ_TCDATA && cdataisv((GCcdata *)o))
    return memcdatav((GCcdata *)o);
#endif
  return (void *)o;
}

static LJ_AINLINE int gc_obj_inarena(global_State *g, GCobj *o)
{
  return !lj_arena_ishuge(gc_obj_key(o)) && o != obj2gco(mainthread(g)) &&
	 o != obj2gco(&g->strempty);
}

/* Every huge object (string and non-string) carries white/black in its
** hugeset slot (bit 1), not the header. mainthread/strempty are dlmalloc, not
** huge. Huge strings are still unlinked from the intern table by gc_sweepstr,
** but their color authority is the slot. */
static LJ_AINLINE int gc_obj_inhugeset(global_State *g, GCobj *o)
{
  UNUSED(g);
  return lj_arena_ishuge(gc_obj_key(o));
}

static LJ_AINLINE int gc_obj_iswhite(global_State *g, GCobj *o)
{
  void *k = gc_obj_key(o);
  if (gc_obj_inarena(g, o))
    return !arena_obj_ismarked(ptr2arena(k), ptr2cell(k));
  if (gc_obj_inhugeset(g, o))
    return !huge_obj_ismarked(g, k);
  /* Reached only by the two dlmalloc FIXED|SFIXED roots (mainthread, strempty):
  ** they have no bitmap/slot and no header white bit under bitmap GC. Roots are
  ** never collectible, hence never white. */
  lj_assertG(o == obj2gco(mainthread(g)) || o == obj2gco(&g->strempty),
	     "non-arena/non-huge object is not a FIXED root: gct=%d", o->gch.gct);
  return 0;
}

static LJ_AINLINE int gc_obj_isblack(global_State *g, GCobj *o)
{
  void *k = gc_obj_key(o);
  if (gc_obj_inarena(g, o))
    return arena_obj_ismarked(ptr2arena(k), ptr2cell(k)) && !isgray(o);
  if (gc_obj_inhugeset(g, o))
    return huge_obj_ismarked(g, k) && !isgray(o);
  /* The two dlmalloc FIXED|SFIXED roots are permanently reachable: report black
  ** so barriers treat them as already-marked (never re-greyed via the header). */
  lj_assertG(o == obj2gco(mainthread(g)) || o == obj2gco(&g->strempty),
	     "non-arena/non-huge object is not a FIXED root: gct=%d", o->gch.gct);
  return !isgray(o);
}

#ifdef LUA_USE_ASSERT
/* Assert-only test hook: counts entries into the NEW non-sweep arena/huge
** branch of gc_obj_isdead -- i.e. an arena/huge object tested for death OUTSIDE
** the GCF_BITMAPSWEEP window, where the authoritative answer is "never dead".
** gc_obj_isdead is a header inline pulled into several TUs, so the counter and
** its accessor use weak linkage to collapse the per-TU copies into one symbol
** at link time. The accessor has default visibility (NOT LJ_FUNC, which is
** hidden on ELF and absent from .dynsym) so test_gc_obj_isdead_authority.lua
** resolves it via ffi.C -- mirrors lj_str_rehash_sweep_hits (lj_str.h). */
#if defined(_WIN32)
__declspec(selectany) uint32_t lj_gc_obj_isdead_nonsweep_counter = 0;
__declspec(dllexport) uint32_t lj_gc_obj_isdead_nonsweep_hits(void);
__declspec(selectany) uint32_t lj_gc_obj_isdead_nonsweep_hits(void)
{
  return lj_gc_obj_isdead_nonsweep_counter;
}
#else
__attribute__((weak)) uint32_t lj_gc_obj_isdead_nonsweep_counter = 0;
#if defined(__ELF__) || defined(__MACH__)
extern __attribute__((weak, visibility("default")))
       uint32_t lj_gc_obj_isdead_nonsweep_hits(void);
__attribute__((weak, visibility("default")))
uint32_t lj_gc_obj_isdead_nonsweep_hits(void)
{
  return lj_gc_obj_isdead_nonsweep_counter;
}
#else
extern __attribute__((weak)) uint32_t lj_gc_obj_isdead_nonsweep_hits(void);
__attribute__((weak)) uint32_t lj_gc_obj_isdead_nonsweep_hits(void)
{
  return lj_gc_obj_isdead_nonsweep_counter;
}
#endif
#endif
#endif

/* Mark-authoritative death test for ALL GC phases. An arena/huge object is
** dead only when (a) it is unmarked AND (b) the collector has reached the
** sweep window for it (GCF_BITMAPSWEEP). T4: the old GCF_DEADAUTH gate was
** removed -- marks are now authoritative through the ENTIRE sweep+rebuild
** window (T3 stopped mid-yield MARK teardown, T1 keeps finalized-cdata MARK
** set), so reading raw marks under GCF_BITMAPSWEEP is correct for both the
** bitmap-sweep and rebuild phases. Outside the sweep window -- in mark/pause,
** or outside any collection -- an arena/huge object is NEVER dead. Non-arena/
** non-huge objects are only the FIXED/SFIXED roots (mainthread, strempty),
** never collected, so the isdead fallback is the constant 0 too. */
static LJ_AINLINE int gc_obj_isdead(global_State *g, GCobj *o)
{
  void *k = gc_obj_key(o);
  if (gc_obj_inarena(g, o)) {
    if (g->gc.gcmarkflags & GCF_BITMAPSWEEP)
      return !arena_obj_ismarked(ptr2arena(k), ptr2cell(k));
#ifdef LUA_USE_ASSERT
    lj_gc_obj_isdead_nonsweep_counter++;
#endif
    return 0;  /* Outside the sweep window an arena object is never dead. */
  }
  if (gc_obj_inhugeset(g, o)) {
    if (g->gc.gcmarkflags & GCF_BITMAPSWEEP)
      return !huge_obj_ismarked(g, k);
#ifdef LUA_USE_ASSERT
    lj_gc_obj_isdead_nonsweep_counter++;
#endif
    return 0;  /* Outside the sweep window a huge object is never dead. */
  }
  return isdead(g, o) != 0;
}

static LJ_AINLINE void gc_obj_makewhite(global_State *g, GCobj *o)
{
  void *k = gc_obj_key(o);
  if (gc_obj_inarena(g, o)) {
    arena_obj_clearmark(ptr2arena(k), ptr2cell(k));
    makewhite(g, o);
  } else if (gc_obj_inhugeset(g, o)) {
    huge_obj_clearmark(g, k);
    makewhite(g, o);
  } else {
    makewhite(g, o);
  }
}

static LJ_AINLINE void gc_obj_resurrect(global_State *g, GCobj *o)
{
  void *k = gc_obj_key(o);
  if (gc_obj_inarena(g, o)) {
    arena_obj_setmark(ptr2arena(k), ptr2cell(k));
  } else if (gc_obj_inhugeset(g, o)) {
    huge_obj_setmark(g, k);
  } else {
    lj_assertG(0, "resurrect of a non-arena/non-huge FIXED root: gct=%d",
	       o->gch.gct);
  }
}
#else
#define gc_obj_iswhite(g, o)		(iswhite((o)) != 0)
#define gc_obj_isblack(g, o)		(isblack((o)) != 0)
#define gc_obj_isdead(g, o)		(isdead((g), (o)) != 0)
#define gc_obj_makewhite(g, o)		makewhite((g), (o))
#define gc_obj_resurrect(g, o)		flipwhite((o))
#endif

/* Collector. */
LJ_FUNC size_t lj_gc_separateudata(global_State *g, int all);
LJ_FUNC void lj_gc_finalize_udata(lua_State *L);
#if LJ_HASFFI
LJ_FUNC void lj_gc_finalize_cdata(lua_State *L);
#else
#define lj_gc_finalize_cdata(L)		UNUSED(L)
#endif
LJ_FUNC void lj_gc_freeall(global_State *g);
LJ_FUNCA int LJ_FASTCALL lj_gc_step(lua_State *L);
LJ_FUNCA void LJ_FASTCALL lj_gc_step_fixtop(lua_State *L);
#if LJ_HASJIT
LJ_FUNC int LJ_FASTCALL lj_gc_step_jit(global_State *g, MSize steps);
#endif
LJ_FUNC void lj_gc_fullgc(lua_State *L);
#if LJ_HASGCARENA
LJ_FUNC int lj_gc_checkheap(global_State *g);
LJ_FUNC void lj_gc_stats_push(lua_State *L);
LJ_FUNC void lj_gc_stats_reset(global_State *g);
#endif

/* GC check: drive collector forward if the GC threshold has been reached. */
#define lj_gc_check(L) \
  { if (LJ_UNLIKELY(G(L)->gc.total >= G(L)->gc.threshold)) \
      lj_gc_step(L); }
#define lj_gc_check_fixtop(L) \
  { if (LJ_UNLIKELY(G(L)->gc.total >= G(L)->gc.threshold)) \
      lj_gc_step_fixtop(L); }

/* Write barriers. */
LJ_FUNC void lj_gc_barrierf(global_State *g, GCobj *o, GCobj *v);
LJ_FUNCA void LJ_FASTCALL lj_gc_barrieruv(global_State *g, TValue *tv);
LJ_FUNC void lj_gc_closeuv(global_State *g, GCupval *uv);
#if LJ_HASJIT
LJ_FUNC void lj_gc_barriertrace(global_State *g, uint32_t traceno);
#endif
#if LJ_HASGCMARK
LJ_FUNC void lj_gc_barrierback_arena(global_State *g, GCobj *o);
LJ_FUNC void lj_gc_grayarena_notify(global_State *g, MSize idx);
LJ_FUNC void lj_gc_graywork_free(global_State *g);
LJ_FUNCA void lj_gc_ssb_flush(global_State *g);
#endif

/* Move the GC propagation frontier back for tables (make it gray again). */
static LJ_AINLINE void lj_gc_barrierback(global_State *g, GCtab *t)
{
  GCobj *o = obj2gco(t);
#if LJ_HASGCMARK
  lj_assertG(!(o->gch.marked & LJ_GC_GRAY) && !gc_obj_isdead(g, o),
	     "bad object states for backward barrier");
  lj_gc_barrierback_arena(g, o);
#else
  lj_assertG(isblack(o) && !isdead(g, o),
	     "bad object states for backward barrier");
  lj_assertG(g->gc.state != GCSfinalize && g->gc.state != GCSpause,
	     "bad GC state");
  black2gray(o);
  setgcrefr(t->gclist, g->gc.grayagain);
  setgcref(g->gc.grayagain, o);
#endif
}

/* Barrier for stores to table objects. TValue and GCobj variant. */
#if LJ_HASGCMARK
#define lj_gc_anybarriert(L, t)  \
  { if (LJ_UNLIKELY(!(obj2gco(t)->gch.marked & LJ_GC_GRAY))) \
      lj_gc_barrierback(G(L), (t)); }
#define lj_gc_barriert(L, t, tv) \
  { if (LJ_UNLIKELY(!(obj2gco(t)->gch.marked & LJ_GC_GRAY))) \
      lj_gc_barrierback(G(L), (t)); }
#define lj_gc_objbarriert(L, t, o)  \
  { if (LJ_UNLIKELY(!(obj2gco(t)->gch.marked & LJ_GC_GRAY))) \
      lj_gc_barrierback(G(L), (t)); }
#else
#define lj_gc_anybarriert(L, t)  \
  { if (LJ_UNLIKELY(isblack(obj2gco(t)))) lj_gc_barrierback(G(L), (t)); }
#define lj_gc_barriert(L, t, tv) \
  { if (tviswhite(tv) && isblack(obj2gco(t))) \
      lj_gc_barrierback(G(L), (t)); }
#define lj_gc_objbarriert(L, t, o)  \
  { if (iswhite(obj2gco(o)) && isblack(obj2gco(t))) \
      lj_gc_barrierback(G(L), (t)); }
#endif

/* Barrier for stores to any other object. TValue and GCobj variant. */
#if LJ_HASGCMARK
#define lj_gc_barrier(L, p, tv) \
  { if (LJ_UNLIKELY(!(obj2gco(p)->gch.marked & LJ_GC_GRAY))) \
      lj_gc_barrierf(G(L), obj2gco(p), gcV(tv)); }
#define lj_gc_objbarrier(L, p, o) \
  { if (LJ_UNLIKELY(!(obj2gco(p)->gch.marked & LJ_GC_GRAY))) \
      lj_gc_barrierf(G(L), obj2gco(p), obj2gco(o)); }
#else
#define lj_gc_barrier(L, p, tv) \
  { if (tviswhite(tv) && isblack(obj2gco(p))) \
      lj_gc_barrierf(G(L), obj2gco(p), gcV(tv)); }
#define lj_gc_objbarrier(L, p, o) \
  { if (iswhite(obj2gco(o)) && isblack(obj2gco(p))) \
      lj_gc_barrierf(G(L), obj2gco(p), obj2gco(o)); }
#endif

/* Allocator. */
LJ_FUNC void *lj_mem_realloc(lua_State *L, void *p, GCSize osz, GCSize nsz);
LJ_FUNC void * LJ_FASTCALL lj_mem_newgco(lua_State *L, GCSize size);
LJ_FUNC void *lj_mem_grow(lua_State *L, void *p,
			  MSize *szp, MSize lim, MSize esz);

#define lj_mem_new(L, s)	lj_mem_realloc(L, NULL, 0, (s))

static LJ_AINLINE void lj_mem_free(global_State *g, void *p, size_t osize)
{
  g->gc.total -= (GCSize)osize;
  g->allocf(g->allocd, p, osize, 0);
}

/*
** GC object allocation. GC objects are segregated from other memory:
** with LJ_HASGCARENA they are placed in arenas (lj_arena.h), otherwise
** they share the lua_Alloc allocator with everything else.
**
**   lj_mem_newgco   non-traversable object, linked to the GC root list.
**   lj_mem_newgcot  traversable object, linked to the GC root list.
**   lj_mem_newagco  unlinked object; the caller manages the nextgc chain.
**   lj_mem_freegco  free any GC object (the inverse of all of the above).
*/
#if LJ_HASGCARENA
#include "lj_arena.h"

LJ_FUNC void *lj_mem_newgco_slow(lua_State *L, GCSize size, int cls,
				 int link);

/*
** Inline fast path: bump-allocate from the current arena.
** cls (ArenaClass_*) and link are compile-time constants at all call sites,
** so the class->pointer selection folds to a single load.
*/
static LJ_AINLINE void *lj_mem_newgco_arena(lua_State *L, GCSize size,
					    int cls, int link)
{
  global_State *g = G(L);
  if (LJ_LIKELY(size < ArenaHugeThreshold)) {
    GCArena *a = mref(cls == ArenaClass_POD ? g->gc.podarena :
		      cls == ArenaClass_Trav ? g->gc.travarena :
		      cls == ArenaClass_Udata ? g->gc.udatarena :
		      cls == ArenaClass_CdataV ? g->gc.cdatavarena :
		      g->gc.arena,
		      GCArena);
    GCobj *o = a ? (GCobj *)arena_alloc(a, size) : NULL;
    if (LJ_LIKELY(o != NULL)) {
      /* POD arenas account cell-space bytes (roundcells*CellSize), not the
      ** requested size, so the word-parallel sweep can subtract freed memory
      ** purely from the bitmap cell delta without reading any object header.
      ** The matching cell-space subtraction is in lj_mem_freegco_ (per-object
      ** path) and the POD branch of gc_bitmap_sweep (bulk path); all three
      ** stay balanced for the shutdown total assertion. */
      g->gc.total += (cls == ArenaClass_POD) ?
		     ((GCSize)arena_roundcells(size) << CellSizeLog2) : size;
#if LJ_HASGCMARK
      if (LJ_UNLIKELY(g->gc.gcmarkflags & GCF_MARKALLOC))
	arena_obj_setmark(a, ptr2cell(o));
#endif
      if (link)
	newwhite(g, o);
      return o;
    }
  }
  return lj_mem_newgco_slow(L, size, cls, link);
}

/* Free a GC object allocated by lj_mem_newgco_arena(). */
static LJ_AINLINE void lj_mem_freegco_(global_State *g, void *p, size_t osize)
{
  if (LJ_LIKELY(!lj_arena_ishuge(p))) {
    GCArena *a = ptr2arena(p);
    GCCellID c = ptr2cell(p);
    GCCellID n = arena_roundcells(osize);
    ArenaFreeList *fl = mref(a->freelist, ArenaFreeList);
    /* POD arenas account cell-space (see lj_mem_newgco_arena); everything
    ** else accounts the requested size. */
    g->gc.total -= (a->flags & ArenaFlag_PODOnly) ?
		   ((GCSize)n << CellSizeLog2) : (GCSize)osize;
    a->freegen++;
    if (c + n == (GCCellID)a->celltop) {  /* Roll back the bump frontier. */
      a->block[arena_blockidx(c)] &= ~arena_blockbit(c);
      a->mark[arena_blockidx(c)] &= ~arena_blockbit(c);
      a->celltop = (GCCellID1)c;
      return;
    }
    a->freecells += n;
    if (fl != NULL && n <= ArenaBins) {
      /* Push onto the intrusive per-size free list. The block keeps its */
      /* allocated bitmap state, so no bitmap access here at all. */
      uint32_t b = n - 1;
      *(GCCellID1 *)p = fl->bins[b];
      fl->bins[b] = (GCCellID1)c;
      fl->binmask |= 1u << b;
      return;
    }
    if (fl != NULL) {
      lj_arena_freerange(a, fl, c, n);
    } else {
      /* No free list yet: flip to the Free bitmap state; the first */
      /* slow-path allocation scavenges these blocks into a new list. */
      a->block[arena_blockidx(c)] &= ~arena_blockbit(c);
      a->mark[arena_blockidx(c)] |= arena_blockbit(c);
    }
  } else {
    g->gc.total -= (GCSize)osize;
    lj_hugeblock_free(g, p, osize);
  }
}

#define lj_mem_newgcot(L, s) \
  lj_mem_newgco_arena(L, (GCSize)(s), ArenaClass_Trav, 1)
#define lj_mem_newagco(L, s, trav) \
  lj_mem_newgco_arena(L, (GCSize)(s), \
		      (trav) ? ArenaClass_Trav : ArenaClass_NonTrav, 0)
/* Userdata: dedicated ArenaClass_Udata arena, NOT linked to g->gc.root
** (udata are enumerated by separateudata via the udata-arena bitmaps +
** hugeset, not the root nextgc chain). link=0 keeps nextgc untouched. */
#define lj_mem_newgcou(L, s) \
  lj_mem_newgco_arena(L, (GCSize)(s), ArenaClass_Udata, 0)
/* VLA/over-aligned cdata: dedicated ArenaClass_CdataV arena, NOT linked
** to any nextgc chain (VLA cdata are enumerated by the rebuild prologue
** CdataV-arena bitmap scan + hugeset, not a chain). link=0 keeps nextgc
** untouched. lj_mem_newgco_arena routes size >= ArenaHugeThreshold
** to the hugeblock path, so a single call handles both small (CdataV arena)
** and huge (hugeset + lj_huge_set_cdatav) VLA cdata. */
#define lj_mem_newgcocv(L, s) \
  lj_mem_newgco_arena(L, (GCSize)(s), ArenaClass_CdataV, 0)
/* POD-only traversable allocation (closures, protos): routed to the POD
** arena so the word-parallel sweep can reclaim it without per-object frees. */
#define lj_mem_newgcot_pod(L, s) \
  lj_mem_newgco_arena(L, (GCSize)(s), ArenaClass_POD, 1)
#define lj_mem_freegco(g, p, s)	lj_mem_freegco_(g, (p), (s))
#else
#define lj_mem_newgcot(L, s)	lj_mem_newgco(L, (GCSize)(s))
#define lj_mem_newgcot_pod(L, s)	lj_mem_newgco(L, (GCSize)(s))
#define lj_mem_newagco(L, s, trav)  lj_mem_new(L, (GCSize)(s))
#define lj_mem_newgcou(L, s)	lj_mem_new(L, (GCSize)(s))
#define lj_mem_newgcocv(L, s)	lj_mem_new(L, (GCSize)(s))
#define lj_mem_freegco(g, p, s)	lj_mem_free(g, (p), (s))
#endif

#define lj_mem_newvec(L, n, t)	((t *)lj_mem_new(L, (GCSize)((n)*sizeof(t))))
#define lj_mem_reallocvec(L, p, on, n, t) \
  ((p) = (t *)lj_mem_realloc(L, p, (on)*sizeof(t), (GCSize)((n)*sizeof(t))))
#define lj_mem_growvec(L, p, n, m, t) \
  ((p) = (t *)lj_mem_grow(L, (p), &(n), (m), (MSize)sizeof(t)))
#define lj_mem_freevec(g, p, n, t)	lj_mem_free(g, (p), (n)*sizeof(t))

#define lj_mem_newobj(L, t)	((t *)lj_mem_newgcot(L, sizeof(t)))
#define lj_mem_newt(L, s, t)	((t *)lj_mem_new(L, (s)))
#define lj_mem_freet(g, p)	lj_mem_free(g, (p), sizeof(*(p)))

#endif

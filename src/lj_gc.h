/*
** Garbage collector.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_GC_H
#define _LJ_GC_H

#include "lj_obj.h"

/* Garbage collector states. Order matters. */
enum {
  GCSpause, GCSpropagate, GCSatomic, GCSsweepstring, GCSsweep, GCSfinalize
};

/* Bitmasks for marked field of GCobj. */
#define LJ_GC_WHITE0	0x01
#define LJ_GC_WHITE1	0x02
#define LJ_GC_BLACK	0x04	/* Non-arena only under LJ_HASGCMARK. */
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
#endif

#if LJ_HASGCMARK
#define LJ_GC_WHITES	LJ_GC_WHITE1	/* Single-white under bitmap GC. */
#else
#define LJ_GC_WHITES	(LJ_GC_WHITE0 | LJ_GC_WHITE1)
#endif
#if LJ_HASGCMARK
#define LJ_GC_COLORS	LJ_GC_WHITES	/* BLACK is in arena bitmap, not header. */
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
#define iswhite(x)	((x)->gch.marked & LJ_GC_WHITES)
#define isblack(x)	(!((x)->gch.marked & LJ_GC_GRAY) && !((x)->gch.marked & LJ_GC_WHITES))
#define isgray(x)	((x)->gch.marked & LJ_GC_GRAY)
#else
#define iswhite(x)	((x)->gch.marked & LJ_GC_WHITES)
#define isblack(x)	((x)->gch.marked & LJ_GC_BLACK)
#define isgray(x)	(!((x)->gch.marked & (LJ_GC_BLACK|LJ_GC_WHITES)))
#endif
#define tviswhite(x)	(tvisgcv(x) && iswhite(gcV(x)))
#define otherwhite(g)	((g)->gc.currentwhite ^ LJ_GC_WHITES)
#define isdead(g, v)	((v)->gch.marked & otherwhite(g) & LJ_GC_WHITES)

#define curwhite(g)	((g)->gc.currentwhite & LJ_GC_WHITES)
#if LJ_HASGCMARK
#define newwhite(g, x)	(obj2gco(x)->gch.marked = (uint8_t)(curwhite(g) | LJ_GC_GRAY))
#else
#define newwhite(g, x)	(obj2gco(x)->gch.marked = (uint8_t)curwhite(g))
#endif
#if LJ_HASGCMARK
#define makewhite(g, x) \
  ((x)->gch.marked = ((x)->gch.marked & (uint8_t)~(LJ_GC_COLORS|LJ_GC_GRAY)) \
                      | curwhite(g))
#else
#define makewhite(g, x) \
  ((x)->gch.marked = ((x)->gch.marked & (uint8_t)~LJ_GC_COLORS) | curwhite(g))
#endif
#define flipwhite(x)	((x)->gch.marked ^= LJ_GC_WHITES)
#if LJ_HASGCMARK
#define black2gray(x)	((x)->gch.marked |= LJ_GC_GRAY)
#else
#define black2gray(x)	((x)->gch.marked &= (uint8_t)~LJ_GC_BLACK)
#endif
#define fixstring(s)	((s)->marked |= LJ_GC_FIXED)
#define markfinalized(x)	((x)->gch.marked |= LJ_GC_FINALIZED)

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
  lj_assertG(!(o->gch.marked & LJ_GC_GRAY) && !isdead(g, o),
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
		      cls == ArenaClass_Trav ? g->gc.travarena : g->gc.arena,
		      GCArena);
    GCobj *o = a ? (GCobj *)arena_alloc(a, size) : NULL;
    if (LJ_LIKELY(o != NULL)) {
      g->gc.total += size;
#if LJ_HASGCMARK
      if (LJ_UNLIKELY(g->gc.gcmarkflags & GCF_MARKALLOC))
	arena_obj_setmark(a, ptr2cell(o));
#endif
      if (link) {
#if LJ_HASGCMARK
	if (LJ_UNLIKELY(g->gc.gcmarkflags & GCF_BITMAPSWEEP)) {
	  newwhite(g, o);
	} else
#endif
	{
	  setgcrefr(o->gch.nextgc, g->gc.root);
	  setgcref(g->gc.root, o);
	  newwhite(g, o);
	}
      }
      return o;
    }
  }
  return lj_mem_newgco_slow(L, size, cls, link);
}

/* Free a GC object allocated by lj_mem_newgco_arena(). */
static LJ_AINLINE void lj_mem_freegco_(global_State *g, void *p, size_t osize)
{
  g->gc.total -= (GCSize)osize;
  if (LJ_LIKELY(!lj_arena_ishuge(p))) {
    GCArena *a = ptr2arena(p);
    GCCellID c = ptr2cell(p);
    GCCellID n = arena_roundcells(osize);
    ArenaFreeList *fl = mref(a->freelist, ArenaFreeList);
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
    lj_hugeblock_free(g, p, osize);
  }
}

#define lj_mem_newgcot(L, s) \
  lj_mem_newgco_arena(L, (GCSize)(s), ArenaClass_Trav, 1)
#define lj_mem_newagco(L, s, trav) \
  lj_mem_newgco_arena(L, (GCSize)(s), \
		      (trav) ? ArenaClass_Trav : ArenaClass_NonTrav, 0)
/* POD-only traversable allocation (closures, protos): routed to the POD
** arena so the word-parallel sweep can reclaim it without per-object frees. */
#define lj_mem_newgcot_pod(L, s) \
  lj_mem_newgco_arena(L, (GCSize)(s), ArenaClass_POD, 1)
#define lj_mem_freegco(g, p, s)	lj_mem_freegco_(g, (p), (s))
#else
#define lj_mem_newgcot(L, s)	lj_mem_newgco(L, (GCSize)(s))
#define lj_mem_newagco(L, s, trav)  lj_mem_new(L, (GCSize)(s))
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

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
#define LJ_GC_BLACK	0x04
#define LJ_GC_FINALIZED	0x08
#define LJ_GC_WEAKKEY	0x08
#define LJ_GC_WEAKVAL	0x10
#define LJ_GC_CDATA_FIN	0x10
#define LJ_GC_FIXED	0x20
#define LJ_GC_SFIXED	0x40
#define LJ_GC_LOGGED	0x80	/* ConcGC: in store buffer this cycle. */
				/* NOTE: 0x80 aliases cdataisv() -- LOGGED
				** must never be set on cdata (leaf type,
				** never a barrier parent). */

#define LJ_GC_WHITES	(LJ_GC_WHITE0 | LJ_GC_WHITE1)
#define LJ_GC_COLORS	(LJ_GC_WHITES | LJ_GC_BLACK)
#define LJ_GC_WEAK	(LJ_GC_WEAKKEY | LJ_GC_WEAKVAL)

/* Macros to test and set GCobj colors.
** With LJ_CONCGC all marked-byte reads on potentially-shared objects are
** relaxed atomic loads (plain mov on x86): the GC thread RMWs the same
** byte concurrently and C11 requires atomic access to avoid UB. Stale
** values are fine -- see the protocol notes in lj_gcconc.h.
*/
#if LJ_CONCGC
#include "lj_atomic.h"
#include "lj_gcconc.h"
#define gcmarked(x)	(lj_atomic_load8((const uint8_t *)&(x)->gch.marked))
#else
#define gcmarked(x)	((x)->gch.marked)
#endif
#define iswhite(x)	(gcmarked(x) & LJ_GC_WHITES)
#define isblack(x)	(gcmarked(x) & LJ_GC_BLACK)
#define isgray(x)	(!(gcmarked(x) & (LJ_GC_BLACK|LJ_GC_WHITES)))
#define tviswhite(x)	(tvisgcv(x) && iswhite(gcV(x)))
#define otherwhite(g)	(g->gc.currentwhite ^ LJ_GC_WHITES)
#define isdead(g, v)	(gcmarked(v) & otherwhite(g) & LJ_GC_WHITES)

#define curwhite(g)	((g)->gc.currentwhite & LJ_GC_WHITES)
#if LJ_CONCGC
/* Object birth/recoloring races with the marker's atomic marked-byte loads
** in gc_traverse_*. Pair the store with relaxed atomics so TSAN sees a
** consistent contract; x86 emits the same plain MOV either way.
*/
#define newwhite(g, x) \
  lj_atomic_store8(&obj2gco(x)->gch.marked, (uint8_t)curwhite(g))
#define makewhite(g, x) do { \
    uint8_t _m = lj_atomic_load8(&(x)->gch.marked); \
    lj_atomic_store8(&(x)->gch.marked, \
		     (uint8_t)((_m & (uint8_t)~LJ_GC_COLORS) | curwhite(g))); \
  } while (0)
#else
#define newwhite(g, x)	(obj2gco(x)->gch.marked = (uint8_t)curwhite(g))
#define makewhite(g, x) \
  ((x)->gch.marked = ((x)->gch.marked & (uint8_t)~LJ_GC_COLORS) | curwhite(g))
#endif
#define flipwhite(x)	((x)->gch.marked ^= LJ_GC_WHITES)
#define black2gray(x)	((x)->gch.marked &= (uint8_t)~LJ_GC_BLACK)
#define markfinalized(x)	((x)->gch.marked |= LJ_GC_FINALIZED)

/* Concurrent GC. */
#if LJ_CONCGC
#define lj_gc_cmark(g)		((g)->gc.cmark)
/* The GC thread RMWs colors in the same byte: flag updates the mutator
** performs on live objects during concurrent marking must be atomic.
*/
#define fixstring(s)		(lj_atomic_or8(&(s)->marked, LJ_GC_FIXED))
#define lj_gc_markedor(g, o, bits) \
  { if (LJ_UNLIKELY((g)->gc.cmark)) lj_atomic_or8(&(o)->marked, (bits)); \
    else (o)->marked |= (bits); }
#define lj_gc_markedand(g, o, bits) \
  { if (LJ_UNLIKELY((g)->gc.cmark)) lj_atomic_and8(&(o)->marked, (bits)); \
    else (o)->marked &= (bits); }
LJ_FUNC int lj_gc_setconcmode(lua_State *L, int enable);
#else
#define lj_gc_cmark(g)		0
#define fixstring(s)		((s)->marked |= LJ_GC_FIXED)
#define lj_gc_markedor(g, o, bits)	((o)->marked |= (bits))
#define lj_gc_markedand(g, o, bits)	((o)->marked &= (bits))
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

/*
** Write barrier trigger. During concurrent marking the mutator may read
** stale colors. Colors only darken within a cycle, so a stale read is
** whiter than reality: child iswhite() checks stay conservative-safe,
** but parent isblack() can miss. Under cmark the parent trigger becomes
** "not yet logged this epoch" (LJ_GC_LOGGED is mutator-private).
*/
#define lj_gc_needbarrier(g, o) \
  (LJ_UNLIKELY(lj_gc_cmark(g)) ? \
   !(gcmarked(o) & LJ_GC_LOGGED) : isblack(o))

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

/* Move the GC propagation frontier back for tables (make it gray again).
** Under concurrent marking: never touch colors (GC thread owns them);
** push the table onto the SPSC log ring, deduplicated by LJ_GC_LOGGED.
** The atomic or is required because the GC thread concurrently RMWs
** color bits in the same byte.
*/
static LJ_AINLINE void lj_gc_barrierback(global_State *g, GCtab *t)
{
  GCobj *o = obj2gco(t);
#if LJ_CONCGC
  if (LJ_UNLIKELY(g->gc.cmark)) {
    if (!(gcmarked(o) & LJ_GC_LOGGED)) {
      ConcGCState *cs = concgcstate(g);
      lj_atomic_or8(&o->gch.marked, LJ_GC_LOGGED);
      if (LJ_UNLIKELY(!lj_concgc_ringpush(cs, o)))
	lj_concgc_logfull(g, o);
    }
    return;
  }
#endif
  lj_assertG(isblack(o) && !isdead(g, o),
	     "bad object states for backward barrier");
  lj_assertG(g->gc.state != GCSfinalize && g->gc.state != GCSpause,
	     "bad GC state");
  black2gray(o);
  setgcrefr(t->gclist, g->gc.grayagain);
  setgcref(g->gc.grayagain, o);
}

/* Barrier for stores to table objects. TValue and GCobj variant. */
#define lj_gc_anybarriert(L, t)  \
  { if (LJ_UNLIKELY(lj_gc_needbarrier(G(L), obj2gco(t)))) \
      lj_gc_barrierback(G(L), (t)); }
#define lj_gc_barriert(L, t, tv) \
  { if (tviswhite(tv) && lj_gc_needbarrier(G(L), obj2gco(t))) \
      lj_gc_barrierback(G(L), (t)); }
#define lj_gc_objbarriert(L, t, o)  \
  { if (iswhite(obj2gco(o)) && lj_gc_needbarrier(G(L), obj2gco(t))) \
      lj_gc_barrierback(G(L), (t)); }

/* Barrier for stores to any other object. TValue and GCobj variant. */
#define lj_gc_barrier(L, p, tv) \
  { if (tviswhite(tv) && lj_gc_needbarrier(G(L), obj2gco(p))) \
      lj_gc_barrierf(G(L), obj2gco(p), gcV(tv)); }
#define lj_gc_objbarrier(L, p, o) \
  { if (iswhite(obj2gco(o)) && lj_gc_needbarrier(G(L), obj2gco(p))) \
      lj_gc_barrierf(G(L), obj2gco(p), obj2gco(o)); }

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

#define lj_mem_newvec(L, n, t)	((t *)lj_mem_new(L, (GCSize)((n)*sizeof(t))))
#define lj_mem_reallocvec(L, p, on, n, t) \
  ((p) = (t *)lj_mem_realloc(L, p, (on)*sizeof(t), (GCSize)((n)*sizeof(t))))
#define lj_mem_growvec(L, p, n, m, t) \
  ((p) = (t *)lj_mem_grow(L, (p), &(n), (m), (MSize)sizeof(t)))
#define lj_mem_freevec(g, p, n, t)	lj_mem_free(g, (p), (n)*sizeof(t))

#define lj_mem_newobj(L, t)	((t *)lj_mem_newgco(L, sizeof(t)))
#define lj_mem_newt(L, s, t)	((t *)lj_mem_new(L, (s)))
#define lj_mem_freet(g, p)	lj_mem_free(g, (p), sizeof(*(p)))

#endif

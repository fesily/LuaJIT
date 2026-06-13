/*
** Concurrent GC: marker thread, handshake protocol, store log.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_GCCONC_H
#define _LJ_GCCONC_H

#include "lj_obj.h"

#if LJ_CONCGC

#include <pthread.h>

/* GC thread phase. Changed only under ConcGCState.lock. */
enum { CONCGC_IDLE, CONCGC_MARK, CONCGC_EXIT };

/* Growable pointer vector, single-owner, malloc'ed (not GC-accounted). */
typedef struct ConcVec {
  GCobj **p;
  MSize n, sz;
} ConcVec;

/*
** Ownership while concurrent marking runs (g->gc.cmark != 0):
**
**   Object color/weak bits of marked     GC thread (atomic RMW)
**   LJ_GC_LOGGED bit of marked           mutator (atomic RMW)
**   gc.grayagain and all gclist fields   mutator (the store log)
**   jobs/threadv/weakv/uvv vectors       GC thread
**   ssb vector                           mutator (gclist-less parents)
**   gc.gray, gc.weak                     unused (empty)
**   gc.root list, allocator, gc.total    mutator
**
** Either side touches the other's state only while the GC thread is
** parked. The GC thread parks only between two object traversals, when
** it holds no interior pointers. The park/resume mutex fully orders
** memory at the boundary.
**
** The mutator runs without fences. Colors only darken during marking
** (white -> gray -> black; the white flip happens single-threaded in
** the atomic phase), so a stale color read shows an older = whiter
** state. Child iswhite() checks in the barrier macros are therefore
** conservative-safe, but parent isblack() checks can miss. Under cmark
** the parent trigger becomes "not yet LOGGED"; a triggered barrier
** never touches colors, it only logs the parent (gc.grayagain via
** gclist, or the ssb vector for upvalues/userdata). LOGGED decisions
** are mutator-only and thus exact. Both sides RMW the shared marked
** byte atomically, so neither loses the other's bits.
**
** Draining (mutator, GC thread parked) clears LOGGED and requeues
** logged black parents for re-traversal; stores racing with the
** re-traversal log the parent again. The final drain runs single-
** threaded before the atomic phase, where colors are exact, so marking
** converges. Drains happen only inside lj_gc_step: the JIT's TBAR
** elimination (gcstep_barrier in lj_opt_fold.c) assumes a logged table
** stays logged between two GC steps.
**
** The GC thread never reads Lua stacks: threads are deferred to the
** atomic phase (threadv), open upvalue values live in stacks and are
** re-marked by gc_mark_uv in the atomic phase, and closed upvalue
** values are deferred to the finish step (uvv) -- which also makes the
** unmodified USETV/USETS barrier asm safe under stale color reads.
**
** Interior buffers the GC thread does read through object headers
** (table array/node parts, the J->trace vector) are never freed or
** moved concurrently: the reallocating code is bracketed with
** lj_concgc_pause_begin/end. An error escaping such a bracket unwinds
** through lj_err_throw, which calls lj_concgc_unpause.
*/
typedef struct ConcGCState {
  global_State *g;
  pthread_t thread;
  pthread_mutex_t lock;
  pthread_cond_t wake_gc;	/* Mutator -> GC thread. */
  pthread_cond_t wake_mut;	/* GC thread -> mutator. */
  volatile uint32_t parkreq;	/* Mutator requests GC thread to park. */
  uint32_t parked;		/* GC thread is parked/idle (under lock). */
  uint32_t phase;		/* CONCGC_IDLE/CONCGC_MARK/CONCGC_EXIT. */
  volatile uint32_t markdone;	/* GC thread ran out of gray objects. */
  uint32_t threadok;		/* GC thread successfully created. */
  uint32_t parkdepth;		/* Nested pause scopes (mutator-owned). */
  uint32_t stepn;		/* GC steps since last drain. */
  uint32_t drains;		/* Drain rounds this cycle. */
  ConcVec jobs;			/* Gray queue (GC-thread-owned). */
  ConcVec threadv;		/* Threads, deferred to the atomic phase. */
  ConcVec weakv;		/* Weak tables found while marking. */
  ConcVec uvv;			/* Closed upvalues, re-marked at finish. */
  ConcVec ssb;			/* Log of gclist-less parents (mutator). */
} ConcGCState;

#define concgcstate(g)	((ConcGCState *)mref((g)->gc.concstate, void))

/* Advisory cross-thread progress flag read (real sync is cs->lock). */
#define lj_concgc_markdone(cs)	__atomic_load_n(&(cs)->markdone, __ATOMIC_RELAXED)

/* Drain at most every Nth GC step. */
#define CONCGC_DRAINSTEP	8
/* Force the single-threaded finish after this many drain rounds. */
#define CONCGC_MAXDRAIN		64
/* Objects traversed per GC thread burst between parkreq checks. */
#define CONCGC_BURST		64

/* lj_gcconc.c -- thread lifecycle and handshake primitives. */
LJ_FUNC int lj_concgc_init(global_State *g);
LJ_FUNC void lj_concgc_shutdown(global_State *g);
LJ_FUNC void lj_concgc_startmark(global_State *g);
LJ_FUNC void lj_concgc_park(global_State *g);
LJ_FUNC void lj_concgc_resume(global_State *g);
LJ_FUNC void lj_concgc_stopmark(global_State *g);
LJ_FUNC void lj_concgc_unpause(global_State *g);
LJ_FUNC void lj_concgc_vecpush(ConcVec *v, GCobj *o);
LJ_FUNC void lj_concgc_freevecs(ConcGCState *cs);

/* lj_gc.c -- one traversal burst, called from the GC thread. */
LJ_FUNC int lj_gc_conc_burst(global_State *g);

/* Park the GC thread around realloc/free of buffers it may read. */
static LJ_AINLINE void lj_concgc_pause_begin(global_State *g)
{
  if (LJ_UNLIKELY(g->gc.cmark)) {
    ConcGCState *cs = concgcstate(g);
    if (cs->parkdepth++ == 0) lj_concgc_park(g);
  }
}

static LJ_AINLINE void lj_concgc_pause_end(global_State *g)
{
  if (LJ_UNLIKELY(g->gc.cmark)) {
    ConcGCState *cs = concgcstate(g);
    if (cs->parkdepth > 0 && --cs->parkdepth == 0)
      lj_concgc_resume(g);
  }
}

#else

#define lj_concgc_pause_begin(g)	UNUSED(g)
#define lj_concgc_pause_end(g)		UNUSED(g)
#define lj_concgc_unpause(g)		UNUSED(g)

#endif

#endif

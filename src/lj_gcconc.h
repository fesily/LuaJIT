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

/* Deferred buffer free: an old table array/node block that the marker may
** still be reading (hazard pointer matched at lj_tab_resize). Freed at the
** single-threaded cycle finish, when the marker is stopped. */
typedef struct DeferBuf {
  void *p;		/* Block to free. */
  size_t sz;		/* Byte size for lj_mem_free accounting. */
} DeferBuf;

typedef struct DeferVec {
  DeferBuf *p;
  MSize n, sz;
} DeferVec;

/*
** Ownership while concurrent marking runs (g->gc.cmark != 0):
**
**   Object color/weak bits of marked     GC thread (atomic RMW)
**   LJ_GC_LOGGED bit of marked           mutator (atomic RMW)
**   logring (mutator -> marker channel)  SPSC: mutator produces, marker
**                                        consumes (lock-free, acq/rel)
**   jobs/threadv/weakv/uvv vectors       GC thread (marker-PRIVATE)
**   gc.gray                              unused during cmark
**   gc.grayagain                         shared: mutator pushes (VM/JIT
**                                        barrier), marker steals (xchg)
**   gc.weak                              unused during cmark (the atomic
**                                        phase reuses them)
**   gc.root list, allocator, gc.total    mutator
**
** New (free-running) model: the mutator NEVER parks the marker per step.
** The marker runs free, draining the logring and its own gray jobs
** continuously. The two threads synchronize exactly once per cycle, at the
** termination handshake (mutator sets finishreq; marker drains both queues,
** sets markdone, idles), immediately before the STW atomic phase.
**
** The mutator runs without fences. Colors only darken during marking
** (white -> gray -> black; the white flip happens single-threaded in
** the atomic phase), so a stale color read shows an older = whiter
** state. Child iswhite() checks in the barrier macros are therefore
** conservative-safe, but parent isblack() checks can miss. Under cmark
** the parent trigger becomes "not yet LOGGED"; a triggered barrier
** never touches colors, it only logs the parent pointer into logring.
** LOGGED decisions are mutator-only and thus exact. Both sides RMW the
** shared marked byte atomically, so neither loses the other's bits.
**
** The marker drains logring entries by clearing LOGGED and requeueing
** logged black parents (and upvalues/userdata) for re-traversal into its
** private jobs; stores racing with the re-traversal re-log the parent.
** The final convergence runs single-threaded in gc_conc_finish + the
** atomic phase (cmark cleared, colors exact), so marking converges.
** The JIT's TBAR elimination (gcstep_barrier in lj_opt_fold.c) assumes a
** logged table stays logged between two GC steps.
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

/* Lock-free SPSC ring for mutator(barrier) -> marker dirty-parent pointers.
** Producer: the single running mutator. Consumer: the marker thread.
** Fixed power-of-two capacity; on full, the producer falls back to a
** one-shot park-drain (lj_concgc_logfull) so entries are never dropped.
** head is advanced by the consumer, tail by the producer.
*/
#define CONCGC_RING_SIZE	(1u << 16)	/* 65536 GCobj* slots. */
#define CONCGC_RING_MASK	(CONCGC_RING_SIZE - 1)

typedef struct SpscRing {
  GCobj *slot[CONCGC_RING_SIZE];
  uint32_t head;	/* Consumer (marker) read index. */
  uint32_t tail;	/* Producer (mutator) write index. */
} SpscRing;

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
  volatile uint32_t finishreq;	/* Mutator asks the marker to finish. */
  SpscRing *logring;		/* Mutator -> marker dirty-parent log. */
  ConcVec jobs;			/* Gray queue (marker-private). */
  ConcVec threadv;		/* Threads, deferred to the atomic phase. */
  ConcVec weakv;		/* Weak tables found while marking. */
  ConcVec uvv;			/* Closed upvalues, re-marked at finish. */
  DeferVec deferbuf;		/* Old table buffers awaiting hazard-safe free. */
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

/* Hazard-pointer deferred buffer free (lj_tab_resize -> cycle finish). */
LJ_FUNC void lj_concgc_deferfree(global_State *g, void *p, size_t sz);
LJ_FUNC void lj_concgc_draindefer(global_State *g);

/* SPSC log ring (mutator producer -- lj_gcconc.c). */
LJ_FUNC int lj_concgc_ringpush(ConcGCState *cs, GCobj *o);

/* lj_gc.c -- marker consumer side and free-running burst. */
LJ_FUNC int lj_gc_conc_burst(global_State *g);
LJ_FUNC MSize lj_concgc_ringdrain(global_State *g, MSize max);
LJ_FUNC MSize lj_concgc_draingrayagain(global_State *g);
LJ_FUNC void lj_concgc_logfull(global_State *g, GCobj *o);

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

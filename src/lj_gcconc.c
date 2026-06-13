/*
** Concurrent GC: marker thread, handshake protocol.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_gcconc_c
#define LUA_CORE

#include "lj_obj.h"

#if LJ_CONCGC

#include <stdlib.h>
#include <string.h>

#include "lj_gc.h"
#include "lj_gcconc.h"
#include "lj_gcstat.h"

/* Cross-thread progress flags (parkreq, markdone) are advisory wake-up
** hints; all real synchronization happens through cs->lock. Relaxed
** atomic accesses keep the data-race-free guarantee (and TSAN) intact.
*/
#define flag_load(p)		__atomic_load_n((p), __ATOMIC_RELAXED)
#define flag_store(p, v)	__atomic_store_n((p), (v), __ATOMIC_RELAXED)

/* -- Work vectors --------------------------------------------------------- */

void lj_concgc_vecpush(ConcVec *v, GCobj *o)
{
  if (LJ_UNLIKELY(v->n >= v->sz)) {
    MSize sz = v->sz ? v->sz*2 : 256;
    GCobj **p = (GCobj **)realloc(v->p, sz*sizeof(GCobj *));
    if (p == NULL) abort();  /* No safe error path off the GC thread. */
    v->p = p;
    v->sz = sz;
  }
  v->p[v->n++] = o;
}

void lj_concgc_freevecs(ConcGCState *cs)
{
  free(cs->jobs.p); free(cs->threadv.p); free(cs->weakv.p);
  free(cs->uvv.p); free(cs->ssb.p);
  memset(&cs->jobs, 0, 5*sizeof(ConcVec));
}

/* -- GC thread main loop -------------------------------------------------- */

static void *gcthread_main(void *arg)
{
  ConcGCState *cs = (ConcGCState *)arg;
  global_State *g = cs->g;
  pthread_mutex_lock(&cs->lock);
  for (;;) {
    while (cs->phase == CONCGC_IDLE || flag_load(&cs->parkreq)) {
      cs->parked = 1;
      pthread_cond_signal(&cs->wake_mut);
      pthread_cond_wait(&cs->wake_gc, &cs->lock);
    }
    if (cs->phase == CONCGC_EXIT)
      break;
    cs->parked = 0;
    pthread_mutex_unlock(&cs->lock);
    /* CONCGC_MARK: traverse gray objects in bursts. */
#if LUAJIT_GC_STAT
    {
      uint64_t _t0 = lj_gcstat_now_ns();
      uint32_t _bursts = 0;
      while (!flag_load(&cs->parkreq) && lj_gc_conc_burst(g))
	_bursts++;
      GCSTAT_ADD(g, gcthread_mark, lj_gcstat_now_ns() - _t0);
      g->stat.gcthread_bursts += _bursts;
    }
#else
    while (!flag_load(&cs->parkreq) && lj_gc_conc_burst(g))
      ;
#endif
    pthread_mutex_lock(&cs->lock);
    if (cs->phase == CONCGC_MARK && !flag_load(&cs->parkreq)) {
      flag_store(&cs->markdone, 1);  /* Out of gray objects: announce and go idle. */
      cs->phase = CONCGC_IDLE;
    }
  }
  cs->parked = 1;
  pthread_cond_signal(&cs->wake_mut);
  pthread_mutex_unlock(&cs->lock);
  return NULL;
}

/* -- Lifecycle ------------------------------------------------------------ */

int lj_concgc_init(global_State *g)
{
  ConcGCState *cs;
  if (concgcstate(g))
    return 1;  /* Already initialized. */
  cs = (ConcGCState *)calloc(1, sizeof(ConcGCState));
  if (cs == NULL)
    return 0;
  cs->g = g;
  cs->phase = CONCGC_IDLE;
  cs->parked = 1;
  pthread_mutex_init(&cs->lock, NULL);
  pthread_cond_init(&cs->wake_gc, NULL);
  pthread_cond_init(&cs->wake_mut, NULL);
  if (pthread_create(&cs->thread, NULL, gcthread_main, cs) != 0) {
    pthread_mutex_destroy(&cs->lock);
    pthread_cond_destroy(&cs->wake_gc);
    pthread_cond_destroy(&cs->wake_mut);
    free(cs);
    return 0;
  }
  cs->threadok = 1;
  setmref(g->gc.concstate, cs);
  return 1;
}

void lj_concgc_shutdown(global_State *g)
{
  ConcGCState *cs = concgcstate(g);
  if (cs == NULL)
    return;
  pthread_mutex_lock(&cs->lock);
  cs->phase = CONCGC_EXIT;
  flag_store(&cs->parkreq, 0);
  pthread_cond_signal(&cs->wake_gc);
  pthread_mutex_unlock(&cs->lock);
  pthread_join(cs->thread, NULL);
  pthread_mutex_destroy(&cs->lock);
  pthread_cond_destroy(&cs->wake_gc);
  pthread_cond_destroy(&cs->wake_mut);
  lj_concgc_freevecs(cs);
  free(cs);
  setmref(g->gc.concstate, NULL);
  g->gc.cmark = 0;
}

/* -- Handshake ------------------------------------------------------------ */

/* Stop the GC thread at its next burst boundary. Full memory ordering. */
void lj_concgc_park(global_State *g)
{
  ConcGCState *cs = concgcstate(g);
  pthread_mutex_lock(&cs->lock);
  flag_store(&cs->parkreq, 1);
  while (!cs->parked)
    pthread_cond_wait(&cs->wake_mut, &cs->lock);
  pthread_mutex_unlock(&cs->lock);
}

void lj_concgc_resume(global_State *g)
{
  ConcGCState *cs = concgcstate(g);
  pthread_mutex_lock(&cs->lock);
  flag_store(&cs->parkreq, 0);
  pthread_cond_signal(&cs->wake_gc);
  pthread_mutex_unlock(&cs->lock);
}

/* Restore a sane GC thread state after an error unwind (lj_err_throw). */
void lj_concgc_unpause(global_State *g)
{
  ConcGCState *cs = concgcstate(g);
  if (cs && cs->parkdepth) {
    cs->parkdepth = 0;
    lj_concgc_resume(g);
  }
}

/* Start (or restart after a drain added work) concurrent marking. */
void lj_concgc_startmark(global_State *g)
{
  ConcGCState *cs = concgcstate(g);
  pthread_mutex_lock(&cs->lock);
  flag_store(&cs->markdone, 0);
  flag_store(&cs->parkreq, 0);
  cs->phase = CONCGC_MARK;
  g->gc.cmark = 1;  /* Ordered by the mutex before the GC thread runs. */
  pthread_cond_signal(&cs->wake_gc);
  pthread_mutex_unlock(&cs->lock);
}

/* Stop concurrent marking and return to single-threaded operation. */
void lj_concgc_stopmark(global_State *g)
{
  ConcGCState *cs = concgcstate(g);
  pthread_mutex_lock(&cs->lock);
  flag_store(&cs->parkreq, 1);
  while (!cs->parked)
    pthread_cond_wait(&cs->wake_mut, &cs->lock);
  cs->phase = CONCGC_IDLE;
  flag_store(&cs->parkreq, 0);
  flag_store(&cs->markdone, 0);
  g->gc.cmark = 0;
  pthread_cond_signal(&cs->wake_gc);  /* Back to the IDLE wait loop. */
  pthread_mutex_unlock(&cs->lock);
  cs->parkdepth = 0;
  cs->stepn = 0;
}

#endif

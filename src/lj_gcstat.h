/*
** GC instrumentation -- phase-level timing for the concurrent GC.
*/

#ifndef _LJ_GCSTAT_H
#define _LJ_GCSTAT_H

#include "lj_obj.h"
#include "lj_gcstat_def.h"

#if LUAJIT_GC_STAT

LJ_FUNC uint64_t lj_gcstat_now_ns(void);
LJ_FUNC void lj_gcstat_reset(global_State *g);
LJ_FUNC int lj_gcstat_dump(global_State *g, const char *filename,
			   int append, const char *label);

/* Time a lexical scope. Opens locals; close with GCSTAT_SCOPE_END(g). */
#define GCSTAT_SCOPE(g, phasename) \
  uint64_t _gcstat_t0 = lj_gcstat_now_ns(); \
  int _gcstat_p = (int)GCSTAT_PHASE_##phasename; \
  GCStat *_gcstat_s = &(g)->stat

#define GCSTAT_SCOPE_END(g) do { \
  uint64_t _dt = lj_gcstat_now_ns() - _gcstat_t0; \
  GCStatPhaseData *_d = &_gcstat_s->phase[_gcstat_p]; \
  _d->ns_total += _dt; _d->count++; \
  if (_dt > _d->ns_max) _d->ns_max = _dt; \
} while (0)

/* Add a measured interval to a phase without opening a lexical scope.
** Use when the start and end live in different functions/threads. */
#define GCSTAT_ADD(g, phasename, dt_ns) do { \
  GCStatPhaseData *_d = &(g)->stat.phase[(int)GCSTAT_PHASE_##phasename]; \
  uint64_t _dt2 = (dt_ns); \
  _d->ns_total += _dt2; _d->count++; \
  if (_dt2 > _d->ns_max) _d->ns_max = _dt2; \
} while (0)

#define GCSTAT_COUNT_CYCLE(g)   ((g)->stat.cycle_count++)
#define GCSTAT_COUNT_DRAIN(g)   ((g)->stat.drain_rounds++)
#define GCSTAT_COUNT_BURST(g)   ((g)->stat.gcthread_bursts++)
#define GCSTAT_PEAK(g) do { \
  if ((g)->gc.total > (g)->stat.bytes_peak) \
    (g)->stat.bytes_peak = (g)->gc.total; \
} while (0)

#else  /* !LUAJIT_GC_STAT */

#define GCSTAT_SCOPE(g, phasename)     ((void)0)
#define GCSTAT_SCOPE_END(g)            ((void)0)
#define GCSTAT_ADD(g, phasename, dt)   ((void)0)
#define GCSTAT_COUNT_CYCLE(g)          ((void)0)
#define GCSTAT_COUNT_DRAIN(g)          ((void)0)
#define GCSTAT_COUNT_BURST(g)          ((void)0)
#define GCSTAT_PEAK(g)                 ((void)0)

#endif

#endif /* _LJ_GCSTAT_H */

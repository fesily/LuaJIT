/*
** GC instrumentation data structures (leaf header -- no LuaJIT deps).
*/

#ifndef _LJ_GCSTAT_DEF_H
#define _LJ_GCSTAT_DEF_H

#include "lj_arch.h"

#if LUAJIT_GC_STAT

#include <stdint.h>

/* Concurrent-GC phases. Order is the CSV column order; keep stable. */
#define GCSTAT_PHASES(_) \
  _(mark_start) \
  _(drainlog) \
  _(conc_park) \
  _(conc_finish) \
  _(atomic) \
  _(gcthread_mark)

typedef enum {
#define GCSTAT_E_(name) GCSTAT_PHASE_##name,
  GCSTAT_PHASES(GCSTAT_E_)
#undef GCSTAT_E_
  GCSTAT_PHASE__COUNT
} GCStatPhase;

typedef struct GCStatPhaseData {
  uint64_t ns_total;	/* Sum of all intervals in this phase. */
  uint64_t ns_max;	/* Longest single interval. */
  uint64_t count;	/* Number of intervals. */
} GCStatPhaseData;

typedef struct GCStat {
  GCStatPhaseData phase[GCSTAT_PHASE__COUNT];
  uint64_t bytes_peak;		/* Peak gc.total observed. */
  uint32_t cycle_count;		/* Concurrent cycles started. */
  uint32_t drain_rounds;	/* Total gc_conc_drainlog calls. */
  uint32_t gcthread_bursts;	/* Burst-loop iterations on GC thread. */
  uint64_t reset_epoch_ns;	/* Wall clock at last reset. */
} GCStat;

#endif

#endif /* _LJ_GCSTAT_DEF_H */

/*
** GC instrumentation implementation.
*/

#define lj_gcstat_c
#define LUA_CORE

#include "lj_obj.h"

#if LUAJIT_GC_STAT

#include <time.h>
#include <stdio.h>
#include <string.h>

#include "lj_gcstat.h"

LJ_FUNC uint64_t lj_gcstat_now_ns(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

LJ_FUNC void lj_gcstat_reset(global_State *g)
{
  memset(&g->stat, 0, sizeof(GCStat));
  g->stat.reset_epoch_ns = lj_gcstat_now_ns();
}

static const char *const gcstat_phase_names[] = {
#define GCSTAT_N_(name) #name,
  GCSTAT_PHASES(GCSTAT_N_)
#undef GCSTAT_N_
};

LJ_FUNC int lj_gcstat_dump(global_State *g, const char *filename, int append,
			   const char *label)
{
  FILE *f = fopen(filename, append ? "a" : "w");
  int i;
  uint64_t now;
  if (!f) return 0;
  if (!append) {
    fprintf(f, "label,epoch_ms,cycle_count,drain_rounds,gcthread_bursts,"
	       "bytes_total,bytes_peak");
    for (i = 0; i < GCSTAT_PHASE__COUNT; i++) {
      fprintf(f, ",%s_ns,%s_max,%s_count",
	      gcstat_phase_names[i], gcstat_phase_names[i],
	      gcstat_phase_names[i]);
    }
    fputc('\n', f);
  }
  now = lj_gcstat_now_ns();
  fprintf(f, "%s,%llu,%u,%u,%u,%llu,%llu",
	  label ? label : "",
	  (unsigned long long)((now - g->stat.reset_epoch_ns) / 1000000ull),
	  g->stat.cycle_count, g->stat.drain_rounds, g->stat.gcthread_bursts,
	  (unsigned long long)g->gc.total,
	  (unsigned long long)g->stat.bytes_peak);
  for (i = 0; i < GCSTAT_PHASE__COUNT; i++) {
    GCStatPhaseData *d = &g->stat.phase[i];
    fprintf(f, ",%llu,%llu,%llu",
	    (unsigned long long)d->ns_total,
	    (unsigned long long)d->ns_max,
	    (unsigned long long)d->count);
  }
  fputc('\n', f);
  fclose(f);
  return 1;
}

#else  /* !LUAJIT_GC_STAT */

/* Empty TU placeholder to satisfy toolchains that warn on empty objects. */
extern int lj_gcstat_unused_stub;
int lj_gcstat_unused_stub = 0;

#endif

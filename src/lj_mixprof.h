/*
** Mixed (Lua + host) callchain sampling on top of the profile hook.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
**
** This module owns the two pieces the low-overhead profiler cannot own by
** itself: reading the native stack of the profiled thread and merging it with
** the Lua callchain. The collection loop runs entirely in C -- the sample
** handler is a C function registered with the ordinary profile interface, so
** no Lua code (and no Lua callback) runs per sample.
*/

#ifndef _LJ_MIXPROF_H
#define _LJ_MIXPROF_H

#include "lj_obj.h"

#if LJ_HASPROFILE

/*
** The host part needs the Rtl* unwinder and the PE unwind info of the frames,
** which only the Windows port provides.
*/
#if LJ_TARGET_WINDOWS
#define LJ_HASMIXPROF	1
#else
#define LJ_HASMIXPROF	0
#endif

/* Maximum number of frames kept per sample. */
#define LJMIXPROF_MAX_FRAMES	96

/*
** Start collecting. Runs the profile hook in 'h' mode (capture host stacks)
** with the given interval, and stores every sample with at most `depth`
** merged frames. Does nothing if mixed sampling is not available.
*/
LJ_FUNC void lj_mixprof_start(lua_State *L, int interval, int depth);

/* Stop collecting and free the samples. */
LJ_FUNC void lj_mixprof_stop(lua_State *L);

/* Is the collector running? */
LJ_FUNC int lj_mixprof_active(void);

/*
** Push a table { <merged callchain> = <number of ticks> } and return the
** number of ticks collected. The chains use the same notation as
** luaJIT_profile_mixedstack().
*/
LJ_FUNC int lj_mixprof_report(lua_State *L);

/*
** Push the clock of the last run { start_us, interval_us, ticks }:
** a record collected at tick k was taken at start_us + k * interval_us,
** both in microseconds since 1970, so a report can be lined up with other
** logs.
*/
LJ_FUNC int lj_mixprof_info(lua_State *L);

/*
** Called by the profile hook at every safepoint sample while host capture is
** enabled: reads both stacks and merges them into the "last sample" state
** that the public luaJIT_profile_hoststack/mixedstack API reads.
*/
LJ_FUNC void lj_mixprof_capture(lua_State *L);

/* Drop the last sample (profile stop, or a new profile run). */
LJ_FUNC void lj_mixprof_reset_capture(void);

/* Accessors behind luaJIT_profile_hoststack()/mixedstack(). */
LJ_FUNC int lj_mixprof_hoststack(lua_State *L, const void **buf, int maxdepth);
LJ_FUNC const char *lj_mixprof_mixedstack(lua_State *L, int depth, size_t *len);

/* Lua module: require("jit.mixprof"). */
LUALIB_API int luaopen_mixprof(lua_State *L);

#endif

#endif

/*
** Memory profiler — heap snapshot walker (read-only).
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
**
** v0: arena bitmap census + huge-set walk + two-snapshot diff.
** Gated behind LUAJIT_ENABLE_MEMPROF AND LJ_HASGCMARK (arena-only).
** Read-only: touches NO hot allocation path.
*/
#ifndef _LJ_MEMPROF_H
#define _LJ_MEMPROF_H

#include "lj_obj.h"

#if LJ_HASGCMARK && defined(LUAJIT_ENABLE_MEMPROF)

#include "lj_arena.h"

/* GC mode to run before a snapshot. */
enum {
  LJ_MEMPROF_GC_NONE = 0,	/* No GC. */
  LJ_MEMPROF_GC_STEP = 1,	/* One incremental step. */
  LJ_MEMPROF_GC_FULL = 2	/* lj_gc_fullgc (consistent live set). */
};

/* Snapshot options (mirrored from the Lua call). */
typedef struct {
  int gc;		/* LJ_MEMPROF_GC_* */
  int details;		/* Build per-object id map for diff (default 0). */
} MemprofOpts;

/*
** Take a heap snapshot and push a Lua table census onto L->top.
** Returns 1 (the table) or raises an error.
** The table layout is documented in lj_memprof.c.
*/
LJ_FUNC int lj_memprof_snapshot(lua_State *L, const MemprofOpts *opts);

/* Build a diff of two snapshot tables (both produced by snapshot).
** Pushes a {leak_suspects=, grown=, shrunk=} table. Returns 1. */
LJ_FUNC int lj_memprof_diff(lua_State *L);

/* -- v1: event-stream mode (Capability B) ------------------------------- */

/* Start emitting ALLOC/REALLOC/FREE events to `outpath`. depth is the stack
** read depth (currently fixed at 1; proto/trace-id attribution). Returns 0
** on success, nonzero if a profiler is already active on another VM. */
LJ_FUNC int lj_memprof_start(lua_State *L, const char *outpath, int depth);

/* Stop the event stream: flush symtab + epilogue, close the output file.
** Only the VM that started the profiler may stop it. */
LJ_FUNC void lj_memprof_stop(lua_State *L);

#endif /* LJ_HASGCMARK && LUAJIT_ENABLE_MEMPROF */

#endif /* _LJ_MEMPROF_H */

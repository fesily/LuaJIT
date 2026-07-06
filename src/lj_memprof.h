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

/* v0.5: retained-size + retaining-path (read-only dominator tree over the
** live-object reference graph). Both build the graph on demand (snapshot-style),
** compute, push a Lua result, and free all scratch via g->allocf. The graph
** walk is strictly read-only — it never mutates GC color/marks.
**
** lj_memprof_retained: push a Lua array (length top, or all if top<=0) of
** {addr=, type=, shallow=, retained=} sorted by retained desc, excluding the
** synthetic super-root. do_fullgc=1 runs a full GC first for a consistent
** reachable live set. Returns 1. */
LJ_FUNC int lj_memprof_retained(lua_State *L, int top, int do_fullgc);

/* lj_memprof_retainers: push the retaining path for the object at `addr` as a
** Lua array of {addr=, type=} from the object up to a root (excluding the
** synthetic super-root). Pushes nil if addr is not a live in-graph object.
** Returns 1. */
LJ_FUNC int lj_memprof_retainers(lua_State *L, lua_Number addr, int do_fullgc);

/* -- v1: event-stream mode (Capability B) ------------------------------- */

/* Start emitting ALLOC/REALLOC/FREE events to `outpath`. depth is the stack
** read depth (currently fixed at 1; proto/trace-id attribution). interval is
** the sampling period in bytes for v5 sampling mode: 0 = EXACT mode (emit
** every alloc, the default — byte-for-byte semantically identical to v4);
** >0 = SAMPLE mode (byte-accumulator gate: emit ~1 event per `interval` bytes
** allocated, with a per-sample weight + sampled-address set so FREE/REALLOC
** stay consistent). Returns 0 on success, nonzero if a profiler is already
** active on another VM. */
LJ_FUNC int lj_memprof_start(lua_State *L, const char *outpath, int depth,
			     uint64_t interval);

/* Stop the event stream: flush symtab + epilogue, close the output file.
** Only the VM that started the profiler may stop it. */
LJ_FUNC void lj_memprof_stop(lua_State *L);

/* v6: set the current allocation label. str==NULL or len==0 clears the label
** (current_label_id = 0). When the profiler is inactive this is a no-op.
** Interning happens here (on the Lua-call path); the emit hook just writes
** the current id. The string bytes are COPIED into the store (the caller's
** GCstr may be collected before stop). */
LJ_FUNC void lj_memprof_setlabel(lua_State *L, const char *str, size_t len);

#endif /* LJ_HASGCMARK && LUAJIT_ENABLE_MEMPROF */

#endif /* _LJ_MEMPROF_H */

/*
** Miscellaneous public C API extensions: the platform and Lua profiler.
**
** This is the sysprof subset of the tarantool misc library, ported to the
** v2.1 tree.
*/

#ifndef _LMISCLIB_H
#define _LMISCLIB_H

#include <stdint.h>

#include "lua.h"

#define LUAMISC_API extern

/* --- Sysprof - platform and lua profiler -------------------------------- */

/*
** Writer function for profile events. Must be async-safe, see also
** `man 7 signal-safety`. Returns the amount of written bytes on success or
** zero in case of error. Setting *data to NULL means end of profiling.
** For details see <lj_wbuf.h>.
*/
typedef size_t (*luam_Sysprof_writer)(const void **data, size_t len, void *ctx);

/*
** Callback on profiler stopping. Required for correctly cleaning up at VM
** finalization when the profiler is still running. Returns zero on success.
*/
typedef int (*luam_Sysprof_on_stop)(void *ctx, uint8_t *buf);

/*
** Backtracing function for the host stack. Should call `frame_writer` on
** each frame in the stack, from the stack top to the bottom. If
** `frame_writer` returns NULL, backtracing must stop.
*/
typedef void (*luam_Sysprof_backtracer)(void *(*frame_writer)(int frame_no,
							      void *addr));

/*
** DEFAULT mode collects only the counters, which are kept in memory and can
** be collected with luaM_sysprof_report() after the profiler stops.
*/
#define LUAM_SYSPROF_DEFAULT 0
/*
** LEAF mode = DEFAULT + streams samples with only the top frames of the host
** and guest stacks, in the format described in <lj_sysprof.h>.
*/
#define LUAM_SYSPROF_LEAF 1
/*
** CALLGRAPH mode = DEFAULT + streams full callchains of the host and guest
** stacks, in the format described in <lj_sysprof.h>.
*/
#define LUAM_SYSPROF_CALLGRAPH 2

struct luam_Sysprof_Counters {
  uint64_t vmst_interp;
  uint64_t vmst_lfunc;
  uint64_t vmst_ffunc;
  uint64_t vmst_cfunc;
  uint64_t vmst_gc;
  uint64_t vmst_exit;
  uint64_t vmst_record;
  uint64_t vmst_opt;
  uint64_t vmst_asm;
  uint64_t vmst_trace;
  /*
  ** XXX: Order of the counters is important: it must be the same as the
  ** order of the vmstates.
  */
  uint64_t samples;
};

/* Profiler options. */
struct luam_Sysprof_Options {
  uint8_t mode;		/* Profiling mode. */
  uint64_t interval;	/* Sampling interval in msec. */
  uint8_t *buf;		/* Custom buffer to write data. */
  size_t len;		/* The buffer's size. */
  void *ctx;		/* Context for the writer and the final callback. */
};

#define PROFILE_SUCCESS 0
#define PROFILE_ERRUSE  1
#define PROFILE_ERRRUN  2
#define PROFILE_ERRMEM  3
#define PROFILE_ERRIO   4

LUAMISC_API int luaM_sysprof_set_writer(luam_Sysprof_writer writer);
LUAMISC_API int luaM_sysprof_set_on_stop(luam_Sysprof_on_stop on_stop);
LUAMISC_API int luaM_sysprof_set_backtracer(luam_Sysprof_backtracer backtracer);
LUAMISC_API int luaM_sysprof_start(lua_State *L,
				   const struct luam_Sysprof_Options *opt);
LUAMISC_API int luaM_sysprof_stop(lua_State *L);
LUAMISC_API int luaM_sysprof_report(struct luam_Sysprof_Counters *counters);

#define LUAM_MISCLIBNAME "misc"
LUALIB_API int luaopen_misc(lua_State *L);

#endif /* _LMISCLIB_H */

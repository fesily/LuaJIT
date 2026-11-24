/*
** DS: host-side PUC Lua 5.1 dump service ABI.
**
** Shared by three parties that must not share a VM ABI: the LuaJIT VM (which
** owns the saved source and the prototype identity), the Injector (which owns
** the private PUC 5.1 state), and the test host.  It therefore depends only on
** <stddef.h>/<stdint.h> and never sees lua_State, Proto or a C++ type.
**
** Rules that keep the boundary sound:
**   * the VM never allocates inside the returned buffer and never frees it; the
**     service transfers ownership only on success, through owner/release;
**   * release() must be called from the service's own CRT (i.e. by exactly the
**     caller that invoked the service), never from inside the VM's writer;
**   * error[] is always NUL-terminated when the call returns 0.
*/
#ifndef _LUA_DSPUCDUMP_H
#define _LUA_DSPUCDUMP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One request addresses exactly one function prototype of one text compile. */
typedef struct lua_DsPucDumpRequest {
  const char *source;	/* Exact bytes the VM parser consumed. Never executed. */
  size_t source_len;
  const char *chunkname;	/* chunkname of the compiling chunk (PUC source). */
  uint32_t node_index;	/* pre-order index of the target proto, root == 0. */
  uint32_t node_count;	/* total protos in the compile, for identity checking. */
  uint32_t firstline;	/* GCproto.firstline -> Proto.linedefined. */
  uint32_t numparams;
  uint32_t nups;	/* Proto.nups */
  uint32_t is_vararg;	/* 0/1: normalized VARARG_ISVARARG of the target. */
  uint32_t child_count;	/* direct child protos (Proto.sizep). */
} lua_DsPucDumpRequest;

typedef struct lua_DsPucDumpResult {
  const char *data;	/* Valid only when the call returned 1. */
  size_t size;
  void *owner;		/* Opaque; freed by release(owner). */
  void (*release)(void *owner);
  char error[256];	/* NUL-terminated reason when the call returned 0. */
} lua_DsPucDumpResult;

/* Returns 1 on success (owner/release transferred), 0 on failure. */
typedef int (*lua_DsPucDumpFn)(const lua_DsPucDumpRequest *request,
			       lua_DsPucDumpResult *result);

#ifdef __cplusplus
}
#endif

#endif

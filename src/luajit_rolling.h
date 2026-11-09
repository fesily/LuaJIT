/*
** LuaJIT -- a Just-In-Time Compiler for Lua. https://luajit.org/
**
** Copyright (C) 2005-2026 Mike Pall. All rights reserved.
**
** Permission is hereby granted, free of charge, to any person obtaining
** a copy of this software and associated documentation files (the
** "Software"), to deal in the Software without restriction, including
** without limitation the rights to use, copy, modify, merge, publish,
** distribute, sublicense, and/or sell copies of the Software, and to
** permit persons to whom the Software is furnished to do so, subject to
** the following conditions:
**
** The above copyright notice and this permission notice shall be
** included in all copies or substantial portions of the Software.
**
** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
** EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
** MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
** IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
** CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
** TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
** SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
**
** [ MIT license: https://www.opensource.org/licenses/mit-license.php ]
*/

#ifndef _LUAJIT_H
#define _LUAJIT_H

#include "lua.h"

#define LUAJIT_VERSION		"LuaJIT 2.1.ROLLING"
#define LUAJIT_VERSION_NUM	20199  /* Deprecated. */
#define LUAJIT_VERSION_SYM	luaJIT_version_2_1_ROLLING
#define LUAJIT_COPYRIGHT	"Copyright (C) 2005-2026 Mike Pall"
#define LUAJIT_URL		"https://luajit.org/"

/* Modes for luaJIT_setmode. */
#define LUAJIT_MODE_MASK	0x00ff

enum {
  LUAJIT_MODE_ENGINE,		/* Set mode for whole JIT engine. */
  LUAJIT_MODE_DEBUG,		/* Set debug mode (idx = level). */

  LUAJIT_MODE_FUNC,		/* Change mode for a function. */
  LUAJIT_MODE_ALLFUNC,		/* Recurse into subroutine protos. */
  LUAJIT_MODE_ALLSUBFUNC,	/* Change only the subroutines. */

  LUAJIT_MODE_TRACE,		/* Flush a compiled trace. */

  LUAJIT_MODE_WRAPCFUNC = 0x10,	/* Set wrapper mode for C function calls. */

  LUAJIT_MODE_MAX
};

/* Flags or'ed in to the mode. */
#define LUAJIT_MODE_OFF		0x0000	/* Turn feature off. */
#define LUAJIT_MODE_ON		0x0100	/* Turn feature on. */
#define LUAJIT_MODE_FLUSH	0x0200	/* Flush JIT-compiled code. */

/* LuaJIT public C API. */

/* Control the JIT engine. */
LUA_API int luaJIT_setmode(lua_State *L, int idx, int mode);

/* Low-overhead profiling API. */
typedef void (*luaJIT_profile_callback)(void *data, lua_State *L,
					int samples, int vmstate);
LUA_API void luaJIT_profile_start(lua_State *L, const char *mode,
				  luaJIT_profile_callback cb, void *data);
LUA_API void luaJIT_profile_stop(lua_State *L);
LUA_API const char *luaJIT_profile_dumpstack(lua_State *L, const char *fmt,
					     int depth, size_t *len);

/* Interpreter breakpoint API.
**
** While a breakpoint session is active, up to LJ_INT3_MAX bytecode slots (in
** any mix of prototypes) deliver a callback. The canonical bytecode array of
** a prototype is never modified: the VM keeps one shadow copy of it, and only
** new instruction fetches read that shadow, where the armed slots hold
** BC_INT3. Every hook, recorder and slow-path re-read keeps reading the
** canonical array, so jit.util.funcbc, dumps, line numbers and tracebacks
** always see the real program. Slots the JIT rewrites at run time (function
** headers, loop instructions, ITERN/ISNEXT and RET/RET0/RET1) cannot be
** armed.
**
** A breakpoint is permanent: every new fetch of the slot delivers the
** callback once and then executes the original instruction, until the
** breakpoint is cancelled, the session is stopped or the VM is closed. The
** callback is a read-only native observer: it must not run Lua code or change
** any interpreter/JIT/hook state, but it may arm (re-arming the slot that is
** being delivered is a no-op), cancel or stop the session.
**
** A request is mounted automatically at the next safe point: the prototype
** is kept out of the JIT (the traces that can reach the armed instruction are
** flushed and the prototype is excluded from compilation) and its shadow is
** created. luaJIT_int3_arm() returning OK means the request was accepted, not
** that it is already mounted: a host call and the callback mount it before
** returning, while a Lua debug hook or the recorder only registers it, and
** the C dispatch that leads to the next safe point mounts it there. The
** instruction the hook belongs to still executes with its own opcode exactly
** once, and the breakpoint fires from the next fetch of that slot on. The
** other calls (start, cancel, stop) are still refused inside a hook, as are
** the GC, foreign VM event, profiler and machine-code contexts.
**
** A breakpoint keeps its slot, its prototype GC root and the JIT exclusion
** until it is explicitly disarmed: luaJIT_int3_cancel() disarms every
** breakpoint but keeps the session, luaJIT_int3_stop() disarms them and
** releases the session. There is no single-slot disarm call.
*/
typedef void (*luaJIT_int3_callback)(void *data, lua_State *L,
				     const void *proto, unsigned int bcpos);
enum {
  LUAJIT_INT3_OK = 0,
  LUAJIT_INT3_BUSY = 1,		/* Unsafe context or conflicting owner. */
  LUAJIT_INT3_INVALID = 2,	/* Bad callback or function/index. */
  LUAJIT_INT3_DISABLED = 3,	/* No session owned by this VM. */
  LUAJIT_INT3_UNAVAILABLE = 4,	/* Profiling support compiled out. */
  LUAJIT_INT3_FULL = 5,		/* All breakpoint slots are in use. */
  LUAJIT_INT3_UNARMABLE = 6,	/* Slot is rewritten by the JIT or the VM. */
  LUAJIT_INT3_NOMEM = 7		/* No memory for the shadow of the prototype. */
};
LUA_API int luaJIT_int3_start(lua_State *L, luaJIT_int3_callback cb,
			      void *data);
LUA_API int luaJIT_int3_arm(lua_State *L, int func_index, unsigned int bcpos);
LUA_API int luaJIT_int3_cancel(lua_State *L);
LUA_API int luaJIT_int3_stop(lua_State *L);

/* Enforce (dynamic) linker error for version mismatches. Call from main. */
LUA_API void LUAJIT_VERSION_SYM(void);

#error "DO NOT USE luajit_rolling.h -- only include build-generated luajit.h"
#endif

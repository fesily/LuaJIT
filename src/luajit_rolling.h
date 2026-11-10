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
** While a breakpoint session is active, up to LJ_INT3_MAX bytecode
** instructions (in any mix of prototypes) are replaced by BC_INT3. Only the
** slots the interpreter never dispatches on its own cannot be armed: function
** headers (in every form, because the frame is not set up when they run), the
** generic-for prep pair (ITERN/ISNEXT) and a JMP slot that the preceding
** comparison or test (ISLT..ISF) branches over. Loop slots, loop back edges
** and RET/RET0/RET1/RETM are armable: an armed prototype never
** traces: the JIT is excluded from it as soon as its first breakpoint is
** mounted, so no trace can rewrite those slots while they carry a trap. The
** JIT stays enabled: mounting flushes only the traces that can reach the
** armed instruction and excludes its prototype from compilation until the
** last breakpoint of that prototype is cancelled. Only an exclusion
** that the breakpoint itself added is released: jit.off() stays in effect
** afterwards, whether it was issued before arming or while mounted.
**
** Arming is asynchronous: LUAJIT_INT3_OK means the request was accepted, not
** that the slot already holds the trap. A call from the host, from a C
** function or from the callback mounts it before returning; a call from a Lua
** debug hook or from the recorder/compiler only queues it, because replacing
** the slot there would race the dispatch or the bytecode patch in progress.
** The next dispatch then mounts every queued request at a safe point: the
** prototype is flushed and excluded from the JIT first, and only then is its
** slot replaced. A queued request holds its GC root and forces the C
** dispatch, so it is never lost and never observed as armed before it is; an
** instruction that is already being dispatched always executes once with its
** own opcode.
**
** A breakpoint is permanent: every execution of the slot delivers the
** callback once and then executes the original instruction, until the
** breakpoint is cancelled, the session is stopped or the VM is closed. The
** callback receives the running closure as an opaque identity (the same value
** lua_topointer() returns for that function, i.e. a GCfunc *), so a debugger
** can tell two closures of one prototype apart and reach their upvalues
** without reconstructing the frame; the pointer is only valid for the duration
** of the call. The callback is a read-only native observer: it must not run
** Lua code or change any interpreter/JIT/hook state, but it may arm (re-arming
** the slot that is being delivered is a no-op), cancel or stop the session.
** The other calls
** (start, cancel, stop) are refused inside a hook, as is arming inside a GC
** hook, the profiler, a foreign VM event or running machine code.
**
** A breakpoint keeps its slot, its prototype GC root and the JIT exclusion
** until it is explicitly disarmed: luaJIT_int3_cancel() disarms every
** breakpoint but keeps the session, luaJIT_int3_stop() disarms them and
** releases the session. There is no single-slot disarm call.
**
** The session belongs to the VM that started it: it is allocated from that
** VM's allocator and released by luaJIT_int3_stop() or, at the latest, when
** the VM is closed (which disarms the remaining breakpoints first, without
** invoking the callback). At most one session may exist per process, because
** it shares the profiler's process-wide exclusivity.
*/
typedef void (*luaJIT_int3_callback)(void *data, lua_State *L,
				     const void *func, unsigned int bcpos);
enum {
  LUAJIT_INT3_OK = 0,
  LUAJIT_INT3_BUSY = 1,		/* Unsafe context or conflicting owner. */
  LUAJIT_INT3_INVALID = 2,	/* Bad callback or function/index. */
  LUAJIT_INT3_DISABLED = 3,	/* No session owned by this VM. */
  LUAJIT_INT3_UNAVAILABLE = 4,	/* Profiling support compiled out. */
  LUAJIT_INT3_FULL = 5,		/* All breakpoint slots are in use. */
  LUAJIT_INT3_UNARMABLE = 6	/* Slot is rewritten by the JIT or the VM. */
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

/*
** Interpreter breakpoints (BC_INT3).
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_INT3_H
#define _LJ_INT3_H

#include "lj_obj.h"
#include "lj_bc.h"

/* Non-zero if a request has been accepted but not mounted yet. The dispatch
** mode forces a C dispatch while this is true, so the next safe point can
** mount it.
*/
LJ_FUNC int lj_int3_has_pending(global_State *g);
/* Mount every pending request if this is a safe point. Leaf function: no
** allocation, no Lua, no GC, no hook delivery.
*/
LJ_FUNC void lj_int3_commit(global_State *g);
/* Arm a bytecode slot of a prototype. This is the internal entry point of the
** module: the public API resolves a function index to its prototype first, and
** an ABI layer that owns prototype identities (a debugger inside the process)
** may call it directly. Not LUA_API: a GCproto is deliberately not part of the
** supported C API surface.
*/
LJ_FUNC int lj_int3_arm(global_State *g, GCproto *pt, BCPos bcpos);
/* Logical view of the bytecode: an armed slot reads and writes as the
** original instruction, everything else reads and writes the real slot.
*/
LJ_FUNC BCIns lj_int3_bcins(global_State *g, const BCIns *pc);
/* Non-zero if the slot is armed, i.e. it holds a planted BC_INT3 trap. */
LJ_FUNC int lj_int3_pending(global_State *g, const BCIns *pc);
/* Patch a copy of the bytecode of a prototype with the logical instruction
** of every armed slot (the dump path must not leak BC_INT3).
*/
LJ_FUNC void lj_int3_bcwrite_view(global_State *g, const GCproto *pt, BCIns *bc);
/* Non-zero if JIT compilation must not be enabled for this prototype. */
LJ_FUNC int lj_int3_jit_on_blocked(global_State *g, GCproto *pt);
/* A caller disabled JIT compilation for this prototype itself: the request
** must keep the PROTO_NOJIT bit when it is released.
*/
LJ_FUNC void lj_int3_nojit_release(global_State *g, GCproto *pt);
/* Non-zero if any VM owns an INT3 session. */
LJ_FUNC int lj_int3_active(void);
/* Force-restore every armed slot and release the session (VM teardown). */
LJ_FUNC void lj_int3_close(global_State *g);
/* Interpreter dispatch target for a BC_INT3 hit. Called from the VM, which
** re-dispatches the returned original instruction. The trap stays armed, so
** every execution of the slot delivers a callback until it is disarmed.
*/
LJ_FUNCA BCIns LJ_FASTCALL lj_int3_hit(lua_State *L, const BCIns *pc);
/* Logical instruction of the slot in front of pc. The VM calls this when it
** has to re-read the opcode of the instruction it is about to execute and the
** physical slot may hold a trap (the hook and recorder continuation paths).
** Leaf function: no allocation, no GC step, no error, no hook delivery and no
** state change.
*/
LJ_FUNCA BCIns LJ_FASTCALL lj_int3_ins(lua_State *L, const BCIns *pc);

#endif

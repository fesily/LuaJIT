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
** mount the shadow.
*/
LJ_FUNC int lj_int3_has_pending(global_State *g);
/* Mount every pending request if this is a safe point. Leaf function: no
** allocation, no Lua, no GC, no hook delivery. */
LJ_FUNC void lj_int3_commit(lua_State *L);
/* Despecialize a failed ISNEXT into JMP/ITERC in the canonical bytecode and
** in a published shadow. pc points one slot past the ISNEXT. Returns the
** canonical ITER target. Leaf function: no allocation, no GC.
*/
LJ_FUNCA BCIns *LJ_FASTCALL lj_int3_isnext(lua_State *L, const BCIns *pc);
/* Non-zero if JIT compilation must not be enabled for this prototype. */
LJ_FUNC int lj_int3_jit_on_blocked(global_State *g, GCproto *pt);
/* A caller disabled JIT compilation for this prototype itself: the request
** must keep the PROTO_NOJIT bit when it is released.
*/
LJ_FUNC void lj_int3_nojit_release(global_State *g, GCproto *pt);
/* Non-zero if any VM owns an INT3 session. */
LJ_FUNC int lj_int3_active(void);
/* Force-release every request, shadow and JIT exclusion (VM teardown). */
LJ_FUNC void lj_int3_close(global_State *g);
/* Interpreter dispatch target for a BC_INT3 hit. Called from the VM, which
** re-dispatches the returned original instruction. The trap stays published,
** so every new fetch of the slot delivers a callback until it is disarmed.
*/
LJ_FUNCA BCIns LJ_FASTCALL lj_int3_hit(lua_State *L, const BCIns *pc);

#endif

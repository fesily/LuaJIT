/*
** Low-overhead profiling.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_PROFILE_H
#define _LJ_PROFILE_H

#include "lj_obj.h"
#include "lj_bc.h"

#if LJ_HASPROFILE

LJ_FUNC void LJ_FASTCALL lj_profile_interpreter(lua_State *L);
#if !LJ_PROFILE_SIGPROF
LJ_FUNC void LJ_FASTCALL lj_profile_hook_enter(global_State *g);
LJ_FUNC void LJ_FASTCALL lj_profile_hook_leave(global_State *g);
LJ_FUNC int lj_profile_lock(void);
LJ_FUNC void lj_profile_unlock(void);
#endif

#endif

/* -- One-shot interpreter interrupt (BC_INT3) ---------------------------- */

/* Logical view of the bytecode: the pending slot reads and writes as the
** original instruction, everything else reads and writes the real slot.
*/
LJ_FUNC BCIns lj_int3_bcins(global_State *g, const BCIns *pc);
LJ_FUNC void lj_int3_setbc(global_State *g, const BCIns *pc, BCIns ins);
LJ_FUNCA void lj_int3_setbc_op(global_State *g, const BCIns *pc, BCOp op);
/* BCPos of the pending slot of this prototype or LJ_INT3_NOBCPOS. */
#define LJ_INT3_NOBCPOS		(~(BCPos)0)
LJ_FUNC BCPos lj_int3_bcpos(global_State *g, const GCproto *pt);
/* Non-zero if JIT compilation must not be enabled for this prototype. */
LJ_FUNC int lj_int3_jit_on_blocked(global_State *g, GCproto *pt);
/* Non-zero if any VM owns an INT3 session. */
LJ_FUNC int lj_int3_active(void);
/* Force-cancel any request and release the session (VM teardown). */
LJ_FUNC void lj_int3_close(global_State *g);
/* Interpreter dispatch target for a BC_INT3 hit. Called from the VM. */
LJ_FUNCA void LJ_FASTCALL lj_int3_hit(lua_State *L, const BCIns *pc);

#endif

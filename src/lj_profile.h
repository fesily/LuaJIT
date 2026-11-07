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
/* Non-zero if a profiler session currently owns a VM. */
LJ_FUNC int lj_profile_active(void);

#endif

#endif

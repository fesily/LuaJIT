/*
** Windows ETW JIT symbol client.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_ETW_H
#define _LJ_ETW_H

#include "lj_obj.h"
#include "lj_jit.h"

#if LJ_HASETW

/* See lj_etw.c for the wire protocol and the symbol record lifecycle. */
LJ_FUNC void lj_etw_initstate(jit_State *J);
LJ_FUNC void lj_etw_freestate(jit_State *J);
LJ_FUNC void lj_etw_addtrace(jit_State *J, GCtrace *T);
LJ_FUNC void lj_etw_deltrace(jit_State *J, GCtrace *T);

/* Runtime switch (process-global, off by default): jit.profile.etw()/etw(on). */
LJ_FUNC void lj_etw_setstate(jit_State *J, int on);
LJ_FUNC int lj_etw_getstate(void);

/* GCtrace.etw_entry may alias other state copied along with the trace. */
#define LJ_ETW_CLEAR_ENTRY(T)		((T)->etw_entry = NULL)

#else
#define lj_etw_initstate(J)		UNUSED(J)
#define lj_etw_freestate(J)		UNUSED(J)
#define lj_etw_addtrace(J, T)		UNUSED(T)
#define lj_etw_deltrace(J, T)		UNUSED(T)
#define LJ_ETW_CLEAR_ENTRY(T)		UNUSED(T)
#define lj_etw_setstate(J, on)		((void)(J), (void)(on))
#define lj_etw_getstate()		0
#endif

#endif

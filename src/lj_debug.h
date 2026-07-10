/*
** Debugging and introspection.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_DEBUG_H
#define _LJ_DEBUG_H

#include "lj_obj.h"

typedef struct lj_Debug {
  /* Common fields. Must be in the same order as in lua.h. */
  int event;
  const char *name;
  const char *namewhat;
  const char *what;
  const char *source;
  int currentline;
  int nups;
  int linedefined;
  int lastlinedefined;
  char short_src[LUA_IDSIZE];
  int i_ci;
  /* Extended fields. Only valid if lj_debug_getinfo() is called with ext = 1.*/
  int nparams;
  int isvararg;
#if LUA_COMPAT_TAILCALL_DEBUG
  int istailcall;
#endif
} lj_Debug;

LJ_FUNC cTValue *lj_debug_frame(lua_State *L, int level, int *size);
LJ_FUNC BCLine LJ_FASTCALL lj_debug_line(GCproto *pt, BCPos pc);
#if defined(LUAJIT_ENABLE_MEMPROF)
/* Exported wrapper around the static debug_frameline for memprof line-precise
** attribution. Returns the actual source line for a Lua function frame (the
** line of the currently executing bytecode), or (BCLine)-1 if it cannot be
** derived (non-Lua frame, no PC). The entire addition is flag-gated so a
** flag-off build is byte-identical. */
LJ_FUNC BCLine lj_debug_frameline(lua_State *L, GCfunc *fn, cTValue *nextframe);
#endif
LJ_FUNC const char *lj_debug_uvname(GCproto *pt, uint32_t idx);
LJ_FUNC const char *lj_debug_uvnamev(cTValue *o, uint32_t idx, TValue **tvp,
				     GCobj **op);
LJ_FUNC const char *lj_debug_slotname(GCproto *pt, const BCIns *pc,
				      BCReg slot, const char **name);
LJ_FUNC const char *lj_debug_funcname(lua_State *L, cTValue *frame,
				      const char **name);
LJ_FUNC void lj_debug_shortname(char *out, GCstr *str, BCLine line);
LJ_FUNC void lj_debug_addloc(lua_State *L, const char *msg,
			     cTValue *frame, cTValue *nextframe);
LJ_FUNC void lj_debug_pushloc(lua_State *L, GCproto *pt, BCPos pc);
LJ_FUNC int lj_debug_getinfo(lua_State *L, const char *what, lj_Debug *ar,
			     int ext);
#if LJ_HASPROFILE
LJ_FUNC void lj_debug_dumpstack(lua_State *L, SBuf *sb, const char *fmt,
				int depth);
#endif

/* Fixed internal variable names. */
#define VARNAMEDEF(_) \
  _(FOR_IDX, "(for index)") \
  _(FOR_STOP, "(for limit)") \
  _(FOR_STEP, "(for step)") \
  _(FOR_GEN, "(for generator)") \
  _(FOR_STATE, "(for state)") \
  _(FOR_CTL, "(for control)")

enum {
  VARNAME_END,
#define VARNAMEENUM(name, str)	VARNAME_##name,
  VARNAMEDEF(VARNAMEENUM)
#undef VARNAMEENUM
  VARNAME__MAX
};

#endif

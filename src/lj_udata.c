/*
** Userdata handling.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_udata_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_udata.h"

GCudata *lj_udata_new(lua_State *L, MSize sz, GCtab *env)
{
  GCudata *ud = (GCudata *)lj_mem_newgcou(L, sizeof(GCudata) + sz);
  global_State *g = G(L);
  newwhite(g, ud);  /* Not finalized. */
  ud->gct = ~LJ_TUDATA;
  ud->udtype = UDTYPE_USERDATA;
  ud->len = sz;
  /* NOBARRIER: The GCudata is new (marked white / light-gray). */
  setgcrefnull(ud->metatable);
  setgcref(ud->env, obj2gco(env));
#if !LJ_HASGCMARK
  /* Classic GC: chain to mainthread->nextgc for lj_gc_separateudata.
  ** Arena GC (LJ_HASGCMARK) enumerates udata via the udata-arena bitmaps
  ** instead, so no chain link is needed there. */
  setgcrefr(ud->nextgc, mainthread(g)->nextgc);
  setgcref(mainthread(g)->nextgc, obj2gco(ud));
#endif
  return ud;
}

void LJ_FASTCALL lj_udata_free(global_State *g, GCudata *ud)
{
#if LJ_HASGCMARK
  lj_assertG(!lj_gc_fin_has(g, obj2gco(ud)),
	     "udata free with fin registry entry: ptr=%p", (void *)ud);
#endif
  lj_mem_freegco(g, ud, sizeudata(ud));
}

#if LJ_64
void *lj_lightud_intern(lua_State *L, void *p)
{
  global_State *g = G(L);
  uint64_t u = (uint64_t)p;
  uint32_t up = lightudup(u);
  uint32_t *segmap = mref(g->gc.lightudseg, uint32_t);
  MSize segnum = g->gc.lightudnum;
  if (segmap) {
    MSize seg;
    for (seg = 0; seg <= segnum; seg++)
      if (segmap[seg] == up)  /* Fast path. */
	return (void *)(((uint64_t)seg << LJ_LIGHTUD_BITS_LO) | lightudlo(u));
    segnum++;
    /* Leave last segment unused to avoid clash with ITERN key. */
    if (segnum >= (1 << LJ_LIGHTUD_BITS_SEG)-1) lj_err_msg(L, LJ_ERR_BADLU);
  }
  if (!((segnum-1) & segnum) && segnum != 1) {
    lj_mem_reallocvec(L, segmap, segnum, segnum ? 2*segnum : 2u, uint32_t);
    setmref(g->gc.lightudseg, segmap);
  }
  g->gc.lightudnum = segnum;
  segmap[segnum] = up;
  return (void *)(((uint64_t)segnum << LJ_LIGHTUD_BITS_LO) | lightudlo(u));
}
#endif

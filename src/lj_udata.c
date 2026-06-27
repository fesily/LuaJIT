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
  GCudata *ud = (GCudata *)lj_mem_newagco(L, sizeof(GCudata) + sz, 1);
  global_State *g = G(L);
  newwhite(g, ud);  /* Not finalized. */
  ud->gct = ~LJ_TUDATA;
  ud->udtype = UDTYPE_USERDATA;
  ud->len = sz;
  /* NOBARRIER: The GCudata is new (marked white). */
  setgcrefnull(ud->metatable);
  setgcref(ud->env, obj2gco(env));
  /* Chain to userdata list (after main thread).
  ** During bitmap sweep the root/udata chains are stale — rebuild
  ** reconstructs them, so skip linking here to avoid double-linking.
  ** Exception (GCF_UDLINK, the rebuild ArenaScan+HugeScan window): a udata
  ** allocated behind the moving cursor would never be relinked, so splice
  ** it onto the sub-chain tail now. For arena udata, clear the cell mark so
  ** the forward ArenaScan cursor (block & mark) does not revisit and
  ** double-link it. For huge udata, set LJ_GC_BLACK as an "already linked"
  ** flag — HugeScan checks BLACK before the dead/alive test and skips the
  ** relink. The slot mark stays set (MARKALLOC); BLACK is cleared in the
  ** bounded HugeClear sub-phase. GCF_HUGECLEAR (HugeScan done) suppresses
  ** the BLACK tag: HugeScan will not revisit, so the tag is unneeded and
  ** would leak past rebuild Done (the bounded HugeClear cursor could miss a
  ** udata allocated behind it). */
#if LJ_HASGCMARK
  if (LJ_UNLIKELY(g->gc.gcmarkflags & GCF_BITMAPSWEEP)) {
    if (g->gc.gcmarkflags & GCF_UDLINK) {
      GCobj *o = obj2gco(ud);
      if (!lj_arena_ishuge(o)) {
	arena_obj_clearmark(ptr2arena(o), ptr2cell(o));
      } else if (!(g->gc.gcmarkflags & GCF_HUGECLEAR)) {
	o->gch.marked |= LJ_GC_BLACK;
      }
      lj_gc_udchain_append(g, o);
    }
  } else
#endif
  {
    setgcrefr(ud->nextgc, mainthread(g)->nextgc);
    setgcref(mainthread(g)->nextgc, obj2gco(ud));
  }
  return ud;
}

void LJ_FASTCALL lj_udata_free(global_State *g, GCudata *ud)
{
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


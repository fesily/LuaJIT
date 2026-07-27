/*
** Function handling (prototypes, functions and upvalues).
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
**
** Portions taken verbatim or adapted from the Lua interpreter.
** Copyright (C) 1994-2008 Lua.org, PUC-Rio. See Copyright Notice in lua.h
*/

#define lj_func_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_func.h"
#include "lj_trace.h"
#include "lj_vm.h"

#include <string.h>

/* -- Prototypes ---------------------------------------------------------- */

void LJ_FASTCALL lj_func_freeproto(global_State *g, GCproto *pt)
{
  lj_mem_freegco(g, pt, pt->sizept);
}

/* -- Upvalues ------------------------------------------------------------ */

#if LJ_HASGCMARK
/* T3a: open UV vector only (no openupval chain). Sorted by uvval descending. */
#define OPENUV_VEC_INIT		8

static void openuv_grow(lua_State *L)
{
  global_State *g = G(L);
  GCRef *old = mref(L->openuv, GCRef);
  MSize oldsz = L->openuvsz;
  MSize newsz = oldsz ? oldsz * 2 : OPENUV_VEC_INIT;
  GCRef *buf = (GCRef *)g->allocf(g->allocd, old,
				  (size_t)oldsz * sizeof(GCRef),
				  (size_t)newsz * sizeof(GCRef));
  if (LJ_UNLIKELY(buf == NULL)) lj_err_mem(L);
  setmref(L->openuv, buf);
  L->openuvsz = newsz;
}

static void openuv_insert(lua_State *L, MSize idx, GCupval *uv)
{
  GCRef *vec;
  if (L->openuvtop >= L->openuvsz)
    openuv_grow(L);
  vec = mref(L->openuv, GCRef);
  if (idx < L->openuvtop)
    memmove(vec + idx + 1, vec + idx,
	    (size_t)(L->openuvtop - idx) * sizeof(GCRef));
  setgcref(vec[idx], obj2gco(uv));
  L->openuvtop++;
}

void lj_func_free_openuv_buf(global_State *g, lua_State *L)
{
  GCRef *vec = mref(L->openuv, GCRef);
  if (vec != NULL) {
    g->allocf(g->allocd, vec, (size_t)L->openuvsz * sizeof(GCRef), 0);
    setmref(L->openuv, NULL);
    L->openuvtop = 0;
    L->openuvsz = 0;
  }
}

/* Path F freeall/lua_close: unconditional freeuv (not closeuv/isdead). */
void lj_func_freeall_openuv(global_State *g, lua_State *L)
{
  GCRef *vec = mref(L->openuv, GCRef);
  MSize i, n = L->openuvtop;
  for (i = 0; i < n; i++) {
    GCupval *uv = gco2uv(gcref(vec[i]));
    lj_assertG(!uv->closed, "closed upvalue in freeall openuv");
    lj_func_freeuv(g, uv);
  }
  L->openuvtop = 0;
}

/* Find existing open upvalue for a stack slot or create a new one. */
static GCupval *func_finduv(lua_State *L, TValue *slot)
{
  global_State *g = G(L);
  GCRef *vec = mref(L->openuv, GCRef);
  MSize i, top = L->openuvtop;
  GCupval *p, *uv;
  for (i = 0; i < top; i++) {
    p = gco2uv(gcref(vec[i]));
    lj_assertG(!p->closed && uvval(p) != &p->tv, "closed upvalue in openuv");
    if (uvval(p) < slot)
      break;
    if (uvval(p) == slot) {
      if (gc_obj_isdead(g, obj2gco(p)))
	gc_obj_resurrect(g, obj2gco(p));
      return p;
    }
  }
  /* Capacity before UV alloc: grow OOM after newagco would leave open UV
  ** off-vector (C4b skip + Path F only walks vector → leak past lua_close). */
  if (L->openuvtop >= L->openuvsz)
    openuv_grow(L);
  uv = (GCupval *)lj_mem_newagco(L, sizeof(GCupval), 1);
  newwhite(g, uv);
  uv->gct = ~LJ_TUPVAL;
  uv->closed = 0;
  setmref(uv->v, slot);
  /* NOBARRIER: The GCupval is new (marked white) and open. uv->nextgc unused. */
  openuv_insert(L, i, uv);
  return uv;
}

/* Close all open upvalues pointing to some stack level or above. */
void LJ_FASTCALL lj_func_closeuv(lua_State *L, TValue *level)
{
  global_State *g = G(L);
  GCRef *vec = mref(L->openuv, GCRef);
  MSize i = 0, n = L->openuvtop, j;
  while (i < n) {
    GCupval *uv = gco2uv(gcref(vec[i]));
    if (uvval(uv) < level)
      break;
    i++;
  }
  if (i == 0)
    return;
  for (j = 0; j < i; j++) {
    GCupval *uv = gco2uv(gcref(vec[j]));
    GCobj *o = obj2gco(uv);
    lj_assertG(!uv->closed && uvval(uv) != &uv->tv, "closed upvalue in openuv");
    if (gc_obj_isdead(g, o))
      lj_func_freeuv(g, uv);
    else
      lj_gc_closeuv(g, uv);
  }
  if (i < n)
    memmove(vec, vec + i, (size_t)(n - i) * sizeof(GCRef));
  L->openuvtop = n - i;
}

void LJ_FASTCALL lj_func_freeuv(global_State *g, GCupval *uv)
{
  /* Open UVs live only on the per-thread openuv vector; caller unlinks first.
  ** prev/next union and nextgc unused under HASGCMARK. */
  lj_mem_freegco(g, uv, sizeof(GCupval));
}

#else  /* !LJ_HASGCMARK */

/* Find existing open upvalue for a stack slot or create a new one. */
static GCupval *func_finduv(lua_State *L, TValue *slot)
{
  global_State *g = G(L);
  GCRef *pp = &L->openupval;
  GCupval *p;
  GCupval *uv;
  /* Search the sorted list of open upvalues. */
  while (gcref(*pp) != NULL && uvval((p = gco2uv(gcref(*pp)))) >= slot) {
    lj_assertG(!p->closed && uvval(p) != &p->tv, "closed upvalue in chain");
    if (uvval(p) == slot) {  /* Found open upvalue pointing to same slot? */
      if (gc_obj_isdead(g, obj2gco(p)))  /* Resurrect it, if it's dead. */
	gc_obj_resurrect(g, obj2gco(p));
      return p;
    }
    pp = &p->nextgc;
  }
  /* No matching upvalue found. Create a new one. */
  uv = (GCupval *)lj_mem_newagco(L, sizeof(GCupval), 1);
  newwhite(g, uv);
  uv->gct = ~LJ_TUPVAL;
  uv->closed = 0;  /* Still open. */
  setmref(uv->v, slot);  /* Pointing to the stack slot. */
  /* NOBARRIER: The GCupval is new (marked white) and open. */
  setgcrefr(uv->nextgc, *pp);  /* Insert into sorted list of open upvalues. */
  setgcref(*pp, obj2gco(uv));
  return uv;
}

/* Close all open upvalues pointing to some stack level or above. */
void LJ_FASTCALL lj_func_closeuv(lua_State *L, TValue *level)
{
  GCupval *uv;
  global_State *g = G(L);
  while (gcref(L->openupval) != NULL &&
	 uvval((uv = gco2uv(gcref(L->openupval)))) >= level) {
    GCobj *o = obj2gco(uv);
    /* Classic: open upvalues stay white/gray until closed. */
    lj_assertG(!gc_obj_isblack(g, o), "bad black upvalue");
    lj_assertG(!uv->closed && uvval(uv) != &uv->tv, "closed upvalue in chain");
    setgcrefr(L->openupval, uv->nextgc);  /* No longer in open list. */
    if (gc_obj_isdead(g, o)) {
      lj_func_freeuv(g, uv);
    } else {
      lj_gc_closeuv(g, uv);
    }
  }
}

void LJ_FASTCALL lj_func_freeuv(global_State *g, GCupval *uv)
{
  /* Open upvalues live on the per-thread openupval chain, unlinked by caller. */
  lj_mem_freegco(g, uv, sizeof(GCupval));
}

#endif  /* LJ_HASGCMARK */

/* Create an empty and closed upvalue. */
static GCupval *func_emptyuv(lua_State *L)
{
  GCupval *uv = (GCupval *)lj_mem_newgcot(L, sizeof(GCupval));
  uv->gct = ~LJ_TUPVAL;
  uv->closed = 1;
  setnilV(&uv->tv);
  setmref(uv->v, &uv->tv);
  return uv;
}

/* -- Functions (closures) ------------------------------------------------ */

GCfunc *lj_func_newC(lua_State *L, MSize nelems, GCtab *env)
{
  GCfunc *fn = (GCfunc *)lj_mem_newgcot_pod(L, sizeCfunc(nelems));
  fn->c.gct = ~LJ_TFUNC;
  fn->c.ffid = FF_C;
  fn->c.nupvalues = (uint8_t)nelems;
  /* NOBARRIER: The GCfunc is new (marked white / light-gray). */
  setmref(fn->c.pc, &G(L)->bc_cfunc_ext);
  setgcref(fn->c.env, obj2gco(env));
  return fn;
}

static GCfunc *func_newL(lua_State *L, GCproto *pt, GCtab *env)
{
  uint32_t count;
  GCfunc *fn = (GCfunc *)lj_mem_newgcot_pod(L, sizeLfunc((MSize)pt->sizeuv));
  fn->l.gct = ~LJ_TFUNC;
  fn->l.ffid = FF_LUA;
#if LJ_HASGCARENA
  {
    /* The arena allocator frees with the exact allocation size, which is
    ** derived from nupvalues. An OOM while the upvalues are created would
    ** leave a smaller nupvalues behind, so set the final count right away.
    ** The refs point at the function itself until they are filled in:
    ** harmless for the GC traversal, unlike cleared refs.
    */
    MSize i;
    for (i = 0; i < pt->sizeuv; i++)
      setgcref(fn->l.uvptr[i], obj2gco(fn));
    fn->l.nupvalues = (uint8_t)pt->sizeuv;
  }
#else
  fn->l.nupvalues = 0;  /* Set to zero until upvalues are initialized. */
#endif
  /* NOBARRIER: Really a setgcref. But the GCfunc is new (marked white / light-gray). */
  setmref(fn->l.pc, proto_bc(pt));
  setgcref(fn->l.env, obj2gco(env));
  /* Saturating 3 bit counter (0..7) for created closures. */
  count = (uint32_t)pt->flags + PROTO_CLCOUNT;
  pt->flags = (uint8_t)(count - ((count >> PROTO_CLC_BITS) & PROTO_CLCOUNT));
  return fn;
}

/* Create a new Lua function with empty upvalues. */
GCfunc *lj_func_newL_empty(lua_State *L, GCproto *pt, GCtab *env)
{
  GCfunc *fn = func_newL(L, pt, env);
  MSize i, nuv = pt->sizeuv;
  /* NOBARRIER: The GCfunc is new (marked white / light-gray). */
  for (i = 0; i < nuv; i++) {
    GCupval *uv = func_emptyuv(L);
    int32_t v = proto_uv(pt)[i];
    uv->immutable = ((v / PROTO_UV_IMMUTABLE) & 1);
    uv->dhash = (uint32_t)(uintptr_t)pt ^ (v << 24);
    setgcref(fn->l.uvptr[i], obj2gco(uv));
  }
  fn->l.nupvalues = (uint8_t)nuv;
  return fn;
}

/* Do a GC check and create a new Lua function with inherited upvalues. */
GCfunc *lj_func_newL_gc(lua_State *L, GCproto *pt, GCfuncL *parent)
{
  GCfunc *fn;
  GCRef *puv;
  MSize i, nuv;
  TValue *base;
  lj_gc_check_fixtop(L);
  fn = func_newL(L, pt, tabref(parent->env));
  /* NOBARRIER: The GCfunc is new (marked white / light-gray). */
  puv = parent->uvptr;
  nuv = pt->sizeuv;
  base = L->base;
  for (i = 0; i < nuv; i++) {
    uint32_t v = proto_uv(pt)[i];
    GCupval *uv;
    if ((v & PROTO_UV_LOCAL)) {
      uv = func_finduv(L, base + (v & 0xff));
      uv->immutable = ((v / PROTO_UV_IMMUTABLE) & 1);
      uv->dhash = (uint32_t)(uintptr_t)mref(parent->pc, char) ^ (v << 24);
    } else {
      uv = &gcref(puv[v])->uv;
    }
    setgcref(fn->l.uvptr[i], obj2gco(uv));
  }
  fn->l.nupvalues = (uint8_t)nuv;
  return fn;
}

void LJ_FASTCALL lj_func_free(global_State *g, GCfunc *fn)
{
  MSize size = isluafunc(fn) ? sizeLfunc((MSize)fn->l.nupvalues) :
			       sizeCfunc((MSize)fn->c.nupvalues);
  lj_mem_freegco(g, fn, size);
}


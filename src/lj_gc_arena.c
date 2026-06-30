/*
** Garbage collector.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
**
** Major portions taken verbatim or adapted from the Lua interpreter.
** Copyright (C) 1994-2008 Lua.org, PUC-Rio. See Copyright Notice in lua.h
*/

#include "lj_arch.h"
#if LJ_HASGCMARK

#define lj_gc_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_buf.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_func.h"
#include "lj_udata.h"
#include "lj_meta.h"
#include "lj_state.h"
#include "lj_frame.h"
#if LJ_HASFFI
#include "lj_ctype.h"
#include "lj_cdata.h"
#endif
#include "lj_trace.h"
#include "lj_dispatch.h"
#include "lj_vm.h"
#include "lj_vmevent.h"
#include "lj_arena.h"

#define GCSTEPSIZE	1024u
#define GCSWEEPMAX	40
#define GCSWEEPCOST	10
#define GCFINALIZECOST	100

/* Macros to set GCobj colors and flags. */
#define white2gray(x) \
  ((x)->gch.marked |= LJ_GC_GRAY)
#define gray2black(x) \
  ((x)->gch.marked &= (uint8_t)~LJ_GC_GRAY)
#define isfinalized(u)		((u)->marked & LJ_GC_FINALIZED)

/* -- Mark phase ---------------------------------------------------------- */

/* Mainthread and strempty live in dlmalloc, not arenas.
** Huge GC objects (>= ArenaHugeThreshold) are arena-aligned allocations
** without arena metadata — caught by lj_arena_ishuge. */
#define gc_inarena(g, o)  \
  (!lj_arena_ishuge(o) && \
   (o) != obj2gco(mainthread(g)) && (o) != obj2gco(&(g)->strempty))

/* Mark a TValue (if needed). */
#define gc_marktv(g, tv) \
  { if (tvisgcv(tv) && gc_obj_iswhite((g), gcV(tv))) gc_mark(g, gcV(tv)); }

/* Mark a GCobj (if needed). */
#define gc_markobj(g, o) \
  { GCobj *mo_ = obj2gco(o); if (gc_obj_iswhite((g), mo_)) gc_mark(g, mo_); }

/* Mark a string object. */
#define gc_mark_str(g, s) do { \
  if (gc_inarena(g, obj2gco(s))) \
    arena_obj_setmark(ptr2arena(s), ptr2cell(s)); \
  else if (lj_arena_ishuge(obj2gco(s))) \
    huge_obj_setmark(g, obj2gco(s)); \
  else \
    (s)->marked |= LJ_GC_BLACK; \
  } while (0)

static void gc_hugegray_push(global_State *g, GCobj *o);
static int gc_hugegray_empty(global_State *g);
static GCobj *gc_hugegray_pop(global_State *g);
static void gc_hugegray_reset(global_State *g);
static void gc_graythread_push(global_State *g, GCobj *o);
static int gc_graythread_empty(global_State *g);
static GCobj *gc_graythread_pop(global_State *g);
static void gc_graythread_reset(global_State *g);
static void gc_weak_push(global_State *g, GCobj *o, int weak);
static void gc_weak_reset(global_State *g);
static void gc_weak_redirect_all(global_State *g);
static void gc_clearweak_stacks(global_State *g);
static void gc_traverse_mainthread(global_State *g);

/* LUA_USE_ASSERT: invariant check for a live hugeset slot. Verifies the
** CDATAV discriminator agrees with the on-block metadata:
**  - CDATAV slot: base is a GCcdataVar prefix; cd = base + GCcdataVar.offset
**    must be a VLA cdata whose memcdatav(cd) points back to base.
**  - non-CDATAV slot: a cdata at base must NOT be a VLA prefix (a VLA
**    registered without the CDATAV flag would regress the A2+A3 fix).
** Entire body gated by LUA_USE_ASSERT; release builds pay nothing. */
#ifdef LUA_USE_ASSERT
static LJ_AINLINE void hugeset_slot_assert(global_State *g, uintptr_t u)
{
#if LJ_HASFFI
  GCobj *base = hugeset_slot_addr(u);
  if (u & HUGESET_CDATAV) {
    GCcdata *cd = (GCcdata *)((char *)base + ((GCcdataVar *)base)->offset);
    lj_assertG(cd->gct == ~LJ_TCDATA,
	       "hugeset CDATAV slot: cd gct mismatch (base=%p cd=%p gct=0x%02x)",
	       (void *)base, (void *)cd, cd->gct);
    lj_assertG(cdataisv(cd),
	       "hugeset CDATAV slot: cd not a VLA cdata (base=%p cd=%p marked=0x%02x)",
	       (void *)base, (void *)cd, cd->marked);
    lj_assertG(memcdatav(cd) == (void *)base,
	       "hugeset CDATAV slot: memcdatav(cd) != base (base=%p cd=%p mem=%p)",
	       (void *)base, (void *)cd, memcdatav(cd));
  } else if (base->gch.gct == ~LJ_TCDATA) {
    /* A non-VLA huge cdata (lj_cdata_new) has its GCobj at base: gct is CDATA
    ** but cdataisv is false. A VLA prefix at an unflagged slot would read
    ** garbage in the gct/marked bytes; if those happen to look like CDATA+VLA
    ** it is a missing-CDATAV-flag regression. */
    GCcdata *cd = (GCcdata *)base;
    lj_assertG(!cdataisv(cd),
	       "hugeset unflagged slot is a VLA cdata prefix -- missing CDATAV flag "
	       "(base=%p marked=0x%02x)", (void *)base, cd->marked);
  }
#else
  (void)g; (void)u;
#endif
}
#else
#define hugeset_slot_assert(g, u)	((void)0)
#endif

/* LUA_USE_ASSERT: invariant check for one allocated cell in a CdataV arena.
** Mirrors hugeset_slot_assert for arena-resident VLA cdata. The cell base p is
** a GCcdataVar prefix; cd = p + GCcdataVar.offset is the GCobj. Asserts the
** base<->object translation B2+B3 relies on: cd is a VLA cdata, memcdatav
** round-trips back to p, and the offset fits uint16_t (matches lj_cdata_newv's
** allocation-time assert). Entire body gated by LUA_USE_ASSERT; release builds
** pay nothing. */
#ifdef LUA_USE_ASSERT
static LJ_AINLINE void cdatav_cell_assert(global_State *g, void *p_)
{
#if LJ_HASFFI
  char *p = (char *)p_;
  GCcdataVar *cv = (GCcdataVar *)p;
  GCcdata *cd = (GCcdata *)(p + cv->offset);
  lj_assertG(cd->gct == ~LJ_TCDATA,
	     "CdataV arena cell: cd gct mismatch (base=%p cd=%p gct=0x%02x)",
	     (void *)p, (void *)cd, cd->gct);
  lj_assertG(cdataisv(cd),
	     "CdataV arena cell: cd not a VLA cdata (base=%p cd=%p marked=0x%02x)",
	     (void *)p, (void *)cd, cd->marked);
  lj_assertG(memcdatav(cd) == (void *)p,
	     "CdataV arena cell: memcdatav(cd) != base (base=%p cd=%p mem=%p)",
	     (void *)p, (void *)cd, memcdatav(cd));
  lj_assertG((char *)cd - p < 65536,
	     "CdataV arena cell: offset exceeds uint16_t (base=%p cd=%p off=%d)",
	     (void *)p, (void *)cd, (int)((char *)cd - p));
#else
  (void)g; (void)p_;
#endif
}
#else
#define cdatav_cell_assert(g, p)	((void)0)
#endif

/* LUA_USE_ASSERT: root-chain probe + anchor-only assertion for the arena
** collector (T0 of the deprecate-arena-gc-root-chain plan). The root chain
** (g->gc.root -> gch.nextgc -> ...) is being deprecated in favor of arena
** bitmaps + the hugeset; these helpers measure the current writers so later
** phases (T1-T6) can verify removal, and T5 can assert the final anchor-only
** state globally.
**
** GC_ROOT_CHAIN_MAX bounds the walk so a corrupted chain cannot loop forever;
** hitting the cap is itself an assertion failure.
**
** gc_root_chain_count(g)       -> bounded total length of g->gc.root chain.
** gc_root_chain_probe_print(g) -> per-gct breakdown to stderr, gated by
**   getenv("LUAJIT_GC_ROOT_CHAIN_PROBE") so normal assert runs are not spammed.
**   Used by the baseline harness test/gc/root_chain_probe_assert.lua.
** gc_assert_root_anchor_only(g)-> asserts the steady state reached after
**   rebuild/freeall/init: g->gc.root == mainthread && mainthread->nextgc == NULL.
**   T0 only DEFINES this helper; T5 wires the global call sites.
**
** Entirely gated by LUA_USE_ASSERT (and LJ_HASGCMARK via the file guard);
** release builds pay nothing. */
#define GC_ROOT_CHAIN_MAX	100000u

#ifdef LUA_USE_ASSERT
#include <stdio.h>
#include <stdlib.h>

static uint32_t gc_root_chain_count(global_State *g)
{
  GCobj *o = gcref(g->gc.root);
  uint32_t n = 0;
  while (o != NULL) {
    n++;
    if (n >= GC_ROOT_CHAIN_MAX) {
      lj_assertG(0,
		 "gc_root_chain_count: hit safety cap %u (corrupted chain?)",
		 GC_ROOT_CHAIN_MAX);
      return n;
    }
    o = gcref(o->gch.nextgc);
  }
  return n;
}

/* Per-gct breakdown printed to stderr when LUAJIT_GC_ROOT_CHAIN_PROBE is set.
** Walks the same chain as gc_root_chain_count; the per-type counters reflect
** which writers currently link to g->gc.root (strings/udata/VLA-cdata are
** expected to be absent -- negative controls). */
static void gc_root_chain_probe_print(global_State *g, const char *tag)
{
  if (LJ_UNLIKELY(getenv("LUAJIT_GC_ROOT_CHAIN_PROBE") != NULL)) {
    GCobj *o = gcref(g->gc.root);
    uint32_t total = 0;
    uint32_t cstr = 0, cupval = 0, cthread = 0, cproto = 0, cfunc = 0;
    uint32_t ctrace = 0, ccdata = 0, ctab = 0, cudata = 0, cother = 0;
    while (o != NULL) {
      if (total >= GC_ROOT_CHAIN_MAX) {
	lj_assertG(0,
		   "gc_root_chain_probe_print: hit safety cap %u (corrupted chain?)",
		   GC_ROOT_CHAIN_MAX);
	break;
      }
      total++;
      if (o->gch.gct == ~LJ_TSTR) cstr++;
      else if (o->gch.gct == ~LJ_TUPVAL) cupval++;
      else if (o->gch.gct == ~LJ_TTHREAD) cthread++;
      else if (o->gch.gct == ~LJ_TPROTO) cproto++;
      else if (o->gch.gct == ~LJ_TFUNC) cfunc++;
      else if (o->gch.gct == ~LJ_TTRACE) ctrace++;
      else if (o->gch.gct == ~LJ_TCDATA) ccdata++;
      else if (o->gch.gct == ~LJ_TTAB) ctab++;
      else if (o->gch.gct == ~LJ_TUDATA) cudata++;
      else cother++;
      o = gcref(o->gch.nextgc);
    }
    fprintf(stderr,
      "[gc_root_chain_probe] %s: total=%u str=%u upval=%u th=%u proto=%u "
      "func=%u trace=%u cdata=%u tab=%u ud=%u other=%u\n",
      tag ? tag : "?", total, cstr, cupval, cthread, cproto, cfunc, ctrace,
      ccdata, ctab, cudata, cother);
  }
}

static LJ_AINLINE void gc_assert_root_anchor_only(global_State *g)
{
  GCobj *root = gcref(g->gc.root);
  GCobj *mt = obj2gco(mainthread(g));
  lj_assertG(root == mt,
	     "root chain not anchored on mainthread alone (root=%p mt=%p)",
	     (void *)root, (void *)mt);
  lj_assertG(gcref(mt->gch.nextgc) == NULL,
	     "mainthread has a nextgc link (anchor-only invariant): nextgc=%p",
	     (void *)gcref(mt->gch.nextgc));
}
#else
#define gc_root_chain_count(g)			((uint32_t)0)
#define gc_root_chain_probe_print(g, tag)	((void)0)
#define gc_assert_root_anchor_only(g)		((void)0)
#endif

/* Mark a GCobj. */
static void gc_mark(global_State *g, GCobj *o)
{
  int gct = o->gch.gct;
  /* VLA cdata: the block base (memcdatav) is the arena/huge classification key
  ** and the mark site; o=cd carries the GCcdata header used below. */
  void *key = gc_obj_key(o);
  int inarena = !lj_arena_ishuge(key) && !(o->gch.marked & LJ_GC_SFIXED);
  int inhuge = !inarena && lj_arena_ishuge(key);
  GCArena *a;
  GCCellID c;
  if (inarena) {
    a = ptr2arena(key);
    c = ptr2cell(key);
    if (arena_obj_ismarked(a, c))
      return;
  } else if (inhuge) {
    if (huge_obj_ismarked(g, key))  /* Slot mark is the dedup gate. */
      return;
  } else if (o->gch.marked & LJ_GC_BLACK) {
    return;  /* Non-arena FIXED root already marked black this cycle. */
  }
  lj_assertG(inarena || inhuge || !(o->gch.marked & LJ_GC_BLACK),
	     "mark of already-black non-arena object");
  white2gray(o);
  if (inarena)
    arena_obj_setmark(a, c);
  else if (inhuge)
    huge_obj_setmark(g, key);
  else
    o->gch.marked |= LJ_GC_BLACK;
  if (LJ_UNLIKELY(gct == ~LJ_TUDATA)) {
    GCtab *mt = tabref(gco2ud(o)->metatable);
    gray2black(o);  /* Userdata are never gray. */
    if (mt) gc_markobj(g, mt);
    gc_markobj(g, tabref(gco2ud(o)->env));
    if (LJ_HASBUFFER && gco2ud(o)->udtype == UDTYPE_BUFFER) {
      SBufExt *sbx = (SBufExt *)uddata(gco2ud(o));
      if (sbufiscow(sbx) && gcref(sbx->cowref))
	gc_markobj(g, gcref(sbx->cowref));
      if (gcref(sbx->dict_str))
	gc_markobj(g, gcref(sbx->dict_str));
      if (gcref(sbx->dict_mt))
	gc_markobj(g, gcref(sbx->dict_mt));
    }
  } else if (LJ_UNLIKELY(gct == ~LJ_TUPVAL)) {
    GCupval *uv = gco2uv(o);
    gc_marktv(g, uvval(uv));
    if (uv->closed)
      gray2black(o);  /* Closed upvalues are never gray. */
  } else if (gct != ~LJ_TSTR && gct != ~LJ_TCDATA) {
    lj_assertG(gct == ~LJ_TFUNC || gct == ~LJ_TTAB ||
	       gct == ~LJ_TTHREAD || gct == ~LJ_TPROTO || gct == ~LJ_TTRACE,
	       "bad GC type %d", gct);
    lj_assertG(o->gch.marked & LJ_GC_GRAY,
      "gc_mark push without gray bit: gct=%d marked=0x%02x",
      o->gch.gct, o->gch.marked);
    if (inarena) {
      arena_gray_push(g, a, (GCCellID1)c);
    } else {
      if (o == obj2gco(mainthread(g))) {
	lj_assertG(gct == ~LJ_TTHREAD, "mainthread is not a thread");
      } else {
	lj_assertG(lj_arena_ishuge(o), "non-arena traversable object is not huge");
	gc_hugegray_push(g, o);
      }
    }
  }
}

/* Mark GC roots. */
static void gc_mark_gcroot(global_State *g)
{
  ptrdiff_t i;
  for (i = 0; i < GCROOT_MAX; i++)
    if (gcref(g->gcroot[i]) != NULL)
      gc_markobj(g, gcref(g->gcroot[i]));
}

/* Start a GC cycle and mark the root set. */
static void gc_mark_start(global_State *g)
{
  gc_hugegray_reset(g);
  gc_graythread_reset(g);
  gc_weak_reset(g);
  g->gc.grayastop = 0;
  setmref(g->gc.ssbtop, mref(g->gc.ssb, GCobj *));
  {
    MSize i;
    for (i = 0; i < g->gc.arenastop; i++) {
      GCArena *a = mref(g->gc.arenas, GCArena *)[i];
      a->flags &= (uint16_t)~ArenaFlag_InGrayHeap;  /* Heap drained above. */
      if (mref(a->greybase, GCCellID1) != NULL)
	arena_gray_reset(a);
    }
  }
  lj_arena_gc_markinit(g);
  gc_markobj(g, mainthread(g));
  gc_markobj(g, tabref(mainthread(g)->env));
  gc_markobj(g, vmthread(g));
  gc_marktv(g, &g->registrytv);
  gc_mark_gcroot(g);
  gc_traverse_mainthread(g);
  g->gc.state = GCSpropagate;
}

/* Mark open upvalues. */
static void gc_mark_uv(global_State *g)
{
  GCupval *uv;
  for (uv = uvnext(&g->uvhead); uv != &g->uvhead; uv = uvnext(uv)) {
    lj_assertG(uvprev(uvnext(uv)) == uv && uvnext(uvprev(uv)) == uv,
	       "broken upvalue chain");
    if (isgray(obj2gco(uv)))
      gc_marktv(g, uvval(uv));
  }
}

/* Mark userdata in mmudata list. */
static void gc_mark_mmudata(global_State *g)
{
  GCobj *root = gcref(g->gc.mmudata);
  GCobj *u = root;
  if (u) {
    do {
      u = gcnext(u);
      gc_obj_makewhite(g, u);  /* Could be from previous GC. */
      gc_mark(g, u);
    } while (u != root);
  }
}

/* Per-udata predicate + mmudata ring splice, factored out so the arena
** bitmap scan and the hugeset scan share the IDENTICAL logic that the old
** mainthread->nextgc chain walk applied. Returns the sizeudata bytes counted
** toward the finalize budget (matching the legacy return accounting). */
static size_t sepudata_one(global_State *g, GCobj *o, int all)
{
  if (!(gc_obj_iswhite(g, o) || all) || isfinalized(gco2ud(o)))
    return 0;  /* Nothing to do. */
  if (!lj_meta_fastg(g, tabref(gco2ud(o)->metatable), MM_gc)) {
    markfinalized(o);  /* No __gc metamethod: mark finalized, leave in place. */
    return 0;
  }
  /* Has __gc: move to mmudata ring. markfinalized before splice so a
  ** re-encounter during this same scan (ring members live in udata arenas)
  ** is skipped by the isfinalized test above. */
  size_t sz = sizeudata(gco2ud(o));
  markfinalized(o);
  if (gcref(g->gc.mmudata)) {  /* Link to end of circular mmudata list. */
    GCobj *root = gcref(g->gc.mmudata);
    setgcrefr(o->gch.nextgc, root->gch.nextgc);
    setgcref(root->gch.nextgc, o);
    setgcref(g->gc.mmudata, o);
  } else {  /* Create circular list. */
    setgcref(o->gch.nextgc, o);
    setgcref(g->gc.mmudata, o);
  }
  return sz;
}

/* Separate userdata objects to be finalized to mmudata list.
**
** T4: enumerates udata by scanning the ArenaFlag_UdataOnly arenas' block
** bitmaps (every allocated cell is a GCudata by class invariant) plus the
** hugeset slots whose gct == ~LJ_TUDATA. This replaces the old
** mainthread->nextgc chain walk, which is no longer maintained at alloc
** time (T3). The predicate is identical to the legacy chain walk — only
** the enumeration source changed.
**
** Preconditions (hold at the atomic() call site, before GCF_BITMAPSWEEP):
**   - gc_obj_iswhite(arena obj) == !arena_obj_ismarked (mark is authoritative).
**   - lj_arena_flushbins(a) is called per arena so binned free blocks read as
**     Free (block=0,mark=1), not Allocated (1,0) — a free block's payload is
**     a freelist cell ID, not a valid GCudata header. */
size_t lj_gc_separateudata(global_State *g, int all)
{
  size_t m = 0;
  MSize i;
  GCArena **arenas = mref(g->gc.arenas, GCArena *);
  /* Scan udata arenas: every allocated cell is a GCudata (class invariant). */
  for (i = 0; i < g->gc.arenastop; i++) {
    GCArena *a = arenas[i];
    uint32_t w, wtop;
    if (!(a->flags & ArenaFlag_UdataOnly)) continue;
    lj_arena_flushbins(a);  /* Free blocks read as Free, not Allocated. */
    wtop = arena_blockidx((GCCellID)a->celltop - 1);
    for (w = UnusedBlockWords; w <= wtop; w++) {
      GCBlockword heads = a->block[w];
      while (heads) {
	uint32_t bitidx = lj_ffs(heads);
	GCCellID c = (w << 5) + bitidx;
	GCobj *o = (GCobj *)arena_cellptr(a, c);
	heads &= heads - 1;
	lj_assertG(o->gch.gct == ~LJ_TUDATA,
		   "non-udata in Udata arena: gct=%d cell=%d",
		   (int)o->gch.gct, (int)c);
	m += sepudata_one(g, o, all);
      }
    }
  }
  /* Scan hugeset for huge udata (huge objects have no arena class). */
  {
    GCRef *slots = mref(g->gc.hugeset, GCRef);
    if (slots != NULL) {
      MSize hi, hmask = g->gc.hugesetmask;
      for (hi = 0; hi <= hmask; hi++) {
	uintptr_t u = gcrefu(slots[hi]);
	GCobj *o;
	if (!hugeset_slot_live(u)) continue;
	hugeset_slot_assert(g, u);
	o = hugeset_slot_obj(u);
	if (o->gch.gct != ~LJ_TUDATA) continue;
	m += sepudata_one(g, o, all);
      }
    }
  }
  return m;
}

/* -- Propagation phase --------------------------------------------------- */

/* Traverse a table. */
static int gc_traverse_tab(global_State *g, GCtab *t)
{
  int weak = 0;
  cTValue *mode;
  GCtab *mt = tabref(t->metatable);
  if (mt)
    gc_markobj(g, mt);
  mode = lj_meta_fastg(g, mt, MM_mode);
  if (mode && tvisstr(mode)) {  /* Valid __mode field? */
    const char *modestr = strVdata(mode);
    int c;
    while ((c = *modestr++)) {
      if (c == 'k') weak |= LJ_GC_WEAKKEY;
      else if (c == 'v') weak |= LJ_GC_WEAKVAL;
    }
    if (weak) {  /* Weak tables are cleared in the atomic phase. */
#if LJ_HASFFI
      if (gcref(g->gcroot[GCROOT_FFI_FIN]) == obj2gco(t)) {
	weak = (int)(~0u & ~LJ_GC_WEAKVAL);
      } else
#endif
      {
	t->marked = (uint8_t)((t->marked & ~LJ_GC_WEAK) | weak);
	gc_weak_push(g, obj2gco(t), weak);
      }
    }
  }
  if (weak == LJ_GC_WEAK)  /* Nothing to mark if both keys/values are weak. */
    return 1;
  if (!(weak & LJ_GC_WEAKVAL)) {  /* Mark array part. */
    MSize i, asize = t->asize;
    for (i = 0; i < asize; i++)
      gc_marktv(g, arrayslot(t, i));
  }
  if (t->hmask > 0) {  /* Mark hash part. */
    Node *node = noderef(t->node);
    MSize i, hmask = t->hmask;
    for (i = 0; i <= hmask; i++) {
      Node *n = &node[i];
      if (!tvisnil(&n->val)) {  /* Mark non-empty slot. */
	lj_assertG(!tvisnil(&n->key), "mark of nil key in non-empty slot");
	if (!(weak & LJ_GC_WEAKKEY)) gc_marktv(g, &n->key);
	if (!(weak & LJ_GC_WEAKVAL)) gc_marktv(g, &n->val);
      }
    }
  }
  return weak;
}

/* Traverse a function. */
static void gc_traverse_func(global_State *g, GCfunc *fn)
{
  gc_markobj(g, tabref(fn->c.env));
  if (isluafunc(fn)) {
    uint32_t i;
    lj_assertG(fn->l.nupvalues <= funcproto(fn)->sizeuv,
	       "function upvalues out of range");
    gc_markobj(g, funcproto(fn));
    for (i = 0; i < fn->l.nupvalues; i++)  /* Mark Lua function upvalues. */
      gc_markobj(g, &gcref(fn->l.uvptr[i])->uv);
  } else {
    uint32_t i;
    for (i = 0; i < fn->c.nupvalues; i++)  /* Mark C function upvalues. */
      gc_marktv(g, &fn->c.upvalue[i]);
  }
}

#if LJ_HASJIT
/* Mark a trace. */
static void gc_marktrace(global_State *g, TraceNo traceno)
{
  GCobj *o = obj2gco(traceref(G2J(g), traceno));
  lj_assertG(traceno != G2J(g)->cur.traceno, "active trace escaped");
  if (gc_obj_iswhite(g, o)) {
    white2gray(o);
    if (gc_inarena(g, o)) {
      GCArena *a = ptr2arena(o);
      GCCellID c = ptr2cell(o);
      arena_obj_setmark(a, c);
      arena_gray_push(g, a, (GCCellID1)c);
    } else {
      lj_assertG(lj_arena_ishuge(o), "non-arena trace is not huge");
      huge_obj_setmark(g, o);  /* Traces are never strings: slot mark. */
      gc_hugegray_push(g, o);
    }
  }
}

/* Traverse a trace. */
static void gc_traverse_trace(global_State *g, GCtrace *T)
{
  IRRef ref;
  if (T->traceno == 0) return;
  for (ref = T->nk; ref < REF_TRUE; ref++) {
    IRIns *ir = &T->ir[ref];
    if (ir->o == IR_KGC)
      gc_markobj(g, ir_kgc(ir));
    if (irt_is64(ir->t) && ir->o != IR_KNULL)
      ref++;
  }
  if (T->link) gc_marktrace(g, T->link);
  if (T->nextroot) gc_marktrace(g, T->nextroot);
  if (T->nextside) gc_marktrace(g, T->nextside);
  gc_markobj(g, gcref(T->startpt));
}

/* The current trace is a GC root while not anchored in the prototype (yet). */
#define gc_traverse_curtrace(g)	gc_traverse_trace(g, &G2J(g)->cur)
#else
#define gc_traverse_curtrace(g)	UNUSED(g)
#endif

/* Traverse a prototype. */
static void gc_traverse_proto(global_State *g, GCproto *pt)
{
  ptrdiff_t i;
  gc_mark_str(g, proto_chunkname(pt));
  for (i = -(ptrdiff_t)pt->sizekgc; i < 0; i++)  /* Mark collectable consts. */
    gc_markobj(g, proto_kgc(pt, i));
#if LJ_HASJIT
  if (pt->trace) gc_marktrace(g, pt->trace);
#endif
}

/* Traverse the frame structure of a stack. */
static MSize gc_traverse_frames(global_State *g, lua_State *th)
{
  TValue *frame, *top = th->top-1, *bot = tvref(th->stack);
  /* Note: extra vararg frame not skipped, marks function twice (harmless). */
  for (frame = th->base-1; frame > bot+LJ_FR2; frame = frame_prev(frame)) {
    GCfunc *fn = frame_func(frame);
    TValue *ftop = frame;
    if (isluafunc(fn)) ftop += funcproto(fn)->framesize;
    if (ftop > top) top = ftop;
    if (!LJ_FR2) gc_markobj(g, fn);  /* Need to mark hidden function (or L). */
  }
  top++;  /* Correct bias of -1 (frame == base-1). */
  if (top > tvref(th->maxstack)) top = tvref(th->maxstack);
  return (MSize)(top - bot);  /* Return minimum needed stack size. */
}

/* Traverse a thread object. */
static void gc_traverse_thread(global_State *g, lua_State *th)
{
  TValue *o, *top = th->top;
  for (o = tvref(th->stack)+1+LJ_FR2; o < top; o++)
    gc_marktv(g, o);
  /* Under single-white, always clear above top — not just at atomic.
  ** With gc_marktv accepting gray objects, stale stack slots from
  ** previous frames would otherwise keep dead objects alive. */
  {
    TValue *stend = tvref(th->stack) + th->stacksize;
    for (; o < stend; o++)
      setnilV(o);
  }
  gc_markobj(g, tabref(th->env));
  lj_state_shrinkstack(th, gc_traverse_frames(g, th));
}

/* Traverse the host-allocated main thread directly: it is SFIXED and cannot
** live in an arena or on a per-arena gray stack. */
static void gc_traverse_mainthread(global_State *g)
{
  gc_traverse_thread(g, mainthread(g));
}

/* Propagate one gray object. Traverse it and turn it black. */
static size_t propagatemark(global_State *g
  , GCobj *o
)
{
  int gct = o->gch.gct;
  lj_assertG(isgray(o), "propagation of non-gray object");
  lj_assertG(o->gch.marked & LJ_GC_GRAY,
    "gray object missing gray bit: gct=%d marked=0x%02x ptr=%p state=%d",
    o->gch.gct, o->gch.marked, (void*)o, g->gc.state);
  lj_assertG(!(o->gch.marked & LJ_GC_BLACK) || !gc_inarena(g, o),
    "arena object has header BLACK bit: ptr=%p gct=%d marked=0x%02x",
    (void*)o, o->gch.gct, o->gch.marked);
  gray2black(o);
  if (LJ_LIKELY(gct == ~LJ_TTAB)) {
    GCtab *t = gco2tab(o);
    if (gc_traverse_tab(g, t) > 0)
      black2gray(o);  /* Keep weak tables gray. */
    return sizeof(GCtab) + sizeof(TValue) * t->asize +
			   (t->hmask ? sizeof(Node) * (t->hmask + 1) : 0);
  } else if (LJ_LIKELY(gct == ~LJ_TFUNC)) {
    GCfunc *fn = gco2func(o);
    gc_traverse_func(g, fn);
    return isluafunc(fn) ? sizeLfunc((MSize)fn->l.nupvalues) :
			   sizeCfunc((MSize)fn->c.nupvalues);
  } else if (LJ_LIKELY(gct == ~LJ_TPROTO)) {
    GCproto *pt = gco2pt(o);
    gc_traverse_proto(g, pt);
    return pt->sizept;
  } else if (LJ_LIKELY(gct == ~LJ_TTHREAD)) {
    lua_State *th = gco2th(o);
    gc_graythread_push(g, o);
    black2gray(o);  /* Threads are never black. */
    gc_traverse_thread(g, th);
    return sizeof(lua_State) + sizeof(TValue) * th->stacksize;
  } else {
#if LJ_HASJIT
    GCtrace *T = gco2trace(o);
    gc_traverse_trace(g, T);
    return ((sizeof(GCtrace)+7)&~7) + (T->nins-T->nk)*sizeof(IRIns) +
	   T->nsnap*sizeof(SnapShot) + T->nsnapmap*sizeof(SnapEntry);
#else
    lj_assertG(0, "bad GC type %d", gct);
    return 0;
#endif
  }
}

/* Gray arena priority: number of entries on the arena's gray stack. */
static LJ_AINLINE MSize grayarena_prio(global_State *g, MSize idx)
{
  GCArena *a = mref(g->gc.arenas, GCArena *)[idx];
  GCCellID1 *top = mref(a->greytop, GCCellID1);
  GCCellID1 *base = mref(a->greybase, GCCellID1);
  if (top == NULL || top <= base) return 0;
  return (MSize)(top - base);
}

/* Sift a heap entry up (toward the root). */
static void grayheap_siftup(global_State *g, MSize *heap, MSize pos)
{
  MSize val = heap[pos];
  MSize prio = grayarena_prio(g, val);
  while (pos > 0) {
    MSize parent = (pos - 1) >> 1;
    if (grayarena_prio(g, heap[parent]) >= prio) break;
    heap[pos] = heap[parent];
    pos = parent;
  }
  heap[pos] = val;
}

/* Sift a heap entry down (toward the leaves). */
static void grayheap_siftdown(global_State *g, MSize *heap, MSize n, MSize pos)
{
  MSize val = heap[pos];
  MSize prio = grayarena_prio(g, val);
  for (;;) {
    MSize child = (pos << 1) + 1;
    if (child >= n) break;
    if (child + 1 < n &&
	grayarena_prio(g, heap[child + 1]) > grayarena_prio(g, heap[child]))
      child++;
    if (prio >= grayarena_prio(g, heap[child])) break;
    heap[pos] = heap[child];
    pos = child;
  }
  heap[pos] = val;
}

/* Pop the arena with the largest gray stack from the heap. */
static GCArena *gc_grayarena_pop(global_State *g)
{
  GCArena **arenas = mref(g->gc.arenas, GCArena *);
  MSize *heap = mref(g->gc.grayastack, MSize);
  while (g->gc.grayastop > 0) {
    MSize idx = heap[0];
    GCArena *a = arenas[idx];
    if ((a->flags & ArenaFlag_TravObjs) && !arena_gray_empty(a))
      return a;
    /* Stale entry — remove from heap and drop its membership flag. */
    a->flags &= (uint16_t)~ArenaFlag_InGrayHeap;
    g->gc.grayastop--;
    if (g->gc.grayastop > 0) {
      heap[0] = heap[g->gc.grayastop];
      grayheap_siftdown(g, heap, g->gc.grayastop, 0);
    }
  }
  return NULL;
}

/* Pop one gray object from an arena and propagate it. */
static size_t gc_propagate_arena(global_State *g, GCArena *a)
{
  GCCellID1 cellid = arena_gray_pop(a);
  GCobj *o = (GCobj *)arena_cellptr(a, cellid);
  lj_assertG(isgray(o), "arena gray stack: non-gray object cellid=%u gct=%d marked=0x%02x",
    (unsigned)cellid, o->gch.gct, o->gch.marked);
  return propagatemark(g, o);
}

/* -- Non-arena gray worklists (huge objects + threads) ------------------- */
/*
** Two contiguous GCobj* stacks that replace the former global gclist-threaded
** gray/grayagain lists. They are raw-allocated via g->allocf (never GC memory)
** and grown on demand, exactly like the per-arena gray stack. Occupancy is
** bounded: hugegray by the count of live huge traversable objects (rare),
** graythread by the count of live coroutine threads. OOM throws via
** lj_err_mem, matching lj_arena_gray_grow -- no graceful degradation, since
** these worklists are tiny in practice and the per-arena stack does the same.
*/

#define GC_PTRSTACK_INIT	16	/* Initial worklist capacity (entries). */

/* Grow (or initially allocate) a GCobj* worklist. Returns the (possibly
** moved) base, guaranteed to have room for one more entry. */
static GCobj **gc_ptrstack_grow(global_State *g, MRef *base, MSize *sz)
{
  GCobj **old = mref(*base, GCobj *);
  MSize oldsz = *sz;
  MSize newsz = oldsz ? oldsz * 2 : GC_PTRSTACK_INIT;
  GCobj **buf = (GCobj **)g->allocf(g->allocd, old,
				    (size_t)oldsz * sizeof(GCobj *),
				    (size_t)newsz * sizeof(GCobj *));
  if (LJ_UNLIKELY(buf == NULL)) lj_err_mem(mainthread(g));
  setmref(*base, buf);
  *sz = newsz;
  return buf;
}

static LJ_AINLINE void gc_ptrstack_push(global_State *g, MRef *base,
					MSize *top, MSize *sz, GCobj *o)
{
  GCobj **buf = mref(*base, GCobj *);
  if (LJ_UNLIKELY(*top >= *sz))
    buf = gc_ptrstack_grow(g, base, sz);
  buf[(*top)++] = o;
}

/* Huge gray worklist: gray huge traversable objects awaiting propagation. */
static LJ_AINLINE void gc_hugegray_push(global_State *g, GCobj *o)
{
  gc_ptrstack_push(g, &g->gc.hugegray, &g->gc.hugegraytop, &g->gc.hugegraysz, o);
}
static LJ_AINLINE int gc_hugegray_empty(global_State *g)
{
  return g->gc.hugegraytop == 0;
}
static LJ_AINLINE GCobj *gc_hugegray_pop(global_State *g)
{
  return mref(g->gc.hugegray, GCobj *)[--g->gc.hugegraytop];
}
static LJ_AINLINE void gc_hugegray_reset(global_State *g)
{
  g->gc.hugegraytop = 0;
}

/* Thread gray worklist: coroutine threads greyed this cycle, re-scanned at
** the atomic phase (replaces the former grayagain list). */
static LJ_AINLINE void gc_graythread_push(global_State *g, GCobj *o)
{
  gc_ptrstack_push(g, &g->gc.graythread, &g->gc.graythreadtop,
		   &g->gc.graythreadsz, o);
}
static LJ_AINLINE int gc_graythread_empty(global_State *g)
{
  return g->gc.graythreadtop == 0;
}
static LJ_AINLINE GCobj *gc_graythread_pop(global_State *g)
{
  return mref(g->gc.graythread, GCobj *)[--g->gc.graythreadtop];
}
static LJ_AINLINE void gc_graythread_reset(global_State *g)
{
  g->gc.graythreadtop = 0;
}

/* Weak table worklists: split by mode for clearer atomic processing. */
static LJ_AINLINE void gc_weak_push(global_State *g, GCobj *o, int weak)
{
  if ((weak & LJ_GC_WEAK) == LJ_GC_WEAK) {
    gc_ptrstack_push(g, &g->gc.weakall, &g->gc.weakalltop, &g->gc.weakallsz, o);
  } else if (weak & LJ_GC_WEAKKEY) {
    gc_ptrstack_push(g, &g->gc.weakkey, &g->gc.weakkeytop, &g->gc.weakkeysz, o);
  } else {
    lj_assertG(weak & LJ_GC_WEAKVAL, "weak table without weak mode");
    gc_ptrstack_push(g, &g->gc.weakval, &g->gc.weakvaltop, &g->gc.weakvalsz, o);
  }
}

static LJ_AINLINE void gc_weak_reset(global_State *g)
{
  g->gc.weakkeytop = 0;
  g->gc.weakvaltop = 0;
  g->gc.weakalltop = 0;
}

static LJ_AINLINE void gc_weak_redirect_obj(global_State *g, GCobj *o)
{
  lj_assertG(o->gch.gct == ~LJ_TTAB, "weak stack contains non-table");
  if (gc_inarena(g, o))
    arena_gray_push(g, ptr2arena(o), (GCCellID1)ptr2cell(o));
  else {
    lj_assertG(lj_arena_ishuge(o), "non-arena weak table is not huge");
    gc_hugegray_push(g, o);
  }
}

static void gc_weak_redirect_stack(global_State *g, MRef stack, MSize top)
{
  GCobj **base = mref(stack, GCobj *);
  MSize i;
  for (i = 0; i < top; i++)
    gc_weak_redirect_obj(g, base[i]);
}

static void gc_weak_redirect_all(global_State *g)
{
  MSize keytop = g->gc.weakkeytop;
  MSize valtop = g->gc.weakvaltop;
  MSize alltop = g->gc.weakalltop;
  gc_weak_reset(g);
  gc_weak_redirect_stack(g, g->gc.weakkey, keytop);
  gc_weak_redirect_stack(g, g->gc.weakval, valtop);
  gc_weak_redirect_stack(g, g->gc.weakall, alltop);
}

static void gc_ptrstack_free(global_State *g, MRef *base, MSize *top, MSize *sz)
{
  if (*sz != 0)
    g->allocf(g->allocd, mref(*base, GCobj *),
	      (size_t)*sz * sizeof(GCobj *), 0);
  setmref(*base, NULL);
  *top = 0;
  *sz = 0;
}

void lj_gc_graywork_free(global_State *g)
{
  gc_ptrstack_free(g, &g->gc.hugegray, &g->gc.hugegraytop,
		   &g->gc.hugegraysz);
  gc_ptrstack_free(g, &g->gc.graythread, &g->gc.graythreadtop,
		   &g->gc.graythreadsz);
  gc_ptrstack_free(g, &g->gc.weakkey, &g->gc.weakkeytop,
		   &g->gc.weakkeysz);
  gc_ptrstack_free(g, &g->gc.weakval, &g->gc.weakvaltop,
		   &g->gc.weakvalsz);
  gc_ptrstack_free(g, &g->gc.weakall, &g->gc.weakalltop,
		   &g->gc.weakallsz);
}

/* Propagate all gray objects. */
static size_t gc_propagate_gray(global_State *g)
{
  size_t m = 0;
  /* Drain huge gray objects (non-arena worklist). */
  while (!gc_hugegray_empty(g))
    m += propagatemark(g, gc_hugegray_pop(g));
  /* Drain all arena gray stacks. */
  {
    GCArena *a;
    while ((a = gc_grayarena_pop(g)) != NULL) {
      while (!arena_gray_empty(a))
	m += gc_propagate_arena(g, a);
      /* Also drain any huge objects pushed during arena traversal. */
      while (!gc_hugegray_empty(g))
	m += propagatemark(g, gc_hugegray_pop(g));
    }
  }
  return m;
}

/* -- Sweep phase --------------------------------------------------------- */

/* Type of GC free functions. */
typedef void (LJ_FASTCALL *GCFreeFunc)(global_State *g, GCobj *o);

/* GC free functions for LJ_TSTR .. LJ_TUDATA. ORDER LJ_T */
static const GCFreeFunc gc_freefunc[] = {
  (GCFreeFunc)lj_str_free,
  (GCFreeFunc)lj_func_freeuv,
  (GCFreeFunc)lj_state_free,
  (GCFreeFunc)lj_func_freeproto,
  (GCFreeFunc)lj_func_free,
#if LJ_HASJIT
  (GCFreeFunc)lj_trace_free,
#else
  (GCFreeFunc)0,
#endif
#if LJ_HASFFI
  (GCFreeFunc)lj_cdata_free,
#else
  (GCFreeFunc)0,
#endif
  (GCFreeFunc)lj_tab_free,
  (GCFreeFunc)lj_udata_free
};

/* Full sweep of a GC list. */
#define gc_fullsweep(g, p)	gc_sweep(g, (p), ~(uint32_t)0)

/* Partial sweep of a GC list. */
static GCRef *gc_sweep(global_State *g, GCRef *p, uint32_t lim)
{
  /* Mask with other white and LJ_GC_FIXED. Or LJ_GC_SFIXED on shutdown. */
  GCobj *o;
  while ((o = gcref(*p)) != NULL && lim-- > 0) {
    if (o->gch.gct == ~LJ_TTHREAD)  /* Need to sweep open upvalues, too. */
      gc_fullsweep(g, &gco2th(o)->openupval);
    if ((g->gc.gcmarkflags & GCF_BITMAPSWEEP) && !lj_arena_ishuge(o) &&
	o != obj2gco(mainthread(g))) {
      if ((o->gch.marked & LJ_GC_FIXED) ||
	  arena_obj_ismarked(ptr2arena(o), ptr2cell(o))) {
	makewhite(g, o);
	p = &o->gch.nextgc;
      } else {
	setgcrefr(*p, o->gch.nextgc);
	gc_freefunc[o->gch.gct - ~LJ_TSTR](g, o);
      }
      continue;
    }
    /* Shutdown sweep (GCF_BITMAPSWEEP clear): the only way to reach here under
    ** LJ_HASGCMARK, since a normal cycle keeps the flag set. Free by the fixed
    ** SFIXED-root identity -- equal to the legacy test with ow == LJ_GC_SFIXED
    ** since ((marked ^ WHITES) & SFIXED) == (marked & SFIXED) -- so shutdown
    ** reads no currentwhite and no longer depends on the atomic white flip. */
    if (o->gch.marked & LJ_GC_SFIXED) {  /* Super-fixed root: keep. */
      p = &o->gch.nextgc;
    } else {  /* Everything else dies at shutdown. */
      setgcrefr(*p, o->gch.nextgc);
      gc_freefunc[o->gch.gct - ~LJ_TSTR](g, o);
    }
    continue;
  }
  return p;
}

/* Sweep one string interning table chain. Preserves hashalg bit. */
static void gc_sweepstr(global_State *g, GCRef *chain)
{
  /* Mask with other white and LJ_GC_FIXED. Or LJ_GC_SFIXED on shutdown. */
  uintptr_t u = gcrefu(*chain);
  GCRef q;
  GCRef *p = &q;
  GCobj *o;
  setgcrefp(q, (u & ~(uintptr_t)1));
  while ((o = gcref(*p)) != NULL) {
    if ((g->gc.gcmarkflags & GCF_BITMAPSWEEP) &&
	o != obj2gco(&g->strempty)) {
      int live = (o->gch.marked & LJ_GC_FIXED) ||
		 (lj_arena_ishuge(o)
		    ? huge_obj_ismarked(g, o)
		    : arena_obj_ismarked(ptr2arena(o), ptr2cell(o)));
      if (live) {
	/* Live: the mark is authoritative for string color -- the cell bitmap
	** for arena strings, the hugeset slot for huge strings. No header
	** recolor needed; the mark is reset per cycle by gc_rebuild_rootchain
	** (arena second pass mark[w] &= ~block[w] for cells, the ~LJ_TSTR slot
	** clear for huge strings). */
	p = &o->gch.nextgc;
      } else {
	setgcrefr(*p, o->gch.nextgc);
	lj_str_free(g, gco2str(o));
      }
      continue;
    }
    /* Shutdown sweep (GCF_BITMAPSWEEP clear): the only way to reach here under
    ** LJ_HASGCMARK. Free by the fixed SFIXED identity -- equal to the legacy
    ** test with ow == LJ_GC_SFIXED since ((marked ^ WHITES) & SFIXED) ==
    ** (marked & SFIXED) -- so shutdown reads no currentwhite. No interned
    ** string is SFIXED (strempty is excluded above), so every string is freed,
    ** matching lj_gc_freeall's "free everything except super-fixed" contract. */
    if (o->gch.marked & LJ_GC_SFIXED) {  /* Super-fixed: keep. */
      p = &o->gch.nextgc;
    } else {  /* Otherwise free it. */
      setgcrefr(*p, o->gch.nextgc);
      lj_str_free(g, gco2str(o));
    }
    continue;
  }
  setgcrefp(*chain, (gcrefu(q) | (u & 1)));
}

/*
** Bitmap-driven sweep: scan arena mark bitmaps to locate dead objects.
** Replaces the linked-list gc_sweep for traversable arena objects.
** String sweep (gc_sweepstr) is kept as-is since dead strings must be
** unlinked from the hash-chain-based intern table.
*/

/* Sweep phase constants. */
enum {
  SweepPhase_Bitmap,	/* Scanning arena bitmaps, freeing dead objects. */
  SweepPhase_Rebuild,	/* Rebuilding the root chain from surviving objects. */
  SweepPhase_Done	/* Bitmap sweep complete. */
};

/* Resumable rebuild sub-phases (g->gc.rebuildphase). Ordered: each phase runs
** to completion (no chunking yet) and advances to the next; the GCSsweep
** driver re-enters the dispatcher until Rebuild_Done. */
enum {
  Rebuild_Prologue,	/* CdataV-arena scan + mmudata mark-clear. */
  Rebuild_ArenaScan,	/* Pass-1 arena scan: makewhite + relink udata/threads. */
  Rebuild_HugeScan,	/* Huge-set scan: free dead, makewhite survivors. */
  Rebuild_HugeClear,	/* Huge-set BLACK clear: bounded tail pass (A2). */
  Rebuild_Epilogue,	/* Terminate udata sub-chain + anchor root on mainthread. */
  Rebuild_ClearMarks,	/* Pass-2: clear arena mark bits. */
  Rebuild_Done		/* Rebuild complete. */
};

/*
** Incremental bitmap sweep. Scans trav arenas for dead objects
** (block=1, mark=0) and frees them. Returns a cost estimate.
** The root chain becomes stale during this phase — it is rebuilt
** after all arenas have been scanned (SweepPhase_Rebuild).
*/
static size_t gc_bitmap_sweep(global_State *g)
{
  GCArena **arenas = mref(g->gc.arenas, GCArena *);
  MSize ai = g->gc.sweepa;
  uint32_t w = g->gc.sweepw;
  uint32_t freed = 0;

  lj_assertG(g->gc.gcmarkflags & GCF_DEADAUTH,
	     "GCF_DEADAUTH must be set during bitmap sweep");
  while (ai < g->gc.arenastop && freed < GCSWEEPMAX) {
    GCArena *a = arenas[ai];
    uint32_t wtop;
    if (!(a->flags & ArenaFlag_TravObjs)) {
      ai++;
      w = UnusedBlockWords;
      continue;
    }
    /* POD-only arena (closures, protos): word-parallel sweep. One linear
    ** metadata pass frees all dead objects and recolors survivors white,
    ** touching no object data. Whole-arena atomic (the transform + scavenge
    ** must pair without an intervening allocation), so it ignores the per-word
    ** cursor and bills its cost as a fixed chunk of the GCSWEEPMAX budget.
    ** Cell-space accounting: freed cells * CellSize, derived from the bitmap
    ** by lj_arena_podsweep, matches the cell-space alloc accounting. */
    if (a->flags & ArenaFlag_PODOnly) {
      GCCellID fcells = lj_arena_podsweep(g, a);
      g->gc.total -= (GCSize)fcells << CellSizeLog2;
      freed += GCSWEEPMAX/2;  /* Bill ~half a step's worth per POD arena. */
      ai++;
      w = UnusedBlockWords;
      continue;
    }
    /* Flush free-list bins before scanning: binned free blocks keep the
    ** allocated bitmap state (block=1, mark=0) for hot-path performance.
    ** Without flushing, bitmap sweep would see them as dead objects.
    ** Must flush on every entry (not just first) because the mutator may
    ** allocate from bins between incremental steps, and freeing during
    ** sweep itself pushes to bins. */
    lj_arena_flushbins(a);
    wtop = arena_blockidx((GCCellID)a->celltop - 1);
    while (w <= wtop && freed < GCSWEEPMAX) {
      GCBlockword dead = a->block[w] & ~a->mark[w];
      while (dead) {
	uint32_t bitidx = lj_ffs(dead);
	GCCellID c = (w << 5) + bitidx;
	GCobj *o = (GCobj *)arena_cellptr(a, c);
	dead &= dead - 1;
	/* Skip open upvalues: they're on per-thread openupval chains,
	** freed by lj_state_free (dead thread) or gc_fullsweep in rebuild. */
	if (o->gch.gct == ~LJ_TUPVAL && !gco2uv(o)->closed)
	  continue;
	/* Strings are swept by gc_sweepstr (hash table phase); skip them. */
	if (o->gch.gct == ~LJ_TSTR)
	  continue;
	/* Dead finalized cdata are not freed here: lj_cdata_free detects the
	** LJ_GC_CDATA_FIN flag and links them onto the mmudata ring (marking
	** them finalized + white) instead of releasing the cell. The cell
	** stays allocated (block=1, mark=0) until gc_finalize runs the __gc
	** callback and re-roots the object, exactly like the list sweep. */
	/* The bitmap (block=1, mark=0) is the sole dead test under HASGCMARK;
	** the header white bit is vestigial. During GCF_BITMAPSWEEP
	** gc_obj_isdead returns exactly !arena_obj_ismarked, so this assert is
	** the mark-based dead invariant, independent of any header color. */
	lj_assertG(gc_obj_isdead(g, o) || (o->gch.marked & LJ_GC_FIXED),
		   "bitmap sweep freeing non-dead object: o=%p gct=%d marked=0x%02x",
		   (void*)o, o->gch.gct, o->gch.marked);
	gc_freefunc[o->gch.gct - ~LJ_TSTR](g, o);
	freed++;
      }
      w++;
    }
    if (w > wtop) {
      ai++;
      w = UnusedBlockWords;
    }
  }

  g->gc.sweepa = ai;
  g->gc.sweepw = (uint16_t)w;
  if (ai >= g->gc.arenastop) {
    /* Last bitmap free is done. Drop death-authority before any rebuild slice
    ** tears down a survivor mark, so mutator barriers can't read a half-cleared
    ** mark as dead. Link-suppression (GCF_BITMAPSWEEP) stays set until Done. */
    g->gc.gcmarkflags &= ~GCF_DEADAUTH;
    g->gc.sweepphase = SweepPhase_Rebuild;
    g->gc.rebuildphase = Rebuild_Prologue;
    /* Arm the CdataV-arena scan cursor (sweepa, sweepw) for the prologue's
    ** first slice; the mmudata walk is gated by rebuild_mmu_started == 0. */
    g->gc.sweepa = 0;
    g->gc.sweepw = UnusedBlockWords;
    g->gc.rebuild_mmu_started = 0;
    setgcrefnull(g->gc.rebuild_mmu_cursor);
  }
  return freed;
}

/*
** Post-sweep pass: makewhite surviving objects, sweep each thread's open
** upvalue list, free dead huge objects, and clear arena mark bits.
**
** The root chain (g->gc.root) is NOT rebuilt: all former consumers now
** enumerate arena objects via the block bitmaps directly. The root reference
** is simply anchored on the (super-fixed) main thread.
**
** The work is split into ordered sub-phases (RebuildPhase) driven by
** g->gc.rebuildphase. gc_rebuild_rootchain is a dispatcher that runs one
** sub-phase per call and the GCSsweep driver re-enters it once per onestep.
 ** Prologue (T4), ArenaScan (T5), and HugeScan (T6) are chunked: they yield
 ** mid-phase via their persisted cursors (the CdataV-arena scan and the
 ** arena/huge scans use the (sweepa, sweepw) / rebuild_hugehi cursors; the
 ** mmudata ring uses a snapshot-root + persisted cursor). Epilogue (T7) is
 ** O(1) and ClearMarks (T8) is one-shot — neither yields; they run to Done
 ** in the dispatcher's internal loop in the same onestep.
*/

/* Resumable CdataV-arena bitmap scan slice. Replaces the legacy VLA-cdata
** chain walk. Scans ArenaFlag_CdataVOnly arenas via the (sweepa, sweepw)
** cursor (same shape as gc_bitmap_sweep), and for each allocated cell base p
** computes cd = p + GCcdataVar.offset (base->cd translation), then frees
** dead (block=1, mark=0) via gc_freefunc and makewhites survivors
** (block=1, mark=1). Mark authority is on the BASE cell (ptr2arena(p),
** ptr2cell(p)), matching MARKALLOC and gc_obj_key; header reads (gct) and
** the makewhite recolor use cd, which carries the valid GCcdata header.
** Bounded by GCSWEEPMAX cells per slice, then yields. Returns nonzero while
** the scan is still in progress. */
#if LJ_HASFFI
static int rebuild_prologue_cdatav(global_State *g)
{
  GCArena **arenas = mref(g->gc.arenas, GCArena *);
  MSize ai = g->gc.sweepa;
  uint32_t w = g->gc.sweepw;
  uint32_t freed = 0;
  lj_assertG(g->gc.state == GCSsweep, "CdataV scan outside GCSsweep");
  while (ai < g->gc.arenastop && freed < GCSWEEPMAX) {
    GCArena *a = arenas[ai];
    uint32_t wtop;
    if (!(a->flags & ArenaFlag_CdataVOnly)) {
      ai++;
      w = UnusedBlockWords;
      continue;
    }
    lj_arena_flushbins(a);
    wtop = arena_blockidx((GCCellID)a->celltop - 1);
    while (w <= wtop && freed < GCSWEEPMAX) {
      GCBlockword alloc = a->block[w];
      while (alloc) {
	uint32_t bitidx = lj_ffs(alloc);
	GCCellID c = (w << 5) + bitidx;
	char *p = (char *)arena_cellptr(a, c);
	GCcdata *cd;
	alloc &= alloc - 1;
	cd = (GCcdata *)(p + ((GCcdataVar *)p)->offset);
	cdatav_cell_assert(g, p);
	if (!arena_obj_ismarked(a, c)) {
	  gc_freefunc[cd->gct - ~LJ_TSTR](g, obj2gco(cd));
	  freed++;
	} else {
	  makewhite(g, obj2gco(cd));
	}
      }
      w++;
    }
    if (w > wtop) {
      ai++;
      w = UnusedBlockWords;
    }
  }
  g->gc.sweepa = ai;
  g->gc.sweepw = (uint16_t)w;
  return ai < g->gc.arenastop;
}
#endif

/* Resumable mmudata ring mark-clear slice. Clears mark bits for udata on the
** mmudata ring so the arena scan won't re-link them (gc_finalize owns their
** lifecycle). The ring is immutable during rebuild (no separateudata/finalize
** runs until SweepPhase_Done), so a snapshot-root + persisted cursor is safe.
** Returns nonzero while the walk is still in progress. */
static int rebuild_prologue_mmu(global_State *g)
{
  MSize budget = GCSWEEPMAX;
  GCobj *root, *u;
  if (!g->gc.rebuild_mmu_started) {
    g->gc.rebuild_mmu_started = 1;
    setgcrefr(g->gc.rebuild_mmu_cursor, g->gc.mmudata);
    if (!gcref(g->gc.mmudata))
      return 0;  /* Empty ring: nothing to clear. */
  }
  root = gcref(g->gc.mmudata);
  lj_assertG(root != NULL, "mmudata ring emptied mid-rebuild");
  u = gcref(g->gc.rebuild_mmu_cursor);
  do {
    u = gcnext(u);
    { /* VLA cdata on the mmudata ring: classify by block base (gc_obj_key),
      ** not the interior cd, or a huge VLA cdata would be misread as arena. */
      void *k = gc_obj_key(u);
      if (!lj_arena_ishuge(k))
	arena_obj_clearmark(ptr2arena(k), ptr2cell(k));
    }
    setgcref(g->gc.rebuild_mmu_cursor, u);
  } while (u != root && budget-- > 0);
  return u != root;  /* Done when the cursor returns to the ring root. */
}

static void rebuild_prologue(global_State *g)
{
  lj_assertG(g->gc.state == GCSsweep, "rebuild prologue outside GCSsweep");
  if (!g->gc.rebuild_mmu_started) {
    /* The CdataV-arena scan runs before the mmudata clear starts
    ** (rebuild_mmu_started is the cross-slice phase marker). */
#if LJ_HASFFI
    if (rebuild_prologue_cdatav(g))
      return;  /* CdataV scan not finished: yield. */
#endif
  }
  if (rebuild_prologue_mmu(g))
    return;  /* mmudata walk not finished: yield. */

  /* Prologue done. Arm the ArenaScan cursor. */
  g->gc.sweepa = 0;
  g->gc.sweepw = UnusedBlockWords;
  g->gc.rebuildphase = Rebuild_ArenaScan;
}

/* Pass-1 arena scan, resumable. Recolors live cells white and sweeps each
** thread's openupval list. Formerly a single O(live) pass -- the source of
** the incremental-GC pause spike -- now sliced via the (sweepa, sweepw)
** cursor exactly like gc_bitmap_sweep: at most GCSWEEPMAX live cells per
** call, then yield. The arenas base is reloaded every call (the vector can
** realloc across yields) and the LIVE g->gc.arenastop bounds the walk
** (arenas appended mid-scan hold MARKALLOC-live cells the forward cursor
** reaches and recolors correctly). */
static void rebuild_arenascan(global_State *g)
{
  GCArena **arenas = mref(g->gc.arenas, GCArena *);
  MSize ai = g->gc.sweepa;
  uint32_t w = g->gc.sweepw;
  uint32_t done = 0;
  lj_assertG(g->gc.state == GCSsweep, "arena scan outside GCSsweep");
  lj_assertG(!(g->gc.gcmarkflags & GCF_DEADAUTH),
	     "arena scan with death-authority still held");
  while (ai < g->gc.arenastop && done < GCSWEEPMAX) {
    GCArena *a = arenas[ai];
    uint32_t wtop;
    if (!(a->flags & ArenaFlag_TravObjs)) {
      ai++; w = UnusedBlockWords; continue;
    }
    /* POD arenas were fully handled by lj_arena_podsweep: dead objects freed,
    ** survivors already recolored white, mark bits cleared. They hold no
    ** udata/thread/openupval to relink, so skip them here. */
    if (a->flags & ArenaFlag_PODOnly) {
      ai++; w = UnusedBlockWords; continue;
    }
    lj_arena_flushbins(a);
    wtop = arena_blockidx((GCCellID)a->celltop - 1);
    while (w <= wtop && done < GCSWEEPMAX) {
      GCBlockword alive = a->block[w] & a->mark[w];
      while (alive) {
	uint32_t bitidx = lj_ffs(alive);
	GCCellID c = (w << 5) + bitidx;
	GCobj *o = (GCobj *)arena_cellptr(a, c);
	alive &= alive - 1;
	if (o->gch.gct == ~LJ_TUPVAL && !gco2uv(o)->closed)
	  continue;
	if (o->gch.gct == ~LJ_TSTR)
	  continue;
	gc_obj_makewhite(g, o);
	if (o->gch.gct == ~LJ_TTHREAD) {
	  gc_fullsweep(g, &gco2th(o)->openupval);
	}
	done++;
      }
      w++;
    }
    if (w > wtop) {
      ai++; w = UnusedBlockWords;
    }
  }
  g->gc.sweepa = ai;
  g->gc.sweepw = (uint16_t)w;
  if (ai >= g->gc.arenastop) {
    g->gc.sweepa = 0;
    g->gc.sweepw = UnusedBlockWords;
    /* Arm the HugeScan cursor: snapshot the hugeset generation so a rehash
    ** between slices is detected (rebuild_hugegen != hugesetgen → restart). */
    g->gc.rebuild_hugehi = 0;
    g->gc.rebuild_hugegen = g->gc.hugesetgen;
    g->gc.rebuildphase = Rebuild_HugeScan;
  }
}

static void rebuild_hugescan(global_State *g)
{
  /* Resumable huge-set scan. Bounded by GCSWEEPMAX slots per slice, then
  ** yields to the dispatcher. Cursor (rebuild_hugehi) persists across slices;
  ** generation snapshot (rebuild_hugegen) detects rehashes — if the hugeset
  ** was rehashed between slices, slot positions changed and the cursor
  ** restarts from 0 with the new generation.
  **
  ** Restart idempotency: processed survivors get LJ_GC_BLACK (header bit,
  ** unused for huge objects under HASGCMARK — color lives in the slot). On
  ** restart, BLACK-tagged objects are skipped BEFORE the dead/alive test (a
  ** cleared slot mark would otherwise look dead → use-after-free). Freed
  ** dead objects tombstone their slots → skipped. A final pass clears BLACK
  ** once the cursor completes (no mutator gap before Epilogue in T6 → no
  ** stale BLACK).
  **
  ** Huge STRINGS are not freed here (owned by gc_sweepstr); their slot mark
  ** is cleared for the per-cycle reset. Upvalues are never huge. */
  GCRef *slots;
  MSize hmask;
  MSize budget = GCSWEEPMAX;

  if (g->gc.rebuild_hugegen != g->gc.hugesetgen) {
    g->gc.rebuild_hugehi = 0;
    g->gc.rebuild_hugegen = g->gc.hugesetgen;
  }
  slots = mref(g->gc.hugeset, GCRef);
  hmask = g->gc.hugesetmask;
  if (slots == NULL) {
    g->gc.rebuildphase = Rebuild_Epilogue;
    return;
  }
  while (g->gc.rebuild_hugehi <= hmask && budget > 0) {
    uintptr_t u = gcrefu(slots[g->gc.rebuild_hugehi]);
    GCobj *o;
    g->gc.rebuild_hugehi++;
    budget--;
    if (!hugeset_slot_live(u)) continue;  /* EMPTY / TOMB. */
    hugeset_slot_assert(g, u);
    { /* base = slot address (mark authority); o = GCobj (cd for CDATAV slots). */
      GCobj *base = hugeset_slot_addr(u);
      o = hugeset_slot_obj(u);
      if (o->gch.gct == ~LJ_TSTR) {
	if (u & HUGESET_MARK)
	  huge_obj_clearmark(g, base);
	continue;
      }
      lj_assertG(o->gch.gct != ~LJ_TUPVAL, "huge upvalue is impossible");
      if (o->gch.marked & LJ_GC_BLACK)
	continue;  /* Already processed (restart safety). */
      if (!(u & HUGESET_MARK)) {
	/* Dead: gc_freefunc -> lj_hugeblock_free tombstones this slot. */
	gc_freefunc[o->gch.gct - ~LJ_TSTR](g, o);
      } else {
	/* Survivor: clear the slot mark (per-cycle white reset) + recolor. */
	huge_obj_clearmark(g, base);
	makewhite(g, o);
	o->gch.marked |= LJ_GC_BLACK;  /* Tag as processed (restart safety). */
	if (o->gch.gct == ~LJ_TTHREAD) {
	  gc_fullsweep(g, &gco2th(o)->openupval);
	}
      }
    }
  }
  if (g->gc.rebuild_hugehi > hmask) {
    /* Main walk done. BLACK bits (set on survivors at line above) are cleared
    ** by a bounded second pass — Rebuild_HugeClear — NOT an unbounded final
    ** loop (A2). Re-snapshot the generation so the clear pass detects a
    ** rehash between its own slices and restarts from 0 (clearing is
    ** idempotent). */
    g->gc.rebuild_hugehi = 0;
    g->gc.rebuild_hugegen = g->gc.hugesetgen;
    g->gc.gcmarkflags |= GCF_HUGECLEAR;
    g->gc.rebuildphase = Rebuild_HugeClear;
  }
}

/* Bounded BLACK-clear tail pass for the huge set. BLACK is set during the
** main HugeScan walk (restart-skip tag on survivors). Every live non-string
** huge object must have BLACK cleared before rebuild Done, or the next mark
** cycle would treat a stale BLACK as "already marked" and skip tracing it
** (premature collection). The old form was an unbounded
** for(hi=0; hi<=hmask; hi++) loop in one slice — O(hugesetmask), violating A2.
** This resumable form clears GCSWEEPMAX slots per slice and yields. A rehash
** between slices (hugesetgen changed) just restarts the cursor from 0 —
** clearing is idempotent, so eventual completion only requires the rehash
** storm to settle (same liveness assumption as the main walk). */
static void rebuild_hugeclear(global_State *g)
{
  GCRef *slots = mref(g->gc.hugeset, GCRef);
  MSize hmask;
  MSize budget = GCSWEEPMAX;
  if (slots == NULL) {
    g->gc.gcmarkflags &= ~GCF_HUGECLEAR;
    g->gc.rebuildphase = Rebuild_Epilogue;
    return;
  }
  if (g->gc.rebuild_hugegen != g->gc.hugesetgen) {
    g->gc.rebuild_hugehi = 0;
    g->gc.rebuild_hugegen = g->gc.hugesetgen;
  }
  hmask = g->gc.hugesetmask;
  while (g->gc.rebuild_hugehi <= hmask && budget > 0) {
    uintptr_t u = gcrefu(slots[g->gc.rebuild_hugehi]);
    GCobj *o;
    g->gc.rebuild_hugehi++;
    budget--;
    if (!hugeset_slot_live(u)) continue;  /* EMPTY / TOMB. */
    hugeset_slot_assert(g, u);
    o = hugeset_slot_obj(u);
    if (o->gch.gct != ~LJ_TSTR)
      o->gch.marked &= (uint8_t)~LJ_GC_BLACK;
  }
  if (g->gc.rebuild_hugehi > hmask) {
    g->gc.gcmarkflags &= ~GCF_HUGECLEAR;  /* HugeScan window fully closed. */
    g->gc.rebuildphase = Rebuild_Epilogue;
  }
}

static void rebuild_epilogue(global_State *g)
{
  /* Anchor the root reference on mainthread. No other objects are chained.
  ** O(1): a single bounded slice — no cursor needed. The dispatcher yields
  ** after this slice (T7) so the mutator runs before ClearMarks. */
  makewhite(g, obj2gco(mainthread(g)));
  gc_fullsweep(g, &mainthread(g)->openupval);
  setgcref(g->gc.root, obj2gco(mainthread(g)));
  gc_assert_root_anchor_only(g);

  g->gc.rebuildphase = Rebuild_ClearMarks;
}

static void rebuild_clearmarks(global_State *g)
{
  GCArena **arenas = mref(g->gc.arenas, GCArena *);
  MSize i;
  GCSize total_before;
  /* Monotonicity (G11): ClearMarks is reached only after ArenaScan+HugeScan+
  ** Epilogue. Rebuild_ClearMarks is set exclusively by rebuild_epilogue, so the
  ** dispatcher arriving here proves the ordered predecessors already completed. */
  lj_assertG(g->gc.rebuildphase == Rebuild_ClearMarks,
	     "ClearMarks entered with rebuildphase=%d (expected Rebuild_ClearMarks)",
	     g->gc.rebuildphase);
  lj_assertG(g->gc.state == GCSsweep, "ClearMarks outside GCSsweep");
  /* GCF_DEADAUTH was dropped at the Bitmap->Rebuild transition (T3): a back-
  ** barrier reading an unmarked survivor during this pass is benign (this
  ** cycle's gray already drained at atomic; next cycle re-marks from roots
  ** with a reset bitmap). That makes a YIELDING ClearMarks safe -- pass-2 is
  ** word-parallel O(arenas*words). Measured ~2.5-3ms at 16M objects (see
  ** .omo/evidence/task-8-*), which EXCEEDS the plan's <1ms estimate but is
  ** still far below the 5ms inc_pause_assert threshold and the pre-fix 109ms
  ** spike. Plan task T8 explicitly authorized this: "if >1ms, note it but
  ** don't chunk." The one-shot form is the simplest correct change; T3 made
  ** chunking the escape hatch should a future pathological heap push pass-2
  ** over budget -- not needed today. This is NOT the incremental-GC spike
  ** (that was pass-1 ArenaScan, fixed by T5). */
  /* No-free assert (G7): ClearMarks only clears bitmap words, never frees. The
  ** total memory counter must be unchanged across this call -- any decrease
  ** would indicate a gc_freefunc was invoked, which this pass must never do. */
  total_before = g->gc.total;
  /* Second pass: clear mark bits on all arenas now that openupval sweeps
  ** are done and no longer need to read them. POD arenas were already left
  ** in final state by lj_arena_podsweep (survivors white, free cells Free),
  ** so skip them. */
  for (i = 0; i < g->gc.arenastop; i++) {
    GCArena *a = arenas[i];
    uint32_t w, wtop = arena_blockidx((GCCellID)a->celltop - 1);
    if (a->flags & ArenaFlag_PODOnly) continue;
    for (w = UnusedBlockWords; w <= wtop; w++)
      a->mark[w] &= ~a->block[w];
  }
  lj_assertG(g->gc.total == total_before,
	     "ClearMarks freed memory (total %lu -> %lu); no gc_freefunc expected",
	     (unsigned long)total_before, (unsigned long)g->gc.total);

  g->gc.rebuildphase = Rebuild_Done;
  /* sweepphase = SweepPhase_Done is set by the dispatcher on Rebuild_Done;
  ** the GCSsweep driver then clears gcmarkflags and runs finalization. */
}

static void gc_rebuild_rootchain(global_State *g)
{
  lj_assertG(!(g->gc.gcmarkflags & GCF_DEADAUTH),
	     "GCF_DEADAUTH must be clear during rebuild");
  lj_assertG(g->gc.gcmarkflags & GCF_BITMAPSWEEP,
	     "GCF_BITMAPSWEEP must be set during rebuild");
  lj_assertG(g->gc.gcmarkflags & GCF_MARKALLOC,
	     "GCF_MARKALLOC must be set during rebuild");
  for (;;) {
    uint8_t phase = g->gc.rebuildphase;
    switch (phase) {
    case Rebuild_Prologue:   rebuild_prologue(g);   break;
    case Rebuild_ArenaScan:  rebuild_arenascan(g);  break;
    case Rebuild_HugeScan:   rebuild_hugescan(g);   break;
    case Rebuild_HugeClear:  rebuild_hugeclear(g);  break;
    case Rebuild_Epilogue:   rebuild_epilogue(g);   break;
    case Rebuild_ClearMarks: rebuild_clearmarks(g); break;
    default:
      lj_assertG(0, "bad rebuild phase %d", g->gc.rebuildphase);
      return;
    }
    /* Sub-phase monotonicity: rebuildphase only increases. Chunked phases
    ** stay on the same phase (equal); completed phases advance (greater). */
    lj_assertG(g->gc.rebuildphase >= phase,
	       "rebuild phase went backward: %d -> %d",
	       (int)phase, (int)g->gc.rebuildphase);
    if (g->gc.rebuildphase == Rebuild_Done) {
      g->gc.sweepphase = SweepPhase_Done;
      return;
    }
    /* Yield the onestep so the mutator runs between sub-phases. Prologue
    ** (T4), ArenaScan (T5), and HugeScan (T6) are chunked — they yield
    ** mid-phase (staying on the same phase) via their persisted cursors.
    ** Epilogue (T7) is O(1) and ClearMarks (T8) is one-shot — neither
    ** yields. They run to Done in the same onestep as the dispatcher's
    ** internal loop. */
    if ((phase == Rebuild_Prologue || phase == Rebuild_ArenaScan ||
	 phase == Rebuild_HugeScan || phase == Rebuild_HugeClear) &&
	g->gc.rebuildphase == phase)
      return;  /* Chunked phase still mid-walk: yield. */
  }
}


/* Check whether we can clear a key or a value slot from a table. */
static int gc_mayclear(global_State *g, cTValue *o, int val)
{
  if (tvisgcv(o)) {  /* Only collectable objects can be weak references. */
    if (tvisstr(o)) {  /* But strings cannot be used as weak references. */
      gc_mark_str(g, strV(o));  /* And need to be marked. */
      return 0;
    }
    if (gc_obj_iswhite(g, gcV(o)))
      return 1;  /* Object is about to be collected. */
    if (tvisudata(o) && val && isfinalized(udataV(o)))
      return 1;  /* Finalized userdata is dropped only from values. */
  }
  return 0;  /* Cannot clear. */
}

/* Clear collected entries from one weak table. */
static void gc_clearweak_tab(global_State *g, GCtab *t)
{
  lj_assertG((t->marked & LJ_GC_WEAK), "clear of non-weak table");
  if ((t->marked & LJ_GC_WEAKVAL)) {
    MSize i, asize = t->asize;
    for (i = 0; i < asize; i++) {
      /* Clear array slot when value is about to be collected. */
      TValue *tv = arrayslot(t, i);
      if (gc_mayclear(g, tv, 1))
	setnilV(tv);
    }
  }
  if (t->hmask > 0) {
    Node *node = noderef(t->node);
    MSize i, hmask = t->hmask;
    for (i = 0; i <= hmask; i++) {
      Node *n = &node[i];
      /* Clear hash slot when key or value is about to be collected. */
      if (!tvisnil(&n->val) && (gc_mayclear(g, &n->key, 0) ||
				gc_mayclear(g, &n->val, 1)))
	setnilV(&n->val);
    }
  }
}


static void gc_clearweak_stack(global_State *g, MRef stack, MSize top)
{
  GCobj **base = mref(stack, GCobj *);
  MSize i;
  for (i = 0; i < top; i++)
    gc_clearweak_tab(g, gco2tab(base[i]));
}

static void gc_clearweak_stacks(global_State *g)
{
  gc_clearweak_stack(g, g->gc.weakkey, g->gc.weakkeytop);
  gc_clearweak_stack(g, g->gc.weakval, g->gc.weakvaltop);
  gc_clearweak_stack(g, g->gc.weakall, g->gc.weakalltop);
  gc_weak_reset(g);
}

/* Call a userdata or cdata finalizer. */
static void gc_call_finalizer(global_State *g, lua_State *L,
			      cTValue *mo, GCobj *o)
{
  /* Save and restore lots of state around the __gc callback. */
  uint8_t oldh = hook_save(g);
  GCSize oldt = g->gc.threshold;
  int errcode;
  lua_State *VL = vmthread(g);
  TValue *top;
  lj_trace_abort(g);
  hook_entergc(g);  /* Disable hooks and new traces during __gc. */
  if (LJ_HASPROFILE && (oldh & HOOK_PROFILE)) lj_dispatch_update(g);
  g->gc.threshold = LJ_MAX_MEM;  /* Prevent GC steps. */
  top = VL->top;
  copyTV(VL, top++, mo);
  if (LJ_FR2) setnilV(top++);
  setgcV(VL, top, o, ~o->gch.gct);
  VL->top = top+1;
  errcode = lj_vm_pcall(VL, top, 1+0, -1);  /* Stack: |mo|o| -> | */
  setgcref(g->cur_L, obj2gco(L));
  hook_restore(g, oldh);
  if (LJ_HASPROFILE && (oldh & HOOK_PROFILE)) lj_dispatch_update(g);
  g->gc.threshold = oldt;  /* Restore GC threshold. */
  if (errcode) {
    lj_vmevent_send(g, ERRFIN,
      copyTV(V, V->top++, L->top-1);
    );
    L->top--;
  }
}

/* Finalize one userdata or cdata object from the mmudata list. */
static void gc_finalize(lua_State *L)
{
  global_State *g = G(L);
  GCobj *o = gcnext(gcref(g->gc.mmudata));
  cTValue *mo;
  lj_assertG(tvref(g->jit_base) == NULL, "finalizer called on trace");
  /* Unchain from list of userdata to be finalized. */
  if (o == gcref(g->gc.mmudata))
    setgcrefnull(g->gc.mmudata);
  else
    setgcrefr(gcref(g->gc.mmudata)->gch.nextgc, o->gch.nextgc);
#if LJ_HASFFI
  if (o->gch.gct == ~LJ_TCDATA) {
    TValue tmp, *tv;
    /* Add cdata back to the GC list and make it white. */
    gc_obj_makewhite(g, o);
    o->gch.marked &= (uint8_t)~LJ_GC_CDATA_FIN;
    /* Resolve finalizer. */
    setcdataV(L, &tmp, gco2cd(o));
    tv = lj_tab_set(L, tabref(g->gcroot[GCROOT_FFI_FIN]), &tmp);
    if (!tvisnil(tv)) {
      copyTV(L, &tmp, tv);
      setnilV(tv);  /* Clear entry in finalizer table. */
      gc_call_finalizer(g, L, &tmp, o);
    }
    return;
  }
#endif
  /* Make the resurrected userdata white for the next cycle. Its arena cell
  ** stays in the udata arena (no chain to re-link onto); isfinalized prevents
  ** re-finalization. */
  gc_obj_makewhite(g, o);
  /* Resolve the __gc metamethod. */
  mo = lj_meta_fastg(g, tabref(gco2ud(o)->metatable), MM_gc);
  if (mo)
    gc_call_finalizer(g, L, mo, o);
}

/* Finalize all userdata objects from mmudata list. */
void lj_gc_finalize_udata(lua_State *L)
{
  while (gcref(G(L)->gc.mmudata) != NULL)
    gc_finalize(L);
}

#if LJ_HASFFI
/* Finalize all cdata objects from finalizer table. */
void lj_gc_finalize_cdata(lua_State *L)
{
  global_State *g = G(L);
  GCtab *t = tabref(g->gcroot[GCROOT_FFI_FIN]);
  Node *node = noderef(t->node);
  ptrdiff_t i;
  setgcrefnull(t->metatable);  /* Mark finalizer table as disabled. */
  for (i = (ptrdiff_t)t->hmask; i >= 0; i--)
    if (!tvisnil(&node[i].val) && tviscdata(&node[i].key)) {
      GCobj *o = gcV(&node[i].key);
      TValue tmp;
      gc_obj_makewhite(g, o);
      o->gch.marked &= (uint8_t)~LJ_GC_CDATA_FIN;
      copyTV(L, &tmp, &node[i].val);
      setnilV(&node[i].val);
      gc_call_finalizer(g, L, &tmp, o);
    }
}
#endif

/* Free all remaining GC objects. */
void lj_gc_freeall(global_State *g)
{
  MSize i;
  /* Free everything, except super-fixed objects (the main thread). */
  /* Force the deterministic shutdown path: with GCF_BITMAPSWEEP clear, the
  ** residual gc_sweep/gc_sweepstr calls below free by the SFIXED-root identity
  ** (independent of currentwhite). The bitmap branch must NOT run here -- the
  ** direct cell scan below frees arena objects, so reading their cell marks
  ** afterwards would be a use-after-free. */
  g->gc.gcmarkflags = 0;
  /*
  ** Arena mode: the 根 chain (g->gc.root) is redundant with the arena block
  ** bitmaps -- it is rebuilt from them after every bitmap sweep. Rather than
  ** walk that chain, enumerate the live arena cells directly (the same scan
  ** gc_rebuild_rootchain uses to relink survivors) and free each object. This
  ** removes the last shutdown reader of the 根 chain.
  **
  ** Coverage:
  **  - Traversable arenas: tables, funcs, protos, threads, upvalues, regular
  **    cdata, traces and udata. All freed by the cell scan below.
  **  - CdataV arenas: VLA/over-aligned cdata. Cell base is a GCcdataVar; the
  **    GCobj cd = base + offset. Freed by the separate CdataV scan below
  **    (before the huge-set scan, so a huge VLA tombstone is not re-seen).
  **  - Non-traversable arenas: only strings (freed via the intern table).
  **    Their gct cannot be read from a bitmap cell, so these arenas are
  **    skipped here.
  **  - Huge objects: no cell bitmap; freed via the address-keyed huge set.
  */
  {
    GCArena **arenas = mref(g->gc.arenas, GCArena *);
#if LJ_HASFFI
    /* VLA cdata first: freeing a huge VLA cdata tombstones its huge-set slot,
    ** so the huge-set scan below won't see (and double-free) it. Small VLA
    ** cdata live in CdataV arenas; huge VLA are in the hugeset (the CdataV
    ** arena scan skips them — they have no cell bitmap). */
    {
      MSize ci;
      for (ci = 0; ci < g->gc.arenastop; ci++) {
	GCArena *a = arenas[ci];
	uint32_t cw, cwtop;
	if (!(a->flags & ArenaFlag_CdataVOnly)) continue;
	lj_arena_flushbins(a);
	cwtop = arena_blockidx((GCCellID)a->celltop - 1);
	for (cw = UnusedBlockWords; cw <= cwtop; cw++) {
	  GCBlockword alloc = a->block[cw];
	  while (alloc) {
	    uint32_t bitidx = lj_ffs(alloc);
	    GCCellID c = (cw << 5) + bitidx;
	    char *p = (char *)arena_cellptr(a, c);
	    GCcdata *cd;
	    alloc &= alloc - 1;
	    cd = (GCcdata *)(p + ((GCcdataVar *)p)->offset);
	    cdatav_cell_assert(g, p);
	    gc_freefunc[cd->gct - ~LJ_TSTR](g, obj2gco(cd));
	  }
	}
      }
    }
#endif
    for (i = 0; i < g->gc.arenastop; i++) {
      GCArena *a = arenas[i];
      uint32_t w, wtop;
      if (!(a->flags & ArenaFlag_TravObjs)) continue;
      /* Flush bins so a cleared block bit means "free": binned free blocks
      ** otherwise keep their allocated bitmap state (block=1). */
      lj_arena_flushbins(a);
      wtop = arena_blockidx((GCCellID)a->celltop - 1);
      for (w = UnusedBlockWords; w <= wtop; w++) {
	GCBlockword alive = a->block[w];
	while (alive) {
	  uint32_t bitidx = lj_ffs(alive);
	  GCCellID c = (w << 5) + bitidx;
	  GCobj *o = (GCobj *)arena_cellptr(a, c);
	  alive &= alive - 1;
	  /* Open upvalues are freed through their owning thread's openupval
	  ** chain (below); skip them here. Safe even if a thread already freed
	  ** this cell: freeing never overwrites the gct/closed header bytes. */
	  if (o->gch.gct == ~LJ_TUPVAL && !gco2uv(o)->closed)
	    continue;
	  if (o->gch.gct == ~LJ_TTHREAD)
	    gc_fullsweep(g, &gco2th(o)->openupval);
	  gc_freefunc[o->gch.gct - ~LJ_TSTR](g, o);
	}
      }
    }
    /* Huge objects: walk the huge set (no cell bitmap exists for them).
    ** Strings are owned by gc_sweepstr; VLA cdata were freed above. */
    {
      GCRef *slots = mref(g->gc.hugeset, GCRef);
      if (slots != NULL) {
	MSize hi, hmask = g->gc.hugesetmask;
	for (hi = 0; hi <= hmask; hi++) {
	  uintptr_t u = gcrefu(slots[hi]);
	  GCobj *o;
	  if (!hugeset_slot_live(u)) continue;  /* EMPTY / TOMB. */
	  hugeset_slot_assert(g, u);
	  o = hugeset_slot_obj(u);
	  if (o->gch.gct == ~LJ_TSTR) continue;  /* Owned by gc_sweepstr. */
	  gc_freefunc[o->gch.gct - ~LJ_TSTR](g, o);
	}
      }
    }
    /* Re-anchor the 根 reference on the (super-fixed) main thread: every other
    ** object is gone, and the stale chain must never be walked again. */
    setgcrefnull(mainthread(g)->nextgc);
    setgcref(g->gc.root, obj2gco(mainthread(g)));
    gc_assert_root_anchor_only(g);
  }
  for (i = g->str.mask; i != ~(MSize)0; i--)  /* Free all string hash chains. */
    gc_sweepstr(g, &g->str.tab[i]);
}

/* -- Collector ----------------------------------------------------------- */

/* Atomic part of the GC cycle, transitioning from mark to sweep phase. */
static void atomic(global_State *g, lua_State *L)
{
  size_t udsize;

  gc_mark_uv(g);  /* Need to remark open upvalues (the thread may be dead). */
  gc_propagate_gray(g);  /* Propagate any left-overs. */

  gc_weak_redirect_all(g);  /* Redirect weak tables to arena/huge gray stacks. */
  lj_assertG(!gc_obj_iswhite(g, obj2gco(mainthread(g))), "main thread turned white");
  gc_markobj(g, L);  /* Mark running thread. */
  gc_traverse_mainthread(g);  /* Stack slots have no barriers. */
  gc_traverse_curtrace(g);  /* Traverse current trace. */
  gc_mark_gcroot(g);  /* Mark GC roots (again). */
  gc_propagate_gray(g);  /* Propagate all of the above. */

  lj_gc_ssb_flush(g);  /* Drain SSB into per-arena gray stacks. */
  /* Drain graythread (thread objects only): redirect arena threads to arena
  ** gray stacks for the atomic re-scan. The mainthread is handled directly. */
  while (!gc_graythread_empty(g)) {
    GCobj *o = gc_graythread_pop(g);
    lj_assertG(o->gch.gct == ~LJ_TTHREAD, "graythread contains non-thread");
    lj_assertG(gc_inarena(g, o), "non-arena thread in graythread");
    arena_gray_push(g, ptr2arena(o), (GCCellID1)ptr2cell(o));
  }
  gc_propagate_gray(g);  /* Propagate it. */

  udsize = lj_gc_separateudata(g, 0);  /* Separate userdata to be finalized. */
#if defined(LUA_USE_ASSERT)
  /* Invariant 4 (T7): post-separateudata(g,0), no white + unfinalized + __gc
  ** udata remains outside the mmudata ring. Read-only re-scan of udata arenas
  ** + hugeset — must NOT mutate any state (no flushbins/markfinalized/splice).
  ** Bins are already flushed from separateudata's per-arena flushbins call,
  ** so a->block[w] reads only true allocated heads. GCF_BITMAPSWEEP is not yet
  ** set (armed below), so gc_obj_iswhite(arena) == !arena_obj_ismarked. */
  {
    MSize vi;
    GCArena **varenas = mref(g->gc.arenas, GCArena *);
    for (vi = 0; vi < g->gc.arenastop; vi++) {
      GCArena *a = varenas[vi];
      uint32_t w, wtop;
      if (!(a->flags & ArenaFlag_UdataOnly)) continue;
      wtop = arena_blockidx((GCCellID)a->celltop - 1);
      for (w = UnusedBlockWords; w <= wtop; w++) {
	GCBlockword heads = a->block[w];
	while (heads) {
	  uint32_t bitidx = lj_ffs(heads);
	  GCCellID c = (w << 5) + bitidx;
	  GCobj *vo = (GCobj *)arena_cellptr(a, c);
	  heads &= heads - 1;
	  if (gc_obj_iswhite(g, vo) && !isfinalized(gco2ud(vo)) &&
	      lj_meta_fastg(g, tabref(gco2ud(vo)->metatable), MM_gc))
	    lj_assertG(0,
		       "post-separateudata: white unfinalized __gc udata missed: "
		       "ptr=%p gct=%d marked=0x%02x cell=%d",
		       (void *)vo, vo->gch.gct, vo->gch.marked, (int)c);
	}
      }
    }
    {
      GCRef *slots = mref(g->gc.hugeset, GCRef);
      if (slots != NULL) {
	MSize hi, hmask = g->gc.hugesetmask;
	for (hi = 0; hi <= hmask; hi++) {
	  uintptr_t u = gcrefu(slots[hi]);
	  GCobj *vo;
	  if (!hugeset_slot_live(u)) continue;
	  hugeset_slot_assert(g, u);
	  vo = hugeset_slot_obj(u);
	  if (vo->gch.gct != ~LJ_TUDATA) continue;
	  if (gc_obj_iswhite(g, vo) && !isfinalized(gco2ud(vo)) &&
	      lj_meta_fastg(g, tabref(gco2ud(vo)->metatable), MM_gc))
	    lj_assertG(0,
		       "post-separateudata: white unfinalized __gc huge udata missed: "
		       "ptr=%p gct=%d marked=0x%02x",
		       (void *)vo, vo->gch.gct, vo->gch.marked);
	}
      }
    }
  }
#endif
  gc_mark_mmudata(g);  /* Mark them. */
  udsize += gc_propagate_gray(g);  /* And propagate the marks. */

  /* All marking done, clear weak tables. */
  gc_clearweak_stacks(g);

  lj_buf_shrink(L, &g->tmpbuf);  /* Shrink temp buffer. */

  /* Prepare for sweep phase. */
  /* No white flip: liveness is the mark bitmap, not a flipping header white.
  ** strempty is an SFIXED root, never swept; reset its vestigial header color
  ** to the exact value the old post-flip path produced (curwhite was 0 during
  ** sweep): FIXED|SFIXED with no white bit, so it reads as a reachable root. */
  g->strempty.marked = LJ_GC_FIXED | LJ_GC_SFIXED;
  setmref(g->gc.sweep, &g->gc.root);
  g->gc.estimate = g->gc.total - (GCSize)udsize;  /* Initial estimate. */
  g->gc.gcmarkflags |= GCF_BITMAPSWEEP | GCF_DEADAUTH | GCF_MARKALLOC;
  g->gc.sweepa = 0;
  g->gc.sweepw = UnusedBlockWords;
  g->gc.sweepphase = SweepPhase_Bitmap;
}

/* GC state machine. Returns a cost estimate for each step performed. */
static size_t gc_onestep_raw(lua_State *L)
{
  global_State *g = G(L);
  switch (g->gc.state) {
  case GCSpause:
    gc_mark_start(g);  /* Start a new GC cycle by marking all GC roots. */
    return 0;
  case GCSpropagate:
    if (!gc_hugegray_empty(g))
      return propagatemark(g, gc_hugegray_pop(g));
    {
      GCArena *a = gc_grayarena_pop(g);
      if (a != NULL)
	return gc_propagate_arena(g, a);
    }
    g->gc.state = GCSatomic;  /* End of mark phase. */
    return 0;
  case GCSatomic:
    if (tvref(g->jit_base))  /* Don't run atomic phase on trace. */
      return LJ_MAX_MEM;
    atomic(g, L);
    g->gc.state = GCSsweepstring;  /* Start of sweep phase. */
    g->gc.sweepstr = 0;
    return 0;
  case GCSsweepstring: {
    GCSize old = g->gc.total;
    gc_sweepstr(g, &g->str.tab[g->gc.sweepstr++]);  /* Sweep one chain. */
    if (g->gc.sweepstr > g->str.mask)
      g->gc.state = GCSsweep;  /* All string hash chains sweeped. */
    lj_assertG(old >= g->gc.total, "sweep increased memory");
    g->gc.estimate -= old - g->gc.total;
    return GCSWEEPCOST;
    }
  case GCSsweep: {
    GCSize old = g->gc.total;
    if (g->gc.gcmarkflags & GCF_BITMAPSWEEP) {
      if (g->gc.sweepphase == SweepPhase_Bitmap) {
	gc_bitmap_sweep(g);
      }
      if (g->gc.sweepphase == SweepPhase_Rebuild) {
	/* Run one rebuild dispatch per onestep. The dispatcher yields after
	** each chunked sub-phase (Prologue/ArenaScan/HugeScan); Epilogue and
	** ClearMarks run to Done in the same dispatch call. */
	gc_rebuild_rootchain(g);
      }
      lj_assertG(old >= g->gc.total, "sweep increased memory");
      g->gc.estimate -= old - g->gc.total;
      if (g->gc.sweepphase == SweepPhase_Done) {
	g->gc.gcmarkflags = 0;
	if (g->str.num <= (g->str.mask >> 2) && g->str.mask > LJ_MIN_STRTAB*2-1)
	  lj_str_resize(L, g->str.mask >> 1);
	lj_arena_shrink(g);
	if (gcref(g->gc.mmudata)) {
	  g->gc.state = GCSfinalize;
	} else {
	  g->gc.state = GCSpause;
	  g->gc.debt = 0;
	}
      }
      return GCSWEEPMAX*GCSWEEPCOST;
    }
    setmref(g->gc.sweep, gc_sweep(g, mref(g->gc.sweep, GCRef), GCSWEEPMAX));
    lj_assertG(old >= g->gc.total, "sweep increased memory");
    g->gc.estimate -= old - g->gc.total;
    if (gcref(*mref(g->gc.sweep, GCRef)) == NULL) {
      g->gc.gcmarkflags = 0;
      if (g->str.num <= (g->str.mask >> 2) && g->str.mask > LJ_MIN_STRTAB*2-1)
	lj_str_resize(L, g->str.mask >> 1);  /* Shrink string table. */
      lj_arena_shrink(g);  /* Coalesce free space, release empty arenas. */
      if (gcref(g->gc.mmudata)) {  /* Need any finalizations? */
	g->gc.state = GCSfinalize;
      } else {  /* Otherwise skip this phase to help the JIT. */
	g->gc.state = GCSpause;  /* End of GC cycle. */
	g->gc.debt = 0;
      }
    }
    return GCSWEEPMAX*GCSWEEPCOST;
    }
  case GCSfinalize:
    if (gcref(g->gc.mmudata) != NULL) {
      GCSize old = g->gc.total;
      if (tvref(g->jit_base))  /* Don't call finalizers on trace. */
	return LJ_MAX_MEM;
      gc_finalize(L);  /* Finalize one userdata object. */
      if (old >= g->gc.total && g->gc.estimate > old - g->gc.total)
	g->gc.estimate -= old - g->gc.total;
      if (g->gc.estimate > GCFINALIZECOST)
	g->gc.estimate -= GCFINALIZECOST;
      return GCFINALIZECOST;
    }
    g->gc.state = GCSpause;  /* End of GC cycle. */
    g->gc.debt = 0;
    return 0;
  default:
    lj_assertG(0, "bad GC state");
    return 0;
  }
}

static size_t gc_onestep(lua_State *L)
{
  size_t cost = gc_onestep_raw(L);
#if defined(LUA_USE_ASSERT) && !defined(LJ_GC_NOSTEPVERIFY)
  /* Read-only free-list consistency check after every incremental step, in
  ** every GC phase. Catches arena double-free / bin corruption / accounting
  ** drift the instant a step produces it, instead of at the next dereference.
  ** Compiled out of release builds; the check itself mutates nothing. */
  lj_gc_checkheap(G(L));
#endif
  return cost;
}

/* Perform a limited amount of incremental GC steps. */
int LJ_FASTCALL lj_gc_step(lua_State *L)
{
  global_State *g = G(L);
  GCSize lim;
  int32_t ostate = g->vmstate;
  setvmstate(g, GC);
  lim = (GCSTEPSIZE/100) * g->gc.stepmul;
  if (lim == 0)
    lim = LJ_MAX_MEM;
  if (g->gc.total > g->gc.threshold)
    g->gc.debt += g->gc.total - g->gc.threshold;
  do {
    lim -= (GCSize)gc_onestep(L);
    if (g->gc.state == GCSpause) {
      g->gc.threshold = (g->gc.estimate/100) * g->gc.pause;
      g->vmstate = ostate;
      return 1;  /* Finished a GC cycle. */
    }
  } while (sizeof(lim) == 8 ? ((int64_t)lim > 0) : ((int32_t)lim > 0));
  if (g->gc.debt < GCSTEPSIZE) {
    g->gc.threshold = g->gc.total + GCSTEPSIZE;
    g->vmstate = ostate;
    return -1;
  } else {
    g->gc.debt -= GCSTEPSIZE;
    g->gc.threshold = g->gc.total;
    g->vmstate = ostate;
    return 0;
  }
}

/* Ditto, but fix the stack top first. */
void LJ_FASTCALL lj_gc_step_fixtop(lua_State *L)
{
  if (curr_funcisL(L)) L->top = curr_topL(L);
  lj_gc_step(L);
}

#if LJ_HASJIT
/* Perform multiple GC steps. Called from JIT-compiled code. */
int LJ_FASTCALL lj_gc_step_jit(global_State *g, MSize steps)
{
  lua_State *L = gco2th(gcref(g->cur_L));
  L->base = tvref(G(L)->jit_base);
  L->top = curr_topL(L);
  while (steps-- > 0 && lj_gc_step(L) == 0)
    ;
  /* Return 1 to force a trace exit. */
  return (G(L)->gc.state == GCSatomic || G(L)->gc.state == GCSfinalize);
}
#endif

#ifdef LUA_USE_ASSERT
/*
** Phase M shadow-verify: with header colors still authoritative, rebuild
** the arena mark bitmap from the live object set and assert it matches.
** Called at the end of a full GC, when every surviving GC object is live,
** so after shadow-marking all live arena objects there must be zero
** allocated-but-unmarked (dead) objects left. This validates the Phase S
** locate primitives (flushbins + setmark + block & ~mark) on the real
** heap without changing any GC behavior.
*/
static void gcverify_count_dead(void *o, int gct, void *ud)
{
  UNUSED(o); UNUSED(gct);
  (*(MSize *)ud)++;
}

static void gc_arena_verify_color(global_State *g, GCArena *a)
{
  uint32_t w, wtop = arena_blockidx((GCCellID)a->celltop - 1);
  lj_arena_flushbins(a);
  for (w = UnusedBlockWords; w <= wtop; w++) {
    GCBlockword heads = a->block[w];
    while (heads) {
      uint32_t bitidx = lj_ffs(heads);
      GCCellID c = (w << 5) + bitidx;
      GCobj *o = (GCobj *)arena_cellptr(a, c);
      heads &= heads - 1;
      /* Called at the tail of lj_gc_fullgc (state == GCSpause), after the mark
      ** phase blackened every survivor and gc_rebuild_rootchain re-whitened the
      ** bitmap. With the bitmap authoritative for arena color, every allocated
      ** object must read back as bitmap-white here. A surviving mark bit means
      ** the cycle ended with a stuck-black object -- a rebuild re-whitening
      ** regression (the next cycle's white-reset would then be wrong).
      **
      ** A header cross-check is deliberately NOT done: gc_mark writes
      ** white2gray(header) and arena_obj_setmark(bitmap) in lockstep, and
      ** allocation co-writes newwhite(header) with the unmarked bitmap, so the
      ** header WHITES bit can never diverge from the bitmap at a mutation site.
      ** Asserting their agreement would be vacuous; the live invariant is the
      ** bitmap reaching the clean all-white state the next cycle depends on.
      ** Verified reachable: defeating the rebuild whitening leaves objects
      ** bitmap-black here and this assert fires. */
      lj_assertG(!arena_obj_ismarked(a, c),
		 "arena object still bitmap-black after full GC rebuild: "
		 "ptr=%p gct=%d marked=0x%02x cell=%d",
		 (void *)o, o->gch.gct, o->gch.marked, (int)c);
    }
  }
}

static void gc_arena_verify(global_State *g)
{
  GCobj *o;
  MSize i, dead = 0;
  lj_assertG(gc_hugegray_empty(g), "arena verify with pending huge gray objects");
  /* Invariant 5 (T7): every mmudata ring member is finalized. The ring holds
  ** udata (spliced by sepudata_one with markfinalized) and cdata (spliced by
  ** lj_cdata_free with markfinalized). Both paths set LJ_GC_FINALIZED before
  ** linking, so a non-finalized member means a ring splice regressed. */
  {
    GCobj *mroot = gcref(g->gc.mmudata);
    if (mroot != NULL) {
      GCobj *mu = mroot;
      do {
	mu = gcnext(mu);
	lj_assertG(mu->gch.marked & LJ_GC_FINALIZED,
		   "mmudata ring member not finalized: ptr=%p gct=%d marked=0x%02x",
		   (void *)mu, mu->gch.gct, mu->gch.marked);
      } while (mu != mroot);
    }
  }
  {
    GCArena **arenas = mref(g->gc.arenas, GCArena *);
    for (i = 0; i < g->gc.arenastop; i++) {
      GCArena *a = arenas[i];
      if (a->flags & ArenaFlag_TravObjs)
	gc_arena_verify_color(g, a);
    }
  }
  /* Huge color cross-check, mirroring gc_arena_verify_color: at GCSpause after
  ** rebuild, every live huge object (string and non-string) must be slot-white.
  ** Non-strings are cleared by the rebuild set-walk; huge strings by the
  ** ~LJ_TSTR slot-clear in that same walk. A stuck slot mark means the
  ** per-cycle reset regressed. */
  {
    GCRef *slots = mref(g->gc.hugeset, GCRef);
    if (slots != NULL) {
      MSize hi, hmask = g->gc.hugesetmask;
      for (hi = 0; hi <= hmask; hi++) {
	uintptr_t u = gcrefu(slots[hi]);
	GCobj *o2;
	if (!hugeset_slot_live(u)) continue;
	hugeset_slot_assert(g, u);
	o2 = hugeset_slot_obj(u);
	lj_assertG(!(u & HUGESET_MARK),
		   "huge object still slot-black after full GC rebuild: "
		   "ptr=%p gct=%d marked=0x%02x", (void *)o2, o2->gch.gct,
		   o2->gch.marked);
      }
    }
  }
  /* Flush bins + clear all GC mark bits: clean slate, allocator-truthful. */
  lj_arena_gcprepare(g);
  /* Shadow-mark every allocated arena object by scanning the block bitmaps
  ** directly, instead of walking the gc.root chain (which is being eliminated).
  ** Trav arenas hold tables, funcs, protos, threads, upvalues, regular cdata,
  ** traces and udata. The bitmap scan finds all of them -- including open
  ** upvalues, which the root chain can't enumerate without per-thread walks.
    ** Huge objects are in the address-keyed huge set, not in any arena
    ** bitmap. huge strings are still owned by gc_sweepstr. mainthread/strempty
    ** are dlmalloc (not in arenas). */
  {
    GCArena **arenas = mref(g->gc.arenas, GCArena *);
    for (i = 0; i < g->gc.arenastop; i++) {
      GCArena *a = arenas[i];
      uint32_t w, wtop;
      if (!(a->flags & ArenaFlag_TravObjs)) continue;
      lj_arena_flushbins(a);
      wtop = arena_blockidx((GCCellID)a->celltop - 1);
      for (w = UnusedBlockWords; w <= wtop; w++) {
	GCBlockword alive = a->block[w];
	while (alive) {
	  uint32_t bitidx = lj_ffs(alive);
	  GCCellID c = (w << 5) + bitidx;
	  GCobj *o2 = (GCobj *)arena_cellptr(a, c);
	  alive &= alive - 1;
	  /* Invariants 1a/1b (T7): a cell is udata iff its arena is
	  ** ArenaFlag_UdataOnly. 1a locks the class invariant for udata arenas
	  ** (every allocated cell is a GCudata, continuous from T4's one-shot
	  ** assert inside separateudata). 1b catches a udata mis-routed into a
	  ** Trav/POD arena (a T3 routing regression). */
	  if (a->flags & ArenaFlag_UdataOnly) {
	    lj_assertG(o2->gch.gct == ~LJ_TUDATA,
		       "non-udata in Udata arena: gct=%d marked=0x%02x cell=%d flags=0x%x",
		       (int)o2->gch.gct, o2->gch.marked, (int)c, a->flags);
	  } else {
	    lj_assertG(o2->gch.gct != ~LJ_TUDATA,
		       "udata in non-Udata arena: gct=%d marked=0x%02x cell=%d flags=0x%x",
		       (int)o2->gch.gct, o2->gch.marked, (int)c, a->flags);
	  }
	  arena_obj_setmark(a, c);
#if LJ_HASFFI
	  if (o2->gch.gct == ~LJ_TCDATA && cdataisv(gco2cd(o2)))
	    arena_obj_shadowmark(memcdatav(gco2cd(o2)));
#endif
	}
      }
    }
    /* Invariant 1b extended (T7): NonTrav arenas (strings) must never hold a
    ** udata. The shadow-mark loop above only covers TravObjs arenas, so scan
    ** NonTrav arenas separately for a mis-routed udata. CdataV arenas are
    ** skipped here (their cell base is a GCcdataVar, not a GCobj — reading
    ** gct from it would be a type-punning read; the CdataV scan below
    ** verifies the class invariant with the base->cd translation). */
    for (i = 0; i < g->gc.arenastop; i++) {
      GCArena *a = arenas[i];
      uint32_t w, wtop;
      if (a->flags & (ArenaFlag_TravObjs | ArenaFlag_CdataVOnly)) continue;
      lj_arena_flushbins(a);
      wtop = arena_blockidx((GCCellID)a->celltop - 1);
      for (w = UnusedBlockWords; w <= wtop; w++) {
	GCBlockword heads = a->block[w];
	while (heads) {
	  uint32_t bitidx = lj_ffs(heads);
	  GCCellID c = (w << 5) + bitidx;
	  GCobj *o2 = (GCobj *)arena_cellptr(a, c);
	  heads &= heads - 1;
	  lj_assertG(o2->gch.gct != ~LJ_TUDATA,
		     "udata in NonTrav arena: gct=%d marked=0x%02x cell=%d flags=0x%x",
		     (int)o2->gch.gct, o2->gch.marked, (int)c, a->flags);
	}
      }
    }
    /* Huge objects have no cell bitmap. Huge strings are still owned by
    ** gc_sweepstr. */
    {
      GCRef *slots = mref(g->gc.hugeset, GCRef);
      if (slots != NULL) {
	MSize hi, hmask = g->gc.hugesetmask;
	for (hi = 0; hi <= hmask; hi++) {
	  uintptr_t u = gcrefu(slots[hi]);
	  if (!hugeset_slot_live(u)) continue;
	  hugeset_slot_assert(g, u);
	  { /* base is huge (no arena bitmap): shadowmark is a no-op; the slot mark
	    ** set by rebuild is the authority. o = cd for CDATAV slots, to read gct. */
	    GCobj *base = hugeset_slot_addr(u);
	    o = hugeset_slot_obj(u);
	    if (o->gch.gct == ~LJ_TSTR) continue;
	    arena_obj_shadowmark(base);
#if LJ_HASFFI
	    if (o->gch.gct == ~LJ_TCDATA && cdataisv(gco2cd(o)))
	      arena_obj_shadowmark(memcdatav(gco2cd(o)));  /* == base, no-op. */
#endif
	  }
	}
      }
    }
  }
#if LJ_HASFFI
  /* VLA cdata in CdataV arenas: shadow-mark the base cell (the allocation
  ** base = GCcdataVar prefix, which is what ptr2cell(p) refers to). Huge VLA
  ** were shadow-marked by the hugeset walk above (arena_obj_shadowmark is a
  ** no-op on huge). Cell base is a GCcdataVar; cd = base + offset carries the
  ** header, but the mark site is the base cell. */
  {
    GCArena **varenas = mref(g->gc.arenas, GCArena *);
    MSize vi;
    for (vi = 0; vi < g->gc.arenastop; vi++) {
      GCArena *a = varenas[vi];
      uint32_t vw, vwtop;
      if (!(a->flags & ArenaFlag_CdataVOnly)) continue;
      lj_arena_flushbins(a);
      vwtop = arena_blockidx((GCCellID)a->celltop - 1);
      for (vw = UnusedBlockWords; vw <= vwtop; vw++) {
	GCBlockword heads = a->block[vw];
	while (heads) {
	  uint32_t bitidx = lj_ffs(heads);
	  GCCellID c = (vw << 5) + bitidx;
	  char *p = (char *)arena_cellptr(a, c);
	  GCcdata *cd;
	  heads &= heads - 1;
	  cd = (GCcdata *)(p + ((GCcdataVar *)p)->offset);
	  cdatav_cell_assert(g, p);
	  arena_obj_shadowmark(p);
	}
      }
    }
  }
#endif
  for (i = 0; i <= g->str.mask; i++) {
    GCRef r = g->str.tab[i];
    /* Chain head low bit is the hashalg flag; mask it off. */
    for (o = (GCobj *)(gcrefu(r) & ~(uintptr_t)1); o != NULL; o = gcnext(o))
      if (!lj_arena_ishuge(o))
	arena_obj_shadowmark(o);
  }
  /* After a full GC nothing dead remains, so no allocated arena object may
  ** be left unmarked. */
  for (i = 0; i < g->gc.arenastop; i++)
    lj_arena_visit_unmarked(mref(g->gc.arenas, GCArena *)[i],
			    gcverify_count_dead, &dead);
  lj_assertG(dead == 0,
	     "arena shadow-verify: %d live objects missed by the bitmap",
	     (int)dead);
  /* Clear the shadow marks again so the allocator's bins (rebuilt lazily)
  ** start from a clean mark bitmap. */
  lj_arena_gcprepare(g);
}
#endif /* LUA_USE_ASSERT */

/*
** Read-only heap consistency checker for the arena allocator. Walks every
** arena's free lists (the intrusive same-size bins and the sorted range
** array) and validates their structural invariants plus the free-cell
** accounting, WITHOUT mutating any GC or allocator state -- so it is safe
** to call between incremental GC steps, in any GC phase. Modeled on Lua's
** ltests checkmemory, Go's gccheckmark and CRuby's verify_internal_consistency,
** but aimed at the arena-specific bug surface (double free, bin/range
** corruption, slab-refill miscounts) that generic VM GC tests never cover.
**
** Returns the number of violations found (0 == healthy). In assert builds
** each violation also fires lj_assertG to pinpoint the offending arena/cell.
**
** Always-valid structural checks (run in every phase):
**  - each bin cell is in [MinCellId, celltop), in the White (looks-allocated)
**    state, and the intrusive list has no cycle
**  - binmask agrees with which bins are non-empty
**  - each range entry is an in-range Free-state head, ascending by numcells
**
** Accounting check is the inequality bins + ranges <= freecells (the lists
** never reference more cells than are actually free); equality only holds
** right after a scavenge, because several free paths bump freecells without
** threading the block onto a list. Over-counting is the signature of a
** double free.
*/
int lj_gc_checkheap(global_State *g)
{
  MSize ai, bad = 0;
  for (ai = 0; ai < g->gc.arenastop; ai++) {
    GCArena *a = mref(g->gc.arenas, GCArena *)[ai];
    ArenaFreeList *fl = mref(a->freelist, ArenaFreeList);
    GCCellID celltop = (GCCellID)a->celltop;
    uint32_t binfree = 0, rangefree = 0, b;
    /* -- POD arena purity: every live object must be word-parallel-sweepable
    ** (no external backing, no globals, no finalizer). Protos and closures are
    ** routed here. Catches a mis-routed alloc the instant it lands, before the
    ** word-parallel sweep frees it blindly. Runs before the fl==NULL skip
    ** below: a fresh bump-only POD arena has no free list yet but still holds
    ** live objects to validate. Read-only: scans only block&mark (marked-live
    ** cells are always valid GCobjs); binned/free cells have mark=0 and are
    ** excluded, so the arena is not mutated. -- */
    if (a->flags & ArenaFlag_PODOnly) {
      uint32_t w, wtop = arena_blockidx(celltop - 1);
      for (w = UnusedBlockWords; w <= wtop; w++) {
	GCBlockword alive = a->block[w] & a->mark[w];
	while (alive) {
	  uint32_t bitidx = lj_ffs(alive);
	  GCobj *o = (GCobj *)arena_cellptr(a, (w << 5) + bitidx);
	  alive &= alive - 1;
	  if (o->gch.gct != ~LJ_TPROTO && o->gch.gct != ~LJ_TFUNC) {
	    lj_assertG(0, "POD arena %d: non-POD object gct=%d at cell %d",
		       (int)ai, o->gch.gct, (int)((w << 5) + bitidx));
	    bad++;
	  }
	}
      }
    }
    if (fl == NULL) continue;  /* No free list allocated yet. */
    /* -- Bins: intrusive same-size free lists. -- */
    for (b = 0; b < ArenaBins; b++) {
      GCCellID n = b + 1;
      GCCellID c = fl->bins[b];
      uint32_t guard = 0, nonempty = (c != 0);
      if (((fl->binmask >> b) & 1u) != nonempty) {
	lj_assertG(0, "arena %d: binmask vs bin[%d] disagree", (int)ai, (int)b);
	bad++;
      }
      while (c != 0) {
	if (c < MinCellId || c >= celltop) {
	  lj_assertG(0, "arena %d bin %d: cell %d out of range",
		     (int)ai, (int)b, (int)c);
	  bad++; break;
	}
	if (arena_cellstate(a, c) != CellState_White) {
	  lj_assertG(0, "arena %d bin %d: cell %d not in allocated state",
		     (int)ai, (int)b, (int)c);
	  bad++; break;
	}
	binfree += n;
	if (++guard > (uint32_t)ArenaUsableCells) {  /* Cycle in the list. */
	  lj_assertG(0, "arena %d bin %d: free-list cycle", (int)ai, (int)b);
	  bad++; break;
	}
	c = *(GCCellID1 *)arena_cellptr(a, c);
      }
    }
    /* -- Ranges: free blocks sorted ascending by cell count. -- */
    {
      uint32_t i, top = fl->rangetop;
      GCCellID1 prevn = 0;
      if (top > ArenaRangeCap) {
	lj_assertG(0, "arena %d: rangetop %d over cap", (int)ai, (int)top);
	bad++; top = ArenaRangeCap;
      }
      for (i = 0; i < top; i++) {
	GCCellID c = fl->ranges[i].id;
	GCCellID n = fl->ranges[i].numcells;
	if (n == 0 || c < MinCellId || c + n > celltop) {
	  lj_assertG(0, "arena %d range %d: [%d+%d] out of range",
		     (int)ai, (int)i, (int)c, (int)n);
	  bad++; continue;
	}
	if (arena_cellstate(a, c) != CellState_Free) {
	  lj_assertG(0, "arena %d range %d: cell %d not a free head",
		     (int)ai, (int)i, (int)c);
	  bad++;
	}
	if (fl->ranges[i].numcells < prevn) {
	  lj_assertG(0, "arena %d range %d: not sorted by numcells",
		     (int)ai, (int)i);
	  bad++;
	}
	prevn = fl->ranges[i].numcells;
	rangefree += n;
      }
    }
    /* -- Accounting. freecells counts every free cell in the arena, but the
    ** bins/ranges only hold blocks that have been threaded into the free
    ** lists: frees while fl==NULL, the bitmap sweep, and the bump-frontier
    ** rollback bump freecells without touching a list, and slab-refill keeps
    ** carved cells counted as free. So equality only holds right after a
    ** scavenge; the robust invariant is that the lists never reference more
    ** cells than are actually free. Over-counting here is the signature of a
    ** double free inflating a bin/range. -- */
    if (binfree + rangefree > a->freecells) {
      lj_assertG(0, "arena %d: bins %d + ranges %d exceed freecells %d",
		 (int)ai, (int)binfree, (int)rangefree, (int)a->freecells);
      bad++;
    }
  }
  return (int)bad;
}

/* Perform a full GC cycle. */
void lj_gc_fullgc(lua_State *L)
{
  global_State *g = G(L);
  int32_t ostate = g->vmstate;
  setvmstate(g, GC);
  /* T0 root-chain baseline: snapshot the pre-collect chain so the harness can
  ** observe which writers currently link to g->gc.root. No-op unless
  ** LUAJIT_GC_ROOT_CHAIN_PROBE is set; release builds compile it out. */
  gc_root_chain_probe_print(g, "fullgc-entry");
  if (g->gc.state <= GCSatomic) {  /* Caught somewhere in the middle. */
    gc_hugegray_reset(g);  /* Reset worklists from partial propagation. */
    gc_graythread_reset(g);
    gc_weak_reset(g);
    setmref(g->gc.ssbtop, mref(g->gc.ssb, GCobj *));  /* Discard SSB. */
    /* Under single-white, the header-based sweep predicate can't reliably
    ** distinguish alive-white from dead-white, so the preserving catch-up
    ** sweep doesn't work.  Enumerate all arena objects via the block bitmaps
    ** and makewhite every one so the next full mark cycle can re-discover them.
    ** No objects are freed — the partial mark phase hasn't reached sweep. */
    {
      GCArena **arenas = mref(g->gc.arenas, GCArena *);
      GCobj *o;
      MSize ii;
      /* Trav arenas: tables, funcs, protos, threads, upvalues, regular cdata,
      ** traces and udata.  Strings are handled by the intern table walk below.
      ** Open upvalues are found directly by the bitmap scan (no per-thread
      ** walk needed). */
      for (ii = 0; ii < g->gc.arenastop; ii++) {
	GCArena *a = arenas[ii];
	uint32_t w, wtop;
	if (!(a->flags & ArenaFlag_TravObjs)) continue;
	lj_arena_flushbins(a);
	wtop = arena_blockidx((GCCellID)a->celltop - 1);
	for (w = UnusedBlockWords; w <= wtop; w++) {
	  GCBlockword alive = a->block[w];
	  while (alive) {
	    uint32_t bitidx = lj_ffs(alive);
	    GCCellID c = (w << 5) + bitidx;
	    o = (GCobj *)arena_cellptr(a, c);
	    alive &= alive - 1;
	    if (o->gch.gct == ~LJ_TSTR) continue;
	    makewhite(g, o);
	  }
	}
      }
      /* Huge objects have no cell bitmap. Huge strings are made white via the
      ** intern-table walk below. */
      {
	GCRef *slots = mref(g->gc.hugeset, GCRef);
	if (slots != NULL) {
	  MSize hi, hmask = g->gc.hugesetmask;
	  for (hi = 0; hi <= hmask; hi++) {
	    uintptr_t u = gcrefu(slots[hi]);
	    if (!hugeset_slot_live(u)) continue;
	    hugeset_slot_assert(g, u);
	    { /* base owns the slot mark; o = cd carries the GCcdata header. */
	      GCobj *base = hugeset_slot_addr(u);
	      o = hugeset_slot_obj(u);
	      if (o->gch.gct == ~LJ_TSTR) continue;
	      huge_obj_clearmark(g, base);  /* Reset slot mark with the header. */
	      makewhite(g, o);
	    }
	  }
	}
      }
      /* mainthread is not in any arena (dlmalloc). */
      makewhite(g, obj2gco(mainthread(g)));
      /* Also makewhite strings in the intern table. The chain HEAD stores the
      ** per-bucket hashalg marker in bit 0 (lj_str.c), so mask it off before
      ** dereferencing; subsequent nextgc links carry no marker. */
      {
        MSize i;
        for (i = 0; i <= g->str.mask; i++) {
          GCobj *o2 = (GCobj *)(gcrefu(g->str.tab[i]) & ~(uintptr_t)1);
          while (o2 != NULL) {
            if (lj_arena_ishuge(o2))
              huge_obj_clearmark(g, o2);  /* Reset slot mark with the header. */
            makewhite(g, o2);
            o2 = gcref(o2->gch.nextgc);
          }
        }
      }
#if LJ_HASFFI
      /* Also makewhite VLA cdata in CdataV arenas (small VLA). Huge VLA
      ** were makewhite by the hugeset walk above. Cell base is a GCcdataVar;
      ** cd = base + offset carries the GCcdata header. */
      {
	GCArena **cva = mref(g->gc.arenas, GCArena *);
	MSize ci;
	for (ci = 0; ci < g->gc.arenastop; ci++) {
	  GCArena *a = cva[ci];
	  uint32_t cw, cwtop;
	  if (!(a->flags & ArenaFlag_CdataVOnly)) continue;
	  lj_arena_flushbins(a);
	  cwtop = arena_blockidx((GCCellID)a->celltop - 1);
	  for (cw = UnusedBlockWords; cw <= cwtop; cw++) {
	    GCBlockword alloc = a->block[cw];
	    while (alloc) {
	      uint32_t bitidx = lj_ffs(alloc);
	      GCCellID c = (cw << 5) + bitidx;
	      char *p = (char *)arena_cellptr(a, c);
	      GCcdata *cd;
	      alloc &= alloc - 1;
	      cd = (GCcdata *)(p + ((GCcdataVar *)p)->offset);
	      cdatav_cell_assert(g, p);
	      makewhite(g, obj2gco(cd));
	    }
	  }
	}
      }
#endif
    }
    g->gc.gcmarkflags = 0;
    g->gc.grayastop = 0;
    {
      MSize ii;
      for (ii = 0; ii < g->gc.arenastop; ii++) {
	GCArena *aa = mref(g->gc.arenas, GCArena *)[ii];
	aa->flags &= (uint16_t)~ArenaFlag_InGrayHeap;  /* grayastop=0 above. */
	if (mref(aa->greybase, GCCellID1) != NULL)
	  arena_gray_reset(aa);
      }
    }
    g->gc.state = GCSpause;
  }
  while (g->gc.state == GCSsweepstring || g->gc.state == GCSsweep)
    gc_onestep(L);  /* Finish sweep. */
  lj_assertG(g->gc.state == GCSfinalize || g->gc.state == GCSpause,
	     "bad GC state");
  /* Now perform a full GC. */
  g->gc.state = GCSpause;
  do { gc_onestep(L); } while (g->gc.state != GCSpause);
  g->gc.threshold = (g->gc.estimate/100) * g->gc.pause;
  g->vmstate = ostate;
#ifdef LUA_USE_ASSERT
  gc_arena_verify(g);  /* Phase M: cross-check the arena mark bitmap. */
#endif
}

/* -- Write barriers ------------------------------------------------------ */

/* Backward barrier for arena objects (called from interpreter/JIT).
** Sets gray bit, then checks mark bitmap: black→dark-gray pushes to SSB,
** white→light-gray just sets gray (no push needed). */
void lj_gc_barrierback_arena(global_State *g, GCobj *o)
{
  o->gch.marked |= LJ_GC_GRAY;
  /* Rebuild window (GCF_BITMAPSWEEP set, GCF_DEADAUTH clear): survivor marks are
  ** being torn down, so an ismarked read here is stale. Skip the queue push --
  ** benign: this cycle's gray drained at atomic; next cycle re-marks from roots
  ** (lj_arena_gc_markinit). Just leaving the gray bit set is enough. */
  if ((g->gc.gcmarkflags & GCF_BITMAPSWEEP) &&
      !(g->gc.gcmarkflags & GCF_DEADAUTH))
    return;
  if (lj_arena_ishuge(o)) {
    /* Every huge object carries black in its hugeset slot; gc_sweepstr still
    ** unlinks dead huge strings from the intern table, but the slot owns
    ** their color. */
    if (huge_obj_ismarked(g, o))
      gc_hugegray_push(g, o);
    return;
  }
  if (arena_obj_ismarked(ptr2arena(o), ptr2cell(o))) {
    GCobj **top = mref(g->gc.ssbtop, GCobj *);
    *top++ = o;
    setmref(g->gc.ssbtop, top);
    if (LJ_UNLIKELY(top >= mref(g->gc.ssblim, GCobj *)))
      lj_gc_ssb_flush(g);
  }
}

/* Notify that an arena's gray stack became non-empty — insert into heap. */
void lj_gc_grayarena_notify(global_State *g, MSize idx)
{
  MSize *heap;
  GCArena *a = mref(g->gc.arenas, GCArena *)[idx];
  if (a->flags & ArenaFlag_InGrayHeap)
    return;  /* Already queued: skip duplicate insert (dedup guard). */
  heap = mref(g->gc.grayastack, MSize);
  if (g->gc.grayastop >= g->gc.grayasz) {
    MSize oldsz = g->gc.grayasz;
    MSize newsz = oldsz ? oldsz * 2 : 16;
    heap = (MSize *)g->allocf(g->allocd, heap,
	      oldsz * sizeof(MSize), newsz * sizeof(MSize));
    setmref(g->gc.grayastack, heap);
    g->gc.grayasz = newsz;
  }
  heap[g->gc.grayastop] = idx;
  grayheap_siftup(g, heap, g->gc.grayastop);
  g->gc.grayastop++;
  a->flags |= ArenaFlag_InGrayHeap;  /* Set only after the insert succeeds. */
}

/* Flush the sequential store buffer into per-arena gray stacks. */
void lj_gc_ssb_flush(global_State *g)
{
  GCobj **base = mref(g->gc.ssb, GCobj *);
  GCobj **top = mref(g->gc.ssbtop, GCobj *);
  setmref(g->gc.ssbtop, base);
  while (base < top) {
    GCobj *o = *base++;
    if (o->gch.marked & LJ_GC_GRAY) {
      arena_gray_push(g, ptr2arena(o), (GCCellID1)ptr2cell(o));
    }
  }
}

/* Move the GC propagation frontier forward. */
void lj_gc_barrierf(global_State *g, GCobj *o, GCobj *v)
{
  lj_assertG(!(o->gch.marked & LJ_GC_GRAY) && !gc_obj_isdead(g, o),
	     "bad object states for forward barrier");
  /* Note: unlike the header-color GC, the inline barrier fast path here only
  ** tests the gray bit, so this is entered for non-gray (white OR black)
  ** parents -- including white parents during GCSfinalize/GCSpause, where a
  ** stock LuaJIT forward barrier never fires. That is benign: the else-branch
  ** below just sets the gray bit. Hence no state assert under LJ_HASGCMARK. */
  lj_assertG(o->gch.gct != ~LJ_TTAB, "barrier object is not a table");
  /* Preserve invariant during propagation. Otherwise it doesn't matter. */
  if (g->gc.state == GCSpropagate || g->gc.state == GCSatomic)
    gc_mark(g, v);  /* Move frontier forward. */
  else
    o->gch.marked |= LJ_GC_GRAY;  /* Set gray to avoid re-triggering barrier. */
}

/* Specialized barrier for closed upvalue. Pass &uv->tv. */
void LJ_FASTCALL lj_gc_barrieruv(global_State *g, TValue *tv)
{
#define TV2MARKED(x) \
  (*((uint8_t *)(x) - offsetof(GCupval, tv) + offsetof(GCupval, marked)))
  if (g->gc.state == GCSpropagate || g->gc.state == GCSatomic)
    gc_mark(g, gcV(tv));
  else
    TV2MARKED(tv) |= LJ_GC_GRAY;  /* Set gray to avoid re-triggering barrier. */
#undef TV2MARKED
}

/* Close upvalue. Also needs a write barrier. */
void lj_gc_closeuv(global_State *g, GCupval *uv)
{
  GCobj *o = obj2gco(uv);
  /* Copy stack slot to upvalue itself and point to the copy. */
  copyTV(mainthread(g), &uv->tv, uvval(uv));
  setmref(uv->v, &uv->tv);
  uv->closed = 1;
  if ((o->gch.marked & LJ_GC_GRAY) && !gc_obj_iswhite(g, o)) {
    if (g->gc.state == GCSpropagate || g->gc.state == GCSatomic) {
      gray2black(o);  /* Make it black and preserve invariant. */
      if (tvisgcv(&uv->tv) && gc_obj_iswhite(g, gcV(&uv->tv)))
	lj_gc_barrierf(g, o, gcV(&uv->tv));
    } else {
      makewhite(g, o);  /* Make it white, i.e. sweep the upvalue. */
      lj_assertG(g->gc.state != GCSfinalize && g->gc.state != GCSpause,
		 "bad GC state");
    }
  }
}

#if LJ_HASJIT
/* Mark a trace if it's saved during the propagation phase. */
void lj_gc_barriertrace(global_State *g, uint32_t traceno)
{
  if (g->gc.state == GCSpropagate || g->gc.state == GCSatomic)
    gc_marktrace(g, traceno);
}
#endif

/* -- Allocator ----------------------------------------------------------- */

/* Call pluggable memory allocator to allocate or resize a fragment. */
void *lj_mem_realloc(lua_State *L, void *p, GCSize osz, GCSize nsz)
{
  global_State *g = G(L);
  lj_assertG((osz == 0) == (p == NULL), "realloc API violation");
  p = g->allocf(g->allocd, p, osz, nsz);
  if (p == NULL && nsz > 0)
    lj_err_mem(L);
  lj_assertG((nsz == 0) == (p == NULL), "allocf API violation");
  lj_assertG(checkptrGC(p),
	     "allocated memory address %p outside required range", p);
  g->gc.total = (g->gc.total - osz) + nsz;
  return p;
}

/* Allocate new GC object and link it to the root set. */
void * LJ_FASTCALL lj_mem_newgco(lua_State *L, GCSize size)
{
  return lj_mem_newgco_arena(L, size, 0, 1);
}


/*
** Out-of-line continuation of lj_mem_newgco_arena(): the current arena
** had no bump space (or the size calls for a huge block).
*/
void *lj_mem_newgco_slow(lua_State *L, GCSize size, int cls, int link)
{
  global_State *g = G(L);
  GCobj *o;
  if (LJ_LIKELY(size < ArenaHugeThreshold)) {
    o = (GCobj *)lj_arena_findspace(g, size, cls);
  } else {
    o = (GCobj *)lj_hugeblock_alloc(g, size);
  }
  if (o == NULL)
    lj_err_mem(L);
  lj_assertG(checkptrGC(o),
	     "allocated memory address %p outside required range", o);
  /* POD arenas account cell-space bytes (matches lj_mem_newgco_arena and the
  ** word-parallel sweep). A POD object never goes huge (protos are small), so
  ** the cell-space form only applies to the in-arena case. */
  if (cls == ArenaClass_POD && !lj_arena_ishuge(o))
    g->gc.total += (GCSize)arena_roundcells(size) << CellSizeLog2;
  else
    g->gc.total += size;
  if (LJ_UNLIKELY(g->gc.gcmarkflags & GCF_MARKALLOC)) {
    /* An object allocated during the sweep window must be slot/bitmap-black so
    ** the sweep does not free it under the caller's nose. Huge objects (string
    ** and non-string) carry color in their hugeset slot, in-arena objects in
    ** the cell bitmap -- mark whichever applies, symmetric with the in-arena
    ** fast path in lj_mem_newgco_arena. */
    if (lj_arena_ishuge(o))
      huge_obj_setmark(g, o);
    else
      arena_obj_setmark(ptr2arena(o), ptr2cell(o));
  }
  if (link)
    newwhite(g, o);
  return o;
}


/* Resize growable vector. */
void *lj_mem_grow(lua_State *L, void *p, MSize *szp, MSize lim, MSize esz)
{
  MSize sz = (*szp) << 1;
  if (sz < LJ_MIN_VECSZ)
    sz = LJ_MIN_VECSZ;
  if (sz > lim)
    sz = lim;
  p = lj_mem_realloc(L, p, (*szp)*esz, sz*esz);
  *szp = sz;
  return p;
}

#endif

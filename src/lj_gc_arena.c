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

#if defined(LUAJIT_ENABLE_GCSTATS_TIMING)
#include <time.h>
#endif

#define GCSTEPSIZE	1024u
#define GCSWEEPMAX	40
#define GCSWEEPCOST	10
#define GCFINALIZECOST	100
/* D: GenGC bitmap free budget per onestep. Cost return is intentionally NOT
 * scaled (stays GCSWEEPMAX*GCSWEEPCOST) so the pacer runs ~5 onesteps per
 * lj_gc_step, each freeing up to 256 cells (was 40) — ~6.4x free throughput.
 * Shrinks the free window during which nursery alloc promotes survivors to
 * Old. ~4KB/onestep (256*CellSize) keeps each call in microseconds. Classic
 * sweep still uses GCSWEEPMAX. */
#define GCSWEEP_BITMAP_MAX	256

/* Post-sweep assert chunking: max arenas verified per SweepHuge_Assert slice.
** ~16 arenas/slice ≈ 256–512KB bitmap traffic, targeting sub-ms pause.
** Hugeset free+demote is one-shot in SweepHuge_Scan. */
#define GCSWEEP_ASSERT_DEMOTE_ARENAS	16

/* Batch gray pop granularity for arena mark propagation (P1-3a). Pop up to N
** gray entries per iteration of the POD / mixed drain; the outer drain loop
** still runs until the gray stack is empty (including same-arena re-pushes
** from traversal). N=8 keeps the local-resolve array small for the hot path
** while amortizing greytop updates across multiple worklist items. */
#define GC_MARK_BATCH_N	8

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

/* Mark a string object. The only non-arena/non-huge string is strempty, an
** SFIXED constant-live root with no bitmap/slot and no header color -- nothing
** to mark. newwhite is light-gray (GRAY only); strings never gray-traverse, so
** clear header GRAY when setting the arena/huge mark — free window forbids
** mark∧GRAY residuals (gc_assert_atomic_end). Classic gc_mark_str clears
** whites for the same leaf-mark effect. */
#define gc_mark_str(g, s) do { \
  GCstr *_s = (s); \
  if (gc_inarena(g, obj2gco(_s))) \
    arena_obj_setmark(ptr2arena(_s), ptr2cell(_s)); \
  else if (lj_arena_ishuge(obj2gco(_s))) \
    huge_obj_setmark(g, obj2gco(_s)); \
  (_s)->marked &= (uint8_t)~LJ_GC_GRAY; \
  } while (0)

static void gc_hugegray_push(global_State *g, GCobj *o);
static int gc_hugegray_empty(global_State *g);
static GCobj *gc_hugegray_pop(global_State *g);
static void gc_hugegray_reset(global_State *g);
static void gc_graythread_push(global_State *g, GCobj *o);
static int gc_graythread_empty(global_State *g);
static void gc_graythread_reset(global_State *g);
static void gc_atomic_rescan_threads(global_State *g);
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

/* T3b-1: root-chain probe/anchor helpers retired under HASGCMARK.
** Enumeration is arena bitmaps + hugeset + strtab; g->gc.root is gone.
*/


#if LJ_HASGCMARK
static FinEntry *fin_tab_find(global_State *g, GCobj *o, int *found);
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
    if (arena_obj_ismarked(a, c)) {
      /* Already marked: do not re-push. Object may still be on greystack/SSB
      ** (legit mark∧GRAY) or pure black. Orphans off every worklist violate
      ** free-window design (Debug: gc_assert_atomic_end). Re-queue floods. */
      return;
    }
  } else if (inhuge) {
    if (huge_obj_ismarked(g, key)) {  /* Slot mark is the dedup gate. */
      return;
    }
  } else {
    /* SFIXED roots (mainthread, strempty) and any other non-arena object:
    ** always live. Empty string "" is strempty and is a normal table key —
    ** gc_mark must be a no-op, not an assert. */
    lj_assertG(o == obj2gco(mainthread(g)) || o == obj2gco(&g->strempty) ||
	       (o->gch.marked & LJ_GC_SFIXED),
	       "gc_mark of unexpected non-arena object: gct=%d marked=0x%02x",
	       gct, o->gch.marked);
    return;
  }
  /* Set the mark bit first. Leaves (STR/CDATA/UDATA/UPVAL) are born light-gray
  ** via newwhite (marked = LJ_GC_GRAY); gray2black below clears that birth GRAY
  ** so the free-window residual invariant holds (zero mark∧GRAY). white2gray is
  ** NOT called on leaves — it would be a no-op (GRAY already set). Non-leaves
  ** call white2gray in the else branch below before being pushed onto a gray
  ** stack (they are not born GRAY-painted by newwhite in the leaf sense). */
  if (inarena)
    arena_obj_setmark(a, c);
  else
    huge_obj_setmark(g, key);
  if (LJ_UNLIKELY(gct == ~LJ_TUDATA)) {
    GCtab *mt = tabref(gco2ud(o)->metatable);
    gray2black(o);  /* Clear birth GRAY (newwhite); mark set above. */
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
    /* P3a (classic-aligned): closed UV → pure black (gray2black clears birth
    ** GRAY so the free-window residual invariant holds for closed UVs); open
    ** UV → mark set, GRAY kept (NOT gray2black'd). Open UV is the deliberate
    ** mark∧GRAY residual exception: its value aliases a stack slot re-marked
    ** by gc_atomic_rescan_threads, the UV header is re-closed by lj_gc_closeuv
    ** (gray2black on close during prop/atomic), and dead open UVs are freed
    ** by Path L (closeuv) / Path F (freeall_openuv) (bitmap sweep skips
    ** !closed). GCMARK barriers prefilter !isgray, so a mark∧GRAY open UV
    ** correctly skips lj_gc_barrierf / barrierback — no path assumes open UV
    ** is pure-black. */
    if (uv->closed)
      gray2black(o);
  } else if (gct == ~LJ_TSTR || gct == ~LJ_TCDATA) {
    /* Leaves: never pushed to a gray worklist. gray2black clears the birth
    ** GRAY so the cell is pure black (mark set, GRAY clear) — same end state
    ** as gc_mark_str. white2gray omitted (no-op on light-gray leaves). */
    gray2black(o);
#if LJ_HASFFI
    /* Registry holds fin outside the mark graph (no FFI_FIN strong values).
    ** Keep fin live for live CDATA_FIN objects (dead ones get fin marked at
    ** separate and via the fin_queue slot at resurrection). */
    if (gct == ~LJ_TCDATA && (o->gch.marked & LJ_GC_CDATA_FIN)) {
      int found = 0;
      FinEntry *e = fin_tab_find(g, o, &found);
      if (found && e != NULL && gcref(e->fin) != NULL)
	gc_markobj(g, gcref(e->fin));
    }
#endif
  } else {
    lj_assertG(gct == ~LJ_TFUNC || gct == ~LJ_TTAB ||
	       gct == ~LJ_TTHREAD || gct == ~LJ_TPROTO || gct == ~LJ_TTRACE,
	       "bad GC type %d", gct);
    white2gray(o);
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

/* Re-root pending fin_queue (obj + cdata fin). Sole call site: gc_mark_start.
** Enqueue already marks inline; free/hugescan demote survivors, so each
** new cycle must re-mark the queue as roots (classic gc_mark_mmudata role). */
static void gc_mark_fin_queue(global_State *g)
{
  FinQueueEntry *q = mref(g->gc.fin_queue, FinQueueEntry);
  MSize mask = g->gc.fin_qmask;
  MSize head = g->gc.fin_qhead, tail = g->gc.fin_qtail, i;
  if (q == NULL || head == tail) return;
  for (i = head; i != tail; i++) {
    FinQueueEntry *e = &q[i & mask];
    GCobj *o = gcref(e->obj);
    if (o != NULL)
      gc_markobj(g, o);
#if LJ_HASFFI
    if (e->kind == FIN_KIND_CDATA) {
      GCobj *fin = gcref(e->fin);
      if (fin != NULL)
	gc_markobj(g, fin);
    }
#endif
  }
}

/* Start a GC cycle and mark the root set. */
static void gc_mark_start(global_State *g)
{
  gc_hugegray_reset(g);
  gc_graythread_reset(g);
  gc_weak_reset(g);
  g->gc.grayastop = 0;
  /* Cycle restart: marks are cleared below, so residual header GRAY without
  ** mark is harmless light-gray. Still drop SSB entries explicitly. */
  setmref(g->gc.ssbtop, mref(g->gc.ssb, GCobj *));
  {
    MSize i;
    for (i = 0; i < g->gc.arenastop; i++) {
      GCArena *a = mref(g->gc.arenas, GCArena *)[i];
      a->flags &= (uint16_t)~ArenaFlag_InGrayHeap;
      if (mref(a->greybase, GCCellID1) != NULL)
	arena_gray_reset(a);
    }
  }
  lj_arena_gc_assert_demote(g);
  /* mainthread is SFIXED: gc_mark early-returns at the non-arena/non-huge
  ** assert branch. gc_traverse_mainthread below walks its stack frames. */
  gc_markobj(g, tabref(mainthread(g)->env));
  gc_markobj(g, vmthread(g));
  gc_marktv(g, &g->registrytv);
  gc_mark_gcroot(g);
  gc_traverse_mainthread(g);
  /* Pending finals are not in the registry; re-root before propagate. */
  gc_mark_fin_queue(g);
  g->gc.gccycle++;
  g->gc.state = GCSpropagate;
}

/* Udata: white (or all) + has __gc + !FINALIZED → fin_queue (registration FIFO).
** Unregister at death judgment; resurrect via makewhite+mark so the object and
** its __gc graph survive the free window until mutator drain. */
static size_t sepudata_one(global_State *g, GCobj *o, int all)
{
  if (!(gc_obj_iswhite(g, o) || all) || isfinalized(gco2ud(o)))
    return 0;
  if (!lj_meta_fastg(g, tabref(gco2ud(o)->metatable), MM_gc)) {
    markfinalized(o);
    lj_gc_fin_unregister(g, o);
    return 0;
  }
  {
    size_t sz = sizeudata(gco2ud(o));
    markfinalized(o);
    lj_gc_fin_queue_push(g, o, FIN_KIND_UDATA, NULL, 0);
    lj_gc_fin_unregister(g, o);
    gc_obj_makewhite(g, o);  /* Could be from previous GC. */
    gc_mark(g, o);
    return sz;
  }
}

#if LJ_HASFFI
/* Cdata: copy fin into queue entry, unregister registry, mark fin + object. */
static size_t sepcdata_one(global_State *g, GCobj *o, int all)
{
  int found = 0;
  FinEntry *e;
  GCobj *fin = NULL;
  uint32_t fin_it = 0;
  if (!(gc_obj_iswhite(g, o) || all) || (o->gch.marked & LJ_GC_FINALIZED))
    return 0;
  e = fin_tab_find(g, o, &found);
  if (found && e != NULL && e->kind == FIN_KIND_CDATA) {
    fin = gcref(e->fin);
    fin_it = e->fin_it;
  }
  o->gch.marked |= LJ_GC_CDATA_FIN;
  markfinalized(o);
  lj_gc_fin_queue_push(g, o, FIN_KIND_CDATA, fin, fin_it);
  if (fin != NULL)
    gc_markobj(g, fin);  /* Keep fin live after registry drop. */
  lj_gc_fin_unregister(g, o);
  gc_obj_makewhite(g, o);
  gc_mark(g, o);
  return 0;
}
#endif

/* -- Finalizer registry (F2 authority; docs/arenagc-finalizer-registry.md) */

#define FIN_TAB_INIT	16

static LJ_AINLINE MSize fin_hash(GCobj *o, MSize mask)
{
  uintptr_t u = u64ptr(o);
  u ^= u >> 4;
  return (MSize)(u & (uintptr_t)mask);
}

#if defined(LUA_USE_ASSERT)
/* True if o is pending on fin_queue (replaces mmudata ring membership). */
static int fin_on_queue(global_State *g, GCobj *o)
{
  FinQueueEntry *q = mref(g->gc.fin_queue, FinQueueEntry);
  MSize mask = g->gc.fin_qmask;
  MSize head = g->gc.fin_qhead, tail = g->gc.fin_qtail, i;
  if (q == NULL || head == tail) return 0;
  for (i = head; i != tail; i++) {
    if (gcref(q[i & mask].obj) == o) return 1;
  }
  return 0;
}
#endif

static void fin_tab_rehash(global_State *g, MSize newmask)
{
  FinEntry *old = mref(g->gc.fin_tab, FinEntry);
  MSize oldmask = g->gc.fin_mask;
  MSize nsz = (newmask + 1) * sizeof(FinEntry);
  FinEntry *tab = (FinEntry *)g->allocf(g->allocd, NULL, 0, nsz);
  MSize i;
  if (LJ_UNLIKELY(tab == NULL))
    lj_err_mem(mainthread(g));
  memset(tab, 0, nsz);
  g->gc.fin_tomb = 0;
  if (old != NULL && oldmask != 0) {
    for (i = 0; i <= oldmask; i++) {
      if (old[i].kind == FIN_KIND_UDATA || old[i].kind == FIN_KIND_CDATA) {
	GCobj *o = gcref(old[i].obj);
	MSize h = fin_hash(o, newmask);
	while (tab[h].kind != FIN_KIND_EMPTY)
	  h = (h + 1) & newmask;
	tab[h] = old[i];
      }
    }
    g->allocf(g->allocd, old, (oldmask + 1) * sizeof(FinEntry), 0);
  }
  setmref(g->gc.fin_tab, tab);
  g->gc.fin_mask = newmask;
}

static FinEntry *fin_tab_find(global_State *g, GCobj *o, int *found)
{
  FinEntry *tab = mref(g->gc.fin_tab, FinEntry);
  MSize mask = g->gc.fin_mask;
  MSize h, n;
  *found = 0;
  if (tab == NULL || mask == 0) return NULL;
  h = fin_hash(o, mask);
  for (n = 0; n <= mask; n++) {
    uint8_t k = tab[h].kind;
    if (k == FIN_KIND_EMPTY) return &tab[h];
    if ((k == FIN_KIND_UDATA || k == FIN_KIND_CDATA) && gcref(tab[h].obj) == o) {
      *found = 1;
      return &tab[h];
    }
    h = (h + 1) & mask;
  }
  return NULL;
}

static FinEntry *fin_tab_insert_slot(global_State *g, GCobj *o)
{
  FinEntry *tab = mref(g->gc.fin_tab, FinEntry);
  MSize mask = g->gc.fin_mask;
  MSize h = fin_hash(o, mask);
  MSize n;
  FinEntry *tomb = NULL;
  lj_assertG(tab != NULL && mask != 0, "fin_tab_insert_slot: empty table");
  for (n = 0; n <= mask; n++) {
    uint8_t k = tab[h].kind;
    if (k == FIN_KIND_EMPTY)
      return tomb ? tomb : &tab[h];
    if (k == FIN_KIND_TOMB) {
      if (tomb == NULL) tomb = &tab[h];
    } else if (gcref(tab[h].obj) == o) {
      return &tab[h];
    }
    h = (h + 1) & mask;
  }
  return tomb;
}

void lj_gc_fin_register(lua_State *L, GCobj *o, int kind, GCobj *fin, uint32_t it)
{
  global_State *g = G(L);
  FinEntry *e;
  int found;
  lj_assertG(kind == FIN_KIND_UDATA || kind == FIN_KIND_CDATA,
	     "fin_register bad kind %d", kind);
  if (g->gc.fin_mask == 0 ||
      (g->gc.fin_num + g->gc.fin_tomb) * 3 > (g->gc.fin_mask + 1) * 2)
    fin_tab_rehash(g, g->gc.fin_mask ? (g->gc.fin_mask << 1) + 1 : FIN_TAB_INIT - 1);
  e = fin_tab_find(g, o, &found);
  if (!found) {
    e = fin_tab_insert_slot(g, o);
    lj_assertG(e != NULL, "fin_register: no free slot after rehash");
    if (e->kind == FIN_KIND_TOMB)
      g->gc.fin_tomb--;
    e->seq = ++g->gc.fin_seq;  /* First registration stamps FIFO order. */
    g->gc.fin_num++;
  }
  setgcref(e->obj, o);
  e->kind = (uint8_t)kind;
  if (kind == FIN_KIND_CDATA && fin != NULL) {
    setgcref(e->fin, fin);
    e->fin_it = it;
    lj_gc_objbarrier(L, o, fin);
  } else {
    setgcrefnull(e->fin);
    e->fin_it = 0;
  }
}

void lj_gc_fin_unregister(global_State *g, GCobj *o)
{
  int found;
  FinEntry *e = fin_tab_find(g, o, &found);
  if (!found || e == NULL) return;
  setgcrefnull(e->obj);
  setgcrefnull(e->fin);
  e->kind = FIN_KIND_TOMB;
  e->fin_it = 0;
  e->seq = 0;
  g->gc.fin_num--;
  g->gc.fin_tomb++;
  /* Do not free the table here: separateudata may unregister mid-scan. */
}

int lj_gc_fin_has(global_State *g, GCobj *o)
{
  int found;
  fin_tab_find(g, o, &found);
  return found;
}

void lj_gc_fin_update_udata(lua_State *L, GCudata *ud)
{
  /* Registration policy: LUAJIT_ENABLE_FIN_UDATA_COMPAT (lj_arch.h).
  ** 0 (default): 5.2+ — register only if mt currently has __gc.
  ** 1 (LJ_DS forces): 5.1 compat — register if mt ever had __gc (sticky
  **    TF_HASGC bit) OR currently has __gc; late-bound mt.__gc works via
  **    the atomic backfill walk (lj_gc_fin_backfill_udata).
  ** sepudata_one always re-checks __gc at death (nil → drop, no call). */
  global_State *g = G(L);
  GCtab *mt = tabref(ud->metatable);
  GCobj *o = obj2gco(ud);
  if (mt == NULL) {
    lj_gc_fin_unregister(g, o);
    return;
  }
  {
#if LUAJIT_ENABLE_FIN_UDATA_COMPAT
    int has_gc = (mt->marked & LJ_GC_HASGC) ||
		 lj_meta_fastg(g, mt, MM_gc) != NULL;
#else
    int has_gc = lj_meta_fastg(g, mt, MM_gc) != NULL;
#endif
    if (has_gc)
      lj_gc_fin_register(L, o, FIN_KIND_UDATA, NULL, 0);
    else
      lj_gc_fin_unregister(g, o);
  }
}

/* COMPAT backfill (LJ 3.0 Finalizers.zh.md §6.3(C)). Called once at atomic,
** BEFORE lj_gc_separateudata, when a table gained __gc (TF_HASGC) since the
** last atomic. Some udata may already carry that mt without being registered
** (setmetatable happened before __gc was inserted). Walk ONLY UdataOnly
** arenas + the huge set and register the missing udata. A dead-but-unswept
** udata found here is registered then immediately judged dead by separate →
** resurrected and finalized (the correct "same-cycle supplement" behavior).
**
** Routing invariant: udata live in UdataOnly arenas or in the huge set. If a
** udata is found elsewhere it is a routing bug; the per-cell gct assert
** inside the UdataOnly walk catches a mis-routed non-udata cell, and the
** huge walk skips non-udata. We do NOT walk other TravObjs arenas here. */
void lj_gc_fin_backfill_udata(global_State *g)
{
#if LUAJIT_ENABLE_FIN_UDATA_COMPAT
  MSize i;
  GCArena **arenas;
  GCRef *slots;
  MSize hmask;
  if (!g->gc.fin_backfill) return;
  g->gc.fin_backfill = 0;
  arenas = mref(g->gc.arenas, GCArena *);
  for (i = 0; i < g->gc.arenastop; i++) {
    GCArena *a = arenas[i];
    uint32_t w, wtop;
    if (!(a->flags & ArenaFlag_UdataOnly)) continue;
    lj_arena_flushbins(a);
    wtop = arena_blockidx((GCCellID)a->celltop - 1);
    for (w = UnusedBlockWords; w <= wtop; w++) {
      GCBlockword alive = a->block[w];
      while (alive) {
	uint32_t bitidx = lj_ffs(alive);
	GCCellID c = (w << 5) + bitidx;
	GCobj *o;
	GCudata *ud;
	GCtab *mt;
	alive &= alive - 1;
	o = (GCobj *)arena_cellptr(a, c);
	/* Routing invariant: UdataOnly arena holds only userdata. A non-udata
	 * gct here means a cell was mis-routed into the wrong arena class. */
	lj_assertG(o->gch.gct == ~LJ_TUDATA,
		   "fin backfill: non-udata in UdataOnly arena: gct=0x%02x",
		   o->gch.gct);
	if (arena_cellstate(a, c) < CellState_White) continue;
	if (lj_gc_fin_has(g, o)) continue;
	ud = gco2ud(o);
	/* Skip finalized udata: its finalizer already ran (it was resurrected
	 * by an earlier cycle's separate, makewhite by gc_finalize, and
	 * survived the intervening sweep). Re-registering would create an
	 * orphan entry that separate cannot process (sepudata_one skips
	 * finalized objects); sweep frees it directly. */
	if (isfinalized(ud)) continue;
	mt = tabref(ud->metatable);
	if (mt == NULL) continue;
	if ((mt->marked & LJ_GC_HASGC) ||
	    lj_meta_fastg(g, mt, MM_gc) != NULL)
	  lj_gc_fin_register(mainthread(g), o, FIN_KIND_UDATA, NULL, 0);
      }
    }
  }
  /* Huge udata: no cell bitmap; walk the address-keyed huge set. */
  slots = mref(g->gc.hugeset, GCRef);
  if (slots == NULL) return;
  hmask = g->gc.hugesetmask;
  for (i = 0; i <= hmask; i++) {
    uintptr_t u = gcrefu(slots[i]);
    GCobj *o;
    GCudata *ud;
    GCtab *mt;
    if (!hugeset_slot_live(u)) continue;
    o = hugeset_slot_obj(u);
    if (o->gch.gct != ~LJ_TUDATA) continue;
    if (lj_gc_fin_has(g, o)) continue;
    ud = gco2ud(o);
    if (isfinalized(ud)) continue;  /* see arena-loop comment */
    mt = tabref(ud->metatable);
    if (mt == NULL) continue;
    if ((mt->marked & LJ_GC_HASGC) ||
	lj_meta_fastg(g, mt, MM_gc) != NULL)
      lj_gc_fin_register(mainthread(g), o, FIN_KIND_UDATA, NULL, 0);
  }
#else
  UNUSED(g);
#endif
}

void lj_gc_fin_free(global_State *g)
{
  if (g->gc.fin_mask != 0) {
    g->allocf(g->allocd, mref(g->gc.fin_tab, FinEntry),
	      (g->gc.fin_mask + 1) * sizeof(FinEntry), 0);
    setmref(g->gc.fin_tab, NULL);
    g->gc.fin_mask = 0;
  }
  if (g->gc.fin_qmask != 0) {
    g->allocf(g->allocd, mref(g->gc.fin_queue, FinQueueEntry),
	      (g->gc.fin_qmask + 1) * sizeof(FinQueueEntry), 0);
    setmref(g->gc.fin_queue, NULL);
    g->gc.fin_qmask = 0;
  }
  g->gc.fin_num = 0;
  g->gc.fin_tomb = 0;
  g->gc.fin_seq = 0;
  g->gc.fin_qhead = 0;
  g->gc.fin_qtail = 0;
}

/* -- fin_queue: pending-finalizer FIFO work queue ------------------------- */
/* Replaces the circular mmudata ring under LJ_HASGCMARK (design Finalizers
** §3/§10). Power-of-two ring: O(1) push/pop with no nextgc re-link. Lazily
** allocated on first push; freed above in lj_gc_fin_free. head==tail ⇒ empty;
** (tail-head)==mask+1 ⇒ full (grow). head/tail wrap mod 2^N; & mask keeps
** indexing correct across wraparound. Drain pops head-first, so finalize call
** order == separateudata enqueue order == registration FIFO.
**
** Drain order == separateudata enqueue order == registration FIFO. */

#define FIN_QUEUE_INIT	16	/* power of two; first allocation size. */

static void fin_queue_grow(global_State *g)
{
  MSize oldmask = g->gc.fin_qmask;
  MSize newmask = oldmask ? (oldmask << 1) | 1 : (MSize)(FIN_QUEUE_INIT - 1);
  MSize newsz = newmask + 1;
  FinQueueEntry *old = mref(g->gc.fin_queue, FinQueueEntry);
  FinQueueEntry *nq = (FinQueueEntry *)g->allocf(g->allocd, NULL, 0,
						 newsz * sizeof(FinQueueEntry));
  MSize count, i;
  if (LJ_UNLIKELY(nq == NULL))
    lj_err_mem(mainthread(g));
  count = g->gc.fin_qtail - g->gc.fin_qhead;
  /* Linearize live entries out of the (possibly wrapped) old ring into the
  ** dense base of the new buffer; old may be NULL on the very first alloc. */
  for (i = 0; i < count; i++)
    nq[i] = old[(g->gc.fin_qhead + i) & oldmask];
  if (old != NULL)
    g->allocf(g->allocd, old, (oldmask + 1) * sizeof(FinQueueEntry), 0);
  setmref(g->gc.fin_queue, nq);
  g->gc.fin_qmask = newmask;
  g->gc.fin_qhead = 0;
  g->gc.fin_qtail = count;
}

void lj_gc_fin_queue_push(global_State *g, GCobj *o, int kind,
			  GCobj *fin, uint32_t fin_it)
{
  FinQueueEntry *q = mref(g->gc.fin_queue, FinQueueEntry);
  MSize mask = g->gc.fin_qmask;
  FinQueueEntry *e;
  lj_assertG(kind == FIN_KIND_UDATA || kind == FIN_KIND_CDATA,
	     "fin_queue_push bad kind %d", kind);
  if (q == NULL || (g->gc.fin_qtail - g->gc.fin_qhead) >= mask + 1) {
    fin_queue_grow(g);
    q = mref(g->gc.fin_queue, FinQueueEntry);
    mask = g->gc.fin_qmask;
  }
  e = &q[g->gc.fin_qtail & mask];
  setgcref(e->obj, o);
  if (kind == FIN_KIND_CDATA && fin != NULL) {
    setgcref(e->fin, fin);
    e->fin_it = fin_it;
  } else {
    setgcrefnull(e->fin);
    e->fin_it = 0;
  }
  e->kind = (uint8_t)kind;
  e->pad[0] = e->pad[1] = e->pad[2] = 0;
  g->gc.fin_qtail++;
}

int lj_gc_fin_queue_pop(global_State *g, FinQueueEntry *out)
{
  FinQueueEntry *q = mref(g->gc.fin_queue, FinQueueEntry);
  MSize mask = g->gc.fin_qmask;
  MSize head = g->gc.fin_qhead;
  if (q == NULL || head == g->gc.fin_qtail) return 0;
  *out = q[head & mask];
  g->gc.fin_qhead = head + 1;
  return 1;
}

int lj_gc_fin_queue_empty(global_State *g)
{
  return g->gc.fin_qhead == g->gc.fin_qtail;
}

/* F3 complete: after separate, dead finalizables leave the registry and sit
** on fin_queue with FINALIZED. Registry survivors must not be white+needs-fin
** (they would have been enqueued). Queue members must carry FINALIZED.
** CDATA_FIN cache sync on remaining registry cdata entries still holds. */
void lj_gc_fin_dual_assert_udata(global_State *g)
{
#if defined(LUA_USE_ASSERT)
  FinEntry *tab = mref(g->gc.fin_tab, FinEntry);
  FinQueueEntry *q = mref(g->gc.fin_queue, FinQueueEntry);
  MSize i, mask = g->gc.fin_mask;
  MSize qmask = g->gc.fin_qmask, head = g->gc.fin_qhead, tail = g->gc.fin_qtail;
  /* Queue: every pending entry is FINALIZED. */
  if (q != NULL) {
    for (i = head; i != tail; i++) {
      GCobj *o = gcref(q[i & qmask].obj);
      lj_assertG(o != NULL && (o->gch.marked & LJ_GC_FINALIZED),
		 "fin F3: fin_queue entry not FINALIZED: ptr=%p", (void *)o);
      lj_assertG(fin_on_queue(g, o), "fin F3: queue walk inconsistency");
    }
  }
  if (tab == NULL || mask == 0) return;
  for (i = 0; i <= mask; i++) {
    GCobj *o;
    if (tab[i].kind != FIN_KIND_UDATA && tab[i].kind != FIN_KIND_CDATA)
      continue;
    o = gcref(tab[i].obj);
    lj_assertG(o != NULL, "fin registry empty obj");
    if (tab[i].kind == FIN_KIND_UDATA) {
      cTValue *mo = lj_meta_fastg(g, tabref(gco2ud(o)->metatable), MM_gc);
      /* White + __gc must already have been enqueued+unregistered. */
      lj_assertG(!(gc_obj_iswhite(g, o) && mo != NULL),
		 "fin F3: white __gc udata still on registry: ptr=%p", (void *)o);
    }
#if LJ_HASFFI
    else {
      lj_assertG((o->gch.marked & LJ_GC_CDATA_FIN),
		 "fin F3: cdata cache sync — registry entry missing CDATA_FIN bit: ptr=%p marked=0x%02x",
		 (void *)o, o->gch.marked);
      lj_assertG(!gc_obj_iswhite(g, o),
		 "fin F3: white registry cdata still on registry: ptr=%p", (void *)o);
    }
#endif
  }
#else
  UNUSED(g);
#endif
}

typedef struct FinSepCand {
  GCobj *o;
  uint32_t seq;
  uint8_t kind;
} FinSepCand;

static int fin_sep_cand_cmp(const void *a, const void *b)
{
  uint32_t sa = ((const FinSepCand *)a)->seq;
  uint32_t sb = ((const FinSepCand *)b)->seq;
  return (sa > sb) - (sa < sb);
}

/* Registry scan → fin_queue. Dead candidates sorted by FinEntry.seq (FIFO).
** all=1 includes black objects (lua_close). */
size_t lj_gc_separateudata(global_State *g, int all)
{
  size_t m = 0;
  FinEntry *tab = mref(g->gc.fin_tab, FinEntry);
  MSize mask = g->gc.fin_mask, i, ncap, ndead = 0;
  FinSepCand *cands;
  if (g->gc.fin_num == 0 || tab == NULL || mask == 0) return 0;
  ncap = g->gc.fin_num;
  cands = (FinSepCand *)g->allocf(g->allocd, NULL, 0, ncap * sizeof(FinSepCand));
  if (LJ_UNLIKELY(cands == NULL))
    lj_err_mem(mainthread(g));
  for (i = 0; i <= mask; i++) {
    FinEntry *e = &tab[i];
    GCobj *o;
    if (e->kind != FIN_KIND_UDATA && e->kind != FIN_KIND_CDATA)
      continue;
    o = gcref(e->obj);
    if (o == NULL) continue;
    if (e->kind == FIN_KIND_UDATA) {
      if (!(gc_obj_iswhite(g, o) || all) || isfinalized(gco2ud(o)))
	continue;
    } else {
      if (!(gc_obj_iswhite(g, o) || all) || (o->gch.marked & LJ_GC_FINALIZED))
	continue;
    }
    lj_assertG(ndead < ncap, "fin separate cand overflow");
    cands[ndead].o = o;
    cands[ndead].seq = e->seq;
    cands[ndead].kind = e->kind;
    ndead++;
  }
  if (ndead > 1)
    qsort(cands, (size_t)ndead, sizeof(FinSepCand), fin_sep_cand_cmp);
  for (i = 0; i < ndead; i++) {
    GCobj *o = cands[i].o;
    if (cands[i].kind == FIN_KIND_UDATA) {
      lj_assertG(o->gch.gct == ~LJ_TUDATA, "fin reg udata kind mismatch");
      m += sepudata_one(g, o, all);
    }
#if LJ_HASFFI
    else if (cands[i].kind == FIN_KIND_CDATA) {
      lj_assertG(o->gch.gct == ~LJ_TCDATA, "fin reg cdata kind mismatch");
      m += sepcdata_one(g, o, all);
    }
#endif
  }
  g->allocf(g->allocd, cands, ncap * sizeof(FinSepCand), 0);
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
      /* No FFI_FIN special case here (classic lj_gc.c has one): Arena F3
      ** keeps cdata finalizers in fin_registry, not a weak FFI_FIN table. */
      t->marked = (uint8_t)((t->marked & ~LJ_GC_WEAK) | weak);
      gc_weak_push(g, obj2gco(t), weak);
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

/* Traverse a thread object.
** Mark only [stack+1+FR2, th->top) — same as classic lj_gc.c. Do NOT expand
** mark range to frame framesize high-water: that reserves the whole frame for
** shrinkstack sizing, but slots past th->top may hold stale pointers to
** already-dropped locals (dead coroutines, closed-UV factories). Marking them
** keeps one residual object alive per full GC (openuv g5/r3).
** JIT/C paths that leave top low already call lj_gc_step_fixtop /
** lj_gc_step_jit which set L->top = curr_topL(L) before stepping. */
static void gc_traverse_thread(global_State *g, lua_State *th)
{
  TValue *o, *top = th->top;
  for (o = tvref(th->stack)+1+LJ_FR2; o < top; o++)
    gc_marktv(g, o);
  /* Clear unmarked slots only at atomic — same as classic lj_gc.c. */
  if (g->gc.state == GCSatomic) {
    top = tvref(th->stack) + th->stacksize;
    for (; o < top; o++)
      setnilV(o);
  }
  gc_markobj(g, tabref(th->env));
  lj_state_shrinkstack(th, gc_traverse_frames(g, th));
  /* T1: after shrinkstack, mark every open UV mark∧GRAY (no atomic fullsweep). */
  {
    GCRef *vec = mref(th->openuv, GCRef);
    MSize i, n = th->openuvtop;
    for (i = 0; i < n; i++)
      gc_mark(g, gcref(vec[i]));
  }
}

/* Traverse the host-allocated main thread directly: it is SFIXED and cannot
** live in an arena or on a per-arena gray stack. */
static void gc_traverse_mainthread(global_State *g)
{
  gc_traverse_thread(g, mainthread(g));
}

/* Shared type-specific propagate helpers. The caller has already verified
** the object is gray and applied gray2black; these own only the
** type-specific traverse and the weak/THREAD keep-gray semantics, so the
** specialized arena drains (gc_propagate_arena_pod) and propagatemark stay
** in sync without copy-paste drift. Cost is returned for mark_cost
** accounting by the caller. */

/* TAB: traverse, then keep gray only when header WEAK bits are set. Do NOT
** use the traverse return value alone: FFI_FIN sets a synthetic non-zero
** weak without LJ_GC_WEAK, which would leave non-weak mark∧GRAY residuals. */
static LJ_AINLINE size_t gc_prop_tab(global_State *g, GCobj *o)
{
  GCtab *t = gco2tab(o);
  gc_traverse_tab(g, t);
  if (t->marked & LJ_GC_WEAK)
    black2gray(o);  /* Keep weak tables gray until clearweak. */
  return sizeof(GCtab) + sizeof(TValue) * t->asize +
         (t->hmask ? sizeof(Node) * (t->hmask + 1) : 0);
}

static LJ_AINLINE size_t gc_prop_func(global_State *g, GCobj *o)
{
  GCfunc *fn = gco2func(o);
  gc_traverse_func(g, fn);
  return isluafunc(fn) ? sizeLfunc((MSize)fn->l.nupvalues) :
                         sizeCfunc((MSize)fn->c.nupvalues);
}

static LJ_AINLINE size_t gc_prop_proto(global_State *g, GCobj *o)
{
  GCproto *pt = gco2pt(o);
  gc_traverse_proto(g, pt);
  return pt->sizept;
}

/* THREAD: permanent-gray. The caller's gray2black cleared GRAY; restore it so
** the thread ends mark∧GRAY for the whole cycle — stack slots cannot pay
** write barriers, so a thread is never pure black. graythread is the
** enumeration set for atomic stack rescan, NOT a "pure black after first
** visit" scheme.
**
** Re-visit safety: gc_mark only fires when gc_obj_iswhite is true (mark
** bitmap clear), so a marked thread is never re-enqueued to arena gray/SSB
** by barriers (none exist for stacks). If one somehow appears on a gray
** stack twice, prop re-traverses it (idempotent) and graythread gains a
** duplicate. Rescan (gc_atomic_rescan_threads) and fullsweep (graythread
** walk) tolerate duplicates; the bound is N_threads not N_visits. Push-once
** membership is not cheap on the ptrstack, so duplicates are accepted rather
** than adding a per-thread dedup structure. */
static LJ_AINLINE size_t gc_prop_thread(global_State *g, GCobj *o)
{
  lua_State *th = gco2th(o);
  black2gray(o);
  gc_graythread_push(g, o);
  gc_traverse_thread(g, th);
  return sizeof(lua_State) + sizeof(TValue) * th->stacksize;
}

#if LJ_HASJIT
static LJ_AINLINE size_t gc_prop_trace(global_State *g, GCobj *o)
{
  GCtrace *T = gco2trace(o);
  gc_traverse_trace(g, T);
  return ((sizeof(GCtrace)+7)&~7) + (T->nins-T->nk)*sizeof(IRIns) +
         T->nsnap*sizeof(SnapShot) + T->nsnapmap*sizeof(SnapEntry);
}
#endif

/* Batch gray pop (P1-3a). Pop up to GC_MARK_BATCH_N entries from the arena's
** gray stack into a caller-provided local array, returning the count popped.
**
** HARD RULE (plan §5): every selected GCobj* is resolved into the local
** array BEFORE greytop is lowered. arena_cellptr is independent of greytop,
** so resolving here is safe. Once greytop is lowered, NEVER re-read batch
** members from gray-stack storage — a same-arena re-push during subsequent
** traversal may overwrite the now-freed stack slots, yielding a stale/aliased
** cellid. The local array is the only authoritative copy for the batch.
**
** Stale ArenaFlag_InGrayHeap after a same-arena re-push (the arena was
** already taken off grayastack; the re-push re-inserts InGrayHeap / a heap
** entry that later looks empty) is EXPECTED. gc_grayarena_pop clears the
** flag and skips stale/empty roots (see ~line 1271); do NOT add ad-hoc
** InGrayHeap clearing inside the drain to "fix" this.
**
** No prefetch here (T5); no sort by cellid. */
static LJ_AINLINE MSize gc_gray_batch_pop(GCArena *a, GCobj **objs)
{
  GCCellID1 *top = mref(a->greytop, GCCellID1);
  GCCellID1 *base = mref(a->greybase, GCCellID1);
  MSize depth = (MSize)(top - base);
  if (depth == 0) return 0;
  if (depth > GC_MARK_BATCH_N) depth = GC_MARK_BATCH_N;
  /* LIFO: objs[0] = most-recently pushed (top-1), matching single-pop order
  ** within the batch. Batch-internal order need not equal pure single-pop
  ** LIFO once re-push lands on the live stack for the next iteration. */
  for (MSize i = 0; i < depth; i++)
    objs[i] = (GCobj *)arena_cellptr(a, top[-1 - (ptrdiff_t)i]);
  setmref(a->greytop, top - depth);
  /* T5 worklist-MLP header prefetch (OFF by default; compile with
  ** -DLUAJIT_GC_MARK_HEADER_PREFETCH to enable). This prefetches the
  ** GCobj *header* of each batch work item — the object the gray stack
  ** entry points at — so the header (gch.marked / gch.gct) is in cache by
  ** the time the drain loop reads it for the GRAY check and dispatch.
  **
  ** This is NOT the rejected edge prefetch from
  ** doc/gc-mark-sweep-adaptation-plan.md (~L299, L352) and
  ** doc/arenagc-p1-mark-monomorph.md §5.1. That rejected scheme inserted
  ** __builtin_prefetch(o) on *child edges* inside gc_traverse_tab /
  ** gc_traverse_func etc. and measured no gain (child tables are
  ** sequentially allocated → covered by the hardware prefetcher; deep
  ** chains are serial pointer-chasing → prefetch can't overlap). T5 is
  ** *worklist MLP*: the gray stack is contiguous GCCellID1 storage,
  ** batch pop is a sequential read, and the resolved GCobj* headers are
  ** independent work items the drain is about to visit. Prefetching them
  ** overlaps the header read with the greytop update and loop overhead.
  ** Never insert __builtin_prefetch into gc_traverse_* (see §5.1). */
#ifdef LUAJIT_GC_MARK_HEADER_PREFETCH
  for (MSize i = 0; i < depth; i++)
    __builtin_prefetch(objs[i], 0, 3);  /* read, high locality */
#endif
  return depth;
}

/* Propagate one gray object. Traverse it and turn it black. */
static size_t propagatemark(global_State *g, GCobj *o)
{
  int gct = o->gch.gct;
  gcstat_inc(g, mark_calls);
  /* Duplicate worklist entries are possible: barrierback may re-enqueue a
  ** table already on a gray stack / SSB. First visit clears GRAY; later
  ** visits are no-ops. */
  if (LJ_UNLIKELY(!(o->gch.marked & LJ_GC_GRAY)))
    return 0;
  lj_assertG(isgray(o), "propagation of non-gray object");
  /* Header bit 0x04 is a free slot under bitmap GC (no writer remains),
  ** so there is nothing to assert here. */
  gray2black(o);
  if (LJ_LIKELY(gct == ~LJ_TTAB))
    return gc_prop_tab(g, o);
  if (LJ_LIKELY(gct == ~LJ_TFUNC))
    return gc_prop_func(g, o);
  if (LJ_LIKELY(gct == ~LJ_TPROTO))
    return gc_prop_proto(g, o);
  if (LJ_LIKELY(gct == ~LJ_TTHREAD))
    return gc_prop_thread(g, o);
#if LJ_HASJIT
  return gc_prop_trace(g, o);
#else
  lj_assertG(0, "bad GC type %d", gct);
  return 0;
#endif
}

/* Drain the gray stack of a PODOnly arena. Allocation routing
** (lj_mem_newgcot_pod -> ArenaClass_POD, T2 audit Finding 1) restricts POD
** arena contents to FUNC and PROTO, so the gray stack only ever holds those
** two gct values. Dispatch directly to gc_prop_func / gc_prop_proto to skip
** the propagatemark gct switch on the hot path.
**
** An unexpected gct is an allocator-class invariant violation: fall back to
** the full propagatemark, which re-checks GRAY (passes), re-applies
** gray2black, accounts mark_calls at entry, and dispatches TAB (weak
** keep-gray), THREAD (permanent-gray), TRACE via the shared helpers. Assert
** in LUA_USE_ASSERT builds so the misclassification is caught.
**
** mark_calls accounting: the fast path inlines gcstat_inc(mark_calls) to
** match propagatemark's per-object accounting; the fallback lets
** propagatemark own the inc.
**
** Batch gray pop (T4): pop up to GC_MARK_BATCH_N entries per iteration into
** a local array (gc_gray_batch_pop enforces the local-copy hard rule). The
** outer loop drains until empty including same-arena re-pushes from
** traversal. Same gray-check / cost-accounting shape as the mixed drain. */
static size_t gc_propagate_arena_pod(global_State *g, GCArena *a)
{
  size_t m = 0;
  for (;;) {
    GCobj *objs[GC_MARK_BATCH_N];
    MSize n = gc_gray_batch_pop(a, objs);
    if (n == 0) break;
    for (MSize i = 0; i < n; i++) {
      GCobj *o = objs[i];
      /* Non-gray: duplicate worklist entry or already processed. */
      if (LJ_UNLIKELY(!(o->gch.marked & LJ_GC_GRAY)))
	continue;
      int gct = o->gch.gct;
      size_t c;
      if (LJ_LIKELY(gct == ~LJ_TFUNC)) {
	gray2black(o);
	c = gc_prop_func(g, o);
	gcstat_inc(g, mark_calls);
      } else if (LJ_LIKELY(gct == ~LJ_TPROTO)) {
	gray2black(o);
	c = gc_prop_proto(g, o);
	gcstat_inc(g, mark_calls);
      } else {
	/* PODOnly gray stack must only hold FUNC/PROTO. Route the unexpected
	** through propagatemark (TAB weak keep-gray, THREAD permanent-gray,
	** TRACE) and assert so the misclassification is caught in debug. */
	c = propagatemark(g, o);
	lj_assertG(gct == ~LJ_TTAB || gct == ~LJ_TTHREAD ||
	           (LJ_HASJIT && gct == ~LJ_TTRACE),
	           "PODOnly arena gray stack held unexpected gct=%d", gct);
      }
      gcstat_add(g, mark_cost, c);
      m += c;
    }
  }
  return m;
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

/* Take the arena with the largest gray stack from the heap.
** Removes it immediately so priorities stay consistent while the caller
** drains the whole stack (GCSpropagate step = one arena). Same-arena
** re-push during the drain re-inserts via lj_gc_grayarena_notify. */
static GCArena *gc_grayarena_pop(global_State *g)
{
  GCArena **arenas = mref(g->gc.arenas, GCArena *);
  MSize *heap = mref(g->gc.grayastack, MSize);
  while (g->gc.grayastop > 0) {
    MSize idx = heap[0];
    GCArena *a = arenas[idx];
    /* Always detach root from the heap (work or stale). */
    a->flags &= (uint16_t)~ArenaFlag_InGrayHeap;
    g->gc.grayastop--;
    if (g->gc.grayastop > 0) {
      heap[0] = heap[g->gc.grayastop];
      grayheap_siftdown(g, heap, g->gc.grayastop, 0);
    }
    if ((a->flags & ArenaFlag_TravObjs) && !arena_gray_empty(a)) {
      gcstat_inc(g, grayarena_pops);
      return a;
    }
    /* Stale empty entry — try next root. */
  }
  return NULL;
}

/* Drain every gray cell currently on this arena's gray stack.
** Same-arena children pushed during the drain are included (locality).
** Cross-arena / huge work is left for later GCSpropagate steps.
** One GCSpropagate step = one arena (not one object) so large heaps finish
** mark before mutator allocation doubles the live set again.
**
** PODOnly arenas hold only FUNC/PROTO (T2 audit Finding 1); route them to
** the specialized drain to skip the propagatemark gct switch. Mixed Trav
** arenas use the general loop below. Both paths use batch gray pop (T4):
** gc_gray_batch_pop copies up to GC_MARK_BATCH_N entries into a local array
** before lowering greytop, so same-arena re-push during traversal cannot
** alias a batch member. The outer loop drains until empty (including
** re-pushes). TAB fast-path is T6; prefetch is T5 (off by default). */
static size_t gc_propagate_arena(global_State *g, GCArena *a)
{
  if (a->flags & ArenaFlag_PODOnly)
    return gc_propagate_arena_pod(g, a);
  {
    size_t m = 0;
    for (;;) {
      GCobj *objs[GC_MARK_BATCH_N];
      MSize n = gc_gray_batch_pop(a, objs);
      if (n == 0) break;
      for (MSize i = 0; i < n; i++) {
	GCobj *o = objs[i];
	/* Non-gray: duplicate worklist entry or already processed. */
	if (LJ_UNLIKELY(!(o->gch.marked & LJ_GC_GRAY)))
	  continue;
	int gct = o->gch.gct;
	size_t c;
	/* TAB fast-path (T6): tables are the dominant non-POD gray work in
	** mixed Trav arenas. Dispatch directly to gc_prop_tab (shared with
	** propagatemark) to skip the gct switch and gray2black re-check.
	** gc_prop_tab owns the weak keep-gray restore (header WEAK bits
	** only); non-TAB types fall through to propagatemark, which covers
	** THREAD permanent-gray, FUNC, PROTO, and TRACE. mark_calls is
	** inlined on the fast path to match propagatemark's accounting; the
	** fallback lets propagatemark own the inc. */
	if (LJ_LIKELY(gct == ~LJ_TTAB)) {
	  gray2black(o);
	  c = gc_prop_tab(g, o);
	  gcstat_inc(g, mark_calls);
	} else {
	  c = propagatemark(g, o);
	}
	gcstat_add(g, mark_cost, c);
	m += c;
      }
    }
    return m;
  }
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
  gcstat_inc(g, hugegray_pops);
  return mref(g->gc.hugegray, GCobj *)[--g->gc.hugegraytop];
}
static LJ_AINLINE void gc_hugegray_reset(global_State *g)
{
  g->gc.hugegraytop = 0;
}

static LJ_AINLINE void gc_graythread_push(global_State *g, GCobj *o)
{
  gc_ptrstack_push(g, &g->gc.graythread, &g->gc.graythreadtop,
		   &g->gc.graythreadsz, o);
}
static LJ_AINLINE int gc_graythread_empty(global_State *g)
{
  return g->gc.graythreadtop == 0;
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
  lj_gc_fin_free(g);
}

/* Propagate all gray objects.
** SSB is part of the gray work: barrierback paints GRAY into SSB first, and
** only ssb_flush moves them onto per-arena greystacks. A drain that ignores
** SSB leaves mark∧GRAY parents off every worklist (free-window design break;
** Debug: gc_assert_atomic_end). Fixed-point: flush SSB, drain huge+arenas,
** repeat until SSB and all gray stacks are empty. */
static size_t gc_propagate_gray(global_State *g)
{
  size_t m = 0;
  for (;;) {
    GCobj **ssb, **ssbtop;
    GCArena *a;
    lj_gc_ssb_flush(g);
    /* Drain huge gray objects (non-arena worklist). */
    while (!gc_hugegray_empty(g)) {
      size_t c = propagatemark(g, gc_hugegray_pop(g));
      gcstat_add(g, mark_cost, c);
      m += c;
    }
    /* Drain all arena gray stacks (one take = whole stack). */
    while ((a = gc_grayarena_pop(g)) != NULL) {
      m += gc_propagate_arena(g, a);
      /* Same-arena re-push during drain re-inserts; finish if still gray. */
      while (!arena_gray_empty(a))
	m += gc_propagate_arena(g, a);
      /* Also drain any huge objects pushed during arena traversal. */
      while (!gc_hugegray_empty(g)) {
	size_t c = propagatemark(g, gc_hugegray_pop(g));
	gcstat_add(g, mark_cost, c);
	m += c;
      }
    }
    /* Barriers during the drain may have refilled SSB; loop until quiet. */
    ssb = mref(g->gc.ssb, GCobj *);
    ssbtop = mref(g->gc.ssbtop, GCobj *);
    if (ssb != NULL && ssbtop != NULL && ssbtop > ssb)
      continue;
    if (!gc_hugegray_empty(g) || g->gc.grayastop != 0)
      continue;
    break;
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

/* T2: linked-list gc_sweep/gc_fullsweep removed under GCMARK.
** Open UV free is Path F (lj_func_freeall_openuv) or Path L (closeuv). */


/* Open-addressing string sweep: free every interned string. Called only from
** the shutdown/freeall path (P2 folds incremental string reclaim into the
** GCSsweep bitmap pass; the GCSsweepstring case is a defensive no-op under
** the flag). No table maintenance here -- the table is being torn down, so
** backward-shift deletion is unnecessary; just free each non-empty slot. */
static MSize gc_sweepstr_oa(global_State *g, MSize start, MSize count)
{
  MSize mask = g->str.mask;
  MSize i;
  gcstat_inc(g, strings_chains_swept);
  for (i = 0; i < count && start <= mask; i++, start++) {
    uintptr_t v = gcrefu(g->str.tab[start]);
    GCobj *o;
    if (v == 0 || v == STRTAB_OA_TOMB) continue;
    o = (GCobj *)(void *)v;
    /* Shutdown skip: keep only super-fixed roots (strempty + mainthread are
    ** SFIXED). FIXED-but-not-SFIXED strings (reserved words, fixed error
    ** messages) MUST be freed here so g->str.num is driven to 0 -- matching
    ** classic chain shutdown which keeps only
    ** SFIXED. Skipping on LJ_GC_FIXED instead leaves those strings alive and
    ** trips close_state's `leaked N strings` assert. strempty is FIXED|SFIXED
    ** so the explicit pointer check is redundant but kept defensively. */
    if (o == obj2gco(&g->strempty) || (o->gch.marked & LJ_GC_SFIXED))
      continue;
    gcstat_inc(g, strings_dead_freed);
    lj_str_free(g, gco2str(o));
  }
  return i;
}

/*
** Bitmap-driven sweep: scan arena mark bitmaps to locate dead objects.
** Replaces the linked-list gc_sweep for traversable arena objects.
** Strings: openaddr NonTrav bitmap free + lj_strtab_remove (no chain sweep).
*/

/* Sweep phase constants. */
enum {
  SweepPhase_Bitmap,	/* Scanning arena bitmaps, freeing dead objects. */
  SweepPhase_Huge,	/* Post-bitmap: hugeset free+demote (+ optional assert). */
  SweepPhase_Done	/* Sweep complete; cycle may end / finalize. */
};

/* Sub-phases within SweepPhase_Huge (g->gc.sweep_hugep).
** Scan is one-shot; Assert is chunked under LUA_USE_ASSERT (release skips it). */
enum {
  SweepHuge_Scan,	/* Free dead hugeset slots + demote survivor MARK. */
  SweepHuge_Assert,	/* Chunked arena demote check (debug only). */
  SweepHuge_Done
};

#include "lj_gc_markalloc_debug.h"

/* D3: demote survivors at arena free completion. Clear mark bits on all
** allocated cells (block=1): Black(1,1)→White(1,0). Called AFTER
** swept_gen=epoch so survivors are is_curwhite (current ∧ !mark), not isdead.
** Dead cells were already freed (block cleared by gc_freefunc); their marks
** are untouched (Free/Extent encoding is stable). */
static LJ_AINLINE void gc_arena_demote_survivors(GCArena *a)
{
  if ((GCCellID)a->celltop > MinCellId) {
    uint32_t w, wtop = arena_blockidx((GCCellID)a->celltop - 1);
    for (w = UnusedBlockWords; w <= wtop; w++)
      a->mark[w] &= ~a->block[w];
  }
}


/*
** Incremental bitmap sweep. Scans trav arenas for dead objects
** (block=1, mark=0) and frees them. Returns a cost estimate.
** T3b-1: g->gc.root retired; enumeration is arena bitmaps + hugeset.
** After aend, SweepPhase_Huge runs hugeset free/demote (not root rebuild).
**
** Epoch model (D4): aend = sweep_aend (snapshot of arenastop at atomic end).
** Current arenas (swept_gen == epoch) are skipped — they are nursery (born or
** already swept this cycle); only other arenas have freeable dead objects.
** Free is incremental (GCSWEEP_BITMAP_MAX / word / POD-arena budget).
*/
static size_t gc_bitmap_sweep(global_State *g)
{
  GCArena **arenas = mref(g->gc.arenas, GCArena *);
  MSize ai = g->gc.sweepa;
  uint32_t w = g->gc.sweepw;
  uint32_t freed = 0;
  MSize aend = g->gc.sweep_aend;

  if (ai == 0 && w == UnusedBlockWords) {
    MARKALLOC_PROGRESS_LOG(
	      "[markalloc-progress] free enter aend=%u arenastop=%u "
	      "total=%zu estimate=%zu strnum=%u hugenum=%u hugemem=%zu state=%u\n",
	      (unsigned)aend,
	      (unsigned)g->gc.arenastop, (size_t)g->gc.total,
	      (size_t)g->gc.estimate, (unsigned)g->str.num,
	      (unsigned)g->gc.hugenum, (size_t)g->gc.hugemem,
	      (unsigned)g->gc.state);
  }

  while (ai < aend && freed < GCSWEEP_BITMAP_MAX) {
    GCArena *a = arenas[ai];
    uint32_t wtop;
    /* Nursery: current arenas (swept_gen == epoch) have no dead objects —
    ** all cells are is_curwhite (!mark ∧ current), not isdead. Skip. */
    if (arena_is_current(g, a)) {
      ai++;
      w = UnusedBlockWords;
      continue;
    }
    /* P2: NonTrav string-arena reclaim, reached BEFORE the TravObjs skip below.
    ** ArenaClass_NonTrav arenas are de-facto strings-only
    ** (verified by gc_arena_verify); they lack ArenaFlag_TravObjs and would
    ** otherwise be skipped at the `!(TravObjs)` continue, leaking every dead
    ** arena string. Word-parallel scan: dead = block & ~mark. For each dead
    ** ~LJ_TSTR cell (not FIXED, not strempty): lj_strtab_remove STRICTLY before
    ** lj_str_free (the freed cell head may be overwritten by freelist metadata
    ** or unmapped). Probe count is billed against GCSWEEP_BITMAP_MAX so a large die-off
    ** cannot blow the incremental step. Survivors (mark=1) are untouched here;
    ** survivors demoted at free completion. */
    if (!(a->flags & (ArenaFlag_TravObjs | ArenaFlag_PODOnly |
		      ArenaFlag_UdataOnly | ArenaFlag_CdataVOnly))) {
      lj_arena_flushbins(a);
      wtop = arena_blockidx((GCCellID)a->celltop - 1);
      while (w <= wtop && freed < GCSWEEP_BITMAP_MAX) {
	GCBlockword dead = a->block[w] & ~a->mark[w];
	while (dead) {
	  uint32_t bitidx = lj_ffs(dead);
	  GCCellID c = (w << 5) + bitidx;
	  GCobj *o = (GCobj *)arena_cellptr(a, c);
	  GCstr *s;
	  MSize probes;
	  dead &= dead - 1;
	  lj_assertG(o->gch.gct == ~LJ_TSTR,
		     "non-string in NonTrav arena (OPENADDR): gct=%d cell=%d flags=0x%x",
		     (int)o->gch.gct, (int)c, a->flags);
	  if (o->gch.gct != ~LJ_TSTR) continue;  /* defensive: not a string */
	  if (o == obj2gco(&g->strempty) || (o->gch.marked & LJ_GC_FIXED))
	    continue;
	  s = gco2str(o);
	  probes = lj_strtab_remove(g, s);  /* backward-shift; num-- by lj_str_free */
	  gcstat_inc(g, strings_dead_freed);
	  lj_str_free(g, s);  /* free cell (may overwrite head / unmap) */
	  freed += 1 + (probes >> 2);  /* bill object + probe/shift cost */
	}
	w++;
      }
      if (w > wtop) {
	a->swept_gen = g->gc.epoch;
	gc_arena_demote_survivors(a);
	ai++;
	w = UnusedBlockWords;
      }
      continue;
    }
#if LJ_HASFFI
    /* P3b: CdataV-arena dead free folded into the main bitmap sweep (was a
    ** separate resumable scan in old rebuild prologue). ArenaFlag_CdataVOnly
    ** arenas lack ArenaFlag_TravObjs and would otherwise be skipped at the
    ** `!(TravObjs)` continue below, leaking every dead VLA cdata. Word-parallel
    ** scan: dead = block & ~mark (liveness authority is the BASE cell's mark).
    ** For each dead base cell p: cd = p + GCcdataVar.offset (base->cd
    ** translation); cdatav_cell_assert validates it; gc_freefunc[cd->gct-...]
    ** frees via pure lj_cdata_free (F2). Bounded by GCSWEEP_BITMAP_MAX cells
    ** per slice. Survivors demoted at free completion;
    ** pending-finalizer cdata stay marked (marked inline at separate, on
    ** fin_queue) through atomic.
    **
    ** Nursery aend note: the old CdataV rebuild prologue scanned ALL arenas
    ** (arenastop); folding into the epoch-model sweep (current arenas skipped
    ** per-arena via arena_is_current) matches the TravObjs nursery isolation --
    ** dead nursery CdataV free next cycle when other. Consistent with the rest
    ** of gc_bitmap_sweep. */
    if (a->flags & ArenaFlag_CdataVOnly) {
      lj_arena_flushbins(a);
      wtop = arena_blockidx((GCCellID)a->celltop - 1);
      while (w <= wtop && freed < GCSWEEP_BITMAP_MAX) {
	GCBlockword dead = a->block[w] & ~a->mark[w];
	while (dead) {
	  uint32_t bitidx = lj_ffs(dead);
	  GCCellID c = (w << 5) + bitidx;
	  char *p = (char *)arena_cellptr(a, c);
	  GCcdata *cd;
	  dead &= dead - 1;
	  /* Defensive: re-check the base cell state before the offset
	  ** translation + header load, mirroring the TravObjs ASAN-freelist
	  ** race guard (same pattern as TravObjs sweep). */
	  if (arena_cellstate(a, c) < CellState_White)
	    continue;
	  cdatav_cell_assert(g, p);
	  cd = (GCcdata *)(p + ((GCcdataVar *)p)->offset);
	  gc_freefunc[cd->gct - ~LJ_TSTR](g, obj2gco(cd));
	  freed++;
	}
	w++;
      }
      if (w > wtop) {
	a->swept_gen = g->gc.epoch;
	gc_arena_demote_survivors(a);
	ai++;
	w = UnusedBlockWords;
      }
      continue;
    }
#endif
    if (!(a->flags & ArenaFlag_TravObjs)) {
      /* No objects to free (or FFI-off CdataV). Stamp current + demote for I1. */
      a->swept_gen = g->gc.epoch;
      gc_arena_demote_survivors(a);
      ai++;
      w = UnusedBlockWords;
      continue;
    }
    /* POD-only arena (closures, protos): word-parallel sweep. One linear
    ** metadata pass frees all dead objects and recolors survivors white,
    ** touching no object data. Whole-arena atomic (the transform + scavenge
    ** must pair without an intervening allocation), so it ignores the per-word
    ** cursor and bills its cost as a fixed chunk of the GCSWEEP_BITMAP_MAX budget.
    ** Cell-space accounting: freed cells * CellSize, derived from the bitmap
    ** by lj_arena_podsweep, matches the cell-space alloc accounting. */
    if (a->flags & ArenaFlag_PODOnly) {
      /* D3: FIRST swept_gen=epoch, THEN podsweep demotes (mark'=b^m). */
      a->swept_gen = g->gc.epoch;
      GCCellID fcells = lj_arena_podsweep(g, a);
      g->gc.total -= (GCSize)fcells << CellSizeLog2;
#if defined(LUAJIT_ENABLE_MEMPROF)
      if (LJ_UNLIKELY(g->gc.gcmarkflags & GCF_MEMPROF))
	lj_memprof_emit_podfree(g, (uint32_t)fcells, (size_t)fcells << CellSizeLog2);
#endif
      freed += GCSWEEP_BITMAP_MAX/2;
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
    while (w <= wtop && freed < GCSWEEP_BITMAP_MAX) {
      GCBlockword dead = a->block[w] & ~a->mark[w];
      while (dead) {
	uint32_t bitidx = lj_ffs(dead);
	GCCellID c = (w << 5) + bitidx;
	GCobj *o = (GCobj *)arena_cellptr(a, c);
	dead &= dead - 1;
	/* A thread freed earlier in this same word may have side-effect-freed
	** its dead open upvalues: gc_freefunc[LJ_TTHREAD] -> lj_state_free ->
	** lj_func_closeuv -> lj_func_freeuv pushes them onto this arena's
	** freelist, ASAN-poisoning the cell. While binned those cells keep
	** block=1, so they remain in this cached `dead` word.
	** lj_arena_flushbins (called after each thread free below) flips them
	** to Free (block=0); re-check the live bitmap before any header load so
	** we never read a poisoned gct/closed byte. (Pre-ASAN code relied on
	** those header bytes surviving the freelist linkword write -- false
	** under the arena ASAN poison contract.) */
	if (arena_cellstate(a, c) < CellState_White)
	  continue;
	/* Skip open upvalues: Path L closeuv / Path F freeall_openuv; not bitmap. */
	if (o->gch.gct == ~LJ_TUPVAL && !gco2uv(o)->closed)
	  continue;
	/* Strings reclaimed on NonTrav OPENADDR path above; skip here. */
	if (o->gch.gct == ~LJ_TSTR)
	  continue;
	/* F2: pending-finalizer cdata are marked at separate (on fin_queue),
	** so they do not appear as dead here. lj_cdata_free is pure free. */
	/* The bitmap (block=1, mark=0) is the sole dead test under HASGCMARK;
	** the header white bit is vestigial. Phase-independent isdead
	** (!mark ∧ other) holds inside the free window (the arena is "other"
	** until swept_gen=epoch at this arena's free completion), so this
	** assert is the mark-based dead invariant, independent of header color. */
	lj_assertG(gc_obj_isdead(g, o) || (o->gch.marked & LJ_GC_FIXED),
		   "bitmap sweep freeing non-dead object: o=%p gct=%d marked=0x%02x",
		   (void*)o, o->gch.gct, o->gch.marked);
	{
	  /* Capture gct before the free: the cell is ASAN-poisoned afterwards.
	  ** Freeing a thread runs lj_state_free -> closeuv, which side-effect-
	  ** frees its dead open upvalues into this arena; flush bins so those
	  ** cells become Free (block=0) and are skipped by the re-check above
	  ** when reached later in this word. */
	  unsigned gct = o->gch.gct;
	  gc_freefunc[gct - ~LJ_TSTR](g, o);
	  if (gct == ~LJ_TTHREAD)
	    lj_arena_flushbins(a);
	}
	freed++;
      }
      w++;
    }
    if (w > wtop) {
      a->swept_gen = g->gc.epoch;
      gc_arena_demote_survivors(a);
      ai++;
      w = UnusedBlockWords;
    }
  }

  g->gc.sweepa = ai;
  g->gc.sweepw = (uint16_t)w;
  gcstat_add(g, sweep_cells, freed);
  MARKALLOC_PROGRESS_LOG(
	    "[markalloc-progress] free step ai=%u/%u w=%u freed=%u\n",
	    (unsigned)ai, (unsigned)aend, (unsigned)w, (unsigned)freed);
  if (ai >= aend) {
    /* All other arenas swept (current arenas were skipped per-arena).
    ** Empty aend (no arenas at atomic end) is not expected in a live VM. */
    lj_assertG(aend > 0, "bitmap sweep aend==0 (no arenas at atomic?)");
    MARKALLOC_PROGRESS_LOG(
	      "[markalloc-progress] free done → Huge aend=%u total=%zu "
	      "strnum=%u hugenum=%u hugemem=%zu\n",
	      (unsigned)aend, (size_t)g->gc.total, (unsigned)g->str.num,
	      (unsigned)g->gc.hugenum, (size_t)g->gc.hugemem);
    g->gc.sweepphase = SweepPhase_Huge;
    g->gc.sweep_hugep = SweepHuge_Scan;
    /* P3b: CdataV-arena dead free is folded into the bitmap sweep above
    ** (ArenaFlag_CdataVOnly branch). Finalizer pending objects live on
    ** fin_queue (marked inline at separate); their MARK is demoted with
    ** other survivors in one-shot SweepHuge_Scan. */
    g->gc.sweepa = 0;
    g->gc.sweepw = UnusedBlockWords;
  }
  return freed;
}

	/*
** Post-bitmap hugeset pass (SweepPhase_Huge).
**
** Arena free already demotes survivors (gc_arena_demote_survivors / podsweep).
** Huge objects have no cell bitmap, so dead free + survivor demote
** (clear HUGESET_MARK) and huge_swept_gen=epoch happen here in one shot.
** Optional SweepHuge_Assert chunk-checks arena demote under LUA_USE_ASSERT
** (release: Scan → Done). Header GRAY is already zero at free entry
** (atomic blacken + empty worklists). T3b-1: no root chain work.
**
** CdataV dead free is folded into gc_bitmap_sweep (P3b). Fin-queue objects
** keep MARK until this hugescan demotes survivors.
*/

/* One-shot hugeset sweep: free dead (MARK=0) and demote survivors
** (clear HUGESET_MARK). No mid-scan yield — mutator cannot run mid-walk.
** Next-cycle lj_arena_gc_assert_demote also checks no residual huge MARK.
** Upvalues are never huge. */
static void gc_sweep_hugeset(global_State *g)
{
  GCRef *slots;
  MSize hi, hmask;

  slots = mref(g->gc.hugeset, GCRef);
  hmask = g->gc.hugesetmask;
  if (slots == NULL) {
    g->gc.huge_swept_gen = g->gc.epoch;  /* Empty huge set: trivially current. */
#if LUA_USE_ASSERT
    g->gc.sweep_asserta = 0;
    g->gc.sweep_hugep = SweepHuge_Assert;
#else
    g->gc.sweep_hugep = SweepHuge_Done;
#endif
    return;
  }
  for (hi = 0; hi <= hmask; hi++) {
    uintptr_t u = gcrefu(slots[hi]);
    GCobj *o;
    if (!hugeset_slot_live(u)) continue;  /* EMPTY / TOMB. */
    hugeset_slot_assert(g, u);
    /* o = GCobj (cd for CDATAV slots); mark authority is the slot itself. */
    o = hugeset_slot_obj(u);
    if (o->gch.gct == ~LJ_TSTR) {
      /* Huge strings: openaddr reclaim (symmetric to arena NonTrav). */
      if (!(u & HUGESET_MARK)) {
	lj_strtab_remove(g, gco2str(o));
	gcstat_inc(g, strings_dead_freed);
	lj_str_free(g, gco2str(o));
      } else {
	setgcrefp(slots[hi], (void *)(u & ~(uintptr_t)HUGESET_MARK));
      }
      continue;  /* not generic gc_freefunc */
    }
    lj_assertG(o->gch.gct != ~LJ_TUPVAL, "huge upvalue is impossible");
    if (!(u & HUGESET_MARK)) {
      gc_freefunc[o->gch.gct - ~LJ_TSTR](g, o);
    } else {
      lj_assertG(o->gch.gct != ~LJ_TTHREAD, "unexpected huge thread");
      setgcrefp(slots[hi], (void *)(u & ~(uintptr_t)HUGESET_MARK));
    }
  }
  /* Every live huge object is "current". Mirror per-arena swept_gen. */
  g->gc.huge_swept_gen = g->gc.epoch;
#if LUA_USE_ASSERT
  g->gc.sweep_asserta = 0;
  g->gc.sweep_hugep = SweepHuge_Assert;
#else
  g->gc.sweep_hugep = SweepHuge_Done;
#endif
}

/* Free already demotes arena survivors; hugescan demotes huge. This phase
** only verifies (chunked). Without LUA_USE_ASSERT it is never entered. */
static void gc_sweep_assert_demote(global_State *g)
{
#if !LUA_USE_ASSERT
  g->gc.sweep_hugep = SweepHuge_Done;
#else
  GCArena **arenas = mref(g->gc.arenas, GCArena *);
  MSize i, n, stop;
  GCSize total_before = g->gc.total;
  lj_assertG(g->gc.sweep_hugep == SweepHuge_Assert,
	     "AssertDemote entered with sweep_hugep=%d",
	     g->gc.sweep_hugep);
  lj_assertG(g->gc.state == GCSsweep, "AssertDemote outside GCSsweep");
  /* Chunked: up to GCSWEEP_ASSERT_DEMOTE_ARENAS arenas/slice then yield.
  ** R1: re-read arenastop each slice — arenas appended mid-sweep have zeroed
  ** bitmaps and swept_gen==epoch, so I1 holds. */
  stop = g->gc.arenastop;
  n = 0;
  for (i = g->gc.sweep_asserta; i < stop && n < GCSWEEP_ASSERT_DEMOTE_ARENAS;
       i++, n++) {
    GCArena *a = arenas[i];
    uint32_t w, wtop;
    /* I1: every visited arena must be current (swept_gen == epoch). */
    lj_assertG(a->swept_gen == g->gc.epoch,
	       "AssertDemote: arena %u not current (I1 broken): "
	       "swept_gen=%u epoch=%u",
	       (unsigned)i, (unsigned)a->swept_gen, (unsigned)g->gc.epoch);
    if ((GCCellID)a->celltop <= MinCellId) continue;
    wtop = arena_blockidx((GCCellID)a->celltop - 1);
    for (w = UnusedBlockWords; w <= wtop; w++)
      lj_assertG((a->mark[w] & a->block[w]) == 0,
		 "AssertDemote residual arena mark: arena %u word %u "
		 "mark&block=0x%x (demote incomplete?)",
		 (unsigned)i, (unsigned)w,
		 (unsigned)(a->mark[w] & a->block[w]));
  }
  g->gc.sweep_asserta = i;
  lj_assertG(g->gc.total == total_before,
	     "AssertDemote arena slice freed memory (total %lu -> %lu)",
	     (unsigned long)total_before, (unsigned long)g->gc.total);
  if (i < stop)
    return;  /* More arenas remain: yield in same phase. */
  MARKALLOC_PROGRESS_LOG(
	    "[markalloc-progress] sweep AssertDemote done total=%zu strnum=%u "
	    "arenastop=%u\n",
	    (size_t)g->gc.total, (unsigned)g->str.num, (unsigned)g->gc.arenastop);
  g->gc.sweep_hugep = SweepHuge_Done;
#endif
}

/* Dispatcher for SweepPhase_Huge: one sub-phase per call; yields only while
** chunked Assert is in progress. */
static void gc_sweep_huge(global_State *g)
{
  lj_assertG(g->gc.state == GCSsweep, "sweep_huge entered outside GCSsweep");
  for (;;) {
    uint8_t phase = g->gc.sweep_hugep;
    switch (phase) {
    case SweepHuge_Scan:
      gcstat_inc(g, sweep_hugescan);
      gc_sweep_hugeset(g);
      break;
    case SweepHuge_Assert:
      gcstat_inc(g, sweep_assert_demote);
      gc_sweep_assert_demote(g);
      break;
    default:
      lj_assertG(0, "bad sweep_huge phase %d", g->gc.sweep_hugep);
      return;
    }
    lj_assertG(g->gc.sweep_hugep >= phase,
	       "sweep_huge phase went backward: %d -> %d",
	       (int)phase, (int)g->gc.sweep_hugep);
    if (g->gc.sweep_hugep == SweepHuge_Done) {
      g->gc.sweepphase = SweepPhase_Done;
      return;
    }
    if (phase == SweepHuge_Assert && g->gc.sweep_hugep == phase)
      return;  /* Still in same phase → yield one onestep. */
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
  /* Weak tables stay mark∧GRAY through clearweak; drop GRAY for free window. */
  gray2black(obj2gco(t));
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
  if (LJ_HASPROFILE && (oldh & HOOK_PROFILE)) lj_dispatch_update(g, 0);
  g->gc.threshold = LJ_MAX_MEM;  /* Prevent GC steps. */
  top = VL->top;
  copyTV(VL, top++, mo);
  if (LJ_FR2) setnilV(top++);
  setgcV(VL, top, o, ~o->gch.gct);
  VL->top = top+1;
  errcode = lj_vm_pcall(VL, top, 1+0, -1);  /* Stack: |mo|o| -> | */
  setgcref(g->cur_L, obj2gco(L));
  hook_restore(g, oldh);
  if (LJ_HASPROFILE && (oldh & HOOK_PROFILE)) lj_dispatch_update(g, 0);
  g->gc.threshold = oldt;  /* Restore GC threshold. */
  if (errcode) {
    lj_vmevent_send(g, ERRFIN,
      copyTV(V, V->top++, L->top-1);
    );
    L->top--;
  }
}

/* Finalize one userdata or cdata object from fin_queue (FIFO head).
** Registry entry was dropped at enqueue; cdata fin is stored on the queue. */
static void gc_finalize(lua_State *L)
{
  global_State *g = G(L);
  FinQueueEntry e;
  GCobj *o;
  cTValue *mo;
  gcstat_inc(g, finalizers);
  lj_assertG(tvref(g->jit_base) == NULL, "finalizer called on trace");
  if (!lj_gc_fin_queue_pop(g, &e))
    return;
  o = gcref(e.obj);
  lj_assertG(o != NULL, "fin_queue empty obj");
#if LJ_HASFFI
  if (e.kind == FIN_KIND_CDATA || o->gch.gct == ~LJ_TCDATA) {
    TValue tmp;
    GCobj *finobj = gcref(e.fin);
    gc_obj_makewhite(g, o);
    o->gch.marked &= (uint8_t)~LJ_GC_CDATA_FIN;
    if (finobj != NULL) {
      setgcV(L, &tmp, finobj, e.fin_it);
      gc_call_finalizer(g, L, &tmp, o);
    }
    return;
  }
#endif
  gc_obj_makewhite(g, o);
  mo = lj_meta_fastg(g, tabref(gco2ud(o)->metatable), MM_gc);
  if (mo)
    gc_call_finalizer(g, L, mo, o);
}

/* Drain all pending finalizers from fin_queue. */
void lj_gc_finalize_udata(lua_State *L)
{
  while (!lj_gc_fin_queue_empty(G(L)))
    gc_finalize(L);
}

#if LJ_HASFFI
/* Close: disable new cdata fin_register (classic: FFI_FIN metatable=NULL). */
void lj_gc_finalize_cdata(lua_State *L)
{
  G(L)->gc.fin_closed = 1;
}
#endif

/* Free all remaining GC objects. */
void lj_gc_freeall(global_State *g)
{
  MSize i;
  /* Free everything, except super-fixed objects (the main thread). */
  /* Force the deterministic shutdown path: gcmarkflags cleared so the
  ** mark-authority branch of gc_sweep (gc.state==GCSatomic|GCSsweep) does
  ** NOT run here. The residual freeall paths free by the SFIXED-root identity
  ** (independent of currentwhite). The bitmap branch must NOT run here -- the
  ** direct cell scan below frees arena objects, so reading their cell marks
  ** afterwards would be a use-after-free. */
  g->gc.gcmarkflags = 0;
  /*
  ** T3b-1: g->gc.root retired; enumeration is the arena block bitmaps +
  ** hugeset. Shutdown frees by walking live arena cells and the hugeset
  ** directly (no root chain).
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
	  /* A thread freed earlier in this word side-effect-frees its dead
	  ** open upvalues (lj_state_free -> closeuv -> freeuv) into this arena,
	  ** ASAN-poisoning those cells. While binned they keep block=1 and stay
	  ** in this cached `alive` word. Flush bins after each thread free
	  ** below flips them to Free (block=0); re-check the live bitmap before
	  ** any header load so we never read a poisoned gct/closed byte. The
	  ** pre-ASAN claim that "freeing never overwrites gct/closed" is false
	  ** under the arena ASAN poison contract. */
	  if (arena_cellstate(a, c) < CellState_White)
	    continue;
	  /* Open UVs: Path F freeall_openuv on THREAD; Path L closeuv mid-cycle.
	  ** Bitmap never frees open UVs (C4b skip). */
	  if (o->gch.gct == ~LJ_TUPVAL && !gco2uv(o)->closed)
	    continue;
	  if (o->gch.gct == ~LJ_TTHREAD)
	    lj_func_freeall_openuv(g, gco2th(o));
	  {
	    unsigned gct = o->gch.gct;
	    gc_freefunc[gct - ~LJ_TSTR](g, o);
	    if (gct == ~LJ_TTHREAD)
	      lj_arena_flushbins(a);
	  }
	}
      }
    }
    /* Huge objects: walk the huge set (no cell bitmap exists for them).
    ** Strings: gc_sweepstr_oa below; VLA cdata were freed above. */
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
	  if (o->gch.gct == ~LJ_TSTR) continue;  /* freeall: gc_sweepstr_oa owns strings. */
	  gc_freefunc[o->gch.gct - ~LJ_TSTR](g, o);
	}
      }
    }
    /* T3b-1: no root-chain re-anchor; mainthread is SFIXED via mainthref. */
  }
  gc_sweepstr_oa(g, 0, g->str.mask + 1);  /* Free all open-addr string slots. */
}

/* -- Collector ----------------------------------------------------------- */

/* Atomic re-scan of coroutine stacks (no stack barriers). Threads are
** permanent-gray (mark∧GRAY) after propagatemark; walk graythread to
** re-mark stack slots without re-greying the thread itself. */
static void gc_atomic_rescan_threads(global_State *g)
{
  GCobj **thr = mref(g->gc.graythread, GCobj *);
  MSize i, n = g->gc.graythreadtop;
  if (thr == NULL) return;
  for (i = 0; i < n; i++) {
    GCobj *o = thr[i];
    if (o == NULL || o == obj2gco(mainthread(g))) continue;
    lj_assertG(o->gch.gct == ~LJ_TTHREAD, "graythread non-thread at rescan");
    lj_assertG(gc_inarena(g, o), "non-arena thread in graythread");
    gc_traverse_thread(g, gco2th(o));
  }
}

/* Atomic part of the GC cycle, transitioning from mark to sweep phase. */
static void atomic(global_State *g, lua_State *L)
{
  size_t udsize;

  /* (1) Drain any leftover gray work from the mark phase. gc_propagate_gray
  ** fixed-points SSB + arena gray + hugegray, so this leaves every worklist
  ** empty before root marks begin. */
  gc_propagate_gray(g);  /* Propagate any left-overs. */

  /* (2) Mark roots: running thread L, mainthread stack slots (no barriers),
  ** current trace, and the fixed GC roots. Weak tables are NOT redirected
  ** here — their strong slots get re-marked once root marks have been
  ** emitted, so redirect happens in step (3) after these marks. */
  lj_assertG(!gc_obj_iswhite(g, obj2gco(mainthread(g))), "main thread turned white");
  gc_markobj(g, L);  /* Mark running thread. */
  gc_traverse_mainthread(g);  /* Stack slots have no barriers. */
  gc_traverse_curtrace(g);  /* Traverse current trace. */
  gc_mark_gcroot(g);  /* Mark GC roots (again). */

  /* (3) Redirect weak tables to arena/huge gray stacks AFTER root marks. This
  ** is the arena analog of classic lj_gc.c re-traversing weak tables once
  ** the world is marked: the strong key/value slots of a weak table must be
  ** re-marked after the new root marks have been emitted, because a
  ** root-reachable strong slot promoted by step (2) would be missed if we
  ** redirected before the roots. The single propagate in step (5) drains
  ** these redirects together with the root and rescan marks. */
  gc_weak_redirect_all(g);  /* Redirect weak tables to arena/huge gray stacks. */

  /* (4) Re-scan coroutine stacks (permanent-gray threads; mainthread
  ** already above). No stack barriers — walks graythread without
  ** re-greying the thread itself. */
  gc_atomic_rescan_threads(g);

  /* (5) ONE propagate drains root marks (2), weak redirects (3), and rescan
  ** stack marks (4) together. gc_propagate_gray loops until SSB + arena gray
  ** + hugegray are all empty (fixed-point), so no intermediate flush or
  ** intermediate propagate is needed between roots, weak redirect, and
  ** rescan — they only enqueue more gray work which this single call drains. */
  gc_propagate_gray(g);

  /* (6) F3 registry scan → fin_queue + resurrect marks, then propagate.
  ** Backfill first; enqueue marks each object (and cdata fin) inline.
  ** Carry-over queue entries were re-rooted in gc_mark_start — no second
  ** full-queue walk here. */
  lj_gc_fin_backfill_udata(g);
  udsize = lj_gc_separateudata(g, 0);
  lj_gc_fin_dual_assert_udata(g);
  udsize += gc_propagate_gray(g);

  /* (7) All marking done, clear weak tables. */
  gc_clearweak_stacks(g);

  lj_buf_shrink(L, &g->tmpbuf);  /* Shrink temp buffer. */

  /* Free window: empty worklists; threads permanent-gray (mark∧GRAY);
  ** closed UVs pure black; open UVs left mark∧GRAY (P3a residual exception
  ** — see gc_mark UPVAL and gc_assert_atomic_end). gc_clearweak_stacks cannot refill SSB/arena-gray/
  ** hugegray: gc_mayclear only leaf-marks strings (gc_mark_str, no push) and
  ** gray2black's the weak tables; no barrierback runs during atomic (mutator
  ** stopped). Rely on the empty-worklist asserts below instead of an
  ** unconditional re-prop. */
  {
    GCobj **ssb = mref(g->gc.ssb, GCobj *);
    GCobj **ssbtop = mref(g->gc.ssbtop, GCobj *);
    lj_assert_check(g, ssb == NULL || ssbtop == ssb,
		     "SSB non-empty at atomic→sweep: top=%p base=%p",
		     (void *)ssbtop, (void *)ssb);
  }
  lj_assert_check(g, g->gc.grayastop == 0 && g->gc.hugegraytop == 0,
				   "arena/huge gray non-empty at atomic→sweep: "
				   "grayastop=%u hugegraytop=%u",
				   (unsigned)g->gc.grayastop, (unsigned)g->gc.hugegraytop);
  /* (8) Open free window: epoch++ FIRST (D1 / I2). Live-thread open UVs
  ** marked in gc_traverse_thread (T1); dead-thread open UVs via Path L.
  ** No atomic openupval fullsweep (T2). graythread_reset is hygiene/canary. */
  g->gc.epoch++;
  gc_graythread_reset(g);
  gc_assert_atomic_end(g);

  /* (9) Prepare for sweep phase. */
  /* No white flip: liveness is the mark bitmap, not a flipping header white.
  ** strempty is an SFIXED root, never swept; reset its vestigial header color
  ** to the exact value the old post-flip path produced (curwhite was 0 during
  ** sweep): FIXED|SFIXED with no white bit, so it reads as a reachable root. */
  g->strempty.marked = LJ_GC_FIXED | LJ_GC_SFIXED;
  g->gc.estimate = g->gc.total - (GCSize)udsize;  /* Initial estimate. */
  /* Old snapshot BEFORE any mutator alloc in the sweep window. Nursery
  ** allocations must not reuse these arenas (see lj_arena_findspace, which
  ** gates on gc.state==GCSsweep + sweep_aend, not a GCF predicate). */
  g->gc.sweep_aend = g->gc.arenastop;
  setmref(g->gc.arena, NULL);
  setmref(g->gc.travarena, NULL);
  setmref(g->gc.podarena, NULL);
  setmref(g->gc.udatarena, NULL);
  setmref(g->gc.cdatavarena, NULL);
  /* Enter free window: scheduling is driven by gc.state==GCSsweep (set after
  ** atomic returns) plus sweepphase; the predicate layer reads swept_gen. */
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
    MARKALLOC_PROGRESS_LOG(
	      "[markalloc-progress] mark_start total=%zu estimate=%zu "
	      "arenastop=%u strnum=%u\n",
	      (size_t)g->gc.total, (size_t)g->gc.estimate,
	      (unsigned)g->gc.arenastop, (unsigned)g->str.num);
    gc_mark_start(g);  /* Start a new GC cycle by marking all GC roots. */
    return 0;
  case GCSpropagate:
    {
      size_t c;
      GCobj **ssb, **ssbtop;
      GCArena *a;
      /* SSB is part of gray work: flush before each slice / empty check. */
      lj_gc_ssb_flush(g);
      /* One step drains a whole worklist unit: all hugegray, or one arena's
      ** full gray stack (gc_propagate_arena). Not one object per step. */
      if (!gc_hugegray_empty(g)) {
	c = 0;
	while (!gc_hugegray_empty(g)) {
	  size_t n = propagatemark(g, gc_hugegray_pop(g));
	  gcstat_add(g, mark_cost, n);
	  c += n;
	}
	lj_gc_ssb_flush(g);
	return c ? c : 1;
      }
      a = gc_grayarena_pop(g);
      if (a != NULL) {
	/* mark_cost counted inside gc_propagate_arena. */
	c = gc_propagate_arena(g, a);
	/* Same-arena re-push (notify) during drain — finish this arena. */
	while (!arena_gray_empty(a))
	  c += gc_propagate_arena(g, a);
	/* Huge objects marked from this arena. */
	while (!gc_hugegray_empty(g)) {
	  size_t n = propagatemark(g, gc_hugegray_pop(g));
	  gcstat_add(g, mark_cost, n);
	  c += n;
	}
	lj_gc_ssb_flush(g);
	return c ? c : 1;
      }
      lj_gc_ssb_flush(g);
      ssb = mref(g->gc.ssb, GCobj *);
      ssbtop = mref(g->gc.ssbtop, GCobj *);
      if ((ssb != NULL && ssbtop != NULL && ssbtop > ssb) ||
	  !gc_hugegray_empty(g) || g->gc.grayastop != 0)
	return 0;  /* Barriers refilled work; stay in propagate. */
      g->gc.state = GCSatomic;
      return 0;
    }
  case GCSatomic:
    if (tvref(g->jit_base))  /* Don't run atomic phase on trace. */
      return LJ_MAX_MEM;
    atomic(g, L);
    MARKALLOC_PROGRESS_LOG(
	      "[markalloc-progress] atomic done total=%zu estimate=%zu "
	      "strnum=%u flags=0x%x\n",
	      (size_t)g->gc.total, (size_t)g->gc.estimate,
	      (unsigned)g->str.num, (unsigned)g->gc.gcmarkflags);
    g->gc.state = GCSsweep;  /* strings reclaimed in bitmap/hugescan (openaddr) */
    return 0;
  case GCSsweepstring:
    /* Unreachable under openaddr (atomic jumps straight to GCSsweep). Keep
    ** enum slot for stats; jump to bitmap sweep if hit. */
    g->gc.state = GCSsweep;
    return GCSWEEPCOST;
  case GCSsweep: {
    /* Bitmap first, then Huge (fall-through after aend). */
    GCSize old = g->gc.total;
    if (g->gc.sweepphase == SweepPhase_Bitmap) {
      gc_bitmap_sweep(g);
      lj_assertG(old >= g->gc.total, "sweep increased memory");
      g->gc.estimate -= old - g->gc.total;
      if (g->gc.sweepphase == SweepPhase_Bitmap)
	return GCSWEEPMAX*GCSWEEPCOST;
      old = g->gc.total;
    }
    lj_assertG(g->gc.sweepphase == SweepPhase_Huge,
	       "sweep expected Huge after Bitmap, got %d", g->gc.sweepphase);
    gc_sweep_huge(g);
    lj_assertG(old >= g->gc.total, "sweep increased memory");
    g->gc.estimate -= old - g->gc.total;
    if (g->gc.sweepphase == SweepPhase_Done) {
#if defined(LUAJIT_ENABLE_MEMPROF)
      g->gc.gcmarkflags &= GCF_MEMPROF;
#else
      g->gc.gcmarkflags = 0;
#endif
      if (g->str.num <= (g->str.mask >> 2) && g->str.mask > LJ_MIN_STRTAB*2-1)
	lj_str_resize(L, g->str.mask >> 1);
      lj_arena_shrink(g);
      if (!lj_gc_fin_queue_empty(g)) {
	g->gc.state = GCSfinalize;
	MARKALLOC_PROGRESS_LOG(
		  "[markalloc-progress] cycle → finalize total=%zu "
		  "arenastop=%u strnum=%u\n",
		  (size_t)g->gc.total, (unsigned)g->gc.arenastop,
		  (unsigned)g->str.num);
      } else {
	gcstat_inc(g, cycles);
	g->gc.stats.last_arenastop = g->gc.arenastop;
	g->gc.stats.last_hugenum = g->gc.hugenum;
	g->gc.stats.last_hugemem = g->gc.hugemem;
	g->gc.state = GCSpause;
	g->gc.debt = 0;
	MARKALLOC_PROGRESS_LOG(
		  "[markalloc-progress] cycle → pause total=%zu "
		  "estimate=%zu arenastop=%u strnum=%u\n",
		  (size_t)g->gc.total, (size_t)g->gc.estimate,
		  (unsigned)g->gc.arenastop, (unsigned)g->str.num);
      }
    }
    return GCSWEEPMAX*GCSWEEPCOST;
    }
  case GCSfinalize:
    if (!lj_gc_fin_queue_empty(g)) {
      GCSize old = g->gc.total;
      if (tvref(g->jit_base))  /* Don't call finalizers on trace. */
	return LJ_MAX_MEM;
      gc_finalize(L);  /* Finalize one object from fin_queue. */
      if (old >= g->gc.total && g->gc.estimate > old - g->gc.total)
	g->gc.estimate -= old - g->gc.total;
      if (g->gc.estimate > GCFINALIZECOST)
	g->gc.estimate -= GCFINALIZECOST;
      return GCFINALIZECOST;
    }
    gcstat_inc(g, cycles);
    g->gc.stats.last_arenastop = g->gc.arenastop;
    g->gc.stats.last_hugenum = g->gc.hugenum;
    g->gc.stats.last_hugemem = g->gc.hugemem;
    g->gc.state = GCSpause;  /* End of GC cycle. */
    g->gc.debt = 0;
    return 0;
  default:
    lj_assertG(0, "bad GC state");
    return 0;
  }
}

#ifdef LUAJIT_ENABLE_GCSTATS_TIMING
static void gcstat_timing_record(global_State *g, uint8_t state,
				 uint8_t sweepphase, uint64_t dt)
{
  uint64_t *t, *m;
  if (state == GCSsweep) {
    if (sweepphase == 1) {  /* SweepPhase_Huge */
      t = &g->gc.stats.time_sweep_huge_ns;
      m = &g->gc.stats.maxpause_sweep_huge_ns;
    } else {  /* SweepPhase_Bitmap (0) or Done (2, shouldn't happen) */
      t = &g->gc.stats.time_sweep_bitmap_ns;
      m = &g->gc.stats.maxpause_sweep_bitmap_ns;
    }
  } else {
    switch (state) {
    case GCSpause:	t = &g->gc.stats.time_pause_ns;	m = &g->gc.stats.maxpause_pause_ns;	break;
    case GCSpropagate:	t = &g->gc.stats.time_propagate_ns;	m = &g->gc.stats.maxpause_propagate_ns;	break;
    case GCSatomic:	t = &g->gc.stats.time_atomic_ns;	m = &g->gc.stats.maxpause_atomic_ns;	break;
    case GCSsweepstring: t = &g->gc.stats.time_sweepstring_ns; m = &g->gc.stats.maxpause_sweepstring_ns; break;
    case GCSfinalize:	t = &g->gc.stats.time_finalize_ns;	m = &g->gc.stats.maxpause_finalize_ns;	break;
    default:	return;
    }
  }
  *t += dt;
  if (dt > *m) *m = dt;
}
#endif

static size_t gc_onestep(lua_State *L)
{
  global_State *g = G(L);
  uint8_t pre_state = g->gc.state;
  uint8_t pre_sweepphase = g->gc.sweepphase;
  size_t cost;
  static uint32_t hb_nsteps;
  static uint8_t hb_last_state = 0xff;
#ifdef LUAJIT_ENABLE_GCSTATS_TIMING
  struct timespec ts0, ts1;
  clock_gettime(CLOCK_MONOTONIC, &ts0);
  cost = gc_onestep_raw(L);
  clock_gettime(CLOCK_MONOTONIC, &ts1);
  {
    uint64_t dt = (uint64_t)(ts1.tv_sec - ts0.tv_sec) * 1000000000u +
		  (uint64_t)ts1.tv_nsec - (uint64_t)ts0.tv_nsec;
    gcstat_timing_record(g, pre_state, pre_sweepphase, dt);
  }
#else
  cost = gc_onestep_raw(L);
#endif
  if (pre_state == GCSsweep) {
    if (pre_sweepphase == 1)  /* SweepPhase_Huge */
      gcstat_inc(g, sweep_huge_steps);
    else
      gcstat_inc(g, sweep_bitmap_steps);
  } else {
    g->gc.stats.nsteps[pre_state]++;
  }
  /* Progress heartbeat (MARKALLOC_PROGRESS). */
  if (MARKALLOC_PROGRESS()) {
    hb_nsteps++;
    if (pre_state != hb_last_state || (hb_nsteps & 0x3ff) == 0) {
      /* Gray work depth: distinguish slow drain vs livelock (depth not falling). */
      MSize gray_cells = 0;
      MSize gi;
      GCobj **ssb = mref(g->gc.ssb, GCobj *);
      GCobj **ssbtop = mref(g->gc.ssbtop, GCobj *);
      MSize ssb_n = (ssb != NULL && ssbtop != NULL && ssbtop > ssb) ?
		    (MSize)(ssbtop - ssb) : 0;
      for (gi = 0; gi < g->gc.grayastop; gi++) {
	MSize idx = mref(g->gc.grayastack, MSize)[gi];
	GCArena *ga = mref(g->gc.arenas, GCArena *)[idx];
	GCCellID1 *gt = mref(ga->greytop, GCCellID1);
	GCCellID1 *gb = mref(ga->greybase, GCCellID1);
	if (gt != NULL && gb != NULL && gt > gb)
	  gray_cells += (MSize)(gt - gb);
      }
      MARKALLOC_PROGRESS_LOG(
	      "[markalloc-progress] onestep n=%u pre_state=%u post_state=%u "
	      "sweepphase=%u flags=0x%x cost=%zu total=%lu "
	      "grayastop=%u gray_cells=%u hugegray=%u ssb=%u "
	      "barrierback=%lu debt=%lu thr=%lu est=%lu\n",
	      (unsigned)hb_nsteps, (unsigned)pre_state, (unsigned)g->gc.state,
	      (unsigned)g->gc.sweepphase, (unsigned)g->gc.gcmarkflags,
	      cost, (unsigned long)g->gc.total,
	      (unsigned)g->gc.grayastop, (unsigned)gray_cells,
	      (unsigned)g->gc.hugegraytop, (unsigned)ssb_n,
	      (unsigned long)g->gc.stats.barrierback,
	      (unsigned long)g->gc.debt, (unsigned long)g->gc.threshold,
	      (unsigned long)g->gc.estimate);
      hb_last_state = pre_state;
    }
  }
#if defined(LUA_USE_ASSERT) && !defined(LJ_GC_NOSTEPVERIFY)
  /* Read-only free-list consistency check after every incremental step, in
  ** every GC phase. Catches arena double-free / bin corruption / accounting
  ** drift the instant a step produces it, instead of at the next dereference.
  ** Compiled out of release builds; the check itself mutates nothing. */
  lj_gc_checkheap(g);
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
    size_t steped = gc_onestep(L);
    /* Saturating subtract: whole-arena drain can return multi-MB cost. */
    if (steped >= lim)
      lim = 0;
    else
      lim -= (GCSize)steped;
    if (g->gc.state == GCSpause) {
      g->gc.threshold = (g->gc.estimate/100) * g->gc.pause;
      g->vmstate = ostate;
      return 1;  /* Finished a GC cycle. */
    }
  } while (lim > 0);
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

/*
** Full-GC O(live) shadow-mark verify is a diagnostic allocator self-test,
** not a GC liveness proof: after sweep demote, marks are already cleared, so
** shadow-marking every allocated cell then asserting dead==0 mostly checks
** setmark/visit_unmarked primitives. Real mark/sweep invariants live earlier:
**   - gc_assert_atomic_end: residual GRAY + black→white edges + class routing
**   - lj_gc_checkheap: freelist/POD + GCSpause stuck-mark / fin_queue FINALIZED
**
** Gate: LUA_USE_ASSERT && !LJ_GC_NOFULLGCVERIFY. FSANITIZE builds define
** LJ_GC_NOFULLGCVERIFY so ASAN/CI game runs do not pay O(live) every fullgc.
** Pure -DLUA_USE_ASSERT (no NOFULLGCVERIFY) keeps the deep self-test.
*/
#if defined(LUA_USE_ASSERT) && !defined(LJ_GC_NOFULLGCVERIFY)
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
      ** phase blackened every survivor and free/hugescan demoted the bitmap.
      ** With the bitmap authoritative for arena color, every allocated
      ** object must read back as bitmap-white here. A surviving mark bit means
      ** the cycle ended with a stuck-black object -- a demote regression
      ** (the next cycle's white-reset would then be wrong).
      **
      ** A header cross-check is deliberately NOT done: gc_mark writes
      ** white2gray(header) and arena_obj_setmark(bitmap) in lockstep, and
      ** allocation co-writes newwhite(header) with the unmarked bitmap, so the
      ** header WHITES bit can never diverge from the bitmap at a mutation site.
      ** Asserting their agreement would be vacuous; the live invariant is the
      ** bitmap reaching the clean all-white state the next cycle depends on.
      ** Verified reachable: defeating demote leaves objects
      ** bitmap-black here and this assert fires. */
      lj_assertG(!arena_obj_ismarked(a, c),
		 "arena object still bitmap-black after full GC demote: "
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
  /* Invariant 5: every fin_queue member is FINALIZED (also in checkheap). */
  {
    FinQueueEntry *q = mref(g->gc.fin_queue, FinQueueEntry);
    MSize qmask = g->gc.fin_qmask, head = g->gc.fin_qhead, tail = g->gc.fin_qtail, qi;
    if (q != NULL) {
      for (qi = head; qi != tail; qi++) {
	GCobj *mu = gcref(q[qi & qmask].obj);
	lj_assertG(mu != NULL && (mu->gch.marked & LJ_GC_FINALIZED),
		   "fin_queue member not finalized: ptr=%p gct=%d marked=0x%02x",
		   (void *)mu, mu ? mu->gch.gct : 0, mu ? mu->gch.marked : 0);
      }
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
  ** demote, every live huge object (string and non-string) must be slot-white
  ** (MARK clear). One-shot gc_sweep_hugeset demotes survivors (free dead +
  ** clear MARK) before this verify. A stuck slot mark means the per-cycle
  ** reset regressed. Also checked in lj_gc_checkheap when state==GCSpause. */
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
		   "huge object still slot-marked after full GC demote: "
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
    ** bitmap. huge strings: openaddr hugescan. mainthread/strempty
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
	  ** ArenaFlag_UdataOnly. Class routing is also checked on marked
	  ** survivors in gc_assert_atomic_end (earlier, marks authoritative). */
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
	  /* P2 §9 item 10: under the flag, NonTrav arenas are the string reclaim
	  ** target, so every allocated cell MUST be ~LJ_TSTR. A non-string here
	  ** would either leak (the bitmap-sweep string branch skips it) or mis-free
	  ** (strtab_remove would fail the slot-found assert). */
	  lj_assertG(o2->gch.gct == ~LJ_TSTR,
		     "non-string in NonTrav arena (OPENADDR): gct=%d marked=0x%02x cell=%d flags=0x%x",
		     (int)o2->gch.gct, o2->gch.marked, (int)c, a->flags);
	}
      }
    }
    /* Huge objects have no cell bitmap. Huge strings: openaddr hugescan. */
    {
      GCRef *slots = mref(g->gc.hugeset, GCRef);
      if (slots != NULL) {
	MSize hi, hmask = g->gc.hugesetmask;
	for (hi = 0; hi <= hmask; hi++) {
	  uintptr_t u = gcrefu(slots[hi]);
	  if (!hugeset_slot_live(u)) continue;
	  hugeset_slot_assert(g, u);
	  { /* base is huge (no arena bitmap): shadowmark is a no-op; the slot mark
	    ** set by hugescan demote is the authority. o = cd for CDATAV slots, to read gct. */
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
    uintptr_t v = gcrefu(g->str.tab[i]);
    if (v == 0 || v == STRTAB_OA_TOMB) continue;
    o = (GCobj *)(void *)v;
    if (!lj_arena_ishuge(o))
      arena_obj_shadowmark(o);
  }
  /* T3b-1: openaddr strings are unlinked (link=0); root chain retired.
  ** String liveness is strtab + hugeset/arena only — no nextgc walk. */
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
#endif /* LUA_USE_ASSERT && !LJ_GC_NOFULLGCVERIFY */

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
**
** Front-loaded GC invariants (formerly only at fullgc tail via
** gc_arena_verify — too late, and the O(live) shadow-mark was near-empty
** after sweep demote):
**  - any phase: fin_queue members must carry LJ_GC_FINALIZED
**  - any phase: class routing on marked-live cells (UdataOnly / NonTrav str)
**  - GCSpause only: TravObjs mark bits all clear (post-demote stuck-black)
**  - GCSpause only: huge slots MARK clear
*/
int lj_gc_checkheap(global_State *g)
{
  MSize ai, bad = 0;
  /* -- fin_queue FINALIZED (any phase; set at separate). -- */
  {
    FinQueueEntry *q = mref(g->gc.fin_queue, FinQueueEntry);
    MSize qmask = g->gc.fin_qmask, head = g->gc.fin_qhead, tail = g->gc.fin_qtail, qi;
    if (q != NULL) {
      for (qi = head; qi != tail; qi++) {
	GCobj *mu = gcref(q[qi & qmask].obj);
	if (mu == NULL || !(mu->gch.marked & LJ_GC_FINALIZED)) {
	  lj_assertG(0, "fin_queue member not finalized: ptr=%p gct=%d "
		     "marked=0x%02x", (void *)mu, mu ? mu->gch.gct : 0,
		     mu ? mu->gch.marked : 0);
	  bad++;
	}
      }
    }
  }
  /* -- GCSpause: post-demote all marks cleared (Trav + huge). -- */
  if (g->gc.state == GCSpause) {
    GCRef *slots = mref(g->gc.hugeset, GCRef);
    if (slots != NULL) {
      MSize hi, hmask = g->gc.hugesetmask;
      for (hi = 0; hi <= hmask; hi++) {
	uintptr_t u = gcrefu(slots[hi]);
	if (!hugeset_slot_live(u)) continue;
	if (u & HUGESET_MARK) {
	  GCobj *o2 = hugeset_slot_obj(u);
	  lj_assertG(0, "huge object still slot-marked at GCSpause: "
		     "ptr=%p gct=%d marked=0x%02x", (void *)o2, o2->gch.gct,
		     o2->gch.marked);
	  bad++;
	}
      }
    }
  }
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
    /* -- Class routing on marked-live cells (any phase). Free/binned cells
    ** have mark=0 so they are excluded; no flushbins (read-only). -- */
    if ((a->flags & ArenaFlag_UdataOnly) ||
	!(a->flags & (ArenaFlag_TravObjs | ArenaFlag_CdataVOnly))) {
      uint32_t w, wtop;
      if ((GCCellID)a->celltop > MinCellId) {
	wtop = arena_blockidx(celltop - 1);
	for (w = UnusedBlockWords; w <= wtop; w++) {
	  GCBlockword alive = a->block[w] & a->mark[w];
	  while (alive) {
	    uint32_t bitidx = lj_ffs(alive);
	    GCCellID c = (w << 5) + bitidx;
	    GCobj *o = (GCobj *)arena_cellptr(a, c);
	    alive &= alive - 1;
	    if (a->flags & ArenaFlag_UdataOnly) {
	      if (o->gch.gct != ~LJ_TUDATA) {
		lj_assertG(0, "non-udata in Udata arena %d: gct=%d cell=%d",
			   (int)ai, (int)o->gch.gct, (int)c);
		bad++;
	      }
	    } else {
	      /* NonTrav (string) arena: openaddr reclaim target. */
	      if (o->gch.gct != ~LJ_TSTR) {
		lj_assertG(0, "non-string in NonTrav arena %d: gct=%d cell=%d",
			   (int)ai, (int)o->gch.gct, (int)c);
		bad++;
	      }
	    }
	  }
	}
      }
    } else if ((a->flags & ArenaFlag_TravObjs) &&
	       !(a->flags & ArenaFlag_UdataOnly)) {
      /* Trav non-Udata: no udata mis-route (1b). */
      uint32_t w, wtop;
      if ((GCCellID)a->celltop > MinCellId) {
	wtop = arena_blockidx(celltop - 1);
	for (w = UnusedBlockWords; w <= wtop; w++) {
	  GCBlockword alive = a->block[w] & a->mark[w];
	  while (alive) {
	    uint32_t bitidx = lj_ffs(alive);
	    GCCellID c = (w << 5) + bitidx;
	    GCobj *o = (GCobj *)arena_cellptr(a, c);
	    alive &= alive - 1;
	    if (o->gch.gct == ~LJ_TUDATA) {
	      lj_assertG(0, "udata in non-Udata Trav arena %d: cell=%d",
			 (int)ai, (int)c);
	      bad++;
	    }
	  }
	}
      }
    }
    /* -- GCSpause stuck-black: TravObjs mark bitmap must be empty. -- */
    if (g->gc.state == GCSpause && (a->flags & ArenaFlag_TravObjs) &&
	(GCCellID)a->celltop > MinCellId) {
      uint32_t w, wtop = arena_blockidx(celltop - 1);
      for (w = UnusedBlockWords; w <= wtop; w++) {
	GCBlockword stuck = a->block[w] & a->mark[w];
	if (stuck) {
	  uint32_t bitidx = lj_ffs(stuck);
	  GCCellID c = (w << 5) + bitidx;
	  GCobj *o = (GCobj *)arena_cellptr(a, c);
	  lj_assertG(0, "arena %d object still bitmap-black at GCSpause: "
		     "ptr=%p gct=%d marked=0x%02x cell=%d",
		     (int)ai, (void *)o, o->gch.gct, o->gch.marked, (int)c);
	  bad++;
	  break;  /* one report per arena is enough */
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
	c = arena_linkword_get(a, c);  /* poisoned free cell head */
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
  if (g->gc.state <= GCSatomic) {  /* Caught somewhere in the middle. */
    gc_hugegray_reset(g);  /* Reset worklists from partial propagation. */
    gc_graythread_reset(g);
    gc_weak_reset(g);
    {
      /* Mid-cycle restart drops greystacks next — cannot flush into them.
      ** gray2black all SSB GRAY entries, then clear the buffer. Residual
      ** header GRAY on marked objects becomes light-gray after mark clear. */
      GCobj **ssb = mref(g->gc.ssb, GCobj *);
      GCobj **top = mref(g->gc.ssbtop, GCobj *);
      while (ssb != NULL && top != NULL && ssb < top) {
	GCobj *o = *ssb++;
	if (o != NULL && (o->gch.marked & LJ_GC_GRAY))
	  gray2black(o);
      }
      setmref(g->gc.ssbtop, mref(g->gc.ssb, GCobj *));
    }
#if defined(LUAJIT_ENABLE_MEMPROF)
    g->gc.gcmarkflags &= GCF_MEMPROF;
#else
    g->gc.gcmarkflags = 0;
#endif
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
    /* Free demote never ran; mark-start assert is pure check. Drop partial marks. */
    lj_arena_gcprepare(g);
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
  /* O(live) shadow-mark self-test: only pure ASSERT builds without
  ** LJ_GC_NOFULLGCVERIFY. GC invariants run earlier (atomic_end / checkheap). */
#if defined(LUA_USE_ASSERT) && !defined(LJ_GC_NOFULLGCVERIFY)
  gc_arena_verify(g);
#endif
}

/* -- Write barriers ------------------------------------------------------ */

/* Backward barrier for arena objects (called from interpreter/JIT).
** Callers prefilter with !isgray; mark-bitmap checks stay here.
** Never set GRAY without a worklist slot — that creates off-stack residual. */
void LJ_FASTCALL lj_gc_barrierback_arena(global_State *g, GCobj *o)
{
  gcstat_inc(g, barrierback);
  /* Unmarked parent (!pure-black): not a black frontier — skip. */
  if (!gc_obj_ismarked(g, o))
    return;
  /* C1: during sweep, barriers must not enqueue. gray2black only
  ** (no traverse / no worklist); the free window is gc.state == GCSsweep. */
  if (LJ_UNLIKELY(g->gc.state == GCSsweep)) {
#if LJ_MARKALLOC_DEBUG
    if (MARKALLOC_PROGRESS()) {
      static uint32_t bb_n;
      bb_n++;
      if ((bb_n & 0xffff) == 0)
	MARKALLOC_PROGRESS_LOG(
		"[markalloc-progress] barrierback_nursery n=%u (no-enqueue)\n",
		(unsigned)bb_n);
    }
#endif
    gray2black(o);
    return;
  }
  /* Callers (VM/JIT) prefilter !isgray. Gray here is still a no-op so a
  ** missed prefilter cannot paint a second worklist edge. */
  if (o->gch.marked & LJ_GC_GRAY)
    return;
  /* pure black → enqueue first, then GRAY (atomic w.r.t. residual invariant). */
  if (lj_arena_ishuge(o)) {
    if (huge_obj_ismarked(g, o)) {
      o->gch.marked |= LJ_GC_GRAY;
      gc_hugegray_push(g, o);
    }
    return;
  }
  if (arena_obj_ismarked(ptr2arena(o), ptr2cell(o))) {
    GCobj **base = mref(g->gc.ssb, GCobj *);
    GCobj **lim = mref(g->gc.ssblim, GCobj *);
    GCobj **top = mref(g->gc.ssbtop, GCobj *);
    /* Corrupt / uninit SSB is a hard bug (layout clobber, missed init).
    ** Never soft-reset: that orphans mark∧GRAY entries already painted. */
    lj_assert_check(g, base != NULL && lim != NULL && base < lim &&
			 top >= base && top <= lim,
		     "barrierback SSB corrupt: top=%p base=%p lim=%p",
		     (void *)top, (void *)base, (void *)lim);
    o->gch.marked |= LJ_GC_GRAY;
    *top++ = o;
    setmref(g->gc.ssbtop, top);
    if (LJ_UNLIKELY(top >= lim)) {
      gcstat_inc(g, ssb_overflow);
      lj_gc_ssb_flush(g);
    }
  }
}

/* Notify that an arena's gray stack became non-empty — insert into heap. */
void lj_gc_grayarena_notify(global_State *g, MSize idx)
{
  MSize *heap;
  gcstat_inc(g, gray_notify);
  GCArena *a = mref(g->gc.arenas, GCArena *)[idx];
  if (a->flags & ArenaFlag_InGrayHeap) {
    /* Dedup: empty→non-empty while still in the gray-arena heap. InGrayHeap
    ** without a heap slot is a worklist orphan (greystack holds GRAY that
    ** gc_propagate_gray will never drain). */
#if defined(LUA_USE_ASSERT)
    {
      MSize *h = mref(g->gc.grayastack, MSize);
      MSize i, n = g->gc.grayastop;
      int found = 0;
      for (i = 0; i < n; i++) {
	if (h[i] == idx) { found = 1; break; }
      }
      lj_assertG(found,
		 "InGrayHeap set but arena not in grayastack: idx=%u grayastop=%u greystack_empty=%d",
		 (unsigned)idx, (unsigned)n, arena_gray_empty(a));
    }
#endif
    return;
  }
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
  GCobj **lim = mref(g->gc.ssblim, GCobj *);
  if (base == NULL || top == NULL || lim == NULL)
    return;
  /* Out-of-range ssbtop means memory clobber (e.g. game L layout) — fail loud. */
  lj_assert_check(g, top >= base && top <= lim,
		   "ssbtop out of range: top=%p base=%p lim=%p",
		   (void *)top, (void *)base, (void *)lim);
  if (top == base)
    return;
  setmref(g->gc.ssbtop, base);
  while (base < top) {
    GCobj *o = *base++;
    if (o != NULL && (o->gch.marked & LJ_GC_GRAY))
      arena_gray_push(g, ptr2arena(o), (GCCellID1)ptr2cell(o));
  }
}

/* Abort on store of a freeable corpse (isdead). Name historical (nursery). */
void lj_gc_nursery_forbid_white(global_State *g, GCobj *v)
{
  lj_assert_check(g, 0,
		   "isdead store forbidden: "
		   "gct=%d marked=0x%02x p=%p",
		   v ? v->gch.gct : -1, v ? v->gch.marked : 0, (void *)v);
}

/* Move the GC propagation frontier forward. */
void lj_gc_barrierf(global_State *g, GCobj *o, GCobj *v)
{
  lj_assertG(!gc_obj_isdead(g, o), "forward barrier on dead object");
  /* Macros only prefilter !isgray; bitmap work is here. */
  if (!gc_obj_ismarked(g, o) || !gc_obj_iswhite(g, v))
    return;
  /* D2: only corpses (isdead) are forbidden; curwhite children are markable. */
  if (LJ_UNLIKELY(gc_obj_isdead(g, v))) {
    lj_gc_nursery_forbid_white(g, v);
    return;
  }
  if (g->gc.state == GCSpropagate || g->gc.state == GCSatomic)
    gc_mark(g, v);
  else if (o->gch.gct == ~LJ_TTAB)
    lj_gc_barrierback(g, gco2tab(o));
  else
    /* Classic makewhite(o): never paint mark∧GRAY without a worklist.
    ** Under GCMARK makewhite only clears header GRAY. */
    makewhite(g, o);
}

/* Specialized barrier for closed upvalue. Pass &uv->tv. */
void LJ_FASTCALL lj_gc_barrieruv(global_State *g, TValue *tv)
{
#define TV2MARKED(x) \
  (*((uint8_t *)(x) - offsetof(GCupval, tv) + offsetof(GCupval, marked)))
  /* Interpreter/JIT must only call this for GC values, but defend against
  ** non-GC stores (nil/bool/number/lightud): gcV() asserts tvisgcv. */
  if (!tvisgcv(tv))
    return;
  if (g->gc.state == GCSpropagate || g->gc.state == GCSatomic)
    gc_mark(g, gcV(tv));
  else if (LJ_UNLIKELY(gc_obj_isdead(g, gcV(tv))))
    lj_gc_nursery_forbid_white(g, gcV(tv));
  else
    /* Classic makewhite(uv): clear GRAY only — never orphan mark∧GRAY. */
    TV2MARKED(tv) &= (uint8_t)~LJ_GC_GRAY;
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
      /* Open UV marked∧GRAY (P3a) or nursery light-gray born-GRAY: blacken on
      ** close and barrier the closed value if it points to a white child. */
      gray2black(o);
      if (tvisgcv(&uv->tv) && gc_obj_iswhite(g, gcV(&uv->tv)))
	lj_gc_barrierf(g, o, gcV(&uv->tv));
    } else if (LJ_UNLIKELY(g->gc.state == GCSsweep)) {
      /* P3a: a surviving open UV enters closeuv as mark∧GRAY (the legit open-UV
      ** residual) during sweep/nursery. makewhite clears GRAY → pure black;
      ** the mark bit keeps it alive this cycle. Nursery-born light-gray
      ** (!mark, GRAY) also lands here and is cleared the same way. The old
      ** "mark∧GRAY after atomic is a hard bug" assert is gone — under P3a it
      ** is the expected open-UV color at sweep entry. */
      makewhite(g, o);
    } else {
      makewhite(g, o);
      lj_assertG(g->gc.state != GCSfinalize && g->gc.state != GCSpause,
		 "bad GC state");
    }
  } else if (gc_obj_isblack(g, o)) {
    /* Pure-black UV being closed (defensive: a closed UV gray2black'd at mark
    ** time, or an open UV blackened by the prop/atomic branch above in a
    ** prior close — closeuv is normally once-per-UV, so this is a safety net).
    ** P3a: surviving OPEN UVs are NOT gray2black'd at atomic end — they enter
    ** closeuv mark∧GRAY and take the first branch above. Barrier the closed
    ** value if it points to a white child. */
    if (tvisgcv(&uv->tv) && gc_obj_iswhite(g, gcV(&uv->tv)))
      lj_gc_barrierf(g, o, gcV(&uv->tv));
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
#if defined(LUAJIT_ENABLE_MEMPROF)
  if (LJ_UNLIKELY(g->gc.gcmarkflags & GCF_MEMPROF))
    lj_memprof_emit_realloc(L, p, osz, nsz);
#endif
  return p;
}

/* Allocate new GC object and link it to the root set. */
void * LJ_FASTCALL lj_mem_newgco(lua_State *L, GCSize size)
{
  return lj_mem_newgco_arena(L, size, ArenaClass_Trav, 1);
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
  /* During sweep, new huge objects need MARK to survive hugescan. */
  if (LJ_UNLIKELY(g->gc.state == GCSsweep &&
		  lj_arena_ishuge(o)))
    huge_obj_setmark(g, o);
#if defined(LUAJIT_ENABLE_MEMPROF)
  if (LJ_UNLIKELY(g->gc.gcmarkflags & GCF_MEMPROF))
    lj_memprof_emit_alloc(L, o, size, cls, link);
#endif
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

/* -- GC stats instrumentation -------------------------------------------- */

void lj_gc_stats_reset(global_State *g)
{
  memset(&g->gc.stats, 0, sizeof(GCstats));
}

void lj_gc_stats_push(lua_State *L)
{
  global_State *g = G(L);
  GCtab *t = lj_tab_new(L, 0, 6);
  GCstats *s = &g->gc.stats;
#define SETNUM(name, val) do { \
  TValue _k; \
  setstrV(L, &_k, lj_str_newlit(L, name)); \
  setnumV(lj_tab_set(L, t, &_k), (lua_Number)(val)); \
} while (0)
#define SETBOOL(name, val) do { \
  TValue _k; \
  setstrV(L, &_k, lj_str_newlit(L, name)); \
  setboolV(lj_tab_set(L, t, &_k), (val)); \
} while (0)
  SETNUM("steps_propagate", s->nsteps[GCSpropagate]);
  SETNUM("steps_atomic", s->nsteps[GCSatomic]);
  SETNUM("steps_sweepstring", s->nsteps[GCSsweepstring]);
  SETNUM("steps_sweep_bitmap", s->sweep_bitmap_steps);
  SETNUM("steps_sweep_huge", s->sweep_huge_steps);
  SETNUM("steps_finalize", s->nsteps[GCSfinalize]);
  SETNUM("cycles", s->cycles);
  SETNUM("mark_calls", s->mark_calls);
  SETNUM("mark_cost", s->mark_cost);
  SETNUM("hugegray_pops", s->hugegray_pops);
  SETNUM("grayarena_pops", s->grayarena_pops);
  SETNUM("sweep_cells", s->sweep_cells);
  SETNUM("pod_sweeps", s->pod_sweeps);
  SETNUM("sweep_hugescan", s->sweep_hugescan);
  SETNUM("sweep_assert_demote", s->sweep_assert_demote);
  SETNUM("barrierback", s->barrierback);
  SETNUM("gray_notify", s->gray_notify);
  SETNUM("ssb_overflow", s->ssb_overflow);
  SETNUM("arenas_created", s->arenas_created);
  SETNUM("arenas_destroyed", s->arenas_destroyed);
  SETNUM("findspace_calls", s->findspace_calls);
  SETNUM("arenas_shrunk", s->arenas_shrunk);
  SETNUM("huge_allocs", s->huge_allocs);
  SETNUM("huge_frees", s->huge_frees);
  SETNUM("strings_chains_swept", s->strings_chains_swept);
  SETNUM("strings_live_walked", s->strings_live_walked);
  SETNUM("strings_dead_freed", s->strings_dead_freed);
  SETNUM("finalizers", s->finalizers);
  SETNUM("hugeset_rehashes", g->gc.hugesetgen);
  SETNUM("arenastop", g->gc.arenastop);
  SETNUM("hugenum", g->gc.hugenum);
  SETNUM("hugemem", g->gc.hugemem);
  SETNUM("last_cycle_arenastop", s->last_arenastop);
  SETNUM("last_cycle_hugenum", s->last_hugenum);
  SETNUM("last_cycle_hugemem", s->last_hugemem);
#ifdef LUAJIT_ENABLE_GCSTATS_TIMING
  SETNUM("time_pause_ns", s->time_pause_ns);
  SETNUM("time_propagate_ns", s->time_propagate_ns);
  SETNUM("time_atomic_ns", s->time_atomic_ns);
  SETNUM("time_sweepstring_ns", s->time_sweepstring_ns);
  SETNUM("time_sweep_bitmap_ns", s->time_sweep_bitmap_ns);
  SETNUM("time_sweep_huge_ns", s->time_sweep_huge_ns);
  SETNUM("time_finalize_ns", s->time_finalize_ns);
  SETNUM("maxpause_pause_ns", s->maxpause_pause_ns);
  SETNUM("maxpause_propagate_ns", s->maxpause_propagate_ns);
  SETNUM("maxpause_atomic_ns", s->maxpause_atomic_ns);
  SETNUM("maxpause_sweepstring_ns", s->maxpause_sweepstring_ns);
  SETNUM("maxpause_sweep_bitmap_ns", s->maxpause_sweep_bitmap_ns);
  SETNUM("maxpause_sweep_huge_ns", s->maxpause_sweep_huge_ns);
  SETNUM("maxpause_finalize_ns", s->maxpause_finalize_ns);
  SETBOOL("timing", 1);
#else
  SETBOOL("timing", 0);
#endif
#undef SETNUM
#undef SETBOOL
  settabV(L, L->top++, t);
}


#ifdef LUA_USE_ASSERT
/* Assert-only test hook declared in lj_gc.h: single strong definition so
** MSVC does not need selectany on a function (C2496). Counter itself is
** weak/selectany in the header because the inline gc_obj_isdead increments
** it from every TU that includes lj_gc.h. */
uint32_t lj_gc_obj_isdead_nonsweep_hits(void)
{
  return lj_gc_obj_isdead_nonsweep_counter;
}
#endif

#endif

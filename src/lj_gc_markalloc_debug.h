/*
** Markalloc / nursery free-window diagnostics (header-only).
** Include only from lj_gc_arena.c after traverse/propagate helpers.
** Runtime env (when LJ_MARKALLOC_DEBUG=1, default):
**   LUAJIT_MARKALLOC_PROGRESS, DIAG, EDGELOG
** Compile out with -DLJ_MARKALLOC_DEBUG=0.
**
** Product builds do NOT pay O(heap) residual scans: free-window entry is
** sealed by design (permanent-gray threads + open-UV mark∧GRAY + empty
** SSB/gray worklists + graythread_reset). Debug verifies with one survivor
** walk (assert) and optional black→white diagnostics (env-gated).
*/

#ifndef _LJ_GC_MARKALLOC_DEBUG_H
#define _LJ_GC_MARKALLOC_DEBUG_H

#ifndef LJ_MARKALLOC_DEBUG
#define LJ_MARKALLOC_DEBUG	1
#endif

#if LJ_MARKALLOC_DEBUG

#include <stdio.h>
#include <stdlib.h>

static int markalloc_env_truthy(const char *name, int default_on)
{
  const char *v = getenv(name);
  if (v == NULL || v[0] == '\0')
    return default_on;
  if (v[0] == '0' && v[1] == '\0')
    return 0;
  if ((v[0] == 'n' || v[0] == 'N') && (v[1] == '\0' || v[1] == 'o' || v[1] == 'O'))
    return 0;
  if ((v[0] == 'f' || v[0] == 'F') && (v[1] == '\0' || v[1] == 'a' || v[1] == 'A'))
    return 0;
  return 1;
}

#define MARKALLOC_PROGRESS()		markalloc_env_truthy("LUAJIT_MARKALLOC_PROGRESS", 0)
#define MARKALLOC_DIAG()		markalloc_env_truthy("LUAJIT_MARKALLOC_DIAG", 0)
#define MARKALLOC_EDGELOG()		(getenv("LUAJIT_MARKALLOC_EDGELOG") != NULL)

#define MARKALLOC_PROGRESS_LOG(...) \
  do { if (MARKALLOC_PROGRESS()) { fprintf(stderr, __VA_ARGS__); fflush(stderr); } } while (0)
#define MARKALLOC_DIAG_LOG(...) \
  do { if (MARKALLOC_DIAG()) { fprintf(stderr, __VA_ARGS__); fflush(stderr); } } while (0)

#else /* !LJ_MARKALLOC_DEBUG */

#define MARKALLOC_PROGRESS()		0
#define MARKALLOC_DIAG()		0
#define MARKALLOC_EDGELOG()		0
#define MARKALLOC_PROGRESS_LOG(...)	((void)0)
#define MARKALLOC_DIAG_LOG(...)		((void)0)

#endif /* LJ_MARKALLOC_DEBUG */

/* Shared object-edge visitor (Debug assert + markalloc diag). One type switch. */
#if defined(LUA_USE_ASSERT) || LJ_MARKALLOC_DEBUG

typedef void (*GCEdgeChildFn)(global_State *g, void *ud,
			      GCobj *parent, GCobj *child, const char *via);

static void gc_edge_child(global_State *g, GCEdgeChildFn fn, void *ud,
			  GCobj *parent, GCobj *child, const char *via)
{
  if (child != NULL)
    fn(g, ud, parent, child, via);
}

static void gc_edge_tv(global_State *g, GCEdgeChildFn fn, void *ud,
		       GCobj *parent, cTValue *tv, const char *via)
{
  if (tvisgcv(tv))
    gc_edge_child(g, fn, ud, parent, gcV(tv), via);
}

static void gc_edge_visit_obj(global_State *g, GCobj *o,
			      GCEdgeChildFn fn, void *ud)
{
  int gct = o->gch.gct;
  if (gct == ~LJ_TTAB) {
    GCtab *t = gco2tab(o);
    MSize i, asize = t->asize;
    GCtab *mt = tabref(t->metatable);
    if (mt)
      gc_edge_child(g, fn, ud, o, obj2gco(mt), "tab.mt");
    for (i = 0; i < asize; i++)
      gc_edge_tv(g, fn, ud, o, arrayslot(t, i), "tab.arr");
    if (t->hmask > 0) {
      Node *node = noderef(t->node);
      MSize hmask = t->hmask;
      for (i = 0; i <= hmask; i++) {
	Node *n = &node[i];
	if (!tvisnil(&n->val)) {
	  gc_edge_tv(g, fn, ud, o, &n->key, "tab.k");
	  gc_edge_tv(g, fn, ud, o, &n->val, "tab.v");
	}
      }
    }
  } else if (gct == ~LJ_TFUNC) {
    GCfunc *fn_ = gco2func(o);
    MSize i, n;
    gc_edge_child(g, fn, ud, o, obj2gco(tabref(fn_->c.env)), "fn.env");
    if (isluafunc(fn_)) {
      gc_edge_child(g, fn, ud, o, obj2gco(funcproto(fn_)), "fn.proto");
      n = fn_->l.nupvalues;
      for (i = 0; i < n; i++)
	gc_edge_child(g, fn, ud, o, gcref(fn_->l.uvptr[i]), "fn.uv");
    } else {
      n = fn_->c.nupvalues;
      for (i = 0; i < n; i++)
	gc_edge_tv(g, fn, ud, o, &fn_->c.upvalue[i], "fn.cuv");
    }
  } else if (gct == ~LJ_TPROTO) {
    GCproto *pt = gco2pt(o);
    ptrdiff_t i;
    gc_edge_child(g, fn, ud, o, obj2gco(proto_chunkname(pt)), "pt.name");
    for (i = -(ptrdiff_t)pt->sizekgc; i < 0; i++)
      gc_edge_child(g, fn, ud, o, proto_kgc(pt, i), "pt.kgc");
#if LJ_HASJIT
    if (pt->trace) {
      GCobj *tr = obj2gco(traceref(G2J(g), pt->trace));
      gc_edge_child(g, fn, ud, o, tr, "pt.trace");
    }
#endif
  } else if (gct == ~LJ_TUPVAL) {
    GCupval *uv = gco2uv(o);
    gc_edge_tv(g, fn, ud, o, uvval(uv), "uv.tv");
  } else if (gct == ~LJ_TTHREAD) {
    lua_State *th = gco2th(o);
    TValue *tv, *top = th->top;
    for (tv = tvref(th->stack) + 1 + LJ_FR2; tv < top; tv++)
      gc_edge_tv(g, fn, ud, o, tv, "th.stack");
    gc_edge_child(g, fn, ud, o, obj2gco(tabref(th->env)), "th.env");
  } else if (gct == ~LJ_TUDATA) {
    /* Must not name this local `ud`: it would shadow the callback userdata
    ** parameter and gc_edge_child would pass the GCudata* as fn's ud — e.g.
    ** gc_assert_bw_on_child's (*edges)++ then mutates GCudata.nextgc. */
    GCudata *uda = gco2ud(o);
    GCtab *mt = tabref(uda->metatable);
    if (mt)
      gc_edge_child(g, fn, ud, o, obj2gco(mt), "ud.mt");
    gc_edge_child(g, fn, ud, o, obj2gco(tabref(uda->env)), "ud.env");
#if LJ_HASBUFFER
    if (uda->udtype == UDTYPE_BUFFER) {
      SBufExt *sbx = (SBufExt *)uddata(uda);
      if (sbufiscow(sbx) && gcref(sbx->cowref))
	gc_edge_child(g, fn, ud, o, gcref(sbx->cowref), "ud.cow");
      if (gcref(sbx->dict_str))
	gc_edge_child(g, fn, ud, o, gcref(sbx->dict_str), "ud.dict_str");
      if (gcref(sbx->dict_mt))
	gc_edge_child(g, fn, ud, o, gcref(sbx->dict_mt), "ud.dict_mt");
    }
#endif
  }
#if LJ_HASJIT
  else if (gct == ~LJ_TTRACE) {
    GCtrace *T = gco2trace(o);
    IRRef ref;
    if (T->traceno == 0)
      return;
    for (ref = T->nk; ref < REF_TRUE; ref++) {
      IRIns *ir = &T->ir[ref];
      if (ir->o == IR_KGC)
	gc_edge_child(g, fn, ud, o, obj2gco(ir_kgc(ir)), "tr.kgc");
      if (irt_is64(ir->t) && ir->o != IR_KNULL)
	ref++;
    }
    if (T->link)
      gc_edge_child(g, fn, ud, o, obj2gco(traceref(G2J(g), T->link)), "tr.link");
    if (T->nextroot)
      gc_edge_child(g, fn, ud, o, obj2gco(traceref(G2J(g), T->nextroot)), "tr.nextroot");
    if (T->nextside)
      gc_edge_child(g, fn, ud, o, obj2gco(traceref(G2J(g), T->nextside)), "tr.nextside");
    if (gcref(T->startpt))
      gc_edge_child(g, fn, ud, o, gcref(T->startpt), "tr.startpt");
  }
#endif
}

#endif /* LUA_USE_ASSERT || LJ_MARKALLOC_DEBUG */

#if LJ_MARKALLOC_DEBUG

typedef struct MarkallocBWDiag {
  uint64_t edges;
  uint64_t parents;
  uint8_t first_pgct;
  uint8_t first_cgct;
  int verbose;
  uint32_t dump_left;
} MarkallocBWDiag;

static const char *markalloc_gct_name(uint8_t gct)
{
  switch (gct) {
  case 4: return "str";
  case 5: return "upval";
  case 6: return "thread";
  case 7: return "proto";
  case 8: return "func";
  case 9: return "trace";
  case 10: return "cdata";
  case 11: return "tab";
  case 12: return "udata";
  default: return "?";
  }
}

static void markalloc_diag_on_child(global_State *g, void *ud,
				    GCobj *parent, GCobj *child, const char *via)
{
  MarkallocBWDiag *d = (MarkallocBWDiag *)ud;
  if (child == NULL || !gc_obj_iswhite(g, child))
    return;
  if (d->edges == 0) {
    d->first_pgct = parent->gch.gct;
    d->first_cgct = child->gch.gct;
  }
  d->edges++;
  if (d->verbose && d->dump_left > 0) {
    d->dump_left--;
    fprintf(stderr,
	    "[markalloc-edge] P=%s@%p C=%s@%p via=%s Pm=%02x Cm=%02x\n",
	    markalloc_gct_name(parent->gch.gct), (void *)parent,
	    markalloc_gct_name(child->gch.gct), (void *)child,
	    via ? via : "?",
	    (unsigned)parent->gch.marked, (unsigned)child->gch.marked);
  }
}

static void gc_markalloc_diag_obj(global_State *g, MarkallocBWDiag *d, GCobj *o)
{
  uint64_t before = d->edges;
  gc_edge_visit_obj(g, o, markalloc_diag_on_child, d);
  if (d->edges > before) {
    d->parents++;
    if (d->verbose) {
      fprintf(stderr,
	      "[markalloc-parent] %s@%p white_children=%llu\n",
	      markalloc_gct_name(o->gch.gct), (void *)o,
	      (unsigned long long)(d->edges - before));
    }
  }
}

static void gc_markalloc_diag_all(global_State *g, MarkallocBWDiag *d)
{
  GCArena **arenas = mref(g->gc.arenas, GCArena *);
  MSize ai;
  int verbose = d->verbose;
  uint32_t dump_left = d->dump_left;
  memset(d, 0, sizeof(*d));
  d->verbose = verbose;
  d->dump_left = dump_left;
  for (ai = 0; ai < g->gc.arenastop; ai++) {
    GCArena *a = arenas[ai];
    uint32_t w, wtop;
    if (!(a->flags & (ArenaFlag_TravObjs | ArenaFlag_PODOnly |
		      ArenaFlag_UdataOnly)))
      continue;
    lj_arena_flushbins(a);
    if ((GCCellID)a->celltop <= MinCellId)
      continue;
    wtop = arena_blockidx((GCCellID)a->celltop - 1);
    for (w = UnusedBlockWords; w <= wtop; w++) {
      GCBlockword live = a->block[w] & a->mark[w];
      while (live) {
	uint32_t bitidx = lj_ffs(live);
	GCCellID c = (w << 5) + bitidx;
	GCobj *o = (GCobj *)arena_cellptr(a, c);
	live &= live - 1;
	if (arena_cellstate(a, c) < CellState_White)
	  continue;
	gc_markalloc_diag_obj(g, d, o);
      }
    }
  }
  {
    GCRef *slots = mref(g->gc.hugeset, GCRef);
    if (slots != NULL) {
      MSize hi, hmask = g->gc.hugesetmask;
      for (hi = 0; hi <= hmask; hi++) {
	uintptr_t u = gcrefu(slots[hi]);
	GCobj *o;
	if (!hugeset_slot_live(u)) continue;
	o = hugeset_slot_obj(u);
	if (o->gch.gct == ~LJ_TSTR) continue;
	if (!huge_obj_ismarked(g, o)) continue;
	gc_markalloc_diag_obj(g, d, o);
      }
    }
  }
  gc_markalloc_diag_obj(g, d, obj2gco(mainthread(g)));
}

/* Optional black→white diagnostics at free entry (env-gated). No fix path. */
static void gc_markalloc_catchup(global_State *g)
{
  MarkallocBWDiag d;
  int always_log = MARKALLOC_DIAG();
  int edgelog = MARKALLOC_EDGELOG();

  if (!always_log && !edgelog)
    return;

  memset(&d, 0, sizeof(d));
  d.verbose = always_log || edgelog;
  d.dump_left = 64;
  gc_markalloc_diag_all(g, &d);
  if (always_log || d.edges != 0) {
    fprintf(stderr,
	    "[markalloc-diag] black→white edges=%llu parents=%llu "
	    "first_parent=%s first_child=%s gc.state=%u\n",
	    (unsigned long long)d.edges,
	    (unsigned long long)d.parents,
	    markalloc_gct_name(d.first_pgct),
	    markalloc_gct_name(d.first_cgct),
	    (unsigned)g->gc.state);
    fflush(stderr);
  }
}

#else /* !LJ_MARKALLOC_DEBUG */

static void gc_markalloc_catchup(global_State *g)
{
  UNUSED(g);
}

#endif /* LJ_MARKALLOC_DEBUG */

/* Atomic→free seal (Debug only). O(1) worklist emptiness is asserted in
** atomic() via lj_assert_check; this walk only checks residual mark∧GRAY
** and black→white edges on survivors.
**
** P3a residual policy: zero mark∧GRAY on every marked cell EXCEPT open
** upvalues and arena THREADs. Open UVs are deliberately left mark∧GRAY
** (classic-aligned: see gc_mark UPVAL in lj_gc_arena.c) — their value
** aliases a stack slot re-marked by gc_atomic_rescan_threads, the UV
** header is re-closed by lj_gc_closeuv (gray2black on close during
** prop/atomic; makewhite during sweep/nursery), and open UVs are owned
** by Path L (lj_state_free → closeuv) / Path F (freeall_openuv) — bitmap
** sweep skips !closed. Arena THREADs are deliberately left mark∧GRAY
** (permanent-gray: see propagatemark THREAD branch) — stack slots
** cannot pay write barriers, so a thread is never pure black; the
** graythread list enumerates threads for atomic stack rescan.
** The arena walk below skips the residual-GRAY assert for both open UV
** and THREAD; for THREAD it still edge-walks stack children (open UV
** skips the edge walk — its value is covered by the thread's rescan).
** The huge walk does not need a UV exclusion (UVs are never huge — see
** lj_gc_arena.c "huge upvalue is impossible"); mainthread is a thread,
** not a UV, and is checked separately (forbids GRAY). */
#if defined(LUA_USE_ASSERT)

static void gc_assert_bw_on_child(global_State *g, void *ud,
				  GCobj *parent, GCobj *child, const char *via)
{
  uint64_t *edges = (uint64_t *)ud;
  (*edges)++;
  /* Freelist cells stay ASAN-poisoned; after a full flushbins they read as
  ** Free (block=0). Do not touch gct/marked on free cells — that is a UAP
  ** under ASAN and means black→dead, not black→white. */
  if (child != obj2gco(mainthread(g)) && child != obj2gco(&g->strempty) &&
      !lj_arena_ishuge(child)) {
    GCArena *a = ptr2arena(child);
    GCCellID c = ptr2cell(child);
    if (a->id < g->gc.arenastop &&
	mref(g->gc.arenas, GCArena *)[a->id] == a &&
	arena_cellstate(a, c) < CellState_White) {
      lj_assertG(0,
		 "atomic black→free: parent gct=%d p=%p child=%p via=%s",
		 parent->gch.gct, (void *)parent, (void *)child,
		 via ? via : "?");
      return;
    }
  }
  if (gc_obj_iswhite(g, child)) {
    /* F3: cdata fin lives in registry, not FFI_FIN keys — no residual. */
    lj_assertG(0,
	       "atomic black→white: parent gct=%d p=%p child gct=%d c=%p via=%s",
	       parent->gch.gct, (void *)parent, child->gch.gct, (void *)child,
	       via ? via : "?");
  }
}

static void gc_assert_bw_obj(global_State *g, GCobj *o, uint64_t *edges)
{
  gc_edge_visit_obj(g, o, gc_assert_bw_on_child, edges);
}

static void gc_assert_atomic_end(global_State *g)
{
  GCArena **arenas = mref(g->gc.arenas, GCArena *);
  MSize ai;
  uint64_t black_parents = 0, edges_checked = 0, residual_gray = 0;

  /* Flush EVERY arena before any edge walk. Binned freelist cells look like
  ** White (block=1,mark=0) until flush; edge targets in not-yet-flushed
  ** arenas would still be poisoned → ASAN UAP on gct reads. */
  for (ai = 0; ai < g->gc.arenastop; ai++)
    lj_arena_flushbins(arenas[ai]);

  for (ai = 0; ai < g->gc.arenastop; ai++) {
    GCArena *a = arenas[ai];
    uint32_t w, wtop;
    int is_nontrav_str = !(a->flags & (ArenaFlag_TravObjs | ArenaFlag_PODOnly |
				       ArenaFlag_UdataOnly
#if LJ_HASFFI
				       | ArenaFlag_CdataVOnly
#endif
			 ));
    if (!(a->flags & (ArenaFlag_TravObjs | ArenaFlag_PODOnly |
		      ArenaFlag_UdataOnly
#if LJ_HASFFI
		      | ArenaFlag_CdataVOnly
#endif
	 )) && !is_nontrav_str)
      continue;
    if ((GCCellID)a->celltop <= MinCellId)
      continue;
    wtop = arena_blockidx((GCCellID)a->celltop - 1);
    for (w = UnusedBlockWords; w <= wtop; w++) {
      GCBlockword live = a->block[w] & a->mark[w];
      while (live) {
	uint32_t bitidx = lj_ffs(live);
	GCCellID c = (w << 5) + bitidx;
	GCobj *o;
	live &= live - 1;
	if (arena_cellstate(a, c) < CellState_White)
	  continue;
#if LJ_HASFFI
	if (a->flags & ArenaFlag_CdataVOnly) {
	  char *p = (char *)arena_cellptr(a, c);
	  o = obj2gco((GCcdata *)(p + ((GCcdataVar *)p)->offset));
	} else
#endif
	  o = (GCobj *)arena_cellptr(a, c);
	/* Class routing (front-loaded from fullgc gc_arena_verify): at
	** atomic→sweep marks are authoritative for survivors. */
	if (a->flags & ArenaFlag_UdataOnly) {
	  lj_assertG(o->gch.gct == ~LJ_TUDATA,
		     "non-udata in Udata arena at atomic: gct=%d p=%p",
		     (int)o->gch.gct, (void *)o);
	} else if (is_nontrav_str) {
	  lj_assertG(o->gch.gct == ~LJ_TSTR,
		     "non-string in NonTrav arena at atomic: gct=%d p=%p",
		     (int)o->gch.gct, (void *)o);
	} else if (a->flags & ArenaFlag_TravObjs) {
	  lj_assertG(o->gch.gct != ~LJ_TUDATA,
		     "udata in non-Udata Trav arena at atomic: p=%p",
		     (void *)o);
	}
	/* NonTrav strings: mark bits only — no residual-GRAY / edge walk. */
	if (is_nontrav_str)
	  continue;
	if (o->gch.marked & LJ_GC_GRAY) {
	  /* P3a (classic-aligned): open UV AND arena THREAD are the legit
	  ** mark∧GRAY residuals. Open UV is left gray (value aliases a stack
	  ** slot covered by the thread's edge walk / rescan); skip the assert
	  ** AND the edge walk. THREAD is permanent-gray (stack slots cannot
	  ** pay write barriers — see propagatemark THREAD branch in
	  ** lj_gc_arena.c); skip the assert but STILL edge-walk its stack
	  ** children (coverage not proven — rescan re-marks slots but does
	  ** not prove all child edges are black). Closed UV, mainthread, and
	  ** every other survivor must still be pure black. mainthread is
	  ** checked separately below and forbids GRAY. */
	  int is_openuv = (o->gch.gct == ~LJ_TUPVAL && !gco2uv(o)->closed);
	  int is_thread = (o->gch.gct == ~LJ_TTHREAD);
	  if (is_openuv)
	    continue;
	  if (is_thread) {
	    black_parents++;
	    gc_assert_bw_obj(g, o, &edges_checked);
	    continue;
	  }
	  residual_gray++;
	  lj_assertG(0,
		     "mark∧GRAY at atomic→sweep: gct=%d marked=0x%02x p=%p",
		     o->gch.gct, o->gch.marked, (void *)o);
	  continue;
	}
	black_parents++;
	gc_assert_bw_obj(g, o, &edges_checked);
      }
    }
  }

  {
    GCRef *slots = mref(g->gc.hugeset, GCRef);
    if (slots != NULL) {
      MSize hi, hmask = g->gc.hugesetmask;
      for (hi = 0; hi <= hmask; hi++) {
	uintptr_t u = gcrefu(slots[hi]);
	GCobj *o;
	if (!hugeset_slot_live(u) || !(u & HUGESET_MARK))
	  continue;
	o = hugeset_slot_obj(u);
	/* No open-UV exclusion here: UVs are never huge (lj_gc_arena.c asserts
	** "huge upvalue is impossible"). THREAD exclusion mirrors the arena
	** walk: a huge thread would also be permanent-gray, but threads are
	** always arena-allocated (graythread asserts gc_inarena), so this
	** branch is defensive only. */
	if (o->gch.marked & LJ_GC_GRAY) {
	  if (o->gch.gct == ~LJ_TTHREAD) {
	    black_parents++;
	    gc_assert_bw_obj(g, o, &edges_checked);
	    continue;
	  }
	  residual_gray++;
	  lj_assertG(0,
		     "mark∧GRAY huge at atomic→sweep: gct=%d marked=0x%02x p=%p",
		     o->gch.gct, o->gch.marked, (void *)o);
	  continue;
	}
	black_parents++;
	gc_assert_bw_obj(g, o, &edges_checked);
      }
    }
  }

  {
    GCobj *mt = obj2gco(mainthread(g));
    if (mt->gch.marked & LJ_GC_GRAY) {
      residual_gray++;
      lj_assertG(0,
		 "mark∧GRAY mainthread at atomic→sweep: marked=0x%02x",
		 mt->gch.marked);
    } else {
      black_parents++;
      gc_assert_bw_obj(g, mt, &edges_checked);
    }
  }

  MARKALLOC_DIAG_LOG(
	  "[markalloc-diag] atomic_end ok residual_gray=%llu "
	  "black_parents=%llu edges_checked=%llu\n",
	  (unsigned long long)residual_gray,
	  (unsigned long long)black_parents,
	  (unsigned long long)edges_checked);
}

#else /* !LUA_USE_ASSERT */

static void gc_assert_atomic_end(global_State *g)
{
  UNUSED(g);
}

#endif /* LUA_USE_ASSERT */

#endif /* _LJ_GC_MARKALLOC_DEBUG_H */

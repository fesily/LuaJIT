/*
** Garbage collector.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
**
** Major portions taken verbatim or adapted from the Lua interpreter.
** Copyright (C) 1994-2008 Lua.org, PUC-Rio. See Copyright Notice in lua.h
*/

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
#if LJ_HASGCARENA
#include "lj_arena.h"
#endif

#define GCSTEPSIZE	1024u
#define GCSWEEPMAX	40
#define GCSWEEPCOST	10
#define GCFINALIZECOST	100

/* Macros to set GCobj colors and flags. */
#if LJ_HASGCMARK
#define white2gray(x) \
  ((x)->gch.marked = ((x)->gch.marked & (uint8_t)~LJ_GC_WHITES) | LJ_GC_GRAY)
#define gray2black(x) \
  ((x)->gch.marked &= (uint8_t)~LJ_GC_GRAY)
#else
#define white2gray(x)		((x)->gch.marked &= (uint8_t)~LJ_GC_WHITES)
#define gray2black(x)		((x)->gch.marked |= LJ_GC_BLACK)
#endif
#define isfinalized(u)		((u)->marked & LJ_GC_FINALIZED)

/* -- Mark phase ---------------------------------------------------------- */

#if LJ_HASGCMARK
/* Mainthread and strempty live in dlmalloc, not arenas.
** Huge GC objects (>= ArenaHugeThreshold) are arena-aligned allocations
** without arena metadata — caught by lj_arena_ishuge. */
#define gc_inarena(g, o)  \
  (!lj_arena_ishuge(o) && \
   (o) != obj2gco(mainthread(g)) && (o) != obj2gco(&(g)->strempty))
/* Fast in-arena check for gc_mark hot path. Avoids two pointer comparisons
** by using SFIXED bit: only mainthread and strempty have it (both set in
** lj_state.c init + atomic). Huge blocks need the address-based check. */
#define gc_mark_inarena(o) \
  (!lj_arena_ishuge(o) && !((o)->gch.marked & LJ_GC_SFIXED))
#endif

/* Mark a TValue (if needed). */
#if LJ_HASGCMARK
/* Under quad-color, makewhite produces pure white (no GRAY, no WHITE1 when
** curwhite=0 during sweep). gc_mark has its own arena_obj_ismarked / BLACK
** early-return, so we call it unconditionally for GC values. */
#define gc_marktv(g, tv) \
  { if (tvisgcv(tv)) gc_mark(g, gcV(tv)); }
#else
#define gc_marktv(g, tv) \
  { if (tviswhite(tv)) gc_mark(g, gcV(tv)); }
#endif

/* Mark a GCobj (if needed). */

#if LJ_HASGCMARK
#define gc_markobj(g, o)	gc_mark(g, obj2gco(o))
#else
#define gc_markobj(g, o) \
  { if (iswhite(obj2gco(o))) gc_mark(g, obj2gco(o)); }
#endif

/* Mark a string object. */
#if LJ_HASGCMARK
#define gc_mark_str(g, s) do { \
  if (gc_inarena(g, obj2gco(s))) \
    arena_obj_setmark(ptr2arena(s), ptr2cell(s)); \
  else { \
    (s)->marked &= (uint8_t)~LJ_GC_WHITES; \
    (s)->marked |= LJ_GC_BLACK; \
  } \
  } while (0)
#else
#define gc_mark_str(g, s)	((s)->marked &= (uint8_t)~LJ_GC_WHITES)
#endif

#if LJ_HASGCMARK
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
#endif

/* Mark a GCobj. */
static void gc_mark(global_State *g, GCobj *o)
{
  int gct = o->gch.gct;
#if LJ_HASGCMARK
  int inarena = gc_mark_inarena(o);
  GCArena *a;
  GCCellID c;
  if (inarena) {
    a = ptr2arena(o);
    c = ptr2cell(o);
    if (arena_obj_ismarked(a, c))
      return;
  } else if (!iswhite(o)) {
    if (o->gch.marked & LJ_GC_BLACK)
      return;
  }
#endif
#if LJ_HASGCMARK
  lj_assertG(inarena || iswhite(o) || isgray(o) ||
	     !(o->gch.marked & (LJ_GC_WHITES|LJ_GC_BLACK|LJ_GC_GRAY)),
	     "mark of already-black non-arena object");
#else
  lj_assertG(iswhite(o) || isgray(o), "mark of non-white/gray object");
#endif
  white2gray(o);
#if LJ_HASGCMARK
  if (inarena)
    arena_obj_setmark(a, c);
  else
    o->gch.marked |= LJ_GC_BLACK;
#endif
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
#if LJ_HASGCMARK
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
#else
    {
      setgcrefr(o->gch.gclist, g->gc.gray);
      setgcref(g->gc.gray, o);
    }
#endif
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
#if LJ_HASGCMARK
  gc_hugegray_reset(g);
  gc_graythread_reset(g);
  gc_weak_reset(g);
#else
  setgcrefnull(g->gc.gray);
  setgcrefnull(g->gc.grayagain);
  setgcrefnull(g->gc.weak);
#endif
#if LJ_HASGCMARK
  /* Restore currentwhite for the new cycle. After sweep, currentwhite had
  ** no WHITE1 bit (flipped at atomic). Restore it so curwhite()=WHITE1
  ** and newwhite()=WHITE1|GRAY for newly allocated objects. */
  g->gc.currentwhite = LJ_GC_WHITES | LJ_GC_FIXED;
  g->gc.grayastop = 0;
  setmref(g->gc.ssbtop, mref(g->gc.ssb, GCobj *));
  {
    MSize i;
    for (i = 0; i < g->gc.arenastop; i++) {
      GCArena *a = mref(g->gc.arenas, GCArena *)[i];
      if (mref(a->greybase, GCCellID1) != NULL)
	arena_gray_reset(a);
    }
  }
  lj_arena_gc_markinit(g);
#endif
  gc_markobj(g, mainthread(g));
  gc_markobj(g, tabref(mainthread(g)->env));
  gc_markobj(g, vmthread(g));
  gc_marktv(g, &g->registrytv);
  gc_mark_gcroot(g);
#if LJ_HASGCMARK
  gc_traverse_mainthread(g);
#endif
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
      makewhite(g, u);  /* Could be from previous GC. */
      gc_mark(g, u);
    } while (u != root);
  }
}

/* Separate userdata objects to be finalized to mmudata list. */
size_t lj_gc_separateudata(global_State *g, int all)
{
  size_t m = 0;
  GCRef *p = &mainthread(g)->nextgc;
  GCobj *o;
  while ((o = gcref(*p)) != NULL) {
    if (!(iswhite(o) || all) || isfinalized(gco2ud(o))) {
      p = &o->gch.nextgc;  /* Nothing to do. */
    } else if (!lj_meta_fastg(g, tabref(gco2ud(o)->metatable), MM_gc)) {
      markfinalized(o);  /* Done, as there's no __gc metamethod. */
      p = &o->gch.nextgc;
    } else {  /* Otherwise move userdata to be finalized to mmudata list. */
      m += sizeudata(gco2ud(o));
      markfinalized(o);
      *p = o->gch.nextgc;
      if (gcref(g->gc.mmudata)) {  /* Link to end of mmudata list. */
	GCobj *root = gcref(g->gc.mmudata);
	setgcrefr(o->gch.nextgc, root->gch.nextgc);
	setgcref(root->gch.nextgc, o);
	setgcref(g->gc.mmudata, o);
      } else {  /* Create circular list. */
	setgcref(o->gch.nextgc, o);
	setgcref(g->gc.mmudata, o);
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
#if LJ_HASGCMARK
	gc_weak_push(g, obj2gco(t), weak);
#else
	setgcrefr(t->gclist, g->gc.weak);
	setgcref(g->gc.weak, obj2gco(t));
#endif
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
  if (iswhite(o)) {
    white2gray(o);
#if LJ_HASGCMARK
    if (gc_inarena(g, o)) {
      GCArena *a = ptr2arena(o);
      GCCellID c = ptr2cell(o);
      arena_obj_setmark(a, c);
      arena_gray_push(g, a, (GCCellID1)c);
    } else {
      o->gch.marked |= LJ_GC_BLACK;
      lj_assertG(lj_arena_ishuge(o), "non-arena trace is not huge");
      gc_hugegray_push(g, o);
    }
#else
    setgcrefr(o->gch.gclist, g->gc.gray);
    setgcref(g->gc.gray, o);
#endif
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
#if LJ_HASGCMARK
  /* Under single-white, always clear above top — not just at atomic.
  ** With gc_marktv accepting gray objects, stale stack slots from
  ** previous frames would otherwise keep dead objects alive. */
  {
    TValue *stend = tvref(th->stack) + th->stacksize;
    for (; o < stend; o++)
      setnilV(o);
  }
#else
  if (g->gc.state == GCSatomic) {
    top = tvref(th->stack) + th->stacksize;
    for (; o < top; o++)  /* Clear unmarked slots. */
      setnilV(o);
  }
#endif
  gc_markobj(g, tabref(th->env));
  lj_state_shrinkstack(th, gc_traverse_frames(g, th));
}

#if LJ_HASGCMARK
/* Traverse the host-allocated main thread directly: it is SFIXED and cannot
** live in an arena or on a per-arena gray stack. */
static void gc_traverse_mainthread(global_State *g)
{
  gc_traverse_thread(g, mainthread(g));
}
#endif

/* Propagate one gray object. Traverse it and turn it black. */
static size_t propagatemark(global_State *g
#if LJ_HASGCMARK
  , GCobj *o
#endif
)
{
#if !LJ_HASGCMARK
  GCobj *o = gcref(g->gc.gray);
#endif
  int gct = o->gch.gct;
  lj_assertG(isgray(o), "propagation of non-gray object");
#if LJ_HASGCMARK
  lj_assertG(o->gch.marked & LJ_GC_GRAY,
    "gray object missing gray bit: gct=%d marked=0x%02x ptr=%p state=%d",
    o->gch.gct, o->gch.marked, (void*)o, g->gc.state);
  lj_assertG(!(o->gch.marked & LJ_GC_BLACK) || !gc_inarena(g, o),
    "arena object has header BLACK bit: ptr=%p gct=%d marked=0x%02x",
    (void*)o, o->gch.gct, o->gch.marked);
#else
  setgcrefr(g->gc.gray, o->gch.gclist);  /* Remove from gray list. */
#endif
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
#if LJ_HASGCMARK
    gc_graythread_push(g, o);
#else
    setgcrefr(th->gclist, g->gc.grayagain);
    setgcref(g->gc.grayagain, o);
#endif
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

#if LJ_HASGCMARK
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
    /* Stale entry — remove from heap. */
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
#endif

/* Propagate all gray objects. */
static size_t gc_propagate_gray(global_State *g)
{
  size_t m = 0;
#if LJ_HASGCMARK
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
#else
  while (gcref(g->gc.gray) != NULL)
    m += propagatemark(g);
#endif
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
  int ow = otherwhite(g);
  GCobj *o;
  while ((o = gcref(*p)) != NULL && lim-- > 0) {
    if (o->gch.gct == ~LJ_TTHREAD)  /* Need to sweep open upvalues, too. */
      gc_fullsweep(g, &gco2th(o)->openupval);
#if LJ_HASGCMARK
    if ((g->gc.gcmarkflags & GCF_BITMAPSWEEP) && !lj_arena_ishuge(o) &&
	o != obj2gco(mainthread(g))) {
      if ((o->gch.marked & LJ_GC_FIXED) ||
	  arena_obj_ismarked(ptr2arena(o), ptr2cell(o))) {
	makewhite(g, o);
	p = &o->gch.nextgc;
      } else {
	setgcrefr(*p, o->gch.nextgc);
	if (o == gcref(g->gc.root))
	  setgcrefr(g->gc.root, o->gch.nextgc);
	gc_freefunc[o->gch.gct - ~LJ_TSTR](g, o);
      }
      continue;
    }
#endif
    if (((o->gch.marked ^ LJ_GC_WHITES) & ow)) {  /* Black or current white? */
      lj_assertG(!isdead(g, o) || (o->gch.marked & LJ_GC_FIXED),
		 "sweep of undead object");
      makewhite(g, o);  /* Value is alive, change to the current white. */
      p = &o->gch.nextgc;
    } else {  /* Otherwise value is dead, free it. */
      lj_assertG(isdead(g, o) || ow == LJ_GC_SFIXED
		 || (LJ_HASGCMARK && !iswhite(o)),
		 "sweep of unlive object");
      setgcrefr(*p, o->gch.nextgc);
      if (o == gcref(g->gc.root))
	setgcrefr(g->gc.root, o->gch.nextgc);  /* Adjust list anchor. */
      gc_freefunc[o->gch.gct - ~LJ_TSTR](g, o);
    }
  }
  return p;
}

/* Sweep one string interning table chain. Preserves hashalg bit. */
static void gc_sweepstr(global_State *g, GCRef *chain)
{
  /* Mask with other white and LJ_GC_FIXED. Or LJ_GC_SFIXED on shutdown. */
  int ow = otherwhite(g);
  uintptr_t u = gcrefu(*chain);
  GCRef q;
  GCRef *p = &q;
  GCobj *o;
  setgcrefp(q, (u & ~(uintptr_t)1));
  while ((o = gcref(*p)) != NULL) {
#if LJ_HASGCMARK
    if ((g->gc.gcmarkflags & GCF_BITMAPSWEEP) && !lj_arena_ishuge(o) &&
	o != obj2gco(&g->strempty)) {
      if ((o->gch.marked & LJ_GC_FIXED) ||
	  arena_obj_ismarked(ptr2arena(o), ptr2cell(o))) {
	makewhite(g, o);
	p = &o->gch.nextgc;
      } else {
	setgcrefr(*p, o->gch.nextgc);
	lj_str_free(g, gco2str(o));
      }
      continue;
    }
#endif
    if (((o->gch.marked ^ LJ_GC_WHITES) & ow)) {  /* Black or current white? */
      lj_assertG(!isdead(g, o) || (o->gch.marked & LJ_GC_FIXED),
		 "sweep of undead string");
      makewhite(g, o);  /* String is alive, change to the current white. */
      p = &o->gch.nextgc;
    } else {  /* Otherwise string is dead, free it. */
      lj_assertG(isdead(g, o) || ow == LJ_GC_SFIXED
		 || (LJ_HASGCMARK && !iswhite(o)),
		 "sweep of unlive string: marked=0x%02x ow=0x%02x cw=0x%02x",
		 o->gch.marked, ow, g->gc.currentwhite);
      setgcrefr(*p, o->gch.nextgc);
      lj_str_free(g, gco2str(o));
    }
  }
  setgcrefp(*chain, (gcrefu(q) | (u & 1)));
}

#if LJ_HASGCMARK
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

  while (ai < g->gc.arenastop && freed < GCSWEEPMAX) {
    GCArena *a = arenas[ai];
    uint32_t wtop;
    if (!(a->flags & ArenaFlag_TravObjs)) {
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
	/* Under HASGCMARK, gray-only objects (from sweep makewhite with
	** curwhite=0) may not pass isdead() but are genuinely dead
	** when the bitmap says block=1, mark=0. */
	lj_assertG(isdead(g, o) || (o->gch.marked & LJ_GC_FIXED)
		   || (LJ_HASGCMARK && !iswhite(o)),
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
  if (ai >= g->gc.arenastop)
    g->gc.sweepphase = SweepPhase_Rebuild;
  return freed;
}

/*
** Post-sweep pass: makewhite surviving objects, rebuild the udata sub-chain
** (mainthread->nextgc) for lj_gc_separateudata, sweep each thread's open
** upvalue list, free dead huge objects, and clear arena mark bits.
**
** The root chain (g->gc.root) is NOT rebuilt: all former consumers now
** enumerate arena objects via the block bitmaps directly. The root reference
** is simply anchored on the (super-fixed) main thread.
*/
static void gc_rebuild_rootchain(global_State *g)
{
  GCArena **arenas = mref(g->gc.arenas, GCArena *);
  GCRef newud;
  GCRef *udtail = &newud;
  MSize i;

  setgcrefnull(newud);

#if LJ_HASFFI
  /* Sweep VLA cdata on their separate chain.  These live in non-trav arenas
  ** where bitmap scanning can't read gct (GCcdataVar prefix).  The chain
  ** is never corrupted by bitmap sweep since it's independent of gc.root.
  ** Survivors stay on cdatavroot, never on gc.root. */
  {
    GCRef newcdatav;
    GCRef *cdatavtail = &newcdatav;
    GCobj *o = gcref(g->gc.cdatavroot);
    setgcrefnull(newcdatav);
    while (o != NULL) {
      GCobj *next = gcnext(o);
      if (iswhite(o)) {
	gc_freefunc[o->gch.gct - ~LJ_TSTR](g, o);
      } else {
	makewhite(g, o);
	setgcref(*cdatavtail, o);
	cdatavtail = &o->gch.nextgc;
      }
      o = next;
    }
    setgcrefnull(*cdatavtail);
    setgcrefr(g->gc.cdatavroot, newcdatav);
  }
#endif

  /* Clear mark bits for udata on the mmudata ring so the bitmap scan
  ** below won't re-link them.  gc_finalize manages their lifecycle. */
  if (gcref(g->gc.mmudata)) {
    GCobj *root = gcref(g->gc.mmudata);
    GCobj *u = root;
    do {
      u = gcnext(u);
      if (!lj_arena_ishuge(u))
	arena_obj_clearmark(ptr2arena(u), ptr2cell(u));
    } while (u != root);
  }

  for (i = 0; i < g->gc.arenastop; i++) {
    GCArena *a = arenas[i];
    uint32_t w, wtop;
    if (!(a->flags & ArenaFlag_TravObjs)) continue;
    lj_arena_flushbins(a);
    wtop = arena_blockidx((GCCellID)a->celltop - 1);
    for (w = UnusedBlockWords; w <= wtop; w++) {
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
	makewhite(g, o);
	if (o->gch.gct == ~LJ_TUDATA) {
	  setgcref(*udtail, o);
	  udtail = &o->gch.nextgc;
	} else if (o->gch.gct == ~LJ_TTHREAD) {
	  gc_fullsweep(g, &gco2th(o)->openupval);
	}
      }
    }
  }

  /* Huge objects have no cell bitmap, so the arena scan above can't see them.
  ** Scan the address-keyed huge set instead: free the dead (white), makewhite
  ** survivors and re-link udata onto the udata sub-chain.
  **
  ** Huge STRINGS are excluded here: they are interned and fully owned by
  ** gc_sweepstr, which runs in the earlier GCSsweepstring phase and already
  ** handles huge strings via their header mark (gc_sweepstr's bitmap branch
  ** is gated on !lj_arena_ishuge). A live huge string is therefore already
  ** white by the time we get here -- treating it as dead would double-free.
  ** Upvalues are never huge (fixed small size). */
  {
    GCRef *slots = mref(g->gc.hugeset, GCRef);
    if (slots != NULL) {
      MSize hi, hmask = g->gc.hugesetmask;
      for (hi = 0; hi <= hmask; hi++) {
	uintptr_t u = gcrefu(slots[hi]);
	GCobj *o;
	if (u == 0 || u == 1) continue;  /* HUGESET_EMPTY / HUGESET_TOMB. */
	o = (GCobj *)u;
	if (o->gch.gct == ~LJ_TSTR) continue;  /* Owned by gc_sweepstr. */
	lj_assertG(o->gch.gct != ~LJ_TUPVAL, "huge upvalue is impossible");
	if (iswhite(o)) {
	  /* Dead: gc_freefunc -> lj_hugeblock_free tombstones this slot. */
	  gc_freefunc[o->gch.gct - ~LJ_TSTR](g, o);
	} else {
	  makewhite(g, o);
	  if (o->gch.gct == ~LJ_TUDATA) {
	    setgcref(*udtail, o);
	    udtail = &o->gch.nextgc;
	  } else if (o->gch.gct == ~LJ_TTHREAD) {
	    gc_fullsweep(g, &gco2th(o)->openupval);
	  }
	}
      }
    }
  }

  /* Terminate the udata sub-chain with NULL. mainthread->nextgc points
  ** to the udata-only sub-chain, which lj_gc_separateudata walks. */
  setgcrefnull(*udtail);
  setgcrefr(mainthread(g)->nextgc, newud);

  /* Anchor the root reference on mainthread. No other objects are chained. */
  makewhite(g, obj2gco(mainthread(g)));
  gc_fullsweep(g, &mainthread(g)->openupval);
  setgcref(g->gc.root, obj2gco(mainthread(g)));

  /* Second pass: clear mark bits on all arenas now that openupval sweeps
  ** are done and no longer need to read them. */
  for (i = 0; i < g->gc.arenastop; i++) {
    GCArena *a = arenas[i];
    uint32_t w, wtop = arena_blockidx((GCCellID)a->celltop - 1);
    for (w = UnusedBlockWords; w <= wtop; w++)
      a->mark[w] &= ~a->block[w];
  }

  g->gc.sweepphase = SweepPhase_Done;

}
#endif

/* Check whether we can clear a key or a value slot from a table. */
static int gc_mayclear(global_State *g, cTValue *o, int val)
{
  if (tvisgcv(o)) {  /* Only collectable objects can be weak references. */
    if (tvisstr(o)) {  /* But strings cannot be used as weak references. */
      gc_mark_str(g, strV(o));  /* And need to be marked. */
      return 0;
    }
    if (iswhite(gcV(o)))
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

#if !LJ_HASGCMARK
/* Clear collected entries from weak tables in a gclist (classic GC). */
static void gc_clearweak(global_State *g, GCobj *o)
{
  while (o) {
    GCtab *t = gco2tab(o);
    gc_clearweak_tab(g, t);
    o = gcref(t->gclist);
  }
}
#endif

#if LJ_HASGCMARK
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
#endif

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
    setgcrefr(o->gch.nextgc, g->gc.root);
    setgcref(g->gc.root, o);
    makewhite(g, o);
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
  /* Add userdata back to the main userdata list and make it white. */
  setgcrefr(o->gch.nextgc, mainthread(g)->nextgc);
  setgcref(mainthread(g)->nextgc, o);
  makewhite(g, o);
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
      makewhite(g, o);
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
  g->gc.currentwhite = LJ_GC_WHITES | LJ_GC_SFIXED;
#if LJ_HASGCMARK
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
  **  - Non-traversable arenas: only strings (freed via the intern table) and
  **    VLA cdata (freed via cdatavroot). Their gct cannot be read from a
  **    bitmap cell, so these arenas are skipped here.
  **  - Huge objects: no cell bitmap; freed via the address-keyed huge set.
  */
  {
    GCArena **arenas = mref(g->gc.arenas, GCArena *);
#if LJ_HASFFI
    /* VLA cdata first: freeing a huge VLA cdata tombstones its huge-set slot,
    ** so the huge-set scan below won't see (and double-free) it. */
    gc_fullsweep(g, &g->gc.cdatavroot);
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
    /* Huge non-string objects: walk the huge set (no cell bitmap exists for
    ** them). Strings are owned by gc_sweepstr; VLA cdata were freed above. */
    {
      GCRef *slots = mref(g->gc.hugeset, GCRef);
      if (slots != NULL) {
	MSize hi, hmask = g->gc.hugesetmask;
	for (hi = 0; hi <= hmask; hi++) {
	  uintptr_t u = gcrefu(slots[hi]);
	  GCobj *o;
	  if (u == 0 || u == 1) continue;  /* HUGESET_EMPTY / HUGESET_TOMB. */
	  o = (GCobj *)u;
	  if (o->gch.gct == ~LJ_TSTR) continue;  /* Owned by gc_sweepstr. */
	  gc_freefunc[o->gch.gct - ~LJ_TSTR](g, o);
	}
      }
    }
    /* Re-anchor the 根 reference on the (super-fixed) main thread: every other
    ** object is gone, and the stale chain must never be walked again. */
    setgcrefnull(mainthread(g)->nextgc);
    setgcref(g->gc.root, obj2gco(mainthread(g)));
  }
#else
  gc_fullsweep(g, &g->gc.root);
#endif
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

#if LJ_HASGCMARK
  gc_weak_redirect_all(g);  /* Redirect weak tables to arena/huge gray stacks. */
#else
  setgcrefr(g->gc.gray, g->gc.weak);  /* Empty the list of weak tables. */
  setgcrefnull(g->gc.weak);
#endif
  lj_assertG(!iswhite(obj2gco(mainthread(g))), "main thread turned white");
  gc_markobj(g, L);  /* Mark running thread. */
#if LJ_HASGCMARK
  gc_traverse_mainthread(g);  /* Stack slots have no barriers. */
#endif
  gc_traverse_curtrace(g);  /* Traverse current trace. */
  gc_mark_gcroot(g);  /* Mark GC roots (again). */
  gc_propagate_gray(g);  /* Propagate all of the above. */

#if LJ_HASGCMARK
  lj_gc_ssb_flush(g);  /* Drain SSB into per-arena gray stacks. */
  /* Drain graythread (thread objects only): redirect arena threads to arena
  ** gray stacks for the atomic re-scan. The mainthread is handled directly. */
  while (!gc_graythread_empty(g)) {
    GCobj *o = gc_graythread_pop(g);
    lj_assertG(o->gch.gct == ~LJ_TTHREAD, "graythread contains non-thread");
    lj_assertG(gc_inarena(g, o), "non-arena thread in graythread");
    arena_gray_push(g, ptr2arena(o), (GCCellID1)ptr2cell(o));
  }
#else
  setgcrefr(g->gc.gray, g->gc.grayagain);  /* Empty the 2nd chance list. */
  setgcrefnull(g->gc.grayagain);
#endif
  gc_propagate_gray(g);  /* Propagate it. */

  udsize = lj_gc_separateudata(g, 0);  /* Separate userdata to be finalized. */
  gc_mark_mmudata(g);  /* Mark them. */
  udsize += gc_propagate_gray(g);  /* And propagate the marks. */

  /* All marking done, clear weak tables. */
#if LJ_HASGCMARK
  gc_clearweak_stacks(g);
#else
  gc_clearweak(g, gcref(g->gc.weak));
#endif

  lj_buf_shrink(L, &g->tmpbuf);  /* Shrink temp buffer. */

  /* Prepare for sweep phase. */
#if LJ_HASGCMARK
  /* Flip current white. With single WHITE1, this toggles WHITE1 bit off,
  ** so curwhite()=0 during sweep: makewhite produces pure white (no bits).
  ** otherwhite() has WHITE1, so isdead correctly detects dead objects. */
  g->gc.currentwhite = (uint8_t)otherwhite(g);
  g->strempty.marked = curwhite(g) | LJ_GC_FIXED | LJ_GC_SFIXED;
#else
  g->gc.currentwhite = (uint8_t)otherwhite(g);  /* Flip current white. */
  g->strempty.marked = g->gc.currentwhite;
#endif
  setmref(g->gc.sweep, &g->gc.root);
  g->gc.estimate = g->gc.total - (GCSize)udsize;  /* Initial estimate. */
#if LJ_HASGCMARK
  g->gc.gcmarkflags |= GCF_BITMAPSWEEP | GCF_MARKALLOC;
  g->gc.sweepa = 0;
  g->gc.sweepw = UnusedBlockWords;
  g->gc.sweepphase = SweepPhase_Bitmap;
#endif
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
#if LJ_HASGCMARK
    if (!gc_hugegray_empty(g))
      return propagatemark(g, gc_hugegray_pop(g));
    {
      GCArena *a = gc_grayarena_pop(g);
      if (a != NULL)
	return gc_propagate_arena(g, a);
    }
#else
    if (gcref(g->gc.gray) != NULL)
      return propagatemark(g);  /* Propagate one gray object. */
#endif
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
#if LJ_HASGCMARK
    if (g->gc.gcmarkflags & GCF_BITMAPSWEEP) {
      if (g->gc.sweepphase == SweepPhase_Bitmap) {
	gc_bitmap_sweep(g);
      }
      if (g->gc.sweepphase == SweepPhase_Rebuild) {
	/* Restore currentwhite before rebuild so makewhite includes WHITE1.
	** Without this, makewhite produces pure white (curwhite=0 during
	** sweep), and gc_marktv would re-mark stale stack references that
	** should be invisible to the next cycle. */
	g->gc.currentwhite = LJ_GC_WHITES | LJ_GC_FIXED;
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
#endif
    setmref(g->gc.sweep, gc_sweep(g, mref(g->gc.sweep, GCRef), GCSWEEPMAX));
    lj_assertG(old >= g->gc.total, "sweep increased memory");
    g->gc.estimate -= old - g->gc.total;
    if (gcref(*mref(g->gc.sweep, GCRef)) == NULL) {
#if LJ_HASGCMARK
      g->gc.gcmarkflags = 0;
      g->gc.currentwhite = LJ_GC_WHITES | LJ_GC_FIXED;
#if LJ_HASFFI
      gc_fullsweep(g, &g->gc.cdatavroot);
#endif
#endif
      if (g->str.num <= (g->str.mask >> 2) && g->str.mask > LJ_MIN_STRTAB*2-1)
	lj_str_resize(L, g->str.mask >> 1);  /* Shrink string table. */
#if LJ_HASGCARENA
      lj_arena_shrink(g);  /* Coalesce free space, release empty arenas. */
#endif
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
#if defined(LUA_USE_ASSERT) && LJ_HASGCARENA && !defined(LJ_GC_NOSTEPVERIFY)
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

#if LJ_HASGCMARK && defined(LUA_USE_ASSERT)
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

static void gc_arena_verify(global_State *g)
{
  GCobj *o;
  MSize i, dead = 0;
  lj_assertG(gc_hugegray_empty(g), "arena verify with pending huge gray objects");
  /* Flush bins + clear all GC mark bits: clean slate, allocator-truthful. */
  lj_arena_gcprepare(g);
  /* Shadow-mark every allocated arena object by scanning the block bitmaps
  ** directly, instead of walking the gc.root chain (which is being eliminated).
  ** Trav arenas hold tables, funcs, protos, threads, upvalues, regular cdata,
  ** traces and udata. The bitmap scan finds all of them -- including open
  ** upvalues, which the root chain can't enumerate without per-thread walks.
  ** Huge non-string objects are in the address-keyed huge set, not in any
  ** arena bitmap. mainthread/strempty are dlmalloc (not in arenas). */
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
	  arena_obj_setmark(a, c);
#if LJ_HASFFI
	  if (o2->gch.gct == ~LJ_TCDATA && cdataisv(gco2cd(o2)))
	    arena_obj_shadowmark(memcdatav(gco2cd(o2)));
#endif
	}
      }
    }
    /* Huge non-string objects have no cell bitmap. */
    {
      GCRef *slots = mref(g->gc.hugeset, GCRef);
      if (slots != NULL) {
	MSize hi, hmask = g->gc.hugesetmask;
	for (hi = 0; hi <= hmask; hi++) {
	  uintptr_t u = gcrefu(slots[hi]);
	  if (u == 0 || u == 1) continue;
	  o = (GCobj *)u;
	  if (o->gch.gct == ~LJ_TSTR) continue;
	  arena_obj_shadowmark(o);
#if LJ_HASFFI
	  if (o->gch.gct == ~LJ_TCDATA && cdataisv(gco2cd(o)))
	    arena_obj_shadowmark(memcdatav(gco2cd(o)));
#endif
	}
      }
    }
  }
#if LJ_HASFFI
  for (o = gcref(g->gc.cdatavroot); o != NULL; o = gcnext(o)) {
    if (!lj_arena_ishuge(o)) {
      arena_obj_shadowmark(o);
      if (cdataisv(gco2cd(o)))
	arena_obj_shadowmark(memcdatav(gco2cd(o)));
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
#endif /* LJ_HASGCMARK && LUA_USE_ASSERT */

#if LJ_HASGCARENA
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
#endif

/* Perform a full GC cycle. */
void lj_gc_fullgc(lua_State *L)
{
  global_State *g = G(L);
  int32_t ostate = g->vmstate;
  setvmstate(g, GC);
  if (g->gc.state <= GCSatomic) {  /* Caught somewhere in the middle. */
#if LJ_HASGCMARK
    gc_hugegray_reset(g);  /* Reset worklists from partial propagation. */
    gc_graythread_reset(g);
    gc_weak_reset(g);
#else
    setgcrefnull(g->gc.gray);  /* Reset lists from partial propagation. */
    setgcrefnull(g->gc.grayagain);
    setgcrefnull(g->gc.weak);
#endif
#if LJ_HASGCMARK
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
      g->gc.currentwhite = LJ_GC_WHITES | LJ_GC_FIXED;
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
      /* Huge non-string objects have no cell bitmap. */
      {
	GCRef *slots = mref(g->gc.hugeset, GCRef);
	if (slots != NULL) {
	  MSize hi, hmask = g->gc.hugesetmask;
	  for (hi = 0; hi <= hmask; hi++) {
	    uintptr_t u = gcrefu(slots[hi]);
	    if (u == 0 || u == 1) continue;
	    o = (GCobj *)u;
	    if (o->gch.gct == ~LJ_TSTR) continue;
	    makewhite(g, o);
	  }
	}
      }
      /* mainthread is not in any arena (dlmalloc). */
      makewhite(g, obj2gco(mainthread(g)));
      /* Also makewhite strings in the intern table. */
      {
        MSize i;
        for (i = 0; i <= g->str.mask; i++) {
          GCRef *sp = &g->str.tab[i];
          while ((o = gcref(*sp)) != NULL) {
            makewhite(g, o);
            sp = &o->gch.nextgc;
          }
        }
      }
#if LJ_HASFFI
      /* Also makewhite VLA cdata on their separate chain. */
      {
        GCRef *cp = &g->gc.cdatavroot;
        while ((o = gcref(*cp)) != NULL) {
          makewhite(g, o);
          cp = &o->gch.nextgc;
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
	if (mref(aa->greybase, GCCellID1) != NULL)
	  arena_gray_reset(aa);
      }
    }
    g->gc.state = GCSpause;
#else
    setmref(g->gc.sweep, &g->gc.root);  /* Sweep everything (preserving it). */
    g->gc.state = GCSsweepstring;  /* Fast forward to the sweep phase. */
    g->gc.sweepstr = 0;
#endif
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
#if LJ_HASGCMARK && defined(LUA_USE_ASSERT)
  gc_arena_verify(g);  /* Phase M: cross-check the arena mark bitmap. */
#endif
}

/* -- Write barriers ------------------------------------------------------ */

#if LJ_HASGCMARK
/* Backward barrier for arena objects (called from interpreter/JIT).
** Sets gray bit, then checks mark bitmap: black→dark-gray pushes to SSB,
** white→light-gray just sets gray (no push needed). */
void lj_gc_barrierback_arena(global_State *g, GCobj *o)
{
  o->gch.marked |= LJ_GC_GRAY;
  if (lj_arena_ishuge(o)) {
    if (o->gch.marked & LJ_GC_BLACK)
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
  MSize *heap = mref(g->gc.grayastack, MSize);
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
#endif

/* Move the GC propagation frontier forward. */
void lj_gc_barrierf(global_State *g, GCobj *o, GCobj *v)
{
#if LJ_HASGCMARK
  lj_assertG(!(o->gch.marked & LJ_GC_GRAY) && !isdead(g, o),
	     "bad object states for forward barrier");
  /* Note: unlike the header-color GC, the inline barrier fast path here only
  ** tests the gray bit, so this is entered for non-gray (white OR black)
  ** parents -- including white parents during GCSfinalize/GCSpause, where a
  ** stock LuaJIT forward barrier never fires. That is benign: the else-branch
  ** below just sets the gray bit. Hence no state assert under LJ_HASGCMARK. */
#else
  lj_assertG(isblack(o) && iswhite(v) && !isdead(g, v) && !isdead(g, o),
	     "bad object states for forward barrier");
  lj_assertG(g->gc.state != GCSfinalize && g->gc.state != GCSpause,
	     "bad GC state");
#endif
  lj_assertG(o->gch.gct != ~LJ_TTAB, "barrier object is not a table");
  /* Preserve invariant during propagation. Otherwise it doesn't matter. */
  if (g->gc.state == GCSpropagate || g->gc.state == GCSatomic)
    gc_mark(g, v);  /* Move frontier forward. */
  else
#if LJ_HASGCMARK
    o->gch.marked |= LJ_GC_GRAY;  /* Set gray to avoid re-triggering barrier. */
#else
    makewhite(g, o);  /* Make it white to avoid the following barrier. */
#endif
}

/* Specialized barrier for closed upvalue. Pass &uv->tv. */
void LJ_FASTCALL lj_gc_barrieruv(global_State *g, TValue *tv)
{
#define TV2MARKED(x) \
  (*((uint8_t *)(x) - offsetof(GCupval, tv) + offsetof(GCupval, marked)))
  if (g->gc.state == GCSpropagate || g->gc.state == GCSatomic)
    gc_mark(g, gcV(tv));
  else
#if LJ_HASGCMARK
    TV2MARKED(tv) |= LJ_GC_GRAY;  /* Set gray to avoid re-triggering barrier. */
#else
    TV2MARKED(tv) = (TV2MARKED(tv) & (uint8_t)~LJ_GC_COLORS) | curwhite(g);
#endif
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
  setgcrefr(o->gch.nextgc, g->gc.root);
  setgcref(g->gc.root, o);
#if LJ_HASGCMARK
  if ((o->gch.marked & LJ_GC_GRAY) && !iswhite(o)) {
#else
  if (isgray(o)) {  /* A closed upvalue is never gray, so fix this. */
#endif
    if (g->gc.state == GCSpropagate || g->gc.state == GCSatomic) {
      gray2black(o);  /* Make it black and preserve invariant. */
      if (tviswhite(&uv->tv))
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
#if LJ_HASGCARENA
  return lj_mem_newgco_arena(L, size, 0, 1);
#else
  global_State *g = G(L);
  GCobj *o = (GCobj *)g->allocf(g->allocd, NULL, 0, size);
  if (o == NULL)
    lj_err_mem(L);
  lj_assertG(checkptrGC(o),
	     "allocated memory address %p outside required range", o);
  g->gc.total += size;
  setgcrefr(o->gch.nextgc, g->gc.root);
  setgcref(g->gc.root, o);
  newwhite(g, o);
  return o;
#endif
}

#if LJ_HASGCARENA

/*
** Out-of-line continuation of lj_mem_newgco_arena(): the current arena
** had no bump space (or the size calls for a huge block).
*/
void *lj_mem_newgco_slow(lua_State *L, GCSize size, int trav, int link)
{
  global_State *g = G(L);
  GCobj *o;
  if (LJ_LIKELY(size < ArenaHugeThreshold)) {
    o = (GCobj *)lj_arena_findspace(g, size, trav);
  } else {
    o = (GCobj *)lj_hugeblock_alloc(g, size);
  }
  if (o == NULL)
    lj_err_mem(L);
  lj_assertG(checkptrGC(o),
	     "allocated memory address %p outside required range", o);
  g->gc.total += size;
#if LJ_HASGCMARK
  if (LJ_UNLIKELY(g->gc.gcmarkflags & GCF_MARKALLOC) &&
      !lj_arena_ishuge(o)) {
    arena_obj_setmark(ptr2arena(o), ptr2cell(o));
  }
#endif
  if (link) {
#if LJ_HASGCMARK
    if (LJ_UNLIKELY(g->gc.gcmarkflags & GCF_BITMAPSWEEP)) {
      newwhite(g, o);
    } else
#endif
    {
      setgcrefr(o->gch.nextgc, g->gc.root);
      setgcref(g->gc.root, o);
      newwhite(g, o);
    }
  }
  return o;
}

#endif

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


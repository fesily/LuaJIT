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
#include "lj_gcconc.h"
#include "lj_gcstat.h"

#define GCSTEPSIZE	1024u
#define GCSWEEPMAX	40
#define GCSWEEPCOST	10
#define GCFINALIZECOST	100

/* Macros to set GCobj colors and flags. */
#define white2gray(x)		((x)->gch.marked &= (uint8_t)~LJ_GC_WHITES)
#define gray2black(x)		((x)->gch.marked |= LJ_GC_BLACK)
#define isfinalized(u)		((u)->marked & LJ_GC_FINALIZED)

/* -- Mark phase ---------------------------------------------------------- */

static void gc_mark(global_State *g, GCobj *o);

/* Mark a TValue (if needed). The slot is snapshotted with a single
** atomic load: under concurrent marking the mutator may rewrite it
** between accesses (GC64 TValue = one naturally aligned 64-bit word).
*/
#if LJ_CONCGC
#define gc_tvload(tv)	lj_atomic_load64(&((const TValue *)(tv))->u64)
#else
#define gc_tvload(tv)	(((const TValue *)(tv))->u64)
#endif
#define gc_marktv(g, tv) \
  { TValue gc_tvc; \
    gc_tvc.u64 = gc_tvload(tv); \
    lj_assertG(!tvisgcv(&gc_tvc) || \
	       (~itype(&gc_tvc) == gcval(&gc_tvc)->gch.gct), \
	       "TValue and GC type mismatch"); \
    if (tviswhite(&gc_tvc)) gc_mark(g, gcV(&gc_tvc)); }

/* Mark a GCobj (if needed). */
#define gc_markobj(g, o) \
  { if (iswhite(obj2gco(o))) gc_mark(g, obj2gco(o)); }

/* Mark a string object. */
#define gc_mark_str(s)		((s)->marked &= (uint8_t)~LJ_GC_WHITES)

#if LJ_CONCGC
/* Mark a string object, safe against the mutator's LOGGED RMW. */
#define gc_mark_str_c(g, s) \
  { if (LJ_UNLIKELY((g)->gc.cmark)) \
      lj_atomic_and8(&(s)->marked, (uint8_t)~LJ_GC_WHITES); \
    else gc_mark_str(s); }
#else
#define gc_mark_str_c(g, s)	gc_mark_str(s)
#endif

#if LJ_CONCGC
/* Mark the children of a userdata. Shared by all (re-)mark paths. */
static void gc_mark_udchildren(global_State *g, GCudata *ud)
{
  GCtab *mt = tabref(ud->metatable);
  if (mt) gc_markobj(g, mt);
  gc_markobj(g, tabref(ud->env));
  if (LJ_HASBUFFER && ud->udtype == UDTYPE_BUFFER) {
    SBufExt *sbx = (SBufExt *)uddata(ud);
    if (sbufiscow(sbx) && gcref(sbx->cowref))
      gc_markobj(g, gcref(sbx->cowref));
    if (gcref(sbx->dict_str))
      gc_markobj(g, gcref(sbx->dict_str));
    if (gcref(sbx->dict_mt))
      gc_markobj(g, gcref(sbx->dict_mt));
  }
}

/* Concurrent marking. Runs on the GC thread, and on the mutator while the
** GC thread is parked (drains). Colors via atomic RMW: the mutator RMWs
** LJ_GC_LOGGED in the same byte. The gray queue is the jobs vector --
** gclist belongs to the mutator's store log during concurrent marking.
** Never reads Lua stacks: threads defer to threadv (traversed in the
** atomic phase) and closed upvalue values defer to uvv (re-read at the
** single-threaded finish; open upvalue values live in stacks and are
** re-marked by gc_mark_uv in the atomic phase).
*/
static void gc_mark_conc(global_State *g, GCobj *o)
{
  ConcGCState *cs = concgcstate(g);
  int gct = o->gch.gct;
  lj_atomic_and8(&o->gch.marked, (uint8_t)~LJ_GC_WHITES);  /* white2gray */
  if (LJ_UNLIKELY(gct == ~LJ_TUDATA)) {
    lj_atomic_or8(&o->gch.marked, LJ_GC_BLACK);  /* Udata are never gray. */
    gc_mark_udchildren(g, gco2ud(o));
  } else if (LJ_UNLIKELY(gct == ~LJ_TUPVAL)) {
    if (gco2uv(o)->closed) {
      lj_atomic_or8(&o->gch.marked, LJ_GC_BLACK);
      lj_concgc_vecpush(&cs->uvv, o);  /* Value re-read at finish. */
    }
  } else if (LJ_UNLIKELY(gct == ~LJ_TTHREAD)) {
    lj_concgc_vecpush(&cs->threadv, o);  /* Stays gray. */
  } else if (gct != ~LJ_TSTR && gct != ~LJ_TCDATA) {
    lj_concgc_vecpush(&cs->jobs, o);
  }
}
#endif

/* Mark a white GCobj. */
static void gc_mark(global_State *g, GCobj *o)
{
  int gct = o->gch.gct;
#if LJ_CONCGC
  if (LJ_UNLIKELY(g->gc.cmark)) {
    gc_mark_conc(g, o);
    return;
  }
#endif
  lj_assertG(iswhite(o), "mark of non-white object");
  lj_assertG(!isdead(g, o), "mark of dead object");
  white2gray(o);
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
    setgcrefr(o->gch.gclist, g->gc.gray);
    setgcref(g->gc.gray, o);
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

#if LJ_CONCGC
/* Seed the concurrent gray queue with the heap objects reachable from a
** thread's live stack slots. Runs on the mutator at cycle start (cmark set,
** marker still idle, jobs vector mutator-owned), so there is no concurrent
** stack mutation and no stack realloc race. The thread object itself stays
** deferred to threadv; atomic() re-traverses every thread regardless, so
** this is a pure optimization -- it hands the marker thread the large object
** graph that hangs off stack roots, which would otherwise be left entirely
** to the single-threaded atomic phase. Skips the atomic-only slot clearing
** and the stack shrink (which would realloc the stack). Marking an already
** non-white child is a no-op, so seeding the same thread twice is harmless.
*/
static void gc_conc_seed_stack(global_State *g, lua_State *th)
{
  TValue *o, *top = th->top;
  for (o = tvref(th->stack)+1+LJ_FR2; o < top; o++)
    gc_marktv(g, o);
  /* GC64 (required by LJ_CONCGC) keeps frame functions in stack slots, so
  ** the slot scan above already covers them; no frame walk needed here.
  */
  gc_markobj(g, tabref(th->env));
}
#endif

/* Start a GC cycle and mark the root set. */
static void gc_mark_start(global_State *g)
{
  setgcrefnull(g->gc.gray);
  setgcrefnull(g->gc.grayagain);
  setgcrefnull(g->gc.weak);
#if LJ_CONCGC
  if (g->gc.concmode && concgcstate(g)) {
    /* Roots go into the jobs vector; the marker thread takes over.
    ** cmark is set before any root can be marked, so all marking in
    ** this cycle consistently uses the concurrent paths.
    */
    ConcGCState *cs = concgcstate(g);
    GCSTAT_SCOPE(g, mark_start);
    GCSTAT_COUNT_CYCLE(g);
    cs->jobs.n = cs->threadv.n = cs->weakv.n = cs->uvv.n = 0;
    cs->logring->head = cs->logring->tail = 0;
    cs->finishreq = 0;
    cs->stepn = 0;
    cs->drains = 0;
    g->gc.cmark = 1;
    gc_markobj(g, mainthread(g));
    gc_markobj(g, tabref(mainthread(g)->env));
    gc_markobj(g, vmthread(g));
    gc_marktv(g, &g->registrytv);
    gc_mark_gcroot(g);
    /* Seed the marker with the heap graph hanging off live stack roots, so
    ** stack-rooted working sets are marked concurrently instead of falling
    ** entirely to the atomic phase. The running thread (cur_L) and the main
    ** thread hold the active locals; other coroutines stay deferred (no cheap
    ** global thread list) and are picked up by atomic()'s thread re-traversal.
    */
    {
      lua_State *curL = gco2th(gcref(g->cur_L));
      gc_conc_seed_stack(g, mainthread(g));
      if (curL && curL != mainthread(g))
	gc_conc_seed_stack(g, curL);
    }
    g->gc.state = GCSpropagate;
    lj_concgc_startmark(g);
    GCSTAT_SCOPE_END(g);
    return;
  }
#endif
  gc_markobj(g, mainthread(g));
  gc_markobj(g, tabref(mainthread(g)->env));
  gc_markobj(g, vmthread(g));
  gc_marktv(g, &g->registrytv);
  gc_mark_gcroot(g);
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
#if LJ_CONCGC
  if (LJ_UNLIKELY(g->gc.cmark)) {
    /* The __mode lookup is unsafe on the GC thread: lj_meta_fastg may
    ** write the mt->nomm negative cache (racing the mutator's nomm
    ** invalidations) and lj_tab_getstr walks hash collision chains the
    ** mutator may relink mid-walk. Conservatively treat any table with
    ** a metatable as possibly weak: queue it for re-examination at the
    ** single-threaded finish and mark its contents strongly. If it is
    ** actually weak this keeps clearable entries alive for one cycle
    ** (floating garbage), which is safe.
    */
    TValue *array;
    Node *node;
    MSize i, asize, hmask;
    if (mt)
      lj_concgc_vecpush(&concgcstate(g)->weakv, obj2gco(t));
    /* Publish the hazard pointer before dereferencing t's array/node so a
    ** concurrent lj_tab_resize defers (not frees) any block we may read.
    ** The SC store pairs with the SC fence+load in lj_tab_resize (Dekker:
    ** at least one side observes the other, so a still-read block is never
    ** freed). Read length before pointer (asize before array, hmask before
    ** node): lj_tab_resize maintains cap(buffer) >= length by growing the
    ** buffer before raising the length and lowering the length before
    ** shrinking the buffer, so a length-then-pointer read never pairs a
    ** small old buffer with a large new length (no out-of-bounds read).
    */
    lj_atomic_store64_seqcst(&g->gc.markhazard.ptr64, (uint64_t)(void *)t);
    asize = lj_atomic_load32_acq(&t->asize);
    array = (TValue *)lj_atomic_load64_acq(&t->array.ptr64);
    for (i = 0; i < asize; i++)
      gc_marktv(g, &array[i]);
    hmask = lj_atomic_load32_acq(&t->hmask);
    node = (Node *)lj_atomic_load64_acq(&t->node.ptr64);
    if (hmask > 0) {
      for (i = 0; i <= hmask; i++) {
	Node *n = &node[i];
	if (!tvisnil(&n->val)) {
	  lj_assertG(!tvisnil(&n->key), "mark of nil key in non-empty slot");
	  gc_marktv(g, &n->key);
	  gc_marktv(g, &n->val);
	}
      }
    }
    lj_atomic_store64_seqcst(&g->gc.markhazard.ptr64, (uint64_t)0);
    return 0;  /* Marked strongly; weakv requeue handles weak re-examination. */
  }
#endif
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
	setgcrefr(t->gclist, g->gc.weak);
	setgcref(g->gc.weak, obj2gco(t));
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
#if LJ_CONCGC
  if (LJ_UNLIKELY(g->gc.cmark)) {
    if (iswhite(o)) {
      lj_atomic_and8(&o->gch.marked, (uint8_t)~LJ_GC_WHITES);
      lj_concgc_vecpush(&concgcstate(g)->jobs, o);
    }
    return;
  }
#endif
  if (iswhite(o)) {
    white2gray(o);
    setgcrefr(o->gch.gclist, g->gc.gray);
    setgcref(g->gc.gray, o);
  }
}

/* Traverse a trace. */
static void gc_traverse_trace(global_State *g, GCtrace *T)
{
  IRRef ref;
  TraceNo link, nextroot, nextside;
  if (T->traceno == 0) return;
  for (ref = T->nk; ref < REF_TRUE; ref++) {
    IRIns *ir = &T->ir[ref];
    if (ir->o == IR_KGC)
      gc_markobj(g, ir_kgc(ir));
    if (irt_is64(ir->t) && ir->o != IR_KNULL)
      ref++;
  }
  /* Single loads: trace flushes rewrite these under the pause bracket. */
  link = T->link; nextroot = T->nextroot; nextside = T->nextside;
  if (link) gc_marktrace(g, link);
  if (nextroot) gc_marktrace(g, nextroot);
  if (nextside) gc_marktrace(g, nextside);
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
  gc_mark_str_c(g, proto_chunkname(pt));
  for (i = -(ptrdiff_t)pt->sizekgc; i < 0; i++)  /* Mark collectable consts. */
    gc_markobj(g, proto_kgc(pt, i));
#if LJ_HASJIT
  {
    /* Single load: trace flushes rewrite pt->trace under the pause
    ** bracket, but this read may race with the bracket's entry.
    */
    TraceNo tr = pt->trace;
    if (tr) gc_marktrace(g, tr);
  }
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
  if (g->gc.state == GCSatomic) {
    top = tvref(th->stack) + th->stacksize;
    for (; o < top; o++)  /* Clear unmarked slots. */
      setnilV(o);
  }
  gc_markobj(g, tabref(th->env));
  lj_state_shrinkstack(th, gc_traverse_frames(g, th));
}

/* Propagate one gray object. Traverse it and turn it black. */
static size_t propagatemark(global_State *g)
{
  GCobj *o = gcref(g->gc.gray);
  int gct = o->gch.gct;
  lj_assertG(isgray(o), "propagation of non-gray object");
  gray2black(o);
  setgcrefr(g->gc.gray, o->gch.gclist);  /* Remove from gray list. */
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
    setgcrefr(th->gclist, g->gc.grayagain);
    setgcref(g->gc.grayagain, o);
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

/* Propagate all gray objects. */
static size_t gc_propagate_gray(global_State *g)
{
  size_t m = 0;
  while (gcref(g->gc.gray) != NULL)
    m += propagatemark(g);
  return m;
}

#if LJ_CONCGC
/* -- Concurrent mark driver ----------------------------------------------
**
** Runs on the GC thread (between park points) and on the mutator during
** drains while the GC thread is parked. The gray queue is the jobs
** vector. Threads and the current trace are never traversed here.
*/

/* Traverse one object from the jobs vector and blacken it. */
static void gc_conc_traverse1(global_State *g, GCobj *o)
{
  int gct = o->gch.gct;
  lj_assertG(gct != ~LJ_TTHREAD, "thread on concurrent gray queue");
  lj_atomic_or8(&o->gch.marked, LJ_GC_BLACK);  /* gray2black */
  if (LJ_LIKELY(gct == ~LJ_TTAB)) {
    GCtab *t = gco2tab(o);
    if (gc_traverse_tab(g, t) > 0)
      lj_atomic_and8(&o->gch.marked, (uint8_t)~LJ_GC_BLACK);  /* Keep gray. */
  } else if (gct == ~LJ_TFUNC) {
    gc_traverse_func(g, gco2func(o));
  } else if (gct == ~LJ_TPROTO) {
    gc_traverse_proto(g, gco2pt(o));
  } else {
#if LJ_HASJIT
    gc_traverse_trace(g, gco2trace(o));
#else
    lj_assertG(0, "bad GC type %d", gct);
#endif
  }
}

/* One burst of concurrent marking. Returns 0 when out of work. */
int lj_gc_conc_burst(global_State *g)
{
  ConcGCState *cs = concgcstate(g);
  MSize n;
  for (n = 0; n < CONCGC_BURST; n++) {
    if (cs->jobs.n == 0)
      return 0;
    gc_conc_traverse1(g, cs->jobs.p[--cs->jobs.n]);
  }
  return 1;
}
#endif

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
    if (((o->gch.marked ^ LJ_GC_WHITES) & ow)) {  /* Black or current white? */
      lj_assertG(!isdead(g, o) || (o->gch.marked & LJ_GC_FIXED),
		 "sweep of undead object");
      makewhite(g, o);  /* Value is alive, change to the current white. */
      p = &o->gch.nextgc;
    } else {  /* Otherwise value is dead, free it. */
      lj_assertG(isdead(g, o) || ow == LJ_GC_SFIXED,
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
    if (((o->gch.marked ^ LJ_GC_WHITES) & ow)) {  /* Black or current white? */
      lj_assertG(!isdead(g, o) || (o->gch.marked & LJ_GC_FIXED),
		 "sweep of undead string");
      makewhite(g, o);  /* String is alive, change to the current white. */
      p = &o->gch.nextgc;
    } else {  /* Otherwise string is dead, free it. */
      lj_assertG(isdead(g, o) || ow == LJ_GC_SFIXED,
		 "sweep of unlive string");
      setgcrefr(*p, o->gch.nextgc);
      lj_str_free(g, gco2str(o));
    }
  }
  setgcrefp(*chain, (gcrefu(q) | (u & 1)));
}

/* Check whether we can clear a key or a value slot from a table. */
static int gc_mayclear(cTValue *o, int val)
{
  if (tvisgcv(o)) {  /* Only collectable objects can be weak references. */
    if (tvisstr(o)) {  /* But strings cannot be used as weak references. */
      gc_mark_str(strV(o));  /* And need to be marked. */
      return 0;
    }
    if (iswhite(gcV(o)))
      return 1;  /* Object is about to be collected. */
    if (tvisudata(o) && val && isfinalized(udataV(o)))
      return 1;  /* Finalized userdata is dropped only from values. */
  }
  return 0;  /* Cannot clear. */
}

/* Clear collected entries from weak tables. */
static void gc_clearweak(global_State *g, GCobj *o)
{
  UNUSED(g);
  while (o) {
    GCtab *t = gco2tab(o);
    lj_assertG((t->marked & LJ_GC_WEAK), "clear of non-weak table");
    if ((t->marked & LJ_GC_WEAKVAL)) {
      MSize i, asize = t->asize;
      for (i = 0; i < asize; i++) {
	/* Clear array slot when value is about to be collected. */
	TValue *tv = arrayslot(t, i);
	if (gc_mayclear(tv, 1))
	  setnilV(tv);
      }
    }
    if (t->hmask > 0) {
      Node *node = noderef(t->node);
      MSize i, hmask = t->hmask;
      for (i = 0; i <= hmask; i++) {
	Node *n = &node[i];
	/* Clear hash slot when key or value is about to be collected. */
	if (!tvisnil(&n->val) && (gc_mayclear(&n->key, 0) ||
				  gc_mayclear(&n->val, 1)))
	  setnilV(&n->val);
      }
    }
    o = gcref(t->gclist);
  }
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
    TValue tmp;
    copyTV(VL, &tmp, VL->top-1);
    VL->top--;
    lj_vmevent_send(g, ERRFIN,
      copyTV(V, V->top++, &tmp);
    );
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
  gc_fullsweep(g, &g->gc.root);
  for (i = g->str.mask; i != ~(MSize)0; i--)  /* Free all string hash chains. */
    gc_sweepstr(g, &g->str.tab[i]);
}

/* -- Collector ----------------------------------------------------------- */

#if LJ_CONCGC
/*
** Drain the mutator's store log. Must run with the GC thread parked
** (during the cycle) or stopped (at the finish): colors are exact then,
** and pushing to the GC-thread-owned vectors is safe.
**
** Per-type requeue rules:
**   Table/func/proto/trace: black -> re-gray + jobs (full re-traversal);
**     white -> mark (forward); gray -> already queued or deferred.
**   Thread: skip (never traversed before the atomic phase).
**   Upvalue (from closeuv): non-white -> mark value now, blacken, and
**     push to uvv so the value is re-read once more at the finish.
**     This restores the "closed upvalues are never gray" invariant
**     before the atomic phase.
**   Userdata: black -> re-mark metatable/env children.
** Returns 1 if any new mark work was generated.
*/
/* Per-object log handler. Called by the marker on each ring entry
** (free-running mode), and by the mutator on remaining ring entries at
** cycle termination (marker idle, mutator single-threaded -- see
** gc_conc_finish). The dirty parent is re-grayed (or its children
** re-marked, for upvalue/userdata which are never gray).
*/
static void gc_conc_logmark(global_State *g, GCobj *o)
{
  ConcGCState *cs = concgcstate(g);
  /* Clear LOGGED first: a racing mutator write can then re-log this object
  ** (correct -- it will appear again in the ring and we re-traverse).
  */
  lj_atomic_and8(&o->gch.marked, (uint8_t)~LJ_GC_LOGGED);
  if (o->gch.gct == ~LJ_TUPVAL) {
    GCupval *uv = gco2uv(o);
    /* Open upvalues are never logged (see gc_conc_log); only closed here. */
    if (uv->closed && !iswhite(o)) {
      gc_marktv(g, &uv->tv);
      lj_atomic_or8(&o->gch.marked, LJ_GC_BLACK);
      lj_concgc_vecpush(&cs->uvv, o);  /* Re-read once more at finish. */
    }
  } else if (o->gch.gct == ~LJ_TUDATA) {
    if (isblack(o))
      gc_mark_udchildren(g, gco2ud(o));
  } else if (o->gch.gct == ~LJ_TTRACE && iswhite(o)) {
    /* Trace forward barrier: trace was published into possibly-black proto. */
    gc_mark(g, o);
  } else if (isblack(o)) {
    /* gclist-bearing parent (table/func/proto): re-gray and requeue. */
    lj_atomic_and8(&o->gch.marked, (uint8_t)~LJ_GC_BLACK);  /* black2gray */
    lj_concgc_vecpush(&cs->jobs, o);
  }
  /* Else: white parent is dead or will be visited; gray is already queued. */
}

/* Marker-side ringdrain: pop up to `max` entries and dispatch logmark.
** Single consumer; safe under SPSC. */
MSize lj_concgc_ringdrain(global_State *g, MSize max)
{
  ConcGCState *cs = concgcstate(g);
  SpscRing *r = cs->logring;
  uint32_t head = r->head;
  uint32_t tail = lj_atomic_load32_acq(&r->tail);
  MSize n = 0;
  while (head != tail && n < max) {
    GCobj *o = r->slot[head & CONCGC_RING_MASK];
    head++; n++;
    gc_conc_logmark(g, o);
  }
  lj_atomic_store32_rel(&r->head, head);
  return n;
}

/* Marker-side grayagain steal: atomically take the entire grayagain chain
** and process it. The mutator (JIT barrier / VM barrierback) pushes with
** link-before-publish order (gclist = old; grayagain = tab) under x86-TSO,
** so any node visible through the head pointer has a valid gclist link.
** Called from the marker loop when ring+jobs are empty but the cycle is not
** yet finished -- this drains dirty tables that the mutator's gc_onestep
** has not had a chance to splice into the ring (e.g. during a long compute
** window with no allocation / GC step). */
MSize lj_concgc_draingrayagain(global_State *g)
{
  GCobj *ga = (GCobj *)lj_atomic_xchg64(&g->gc.grayagain.gcptr64, 0);
  MSize n = 0;
  while (ga != NULL) {
    GCobj *next = gcref(ga->gch.gclist);
    gc_conc_logmark(g, ga);
    ga = next;
    n++;
  }
  return n;
}

/* Ring full fallback: park the marker, drain inline, push the missed entry,
** resume. Rare; if it fires often, enlarge CONCGC_RING_SIZE. */
void lj_concgc_logfull(global_State *g, GCobj *o)
{
  ConcGCState *cs = concgcstate(g);
  lj_concgc_park(g);
  while (lj_concgc_ringdrain(g, CONCGC_RING_SIZE)) ;
  /* Now empty; the push must succeed. */
  (void)lj_concgc_ringpush(cs, o);
  lj_concgc_resume(g);
}

/* Termination convergence drain. Called from gc_conc_finish AFTER the
** marker is idle (markdone observed). Single-threaded: the marker is
** parked at the IDLE wait, the mutator is executing this code path so
** no new ring entries arrive. Drains everything the marker did not get
** to before idling.
*/
static int gc_conc_drainlog(global_State *g)
{
  ConcGCState *cs = concgcstate(g);
  SpscRing *r = cs->logring;
  uint32_t head = r->head;
  uint32_t tail = r->tail;  /* No producer running; plain read. */
  int work = (head != tail);
  GCobj *ga;
  GCSTAT_SCOPE(g, drainlog);
  GCSTAT_COUNT_DRAIN(g);
  while (head != tail) {
    GCobj *o = r->slot[head & CONCGC_RING_MASK];
    head++;
    gc_conc_logmark(g, o);
  }
  r->head = head;
  /* Also drain g->gc.grayagain: JIT-compiled TBAR (vm_x64.dasc / asm_tbar)
  ** still pushes barrier-logged tables onto the grayagain chain via
  ** tab->gclist, bypassing the SPSC ring. Process those here too.
  */
  ga = (GCobj *)lj_atomic_xchg64(&g->gc.grayagain.gcptr64, 0);
  while (ga != NULL) {
    GCobj *next = gcref(ga->gch.gclist);
    if (!work) work = 1;
    gc_conc_logmark(g, ga);
    ga = next;
  }
  GCSTAT_SCOPE_END(g);
  return work;
}

/*
** Finish concurrent marking: stop the marker thread, then converge
** single-threaded (colors exact, no concurrent stores -- the mutator is
** executing this very function). The log MUST be drained before jobs are
** requeued onto gc.gray: log entries chain through gclist, which the
** gray list reuses. After this the regular atomic() runs with cmark == 0.
*/
static void gc_conc_finish(global_State *g)
{
  ConcGCState *cs = concgcstate(g);
  MSize i;
  GCSTAT_SCOPE(g, conc_finish);
  lj_concgc_stopmark(g);  /* Clears cmark. */
  gc_conc_drainlog(g);  /* Frees all gclist fields (cmark=0: marks direct). */
  while (cs->jobs.n > 0) {  /* Requeue leftover gray queue. */
    GCobj *o = cs->jobs.p[--cs->jobs.n];
    lj_assertG(isgray(o), "non-gray object on concurrent gray queue");
    setgcrefr(o->gch.gclist, g->gc.gray);
    setgcref(g->gc.gray, o);
  }
  /* Closed upvalues collected during marking: re-read their values. */
  for (i = 0; i < cs->uvv.n; i++) {
    GCupval *uv = gco2uv(cs->uvv.p[i]);
    if (!iswhite(obj2gco(uv)))
      gc_marktv(g, &uv->tv);
  }
  cs->uvv.n = 0;
  /* Tables with metatables were marked strongly and collected in weakv
  ** (the __mode lookup is unsafe on the GC thread). Re-traverse them on
  ** the normal path, which derives the weak bits and chains actual weak
  ** tables onto gc.weak. Entries may repeat (re-traversals after drains);
  ** dedup via the LOGGED bit, free here: single-threaded, log drained.
  */
  for (i = 0; i < cs->weakv.n; i++) {
    GCobj *o = cs->weakv.p[i];
    if (!iswhite(o) && !(o->gch.marked & LJ_GC_LOGGED)) {
      o->gch.marked |= LJ_GC_LOGGED;
      o->gch.marked &= (uint8_t)~LJ_GC_BLACK;  /* Re-gray for traversal. */
      setgcrefr(o->gch.gclist, g->gc.gray);
      setgcref(g->gc.gray, o);
    }
  }
  for (i = 0; i < cs->weakv.n; i++)
    cs->weakv.p[i]->gch.marked &= (uint8_t)~LJ_GC_LOGGED;
  cs->weakv.n = 0;
  gc_propagate_gray(g);  /* Cannot create new log entries (cmark=0). */
  /* Deferred threads rejoin via grayagain for the atomic phase. */
  for (i = 0; i < cs->threadv.n; i++) {
    GCobj *o = cs->threadv.p[i];
    setgcrefr(o->gch.gclist, g->gc.grayagain);
    setgcref(g->gc.grayagain, o);
  }
  cs->threadv.n = 0;
  GCSTAT_SCOPE_END(g);
}

/* Set concurrent GC mode. Returns previous mode, -1 on init failure. */
int lj_gc_setconcmode(lua_State *L, int enable)
{
  global_State *g = G(L);
  int prev = g->gc.concmode;
  if (enable) {
    if (!lj_concgc_init(g))
      return -1;
#if LJ_HASJIT
    /* Traces compiled while concmode was off may have elided TBARs for
    ** trace-allocated tables (fold barrier_tnew_tdup), which is unsound
    ** once the marker can blacken them asynchronously.
    */
    if (!prev)
      lj_trace_flushall(L);
#endif
    g->gc.concmode = 1;
  } else {
    if (g->gc.cmark) {
      /* Mid-cycle: finish marking synchronously, then disable. */
      gc_conc_finish(g);
      g->gc.state = GCSatomic;
    }
    g->gc.concmode = 0;
  }
  return prev;
}
#endif

/* Atomic part of the GC cycle, transitioning from mark to sweep phase. */
static void atomic(global_State *g, lua_State *L)
{
  size_t udsize;

#if LJ_CONCGC
  lj_assertG(!g->gc.cmark, "atomic phase entered while marker running");
#endif
  GCSTAT_SCOPE(g, atomic);
  GCSTAT_PEAK(g);
  gc_mark_uv(g);  /* Need to remark open upvalues (the thread may be dead). */
  gc_propagate_gray(g);  /* Propagate any left-overs. */

  setgcrefr(g->gc.gray, g->gc.weak);  /* Empty the list of weak tables. */
  setgcrefnull(g->gc.weak);
  lj_assertG(!iswhite(obj2gco(mainthread(g))), "main thread turned white");
  gc_markobj(g, L);  /* Mark running thread. */
  gc_marktv(g, &g->registrytv);  /* Registry may have been replaced. */
  gc_traverse_curtrace(g);  /* Traverse current trace. */
  gc_mark_gcroot(g);  /* Mark GC roots (again). */
  gc_propagate_gray(g);  /* Propagate all of the above. */

  setgcrefr(g->gc.gray, g->gc.grayagain);  /* Empty the 2nd chance list. */
  setgcrefnull(g->gc.grayagain);
  gc_propagate_gray(g);  /* Propagate it. */

  udsize = lj_gc_separateudata(g, 0);  /* Separate userdata to be finalized. */
  gc_mark_mmudata(g);  /* Mark them. */
  udsize += gc_propagate_gray(g);  /* And propagate the marks. */

  /* All marking done, clear weak tables. */
  gc_clearweak(g, gcref(g->gc.weak));

  lj_buf_shrink(L, &g->tmpbuf);  /* Shrink temp buffer. */

  /* Prepare for sweep phase. */
  g->gc.currentwhite = (uint8_t)otherwhite(g);  /* Flip current white. */
  g->strempty.marked = g->gc.currentwhite;
  setmref(g->gc.sweep, &g->gc.root);
  g->gc.estimate = g->gc.total - (GCSize)udsize;  /* Initial estimate. */
  GCSTAT_SCOPE_END(g);
}

/* GC state machine. Returns a cost estimate for each step performed. */
static size_t gc_onestep(lua_State *L)
{
  global_State *g = G(L);
  switch (g->gc.state) {
  case GCSpause:
    gc_mark_start(g);  /* Start a new GC cycle by marking all GC roots. */
    return 0;
  case GCSpropagate:
#if LJ_CONCGC
    if (LJ_UNLIKELY(g->gc.cmark)) {
      /* Free-running marker: the mutator never blocks here. It asks the
      ** marker to finish (set finishreq), then polls markdone on each step
      ** while continuing user code. The marker drains the ring + jobs
      ** concurrently; on convergence it sets markdone and the mutator
      ** invokes gc_conc_finish (STW termination) on the next step.
      */
      ConcGCState *cs = concgcstate(g);
      uint32_t *fr = (uint32_t *)&cs->finishreq;
      /* JIT-compiled barrierback (vm_x64.dasc / asm_tbar) logs dirty tables
      ** by pushing them onto g->gc.grayagain via tab->gclist -- it cannot
      ** cheaply do the SPSC ring push inline. Splice grayagain into the ring
      ** here so the marker processes them while busy. The marker also steals
      ** grayagain directly (lj_concgc_draingrayagain) when idle, covering
      ** compute windows where the mutator runs no GC steps. Both paths use
      ** atomic xchg on grayagain to avoid tearing.
      */
      {
	GCobj *ga = (GCobj *)lj_atomic_xchg64(&g->gc.grayagain.gcptr64, 0);
	while (ga != NULL) {
	  GCobj *next = gcref(ga->gch.gclist);
	  if (LJ_UNLIKELY(!lj_concgc_ringpush(cs, ga)))
	    lj_concgc_logfull(g, ga);
	  ga = next;
	}
      }
      /* Give the marker time to keep up: only request finish once the
      ** mutator has driven enough GC steps that the heap proportional
      ** budget has been spent. Otherwise the mutator races ahead and the
      ** marker is asked to converge before it has a real working set.
      */
      if (++cs->stepn >= CONCGC_DRAINSTEP * 16) {
	if (!lj_atomic_load32(fr))
	  lj_atomic_store32(fr, 1);
      }
      if (lj_concgc_markdone(cs)) {
	gc_conc_finish(g);
	g->gc.state = GCSatomic;
	return 0;
      }
      return GCSWEEPMAX*GCSWEEPCOST;  /* Nominal cost; work is off-thread. */
    }
#endif
    if (gcref(g->gc.gray) != NULL)
      return propagatemark(g);  /* Propagate one gray object. */
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
    setmref(g->gc.sweep, gc_sweep(g, mref(g->gc.sweep, GCRef), GCSWEEPMAX));
    lj_assertG(old >= g->gc.total, "sweep increased memory");
    g->gc.estimate -= old - g->gc.total;
    if (gcref(*mref(g->gc.sweep, GCRef)) == NULL) {
      if (g->str.num <= (g->str.mask >> 2) && g->str.mask > LJ_MIN_STRTAB*2-1)
	lj_str_resize(L, g->str.mask >> 1);  /* Shrink string table. */
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

/* Perform a full GC cycle. */
void lj_gc_fullgc(lua_State *L)
{
  global_State *g = G(L);
  int32_t ostate = g->vmstate;
#if LJ_CONCGC
  uint8_t oconcmode = g->gc.concmode;
#endif
  setvmstate(g, GC);
#if LJ_CONCGC
  g->gc.concmode = 0;  /* Fully synchronous cycle below. */
  if (g->gc.cmark) {
    /* Stop the marker; the fast-forward below discards partial marks.
    ** LOGGED bits must be cleared or future logging would skip objects.
    */
    ConcGCState *cs = concgcstate(g);
    GCobj *o;
    SpscRing *r;
    uint32_t h, t;
    lj_concgc_stopmark(g);
    o = gcref(g->gc.grayagain);
    while (o != NULL) {
      o->gch.marked &= (uint8_t)~LJ_GC_LOGGED;
      o = gcref(o->gch.gclist);
    }
    r = cs->logring;  /* Clear LOGGED on every queued entry. */
    h = r->head; t = r->tail;
    while (h != t) {
      r->slot[h & CONCGC_RING_MASK]->gch.marked &= (uint8_t)~LJ_GC_LOGGED;
      h++;
    }
    r->head = r->tail = 0;
    cs->jobs.n = cs->threadv.n = cs->weakv.n = cs->uvv.n = 0;
    cs->finishreq = 0;
  }
#endif
  if (g->gc.state <= GCSatomic) {  /* Caught somewhere in the middle. */
    setmref(g->gc.sweep, &g->gc.root);  /* Sweep everything (preserving it). */
    setgcrefnull(g->gc.gray);  /* Reset lists from partial propagation. */
    setgcrefnull(g->gc.grayagain);
    setgcrefnull(g->gc.weak);
    g->gc.state = GCSsweepstring;  /* Fast forward to the sweep phase. */
    g->gc.sweepstr = 0;
  }
  while (g->gc.state == GCSsweepstring || g->gc.state == GCSsweep)
    gc_onestep(L);  /* Finish sweep. */
  lj_assertG(g->gc.state == GCSfinalize || g->gc.state == GCSpause,
	     "bad GC state");
  /* Now perform a full GC. */
  g->gc.state = GCSpause;
  do { gc_onestep(L); } while (g->gc.state != GCSpause);
  g->gc.threshold = (g->gc.estimate/100) * g->gc.pause;
#if LJ_CONCGC
  g->gc.concmode = oconcmode;
#endif
  g->vmstate = ostate;
}

/* -- Write barriers ------------------------------------------------------ */

#if LJ_CONCGC
/* Log a barrier parent during concurrent marking. Never touches colors.
** Pushes the dirty-parent pointer onto the lock-free SPSC ring; the marker
** consumes it and re-grays / re-traverses via gc_conc_logmark. On ring
** full the mutator falls back to a one-shot park-drain (logfull).
*/
static void gc_conc_log(global_State *g, GCobj *o)
{
  /* Skip open upvalues: their value lives in a stack slot, which the
  ** atomic-phase thread scan covers; they are never black (matches the
  ** non-concurrent barrier, which cannot trigger for them either).
  */
  if (o->gch.gct == ~LJ_TUPVAL && !gco2uv(o)->closed)
    return;
  if (!(gcmarked(o) & LJ_GC_LOGGED)) {
    ConcGCState *cs = concgcstate(g);
    lj_atomic_or8(&o->gch.marked, LJ_GC_LOGGED);
    if (LJ_UNLIKELY(!lj_concgc_ringpush(cs, o)))
      lj_concgc_logfull(g, o);
  }
}
#endif

/* Move the GC propagation frontier forward. */
void lj_gc_barrierf(global_State *g, GCobj *o, GCobj *v)
{
#if LJ_CONCGC
  if (LJ_UNLIKELY(g->gc.cmark)) {
    gc_conc_log(g, o);  /* Log the parent; the drain re-marks children. */
    return;
  }
#endif
  lj_assertG(isblack(o) && iswhite(v) && !isdead(g, v) && !isdead(g, o),
	     "bad object states for forward barrier");
  lj_assertG(g->gc.state != GCSfinalize && g->gc.state != GCSpause,
	     "bad GC state");
  lj_assertG(o->gch.gct != ~LJ_TTAB, "barrier object is not a table");
  /* Preserve invariant during propagation. Otherwise it doesn't matter. */
  if (g->gc.state == GCSpropagate || g->gc.state == GCSatomic)
    gc_mark(g, v);  /* Move frontier forward. */
  else
    makewhite(g, o);  /* Make it white to avoid the following barrier. */
}

/* Specialized barrier for closed upvalue. Pass &uv->tv. */
void LJ_FASTCALL lj_gc_barrieruv(global_State *g, TValue *tv)
{
#define TV2MARKED(x) \
  (*((uint8_t *)(x) - offsetof(GCupval, tv) + offsetof(GCupval, marked)))
#if LJ_CONCGC
  if (LJ_UNLIKELY(g->gc.cmark)) {
    gc_conc_log(g, (GCobj *)((char *)tv - offsetof(GCupval, tv)));
    return;
  }
#endif
  if (g->gc.state == GCSpropagate || g->gc.state == GCSatomic)
    gc_mark(g, gcV(tv));
  else
    TV2MARKED(tv) = (TV2MARKED(tv) & (uint8_t)~LJ_GC_COLORS) | curwhite(g);
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
#if LJ_CONCGC
  if (LJ_UNLIKELY(g->gc.cmark)) {
    /* Colors are GC-thread-owned and possibly stale; log unconditionally.
    ** The drain restores the "closed upvalues are never gray" invariant
    ** and (re-)marks the value before the atomic phase.
    */
    gc_conc_log(g, o);
    return;
  }
#endif
  if (isgray(o)) {  /* A closed upvalue is never gray, so fix this. */
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
#if LJ_CONCGC
  if (LJ_UNLIKELY(g->gc.cmark)) {
    /* Log the trace itself; the drain marks logged white traces. */
    gc_conc_log(g, obj2gco(traceref(G2J(g), traceno)));
    return;
  }
#endif
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


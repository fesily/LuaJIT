/*
** Memory profiler — heap snapshot walker (read-only).
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
**
** v0 (Capability A, design doc 2.1/9): arena bitmap census.
**
** Walks g->gc.arenas[0..arenastop) scanning each arena's block/mark bitmaps.
** For every allocated head cell (block bit set) reads o->gch.gct to classify
** the object type, derives the cell extent (1 + run of CellState_Extent cells)
** for the arena footprint, and reads arena_obj_ismarked for the live/dead axis.
** Huge objects are enumerated via g->gc.hugeset. The result is pushed as a Lua
** table census. Two snapshots can be diffed (lj_memprof_diff) to surface leak
** suspects.
**
** Read-only: no hot-path edits, no GC phase mutation, no allocation-path
** changes. An optional gc={full|step|none} option drives lj_gc_fullgc /
** lj_gc_step BEFORE the walk to produce a stable live set.
**
** Gated: LJ_HASGCMARK && defined(LUAJIT_ENABLE_MEMPROF). With the flag off
** this translation unit compiles to nothing (see lib_memprof.c).
*/
#include "lj_memprof.h"

#if LJ_HASGCMARK && defined(LUAJIT_ENABLE_MEMPROF)

#include "lj_gc.h"
#include "lj_err.h"
#include "lj_buf.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_lib.h"
#include "lauxlib.h"
#include "lj_frame.h"
#include "lj_debug.h"
#if LJ_HASJIT
#include "lj_jit.h"
#include "lj_trace.h"
#endif

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* -- Type classification -------------------------------------------------- */

/* Object type buckets, indexed by (gct - ~LJ_TSTR). Tags run ~LJ_TSTR..~LJ_TUDATA
** (src/lj_obj.h:264-274): STR, UPVAL, THREAD, PROTO, FUNC, TRACE, CDATA, TAB,
** UDATA — 9 buckets. */
#define MEMPROF_NTYPES		((int)(~LJ_TUDATA - ~LJ_TSTR) + 1)  /* 9 */

static int gct_to_idx(uint32_t gct)
{
  /* gct is stored as ~LJ_Txxx; e.g. a string has gct = ~LJ_TSTR = 4.
  ** The census bucket index is gct - ~LJ_TSTR (0=STR .. 8=UDATA). */
  int base = (int)(~LJ_TSTR);
  int idx = (int)gct - base;
  if (idx < 0 || idx >= MEMPROF_NTYPES)
    return -1;
  return idx;
}

/* Reuse the existing type-name table (lj_obj.c:17) keyed by itype.
** gct == ~LJ_Txxx, which IS the itypename index (LJ_TNIL=~0 -> 0, etc.). */
static const char *gct_name(uint32_t gct)
{
  if (gct > ~LJ_TNUMX)
    return "?";
  return lj_obj_itypename[gct];
}

/* Arena-class axis names, indexed by a small enum local to this file. */
enum {
  MP_CLASS_NONTRAV = 0,
  MP_CLASS_TRAV,
  MP_CLASS_POD,
  MP_CLASS_UDATA,
  MP_CLASS_CDATAV,
  MP_CLASS_NTYPES
};

static int arena_class_idx(uint16_t flags)
{
  if (flags & ArenaFlag_PODOnly)  return MP_CLASS_POD;
  if (flags & ArenaFlag_UdataOnly) return MP_CLASS_UDATA;
  if (flags & ArenaFlag_CdataVOnly) return MP_CLASS_CDATAV;
  if (flags & ArenaFlag_TravObjs)  return MP_CLASS_TRAV;
  return MP_CLASS_NONTRAV;
}

static const char *const mp_class_name[MP_CLASS_NTYPES] = {
  "nontrav", "trav", "pod", "udata", "cdatav"
};

/* -- Census accumulator --------------------------------------------------- */

typedef struct MemprofCensus {
  uint64_t type_count[MEMPROF_NTYPES];
  uint64_t type_bytes[MEMPROF_NTYPES];
  uint64_t type_live[MEMPROF_NTYPES];
  uint64_t type_dead[MEMPROF_NTYPES];
  uint64_t cls_count[MP_CLASS_NTYPES];
  uint64_t cls_bytes[MP_CLASS_NTYPES];
  uint64_t total_count;
  uint64_t total_bytes;
  uint64_t total_live;
  uint64_t total_dead;
  uint64_t huge_count[MEMPROF_NTYPES];
  uint64_t huge_total_count;
  uint64_t huge_total_bytes;
  uint64_t arenas;
} MemprofCensus;

static void census_init(MemprofCensus *c)
{
  memset(c, 0, sizeof(*c));
}

static void census_add(MemprofCensus *c, int tidx, int cls,
		       uint64_t bytes, int marked)
{
  c->type_count[tidx]++;
  c->type_bytes[tidx] += bytes;
  if (marked) c->type_live[tidx]++; else c->type_dead[tidx]++;
  c->cls_count[cls]++;
  c->cls_bytes[cls] += bytes;
  c->total_count++;
  c->total_bytes += bytes;
  if (marked) c->total_live++; else c->total_dead++;
}

static void census_add_huge(MemprofCensus *c, int tidx)
{
  c->huge_count[tidx]++;
  c->huge_total_count++;
}

/* -- Arena walk ----------------------------------------------------------- */

/* Walk one arena: scan cells from MinCellId. For each allocated head cell
** (block bit set) read gct, derive the cell extent, classify, record. Free-list
** binned blocks keep block=1 (White) by design (lj_arena.h:146) but carry no
** valid GCobj header; a head cell whose gct is outside ~LJ_TSTR..~LJ_TUDATA is
** treated as a binned free block and skipped. */
static void walk_arena(MemprofCensus *cens, GCArena *a)
{
  GCCellID c0, cell;
  int cls = arena_class_idx(a->flags);
  cens->arenas++;
  for (cell = MinCellId; cell < (GCCellID)a->celltop; ) {
    GCBlockword bw = a->block[arena_blockidx(cell)];
    if (!(bw & arena_blockbit(cell))) {
      cell++;
      continue;
    }
    {
      GCobj *o = (GCobj *)arena_cellptr(a, cell);
      uint32_t gct = o->gch.gct;
      int tidx = gct_to_idx(gct);
      GCCellID extent = 1;
      int marked;
      c0 = cell;
      while (cell + extent < (GCCellID)a->celltop) {
	CellState s = arena_cellstate(a, (GCCellID)(cell + extent));
	if (s != CellState_Extent)
	  break;
	extent++;
      }
      if (tidx < 0) {
	cell = (GCCellID)(cell + extent);
	continue;
      }
      marked = arena_obj_ismarked(a, c0);
      census_add(cens, tidx, cls, (uint64_t)extent << CellSizeLog2, marked);
      cell = (GCCellID)(cell + extent);
    }
  }
}

static void walk_huge(MemprofCensus *c, global_State *g)
{
  GCRef *slots = mref(g->gc.hugeset, GCRef);
  MSize hmask = g->gc.hugesetmask;
  MSize hi;
  if (slots == NULL) return;
  for (hi = 0; hi <= hmask; hi++) {
    uintptr_t u = gcrefu(slots[hi]);
    GCobj *o;
    int tidx;
    if (!hugeset_slot_live(u)) continue;
    o = hugeset_slot_obj(u);
    tidx = gct_to_idx(o->gch.gct);
    if (tidx < 0) continue;
    census_add_huge(c, tidx);
  }
  c->huge_total_bytes = (uint64_t)g->gc.hugemem;
}

/* -- Lua table construction ----------------------------------------------- */

static void set_num(lua_State *L, GCtab *t, const char *k, lua_Number v)
{
  TValue key;
  setstrV(L, &key, lj_str_newz(L, k));
  setnumV(lj_tab_set(L, t, &key), v);
}

static void set_str(lua_State *L, GCtab *t, const char *k, const char *v)
{
  TValue key, val;
  setstrV(L, &key, lj_str_newz(L, k));
  setstrV(L, &val, lj_str_newz(L, v));
  copyTV(L, lj_tab_set(L, t, &key), &val);
}

static void set_count_bytes(lua_State *L, GCtab *parent,
			    const char *k, uint64_t count, uint64_t bytes,
			    uint64_t live, uint64_t dead)
{
  TValue key;
  GCtab *sub = lj_tab_new_ah(L, 0, 4);
  set_num(L, sub, "count", (lua_Number)count);
  set_num(L, sub, "bytes", (lua_Number)bytes);
  set_num(L, sub, "live", (lua_Number)live);
  set_num(L, sub, "dead", (lua_Number)dead);
  setstrV(L, &key, lj_str_newz(L, k));
  settabV(L, lj_tab_set(L, parent, &key), sub);
}

static void push_type_table(lua_State *L, const MemprofCensus *c)
{
  GCtab *t = lj_tab_new_ah(L, 0, MEMPROF_NTYPES);
  int i;
  for (i = 0; i < MEMPROF_NTYPES; i++) {
    /* gct for bucket i: ~LJ_TSTR + i (4,5,...,12 == STR..UDATA). */
    uint32_t gct = (uint32_t)(~LJ_TSTR) + (uint32_t)i;
    set_count_bytes(L, t, gct_name(gct),
		    c->type_count[i], c->type_bytes[i],
		    c->type_live[i], c->type_dead[i]);
  }
  set_count_bytes(L, t, "huge",
		  c->huge_total_count, c->huge_total_bytes, 0, 0);
  settabV(L, L->top++, t);
}

static void push_class_table(lua_State *L, const MemprofCensus *c)
{
  GCtab *t = lj_tab_new_ah(L, 0, MP_CLASS_NTYPES);
  int i;
  for (i = 0; i < MP_CLASS_NTYPES; i++) {
    set_count_bytes(L, t, mp_class_name[i],
		    c->cls_count[i], c->cls_bytes[i], 0, 0);
  }
  settabV(L, L->top++, t);
}

/* -- Blob format for per-object diff -------------------------------------- */
/* Each object is 14 bytes: 8 addr + 1 type-idx + 4 size + 1 live.
** Built via SBuf with ZERO per-object Lua allocation. */
#define MEMPROF_BLOB_ENTSZ	14

static void blob_read(const char *p, uintptr_t *addr, uint8_t *tidx,
		      uint32_t *sz, uint8_t *live)
{
  memcpy(addr, p, 8);
  *tidx = (uint8_t)p[8];
  memcpy(sz, p + 9, 4);
  *live = (uint8_t)p[13];
}

static int cmp_addr(const void *a, const void *b)
{
  uintptr_t x = *(const uintptr_t *)a, y = *(const uintptr_t *)b;
  return x < y ? -1 : (x > y ? 1 : 0);
}

/* -- Public API ----------------------------------------------------------- */

LJ_FUNC int lj_memprof_snapshot(lua_State *L, const MemprofOpts *opts)
{
  global_State *g = G(L);
  MemprofCensus c;
  GCtab *root;
  TValue key;

  census_init(&c);

  if (opts->gc == LJ_MEMPROF_GC_FULL) {
    lj_gc_fullgc(L);
  } else if (opts->gc == LJ_MEMPROF_GC_STEP) {
    lj_gc_step(L);
  }

  {
    GCArena **arenas = mref(g->gc.arenas, GCArena *);
    MSize n = g->gc.arenastop, i;
    if (arenas != NULL) {
      for (i = 0; i < n; i++) {
	GCArena *a = arenas[i];
	if (a != NULL)
	  walk_arena(&c, a);
      }
    }
  }

  walk_huge(&c, g);

  root = lj_tab_new_ah(L, 0, 10);
  set_str(L, root, "gc",
	  opts->gc == LJ_MEMPROF_GC_FULL ? "full" :
	  opts->gc == LJ_MEMPROF_GC_STEP ? "step" : "none");

  set_count_bytes(L, root, "total",
		  c.total_count, c.total_bytes, c.total_live, c.total_dead);

  push_type_table(L, &c);
  {
    GCtab *t = tabV(L->top - 1);
    setstrV(L, &key, lj_str_newlit(L, "by_type"));
    settabV(L, lj_tab_set(L, root, &key), t);
    L->top--;
  }

  push_class_table(L, &c);
  {
    GCtab *t = tabV(L->top - 1);
    setstrV(L, &key, lj_str_newlit(L, "by_class"));
    settabV(L, lj_tab_set(L, root, &key), t);
    L->top--;
  }

  {
    GCtab *h = lj_tab_new_ah(L, 0, 2);
    set_num(L, h, "count", (lua_Number)c.huge_total_count);
    set_num(L, h, "bytes", (lua_Number)c.huge_total_bytes);
    setstrV(L, &key, lj_str_newlit(L, "huge"));
    settabV(L, lj_tab_set(L, root, &key), h);
  }

  {
    GCtab *it = lj_tab_new_ah(L, 0, 3);
    MSize snum = g->str.num;
    MSize smask = g->str.mask;
    set_num(L, it, "num", (lua_Number)snum);
    set_num(L, it, "mask", (lua_Number)smask);
    set_num(L, it, "load", smask ? (lua_Number)snum / (lua_Number)(smask + 1) : 0);
    setstrV(L, &key, lj_str_newlit(L, "intern"));
    settabV(L, lj_tab_set(L, root, &key), it);
  }

  set_num(L, root, "arenas", (lua_Number)c.arenas);
  set_num(L, root, "gc_state", (lua_Number)g->gc.state);

  if (opts->details) {
    SBuf *sb = lj_buf_tmp_(L);
    GCArena **arenas = mref(g->gc.arenas, GCArena *);
    MSize n = g->gc.arenastop, i;
    lj_buf_need(sb, 14 * 4096);
    lj_buf_reset(sb);
    if (arenas != NULL) {
      for (i = 0; i < n; i++) {
	GCArena *a = arenas[i];
	GCCellID cc;
	if (a == NULL) continue;
	for (cc = MinCellId; cc < (GCCellID)a->celltop; ) {
	  GCBlockword bw = a->block[arena_blockidx(cc)];
	  if (!(bw & arena_blockbit(cc))) { cc++; continue; }
	  {
	    GCobj *o = (GCobj *)arena_cellptr(a, cc);
	    uint32_t gct = o->gch.gct;
	    int tidx = gct_to_idx(gct);
	    GCCellID extent = 1;
	    uintptr_t addr = (uintptr_t)o;
	    uint32_t sizebytes;
	    uint8_t live;
	    char *p;
	    while (cc + extent < (GCCellID)a->celltop) {
	      if (arena_cellstate(a, (GCCellID)(cc + extent)) != CellState_Extent)
		break;
	      extent++;
	    }
	    if (tidx < 0) { cc = (GCCellID)(cc + extent); continue; }
	    sizebytes = (uint32_t)((uint64_t)extent << CellSizeLog2);
	    live = arena_obj_ismarked(a, cc) ? 1 : 0;
	    p = lj_buf_more(sb, 14);
	    memcpy(p, &addr, 8);
	    p[8] = (char)(uint8_t)tidx;
	    memcpy(p + 9, &sizebytes, 4);
	    p[13] = (char)live;
	    sb->w += 14;
	    cc = (GCCellID)(cc + extent);
	  }
	}
      }
    }
    {
      GCstr *blob = lj_buf_tostr(sb);
      TValue k;
      setstrV(L, &k, lj_str_newlit(L, "objects"));
      setstrV(L, lj_tab_set(L, root, &k), blob);
    }
    lj_buf_reset(sb);
  }

  settabV(L, L->top++, root);
  return 1;
}

/* diff(s1, s2): compare two snapshot tables.
** Returns {leak_suspects=, grown=, shrunk=, new_count=, gone_count=}.
** grown/shrunk are per-type count deltas from by_type (always available).
** new_count/gone_count/leak_suspects require details=true blobs, parsed in C
** with a sorted address array + binary search (zero Lua per-object alloc). */
LJ_FUNC int lj_memprof_diff(lua_State *L)
{
  luaL_checktype(L, 1, LUA_TTABLE);
  luaL_checktype(L, 2, LUA_TTABLE);
  int base = lua_gettop(L) + 1;
  lua_createtable(L, 0, 6);

  /* grown/shrunk from by_type (always present). */
  lua_getfield(L, 1, "by_type");
  lua_getfield(L, 2, "by_type");
  if (!lua_isnil(L, base+1) && !lua_isnil(L, base+2)) {
    int grown_idx = base + 3;
    int shrunk_idx = base + 4;
    lua_newtable(L);
    lua_newtable(L);
    lua_pushnil(L);
    while (lua_next(L, base+2) != 0) {
      lua_Number c2, c1;
      lua_getfield(L, -1, "count");
      c2 = lua_tonumber(L, -1);
      lua_pop(L, 1);
      lua_pushvalue(L, -2);
      lua_gettable(L, base+1);
      if (lua_isnil(L, -1)) {
	c1 = 0;
	lua_pop(L, 1);
      } else {
	lua_getfield(L, -1, "count");
	c1 = lua_tonumber(L, -1);
	lua_pop(L, 2);
      }
      if (c2 > c1) {
	lua_pushvalue(L, -2);
	lua_pushnumber(L, c2 - c1);
	lua_settable(L, grown_idx);
      } else if (c1 > c2) {
	lua_pushvalue(L, -2);
	lua_pushnumber(L, c1 - c2);
	lua_settable(L, shrunk_idx);
      }
      lua_pop(L, 1);
    }
    lua_setfield(L, base, "shrunk");
    lua_setfield(L, base, "grown");
  }
  lua_pop(L, 2);	/* pop s1bt, s2bt regardless of nil-or-not */

  /* Per-object diff from blobs. */
  lua_getfield(L, 1, "objects");
  lua_getfield(L, 2, "objects");
  if (lua_isnil(L, base+1) || lua_isnil(L, base+2)) {
    lua_pop(L, 2);
    lua_pushinteger(L, 0);
    lua_setfield(L, base, "new_count");
    lua_pushinteger(L, 0);
    lua_setfield(L, base, "gone_count");
    lua_newtable(L);
    lua_setfield(L, base, "leak_suspects");
    lua_pushstring(L, "snapshots missing objects blob (use details=true)");
    lua_setfield(L, base, "note");
    return 1;
  }
  {
    global_State *g = G(L);
    size_t len1, len2;
    const char *b1 = lua_tolstring(L, base+1, &len1);
    const char *b2 = lua_tolstring(L, base+2, &len2);
    const char *e1 = b1 + len1, *e2 = b2 + len2;
    MSize n1 = (MSize)(len1 / MEMPROF_BLOB_ENTSZ);
    uintptr_t *addrs = NULL;
    int new_count = 0, gone_count = 0;
    uint64_t new_by_type[MEMPROF_NTYPES];
    const char *p;
    memset(new_by_type, 0, sizeof(new_by_type));

    if (n1 > 0) {
      addrs = (uintptr_t *)lj_mem_new(L, (size_t)n1 * sizeof(uintptr_t));
      {
	MSize i = 0;
	uintptr_t a; uint8_t t; uint32_t s; uint8_t lv;
	for (p = b1; p + MEMPROF_BLOB_ENTSZ <= e1; p += MEMPROF_BLOB_ENTSZ) {
	  blob_read(p, &a, &t, &s, &lv);
	  if (i < n1) addrs[i++] = a;
	}
	qsort(addrs, n1, sizeof(uintptr_t), cmp_addr);
      }
    }

    for (p = b2; p + MEMPROF_BLOB_ENTSZ <= e2; p += MEMPROF_BLOB_ENTSZ) {
      uintptr_t a; uint8_t t; uint32_t s; uint8_t lv;
      size_t lo = 0, hi = n1;
      int found = 0;
      blob_read(p, &a, &t, &s, &lv);
      while (lo < hi) {
	size_t mid = lo + (hi - lo) / 2;
	if (addrs[mid] < a) lo = mid + 1;
	else if (addrs[mid] > a) hi = mid;
	else { found = 1; break; }
      }
      if (!found) {
	new_count++;
	if (t < MEMPROF_NTYPES) new_by_type[t]++;
      }
    }

    {
      MSize n2 = (MSize)(len2 / MEMPROF_BLOB_ENTSZ);
      if (n2 > 0) {
	uintptr_t *addrs2 = (uintptr_t *)lj_mem_new(L, (size_t)n2 * sizeof(uintptr_t));
	MSize i = 0;
	uintptr_t a; uint8_t t; uint32_t s; uint8_t lv;
	const char *q;
	for (q = b2; q + MEMPROF_BLOB_ENTSZ <= e2; q += MEMPROF_BLOB_ENTSZ) {
	  blob_read(q, &a, &t, &s, &lv);
	  if (i < n2) addrs2[i++] = a;
	}
	qsort(addrs2, n2, sizeof(uintptr_t), cmp_addr);
	for (p = b1; p + MEMPROF_BLOB_ENTSZ <= e1; p += MEMPROF_BLOB_ENTSZ) {
	  size_t lo = 0, hi = n2;
	  int found = 0;
	  uintptr_t a; uint8_t t; uint32_t s; uint8_t lv;
	  blob_read(p, &a, &t, &s, &lv);
	  while (lo < hi) {
	    size_t mid = lo + (hi - lo) / 2;
	    if (addrs2[mid] < a) lo = mid + 1;
	    else if (addrs2[mid] > a) hi = mid;
	    else { found = 1; break; }
	  }
	  if (!found) gone_count++;
	}
	lj_mem_free(g, addrs2, (size_t)n2 * sizeof(uintptr_t));
      }
    }
    if (addrs) lj_mem_free(g, addrs, (size_t)n1 * sizeof(uintptr_t));
    lua_pop(L, 2);

    lua_pushinteger(L, new_count);
    lua_setfield(L, base, "new_count");
    lua_pushinteger(L, gone_count);
    lua_setfield(L, base, "gone_count");

    lua_newtable(L);
    {
      int i;
      for (i = 0; i < MEMPROF_NTYPES; i++) {
	if (new_by_type[i] > 0) {
	  uint32_t gct = (uint32_t)(~LJ_TSTR) + (uint32_t)i;
	  lua_pushnumber(L, (lua_Number)new_by_type[i]);
	  lua_setfield(L, -2, gct_name(gct));
	}
      }
    }
    lua_setfield(L, base, "leak_suspects");
  }
  return 1;
}

/* ======================================================================== **
** v0.5: Retained-size + retaining-path analysis (read-only dominator tree).
**
** Builds a read-only object reference graph over the live set, computes a
** dominator tree (Cooper-Harvey-Kennedy "A Simple, Fast Dominance Algorithm"),
** and derives retained sizes and retaining paths.
**
** It mirrors the GC's per-type traverse logic (gc_traverse_tab/_func/_proto/
** _thread in lj_gc_arena.c and the udata/upval cases in gc_mark) but ONLY
** READS the reference fields — it never calls gc_mark/gc_marktv/gray/mark,
** never flips a color bit, never touches a gray list, never mutates the arena
** mark bitmap. The GC state is left byte-for-byte unperturbed.
**
** All scratch (node table, CSR adjacency, dom arrays) is allocated via
** g->allocf and freed before the API returns; no Lua tables are used for the
** graph. Exposed as memprof.retained{top=N} and memprof.retainers(addr).
**
** Node set = every allocated arena cell (block bit set, valid gct) + every
** live huge slot + the SFIXED roots (mainthread, vmthread), plus a synthetic
** super-root (node 0) whose out-edges mirror gc_mark_start's root set
** (mainthread, mainthread->env, vmthread, registrytv, g->gcroot[]). After a
** full GC (the default for these APIs) every allocated object is reachable
** from the roots, so the dominator tree is well-defined over the whole set.
** ======================================================================== */

/* Graph node. Node 0 is the synthetic super-root. */
typedef struct MpGNode {
  uintptr_t	addr;	/* GCobj address; 0 for the super-root. */
  uint32_t	gct;	/* ~LJ_Txxx tag; 0 for the super-root. */
  uint32_t	shallow;/* Shallow size in bytes (type-based, matches propagatemark). */
} MpGNode;

/* Edge callback: called for each outgoing reference target. The callback
** decides whether to record (target must be a live in-graph object). */
typedef void (*MpRefCb)(void *ctx, GCobj *target);

typedef struct MpGraph {
  global_State	*g;
  uint32_t	nnodes;	/* including super-root (node 0). */
  MpGNode	*nodes;	/* nnodes entries, sorted by addr; nodes[0] = super-root. */
  uint32_t	*head;	/* CSR: out-edges of node i are dst[head[i]..head[i+1]). */
  uint32_t	*dst;	/* head[nnodes] entries. */
} MpGraph;

/* -- allocf helpers ------------------------------------------------------- */

static void *mp_alloc(global_State *g, size_t sz)
{
  void *p = g->allocf(g->allocd, NULL, 0, sz);
  if (p != NULL && sz) memset(p, 0, sz);
  return p;
}

static void mp_free(global_State *g, void *p, size_t sz)
{
  if (p != NULL) g->allocf(g->allocd, p, sz, 0);
}

/* -- shallow size (mirrors propagatemark's per-type accounting) ----------- */

static uint32_t mp_shallow_size(GCobj *o)
{
  uint32_t gct = o->gch.gct;
  if (gct == (uint32_t)~LJ_TSTR) {
    GCstr *s = gco2str(o);
    return (uint32_t)(sizeof(GCstr) + s->len + 1);
  } else if (gct == (uint32_t)~LJ_TTAB) {
    GCtab *t = gco2tab(o);
    return (uint32_t)(sizeof(GCtab) + sizeof(TValue) * (size_t)t->asize +
		      (t->hmask ? sizeof(Node) * (size_t)(t->hmask + 1) : 0));
  } else if (gct == (uint32_t)~LJ_TFUNC) {
    GCfunc *fn = gco2func(o);
    return isluafunc(fn) ?
      (uint32_t)sizeLfunc((MSize)fn->l.nupvalues) :
      (uint32_t)sizeCfunc((MSize)fn->c.nupvalues);
  } else if (gct == (uint32_t)~LJ_TPROTO) {
    GCproto *pt = gco2pt(o);
    return (uint32_t)pt->sizept;
  } else if (gct == (uint32_t)~LJ_TTHREAD) {
    lua_State *th = gco2th(o);
    return (uint32_t)(sizeof(lua_State) + sizeof(TValue) * (size_t)th->stacksize);
  } else if (gct == (uint32_t)~LJ_TUDATA) {
    return (uint32_t)sizeudata(gco2ud(o));
  } else if (gct == (uint32_t)~LJ_TUPVAL) {
    return (uint32_t)sizeof(GCupval);
  } else if (gct == (uint32_t)~LJ_TCDATA) {
    GCcdata *cd = gco2cd(o);
    if (cdataisv(cd))
      return (uint32_t)(sizeof(GCcdata) + sizeof(GCcdataVar) + cdatavlen(cd));
    return (uint32_t)sizeof(GCcdata);
  } else if (gct == (uint32_t)~LJ_TTRACE) {
#if LJ_HASJIT
    GCtrace *T = (GCtrace *)gco2trace(o);
    return (uint32_t)(((sizeof(GCtrace) + 7) & ~(size_t)7) +
		      (size_t)(T->nins - T->nk) * sizeof(IRIns) +
		      (size_t)T->nsnap * sizeof(SnapShot) +
		      (size_t)T->nsnapmap * sizeof(SnapEntry));
#else
    return 0;
#endif
  }
  return 0;
}

/* -- read-only reference enumeration (mirrors the GC traverse fns) --------
**
** For each object type, reads exactly the same reference fields the GC's
** traverse functions read, but instead of gc_markobj/gc_marktv it invokes
** the callback with the target GCobj pointer. The callback performs the
** in-graph membership check. Strings and cdata have no outgoing refs (the GC
** treats them as leaves in gc_mark), so they are not enumerated here. */

static void mp_enum_refs(global_State *g, GCobj *o, void *ctx, MpRefCb cb)
{
  uint32_t gct = o->gch.gct;
  if (gct == (uint32_t)~LJ_TTAB) {
    GCtab *t = gco2tab(o);
    MSize i, asize = t->asize;
    /* metatable (gc_traverse_tab: tabref(t->metatable)). */
    if (gcrefu(t->metatable) != 0)
      cb(ctx, obj2gco(tabref(t->metatable)));
    /* array part (gc_traverse_tab: arrayslot(t,i) for i in [0,asize)). */
    if (asize > 0) {
      for (i = 0; i < asize; i++) {
	cTValue *tv = arrayslot(t, i);
	if (tvisgcv(tv)) cb(ctx, gcV(tv));
      }
    }
    /* hash part (gc_traverse_tab: noderef(t->node) for i in [0,hmask]). */
    if (t->hmask > 0) {
      Node *node = noderef(t->node);
      MSize hmask = t->hmask;
      for (i = 0; i <= hmask; i++) {
	Node *n = &node[i];
	if (!tvisnil(&n->val)) {
	  if (tvisgcv(&n->key)) cb(ctx, gcV(&n->key));
	  if (tvisgcv(&n->val)) cb(ctx, gcV(&n->val));
	}
      }
    }
  } else if (gct == (uint32_t)~LJ_TFUNC) {
    GCfunc *fn = gco2func(o);
    /* env (gc_traverse_func: tabref(fn->c.env)). */
    if (gcrefu(fn->c.env) != 0)
      cb(ctx, obj2gco(tabref(fn->c.env)));
    if (isluafunc(fn)) {
      uint32_t i;
      GCproto *pt = funcproto(fn);
      cb(ctx, obj2gco(pt));
      for (i = 0; i < fn->l.nupvalues; i++) {  /* Lua upvalues. */
	GCobj *uv = gcref(fn->l.uvptr[i]);
	if (uv != NULL) cb(ctx, uv);
      }
    } else {
      uint32_t i;
      for (i = 0; i < fn->c.nupvalues; i++) {  /* C upvalues (TValue each). */
	cTValue *tv = &fn->c.upvalue[i];
	if (tvisgcv(tv)) cb(ctx, gcV(tv));
      }
    }
  } else if (gct == (uint32_t)~LJ_TPROTO) {
    GCproto *pt = gco2pt(o);
    ptrdiff_t i;
    /* chunkname (gc_traverse_proto: proto_chunkname). */
    if (gcrefu(pt->chunkname) != 0)
      cb(ctx, obj2gco(strref(pt->chunkname)));
    /* collectable constants (proto_kgc for i in [-sizekgc,0)). */
    for (i = -(ptrdiff_t)pt->sizekgc; i < 0; i++) {
      GCobj *k = proto_kgc(pt, (size_t)i);
      if (k != NULL) cb(ctx, k);
    }
#if LJ_HASJIT
    if (pt->trace) {
      GCtrace *T = traceref(G2J(g), pt->trace);
      if (T != NULL) cb(ctx, obj2gco(T));
    }
#endif
  } else if (gct == (uint32_t)~LJ_TTHREAD) {
    lua_State *th = gco2th(o);
    TValue *bot = tvref(th->stack);
    TValue *top = th->top;
    TValue *stend = bot + th->stacksize;
    TValue *p;
    /* stack slots (gc_traverse_thread: tvref(th->stack)+1+LJ_FR2 .. th->top). */
    if (top > stend) top = stend;  /* defensive */
    if (bot != NULL) {
      for (p = bot + 1 + LJ_FR2; p < top; p++) {
	if (tvisgcv(p)) cb(ctx, gcV(p));
      }
      /* env (gc_traverse_thread: tabref(th->env)). */
      if (gcrefu(th->env) != 0)
	cb(ctx, obj2gco(tabref(th->env)));
      /* frame functions (gc_traverse_frames: frame_func for each frame).
      ** Mirrors the GC walk; read-only (no lj_state_shrinkstack call). The
      ** stack-slot scan above already covers most frame-func slots, but we
      ** replicate the frame walk to match the GC's edge set exactly. */
      {
	TValue *frame, *base = th->base;
	uint32_t guard = 0;
	if (base != NULL && base >= bot + 1 + LJ_FR2 && base <= stend) {
	  for (frame = base - 1; frame > bot + LJ_FR2; frame = frame_prev(frame)) {
	    GCfunc *fn;
	    if (frame < bot + 1 + LJ_FR2 || frame >= stend) break;  /* defensive */
	    fn = frame_func(frame);
	    if (fn != NULL) cb(ctx, obj2gco(fn));
	    if (++guard > th->stacksize + 8) break;  /* anti-loop */
	  }
	}
      }
    }
  } else if (gct == (uint32_t)~LJ_TUDATA) {
    GCudata *u = gco2ud(o);
    /* metatable + env (gc_mark UDATA case). */
    if (gcrefu(u->metatable) != 0)
      cb(ctx, obj2gco(tabref(u->metatable)));
    if (gcrefu(u->env) != 0)
      cb(ctx, obj2gco(tabref(u->env)));
#if LJ_HASBUFFER
    if (u->udtype == UDTYPE_BUFFER) {
      SBufExt *sbx = (SBufExt *)uddata(u);
      if (sbufiscow(sbx) && gcrefu(sbx->cowref) != 0)
	cb(ctx, gcref(sbx->cowref));
      if (gcrefu(sbx->dict_str) != 0)
	cb(ctx, gcref(sbx->dict_str));
      if (gcrefu(sbx->dict_mt) != 0)
	cb(ctx, gcref(sbx->dict_mt));
    }
#endif
  } else if (gct == (uint32_t)~LJ_TUPVAL) {
    GCupval *uv = gco2uv(o);
    /* closed upvalue value (gc_mark UPVAL case: gc_marktv(uvval(uv))). */
    cTValue *v = uvval(uv);
    if (v != NULL && tvisgcv(v)) cb(ctx, gcV(v));
  } else if (gct == (uint32_t)~LJ_TTRACE) {
#if LJ_HASJIT
    GCtrace *T = (GCtrace *)gco2trace(o);
    IRRef ref;
    if (T->traceno == 0) return;
    for (ref = T->nk; ref < REF_TRUE; ref++) {
      IRIns *ir = &T->ir[ref];
      if (ir->o == IR_KGC) {
	GCobj *k = ir_kgc(ir);
	if (k != NULL) cb(ctx, k);
      }
      if (irt_is64(ir->t) && ir->o != IR_KNULL) ref++;
    }
    if (T->link) { GCtrace *L2 = traceref(G2J(g), T->link); if (L2) cb(ctx, obj2gco(L2)); }
    if (T->nextroot) { GCtrace *R = traceref(G2J(g), T->nextroot); if (R) cb(ctx, obj2gco(R)); }
    if (T->nextside) { GCtrace *S = traceref(G2J(g), T->nextside); if (S) cb(ctx, obj2gco(S)); }
    if (gcrefu(T->startpt) != 0) cb(ctx, gcref(T->startpt));
#endif
  }
  /* STR, CDATA: no outgoing refs (GC treats them as leaves). */
  UNUSED(g);
}

/* Super-root out-edges: mirror gc_mark_start's root set. */
static void mp_enum_root_refs(global_State *g, void *ctx, MpRefCb cb)
{
  lua_State *mt = mainthread(g);
  cb(ctx, obj2gco(mt));
  if (gcrefu(mt->env) != 0)
    cb(ctx, obj2gco(tabref(mt->env)));
  if (gcrefu(g->vmthref) != 0)
    cb(ctx, obj2gco(vmthread(g)));
  if (tvisgcv(&g->registrytv))
    cb(ctx, gcV(&g->registrytv));
  {
    ptrdiff_t i;
    for (i = 0; i < (ptrdiff_t)GCROOT_MAX; i++) {
      if (gcrefu(g->gcroot[i]) != 0)
	cb(ctx, gcref(g->gcroot[i]));
    }
  }
}

/* -- node enumeration (reuses the v0 arena-bitmap + huge-set walk) -------- */

/* Count allocated arena cells with a valid gct. */
static uint32_t mp_count_arena(global_State *g)
{
  GCArena **arenas = mref(g->gc.arenas, GCArena *);
  MSize n = g->gc.arenastop, i;
  uint32_t count = 0;
  if (arenas == NULL) return 0;
  for (i = 0; i < n; i++) {
    GCArena *a = arenas[i];
    GCCellID cell;
    if (a == NULL) continue;
    for (cell = MinCellId; cell < (GCCellID)a->celltop; ) {
      GCBlockword bw = a->block[arena_blockidx(cell)];
      if (!(bw & arena_blockbit(cell))) { cell++; continue; }
      {
	GCobj *o = (GCobj *)arena_cellptr(a, cell);
	uint32_t gct = o->gch.gct;
	GCCellID extent = 1;
	int tidx = gct_to_idx(gct);
	while (cell + extent < (GCCellID)a->celltop) {
	  if (arena_cellstate(a, (GCCellID)(cell + extent)) != CellState_Extent)
	    break;
	  extent++;
	}
	if (tidx >= 0) count++;
	cell = (GCCellID)(cell + extent);
      }
    }
  }
  return count;
}

/* Count live huge slots with a valid gct (mp_fill_nodes also fills these). */
static uint32_t mp_count_huge(global_State *g)
{
  GCRef *slots = mref(g->gc.hugeset, GCRef);
  MSize hmask = g->gc.hugesetmask;
  MSize hi;
  uint32_t count = 0;
  if (slots == NULL) return 0;
  for (hi = 0; hi <= hmask; hi++) {
    uintptr_t u = gcrefu(slots[hi]);
    GCobj *o;
    if (!hugeset_slot_live(u)) continue;
    o = hugeset_slot_obj(u);
    if (gct_to_idx(o->gch.gct) >= 0) count++;
  }
  return count;
}

/* Fill nodes[] from arena + huge + fixed roots. Returns the next free index. */
static uint32_t mp_fill_nodes(global_State *g, MpGNode *nodes, uint32_t from)
{
  GCArena **arenas = mref(g->gc.arenas, GCArena *);
  MSize n = g->gc.arenastop, i;
  uint32_t k = from;
  if (arenas != NULL) {
    for (i = 0; i < n; i++) {
      GCArena *a = arenas[i];
      GCCellID cell;
      if (a == NULL) continue;
      for (cell = MinCellId; cell < (GCCellID)a->celltop; ) {
	GCBlockword bw = a->block[arena_blockidx(cell)];
	if (!(bw & arena_blockbit(cell))) { cell++; continue; }
	{
	  GCobj *o = (GCobj *)arena_cellptr(a, cell);
	  uint32_t gct = o->gch.gct;
	  GCCellID extent = 1;
	  int tidx = gct_to_idx(gct);
	  while (cell + extent < (GCCellID)a->celltop) {
	    if (arena_cellstate(a, (GCCellID)(cell + extent)) != CellState_Extent)
	      break;
	    extent++;
	  }
	  if (tidx >= 0) {
	    nodes[k].addr = (uintptr_t)o;
	    nodes[k].gct = gct;
	    nodes[k].shallow = mp_shallow_size(o);
	    k++;
	  }
	  cell = (GCCellID)(cell + extent);
	}
      }
    }
  }
  /* huge set. */
  {
    GCRef *slots = mref(g->gc.hugeset, GCRef);
    MSize hmask = g->gc.hugesetmask;
    MSize hi;
    if (slots != NULL) {
      for (hi = 0; hi <= hmask; hi++) {
	uintptr_t u = gcrefu(slots[hi]);
	GCobj *o;
	if (!hugeset_slot_live(u)) continue;
	o = hugeset_slot_obj(u);
	if (gct_to_idx(o->gch.gct) >= 0) {
	  nodes[k].addr = (uintptr_t)o;
	  nodes[k].gct = o->gch.gct;
	  nodes[k].shallow = mp_shallow_size(o);
	  k++;
	}
      }
    }
  }
  /* SFIXED roots: mainthread + vmthread (not in arena/huge). */
  if (gcrefu(g->mainthref) != 0) {
    lua_State *mt = mainthread(g);
    nodes[k].addr = (uintptr_t)obj2gco(mt);
    nodes[k].gct = (uint32_t)~LJ_TTHREAD;
    nodes[k].shallow = (uint32_t)(sizeof(lua_State) + sizeof(TValue) * (size_t)mt->stacksize);
    k++;
  }
  if (gcrefu(g->vmthref) != 0) {
    lua_State *vt = vmthread(g);
    if ((uintptr_t)obj2gco(vt) != (uintptr_t)obj2gco(mainthread(g))) {
      nodes[k].addr = (uintptr_t)obj2gco(vt);
      nodes[k].gct = (uint32_t)~LJ_TTHREAD;
      nodes[k].shallow = (uint32_t)(sizeof(lua_State) + sizeof(TValue) * (size_t)vt->stacksize);
      k++;
    }
  }
  return k;
}

/* Binary search nodes[0..nnodes-1] (sorted by addr) for addr. Returns index
** or -1. Node 0 (super-root, addr 0) is included in the search range. */
static int32_t mp_node_lookup(const MpGraph *gr, uintptr_t addr)
{
  int32_t lo = 0, hi = (int32_t)gr->nnodes - 1;
  while (lo <= hi) {
    int32_t mid = lo + (hi - lo) / 2;
    if (gr->nodes[mid].addr < addr) lo = mid + 1;
    else if (gr->nodes[mid].addr > addr) hi = mid - 1;
    else return mid;
  }
  return -1;
}

static int mp_cmp_node_addr(const void *a, const void *b)
{
  const MpGNode *x = (const MpGNode *)a, *y = (const MpGNode *)b;
  if (x->addr < y->addr) return -1;
  if (x->addr > y->addr) return 1;
  return 0;
}

/* Edge callback contexts. */
typedef struct MpCountCtx {
  const MpGraph *gr;
  uint32_t count;
} MpCountCtx;

static void mp_cb_count(void *ctx, GCobj *target)
{
  MpCountCtx *c = (MpCountCtx *)ctx;
  int32_t id = mp_node_lookup(c->gr, (uintptr_t)target);
  if (id > 0) c->count++;  /* skip super-root (0) and not-found (-1) */
}

typedef struct MpEmitCtx {
  const MpGraph *gr;
  uint32_t *dst;
  uint32_t *cur;	/* per-node fill cursor. */
  uint32_t node;	/* current source node id. */
} MpEmitCtx;

static void mp_cb_emit(void *ctx, GCobj *target)
{
  MpEmitCtx *e = (MpEmitCtx *)ctx;
  int32_t id = mp_node_lookup(e->gr, (uintptr_t)target);
  if (id > 0)
    e->dst[e->cur[e->node]++] = (uint32_t)id;
}

/* Build the graph: enumerate nodes, sort by addr, count + fill CSR adjacency.
** Returns 0 on success, nonzero on OOM. */
static int mp_graph_build(lua_State *L, int do_fullgc, MpGraph *gr)
{
  global_State *g = G(L);
  uint32_t n_arena, nnodes, i;
  MpGNode *nodes;
  uint32_t *head, *dst, *cur;
  MpCountCtx cc;
  MpEmitCtx ec;

  gr->g = g; gr->nodes = NULL; gr->head = NULL; gr->dst = NULL;
  gr->nnodes = 0;

  if (do_fullgc)
    lj_gc_fullgc(L);  /* consistent live set; marks cleared after. */

  n_arena = mp_count_arena(g);
  {
    uint32_t n_huge = mp_count_huge(g);
    /* +2 for mainthread + vmthread, +1 for super-root. */
    nnodes = n_arena + n_huge + 2 + 1;
  }
  nodes = (MpGNode *)mp_alloc(g, (size_t)nnodes * sizeof(MpGNode));
  if (nodes == NULL) return 1;
  /* super-root = node 0 (addr 0 sorts first). */
  nodes[0].addr = 0; nodes[0].gct = 0; nodes[0].shallow = 0;
  {
    uint32_t filled = mp_fill_nodes(g, nodes, 1);
    nnodes = filled;  /* filled = next free index = total nodes (super-root at 0). */
  }
  qsort(nodes, (size_t)nnodes, sizeof(MpGNode), mp_cmp_node_addr);
  gr->nodes = nodes;
  gr->nnodes = nnodes;

  head = (uint32_t *)mp_alloc(g, (size_t)(nnodes + 1) * sizeof(uint32_t));
  if (head == NULL) { mp_free(g, nodes, (size_t)nnodes * sizeof(MpGNode)); gr->nodes=NULL; return 1; }
  gr->head = head;

  /* Count out-edges per node -> prefix-sum into head. */
  head[0] = 0;
  cc.gr = gr;
  for (i = 0; i < nnodes; i++) {
    cc.count = 0;
    if (i == 0)
      mp_enum_root_refs(g, (void *)&cc, (MpRefCb)mp_cb_count);
    else {
      mp_enum_refs(g, (GCobj *)nodes[i].addr, (void *)&cc, (MpRefCb)mp_cb_count);
    }
    head[i + 1] = head[i] + cc.count;
  }

  dst = (uint32_t *)mp_alloc(g, (size_t)head[nnodes] * sizeof(uint32_t));
  if (dst == NULL && head[nnodes] > 0) {
    mp_free(g, head, (size_t)(nnodes + 1) * sizeof(uint32_t));
    mp_free(g, nodes, (size_t)nnodes * sizeof(MpGNode));
    gr->head = NULL; gr->nodes = NULL; return 1;
  }
  gr->dst = dst;
  if (head[nnodes] == 0) return 0;

  cur = (uint32_t *)mp_alloc(g, (size_t)nnodes * sizeof(uint32_t));
  if (cur == NULL) {
    mp_free(g, dst, (size_t)head[nnodes] * sizeof(uint32_t));
    mp_free(g, head, (size_t)(nnodes + 1) * sizeof(uint32_t));
    mp_free(g, nodes, (size_t)nnodes * sizeof(MpGNode));
    gr->dst = NULL; gr->head = NULL; gr->nodes = NULL; return 1;
  }
  for (i = 0; i < nnodes; i++) cur[i] = head[i];

  ec.gr = gr; ec.dst = dst; ec.cur = cur;
  for (i = 0; i < nnodes; i++) {
    ec.node = i;
    if (i == 0)
      mp_enum_root_refs(g, (void *)&ec, (MpRefCb)mp_cb_emit);
    else
      mp_enum_refs(g, (GCobj *)nodes[i].addr, (void *)&ec, (MpRefCb)mp_cb_emit);
  }
  mp_free(g, cur, (size_t)nnodes * sizeof(uint32_t));
  return 0;
}

static void mp_graph_free(global_State *g, MpGraph *gr)
{
  if (gr->dst) mp_free(g, gr->dst, (size_t)gr->head[gr->nnodes] * sizeof(uint32_t));
  if (gr->head) mp_free(g, gr->head, (size_t)(gr->nnodes + 1) * sizeof(uint32_t));
  if (gr->nodes) mp_free(g, gr->nodes, (size_t)gr->nnodes * sizeof(MpGNode));
  gr->dst = NULL; gr->head = NULL; gr->nodes = NULL; gr->nnodes = 0;
}

/* -- dominator tree (Cooper-Harvey-Kennedy) ------------------------------ */

static uint32_t mp_intersect(uint32_t b1, uint32_t b2, const int32_t *idom,
			     const int32_t *postnum)
{
  while (b1 != b2) {
    while (postnum[b1] < postnum[b2]) b1 = (uint32_t)idom[b1];
    while (postnum[b2] < postnum[b1]) b2 = (uint32_t)idom[b2];
  }
  return b1;
}

/* Build reverse CSR (predecessors) from the forward graph. */
static int mp_build_rev(global_State *g, const MpGraph *gr,
			uint32_t **prhead, uint32_t **prdst)
{
  uint32_t nnodes = gr->nnodes, i, e;
  uint32_t *rhead = (uint32_t *)mp_alloc(g, (size_t)(nnodes + 1) * sizeof(uint32_t));
  uint32_t *rdst, *rcur;
  if (rhead == NULL) return 1;
  /* count in-degrees into rhead[1..nnodes]. */
  for (i = 0; i < nnodes; i++) rhead[i] = 0;
  for (i = 0; i < nnodes; i++)
    for (e = gr->head[i]; e < gr->head[i + 1]; e++)
      rhead[gr->dst[e] + 1]++;
  /* prefix sum. */
  for (i = 0; i < nnodes; i++) rhead[i + 1] += rhead[i];
  rdst = (uint32_t *)mp_alloc(g, (size_t)rhead[nnodes] * sizeof(uint32_t));
  if (rdst == NULL && rhead[nnodes] > 0) { mp_free(g, rhead, (size_t)(nnodes+1)*sizeof(uint32_t)); return 1; }
  if (rhead[nnodes] == 0) { *prhead = rhead; *prdst = rdst; return 0; }
  rcur = (uint32_t *)mp_alloc(g, (size_t)nnodes * sizeof(uint32_t));
  if (rcur == NULL) {
    mp_free(g, rdst, (size_t)rhead[nnodes] * sizeof(uint32_t));
    mp_free(g, rhead, (size_t)(nnodes + 1) * sizeof(uint32_t));
    return 1;
  }
  for (i = 0; i < nnodes; i++) rcur[i] = rhead[i];
  for (i = 0; i < nnodes; i++)
    for (e = gr->head[i]; e < gr->head[i + 1]; e++) {
      uint32_t m = gr->dst[e];
      rdst[rcur[m]++] = i;
    }
  mp_free(g, rcur, (size_t)nnodes * sizeof(uint32_t));
  *prhead = rhead; *prdst = rdst;
  return 0;
}

/* Compute idom[] (immediate dominator per node) and postnum[] (post-order
** number; -1 if unreachable from super-root). Returns 0 on success, nonzero
** on OOM. idom[0]=0 (super-root dominates itself). */
static int mp_dominators(global_State *g, const MpGraph *gr,
			 int32_t **pidom, int32_t **ppostnum, uint32_t **ppo,
			 uint32_t *ppocount)
{
  uint32_t nnodes = gr->nnodes, i, e;
  uint32_t *rhead = NULL, *rdst = NULL;
  uint32_t *iter, *stack, *po;
  uint8_t *visited;
  int32_t *idom, *postnum;
  uint32_t pocount = 0, sp = 0;
  int changed;

  *pidom = NULL; *ppostnum = NULL; *ppo = NULL; *ppocount = 0;
  if (nnodes == 0) return 1;

  if (mp_build_rev(g, gr, &rhead, &rdst) != 0) return 1;

  iter    = (uint32_t *)mp_alloc(g, (size_t)nnodes * sizeof(uint32_t));
  stack   = (uint32_t *)mp_alloc(g, (size_t)nnodes * sizeof(uint32_t));
  po      = (uint32_t *)mp_alloc(g, (size_t)nnodes * sizeof(uint32_t));
  visited = (uint8_t *)mp_alloc(g, (size_t)nnodes);
  idom    = (int32_t *)mp_alloc(g, (size_t)nnodes * sizeof(int32_t));
  postnum = (int32_t *)mp_alloc(g, (size_t)nnodes * sizeof(int32_t));
  if (!iter || !stack || !po || !visited || !idom || !postnum) {
    mp_free(g, iter, (size_t)nnodes * sizeof(uint32_t));
    mp_free(g, stack, (size_t)nnodes * sizeof(uint32_t));
    mp_free(g, po, (size_t)nnodes * sizeof(uint32_t));
    mp_free(g, visited, (size_t)nnodes);
    mp_free(g, idom, (size_t)nnodes * sizeof(int32_t));
    mp_free(g, postnum, (size_t)nnodes * sizeof(int32_t));
    if (rdst) mp_free(g, rdst, (size_t)rhead[nnodes] * sizeof(uint32_t));
    mp_free(g, rhead, (size_t)(nnodes + 1) * sizeof(uint32_t));
    return 1;
  }

  for (i = 0; i < nnodes; i++) { postnum[i] = -1; idom[i] = -1; visited[i] = 0; iter[i] = gr->head[i]; }

  /* iterative post-order DFS from super-root (node 0). */
  stack[sp++] = 0; visited[0] = 1;
  while (sp > 0) {
    uint32_t n = stack[sp - 1];
    if (iter[n] < gr->head[n + 1]) {
      uint32_t m = gr->dst[iter[n]++];
      if (!visited[m]) { visited[m] = 1; stack[sp++] = m; }
    } else {
      postnum[n] = (int32_t)pocount;
      po[pocount++] = n;
      sp--;
    }
  }

  /* CHK fixpoint: process nodes in reverse post-order (super-root first).
  ** po[pocount-1] == 0 (super-root, highest postnum); skip it. */
  idom[0] = 0;
  do {
    changed = 0;
    for (i = pocount - 1; i-- > 0; ) {  /* skip the last (super-root). */
      uint32_t n = po[i];
      int32_t new_idom = -1;
      for (e = rhead[n]; e < rhead[n + 1]; e++) {
	uint32_t p = rdst[e];
	if (postnum[p] != -1 && idom[p] != -1) {
	  if (new_idom == -1) new_idom = (int32_t)p;
	  else new_idom = (int32_t)mp_intersect((uint32_t)new_idom, p, idom, postnum);
	}
      }
      if (new_idom != -1 && idom[n] != new_idom) {
	idom[n] = new_idom;
	changed = 1;
      }
    }
  } while (changed);

  /* Unreachable nodes: dominated by the super-root (retained = shallow). */
  for (i = 0; i < nnodes; i++)
    if (postnum[i] == -1) idom[i] = 0;

  mp_free(g, iter, (size_t)nnodes * sizeof(uint32_t));
  mp_free(g, stack, (size_t)nnodes * sizeof(uint32_t));
  mp_free(g, visited, (size_t)nnodes);
  if (rdst) mp_free(g, rdst, (size_t)rhead[nnodes] * sizeof(uint32_t));
  mp_free(g, rhead, (size_t)(nnodes + 1) * sizeof(uint32_t));

  *pidom = idom; *ppostnum = postnum; *ppo = po; *ppocount = pocount;
  return 0;
}

/* Retained size per node: shallow + sum over dominator-tree children.
** Process nodes in increasing post-order (leaves first); each node adds its
** retained to its dominator (which has a higher postnum, not yet processed). */
static uint64_t *mp_retained_sizes(global_State *g, const MpGraph *gr,
				   const int32_t *idom, const uint32_t *po,
				   uint32_t pocount)
{
  uint32_t i;
  uint64_t *retained = (uint64_t *)mp_alloc(g, (size_t)gr->nnodes * sizeof(uint64_t));
  if (retained == NULL) return NULL;
  for (i = 0; i < gr->nnodes; i++) retained[i] = gr->nodes[i].shallow;
  for (i = 0; i < pocount; i++) {
    uint32_t n = po[i];
    int32_t d;
    if (n == 0) continue;  /* super-root: no dominator to accumulate into. */
    d = idom[n];
    if (d >= 0 && (uint32_t)d != n)
      retained[d] += retained[n];
  }
  return retained;
}

/* -- public C entry points (called by lib_memprof.c) --------------------- */

/* Sort entry for retained{top=N}. */
typedef struct MpRetEntry {
  uint32_t id;
  uint64_t retained;
} MpRetEntry;

static int mp_cmp_ret_desc(const void *a, const void *b)
{
  const MpRetEntry *x = (const MpRetEntry *)a, *y = (const MpRetEntry *)b;
  if (x->retained < y->retained) return 1;
  if (x->retained > y->retained) return -1;
  return 0;
}

/* lj_memprof_retained: push a Lua array of {addr,type,shallow,retained} sorted
** by retained desc, top N entries (excluding the super-root). Returns 1. */
LJ_FUNC int lj_memprof_retained(lua_State *L, int top, int do_fullgc)
{
  global_State *g = G(L);
  MpGraph gr;
  int32_t *idom = NULL, *postnum = NULL;
  uint32_t *po = NULL, pocount = 0;
  uint64_t *retained = NULL;
  MpRetEntry *arr = NULL;
  uint32_t i, k, nreal, limit;
  GCtab *res;
  int rc;

  rc = mp_graph_build(L, do_fullgc, &gr);
  if (rc != 0)
    return luaL_error(L, "memprof.retained: out of memory building graph");
  if (mp_dominators(g, &gr, &idom, &postnum, &po, &pocount) != 0) {
    mp_graph_free(g, &gr);
    return luaL_error(L, "memprof.retained: out of memory computing dominators");
  }
  retained = mp_retained_sizes(g, &gr, idom, po, pocount);
  if (retained == NULL) {
    mp_free(g, idom, (size_t)gr.nnodes * sizeof(int32_t));
    mp_free(g, postnum, (size_t)gr.nnodes * sizeof(int32_t));
    mp_free(g, po, (size_t)pocount * sizeof(uint32_t));
    mp_graph_free(g, &gr);
    return luaL_error(L, "memprof.retained: out of memory");
  }

  nreal = gr.nnodes - 1;  /* exclude super-root. */
  arr = (MpRetEntry *)mp_alloc(g, (size_t)nreal * sizeof(MpRetEntry));
  if (arr == NULL && nreal > 0) {
    mp_free(g, retained, (size_t)gr.nnodes * sizeof(uint64_t));
    mp_free(g, idom, (size_t)gr.nnodes * sizeof(int32_t));
    mp_free(g, postnum, (size_t)gr.nnodes * sizeof(int32_t));
    mp_free(g, po, (size_t)pocount * sizeof(uint32_t));
    mp_graph_free(g, &gr);
    return luaL_error(L, "memprof.retained: out of memory");
  }
  for (i = 1, k = 0; i < gr.nnodes; i++, k++) {
    arr[k].id = i;
    arr[k].retained = retained[i];
  }
  if (nreal > 0)
    qsort(arr, (size_t)nreal, sizeof(MpRetEntry), mp_cmp_ret_desc);

  limit = (uint32_t)top;
  if (limit == 0 || limit > nreal) limit = nreal;

  res = lj_tab_new_ah(L, (int)limit, 0);
  for (i = 0; i < limit; i++) {
    GCtab *row = lj_tab_new_ah(L, 0, 4);
    TValue kv, vv, key;
    uint32_t id = arr[i].id;
    setstrV(L, &kv, lj_str_newlit(L, "addr"));
    setnumV(&vv, (lua_Number)gr.nodes[id].addr);
    copyTV(L, lj_tab_set(L, row, &kv), &vv);
    setstrV(L, &kv, lj_str_newlit(L, "type"));
    setstrV(L, &vv, lj_str_newz(L, gct_name(gr.nodes[id].gct)));
    copyTV(L, lj_tab_set(L, row, &kv), &vv);
    setstrV(L, &kv, lj_str_newlit(L, "shallow"));
    setnumV(&vv, (lua_Number)gr.nodes[id].shallow);
    copyTV(L, lj_tab_set(L, row, &kv), &vv);
    setstrV(L, &kv, lj_str_newlit(L, "retained"));
    setnumV(&vv, (lua_Number)arr[i].retained);
    copyTV(L, lj_tab_set(L, row, &kv), &vv);
    setnumV(&key, (lua_Number)(i + 1));
    settabV(L, lj_tab_set(L, res, &key), row);
  }

  mp_free(g, arr, (size_t)nreal * sizeof(MpRetEntry));
  mp_free(g, retained, (size_t)gr.nnodes * sizeof(uint64_t));
  mp_free(g, idom, (size_t)gr.nnodes * sizeof(int32_t));
  mp_free(g, postnum, (size_t)gr.nnodes * sizeof(int32_t));
  mp_free(g, po, (size_t)pocount * sizeof(uint32_t));
  mp_graph_free(g, &gr);

  settabV(L, L->top++, res);
  return 1;
}

/* lj_memprof_retainers: push the retaining path for `addr` as a Lua array of
** {addr,type} from the object up to a root (excluding the synthetic
** super-root). Returns 1 (array, possibly empty) or pushes nil if addr is not
** a live in-graph object. */
LJ_FUNC int lj_memprof_retainers(lua_State *L, lua_Number addr_num, int do_fullgc)
{
  global_State *g = G(L);
  MpGraph gr;
  uint32_t *rhead = NULL, *rdst = NULL;
  uint8_t *vis = NULL;
  int32_t *prev = NULL;
  uint32_t *queue = NULL;
  int32_t target;
  uint32_t qh, qt, e, n, steps = 0;
  GCtab *res;
  int rc;

  rc = mp_graph_build(L, do_fullgc, &gr);
  if (rc != 0)
    return luaL_error(L, "memprof.retainers: out of memory building graph");
  if (mp_build_rev(g, &gr, &rhead, &rdst) != 0) {
    mp_graph_free(g, &gr);
    return luaL_error(L, "memprof.retainers: out of memory");
  }

  target = mp_node_lookup(&gr, (uintptr_t)(lua_Number)addr_num);
  if (target <= 0) {
    /* not a live in-graph object (or is the super-root). */
    if (rdst) mp_free(g, rdst, (size_t)rhead[gr.nnodes] * sizeof(uint32_t));
    mp_free(g, rhead, (size_t)(gr.nnodes + 1) * sizeof(uint32_t));
    mp_graph_free(g, &gr);
    lua_pushnil(L);
    return 1;
  }

  vis   = (uint8_t *)mp_alloc(g, (size_t)gr.nnodes);
  prev  = (int32_t *)mp_alloc(g, (size_t)gr.nnodes * sizeof(int32_t));
  queue = (uint32_t *)mp_alloc(g, (size_t)gr.nnodes * sizeof(uint32_t));
  if (!vis || !prev || !queue) {
    mp_free(g, vis, (size_t)gr.nnodes);
    mp_free(g, prev, (size_t)gr.nnodes * sizeof(int32_t));
    mp_free(g, queue, (size_t)gr.nnodes * sizeof(uint32_t));
    if (rdst) mp_free(g, rdst, (size_t)rhead[gr.nnodes] * sizeof(uint32_t));
    mp_free(g, rhead, (size_t)(gr.nnodes + 1) * sizeof(uint32_t));
    mp_graph_free(g, &gr);
    return luaL_error(L, "memprof.retainers: out of memory");
  }
  for (n = 0; n < gr.nnodes; n++) { vis[n] = 0; prev[n] = -1; }

  /* BFS from target over reverse edges (predecessors) to super-root (node 0). */
  qh = 0; qt = 0;
  queue[qt++] = (uint32_t)target;
  vis[target] = 1;
  prev[target] = -2;  /* sentinel: start of path. */
  while (qh < qt) {
    n = queue[qh++];
    if (n == 0) break;  /* reached super-root. */
    if (++steps > gr.nnodes) break;  /* anti-loop (shouldn't happen). */
    for (e = rhead[n]; e < rhead[n + 1]; e++) {
      uint32_t p = rdst[e];
      if (!vis[p]) { vis[p] = 1; prev[p] = (int32_t)n; queue[qt++] = p; }
    }
  }

  res = lj_tab_new_ah(L, 0, 0);
  if (vis[0]) {
    /* reconstruct path: prev[0] = first real root R; walk back to target. */
    int32_t idx = prev[0];
    uint32_t count = 0, i;
    /* first pass: count nodes on the path (R .. target). */
    while (idx >= 0) {
      count++;
      if ((uint32_t)idx == (uint32_t)target) break;
      idx = prev[idx];
    }
    /* Build the array directly in reverse: path is [target, ..., R]. */
    {
      int32_t *order = (int32_t *)mp_alloc(g, (size_t)count * sizeof(int32_t));
      if (order == NULL) {
	mp_free(g, vis, (size_t)gr.nnodes);
	mp_free(g, prev, (size_t)gr.nnodes * sizeof(int32_t));
	mp_free(g, queue, (size_t)gr.nnodes * sizeof(uint32_t));
	if (rdst) mp_free(g, rdst, (size_t)rhead[gr.nnodes] * sizeof(uint32_t));
	mp_free(g, rhead, (size_t)(gr.nnodes + 1) * sizeof(uint32_t));
	mp_graph_free(g, &gr);
	return luaL_error(L, "memprof.retainers: out of memory");
      }
      idx = prev[0];
      for (i = 0; i < count; i++) {
	order[count - 1 - i] = idx;
	if ((uint32_t)idx == (uint32_t)target) break;
	idx = prev[idx];
      }
      for (i = 0; i < count; i++) {
	GCtab *row = lj_tab_new_ah(L, 0, 2);
	TValue kv, vv, ka;
	uint32_t id = (uint32_t)order[i];
	setstrV(L, &kv, lj_str_newlit(L, "addr"));
	setnumV(&vv, (lua_Number)gr.nodes[id].addr);
	copyTV(L, lj_tab_set(L, row, &kv), &vv);
	setstrV(L, &kv, lj_str_newlit(L, "type"));
	setstrV(L, &vv, lj_str_newz(L, gct_name(gr.nodes[id].gct)));
	copyTV(L, lj_tab_set(L, row, &kv), &vv);
	setnumV(&ka, (lua_Number)(i + 1));
	settabV(L, lj_tab_set(L, res, &ka), row);
      }
      mp_free(g, order, (size_t)count * sizeof(int32_t));
    }
  }

  mp_free(g, vis, (size_t)gr.nnodes);
  mp_free(g, prev, (size_t)gr.nnodes * sizeof(int32_t));
  mp_free(g, queue, (size_t)gr.nnodes * sizeof(uint32_t));
  if (rdst) mp_free(g, rdst, (size_t)rhead[gr.nnodes] * sizeof(uint32_t));
  mp_free(g, rhead, (size_t)(gr.nnodes + 1) * sizeof(uint32_t));
  mp_graph_free(g, &gr);

  settabV(L, L->top++, res);
  return 1;
}

/* ======================================================================== **
** v1: Event-stream mode (Capability B, design doc 2.2/3/9).
**
** While active (GCF_MEMPROF set in g->gc.gcmarkflags), every GC-object
** ALLOC/REALLOC/FREE emits a compact ULEB128 record into an in-memory SBuf.
** At stop the symtab sideband (proto/trace -> file:line) is appended, then the
** epilogue, and the whole buffer is flushed to the output file.
**
** Attribution (design 3.2, option B — proto/trace-id + offline symtab):
**   vmstate >= 0          -> TRACE, src_id = trace number
**   vmstate ~INTERP/~C    -> read level-0 frame: LFUNC(proto*) or CFUNC(ffid)
**   else (GC/EXIT/...)    -> INT, src_id = 0
** ALLOC records carry the arena-class (cls) as the type axis and gct=0
** (pending: the caller sets gct after the inline returns). FREE records carry
** the precise gct (valid at free time). POD sweep is aggregate (PODFREE).
**
** v2 (survival-rate): every ALLOC / REALLOC / FREE record appends ONE uleb128
**   = g->gc.stats.cycles
** the monotonic completed-GC-cycle counter (uint64, GCstats, lj_obj.h;
** incremented at the 3 GCSpause cycle-end sites in lj_gc_arena.c). This lets
** the offline tool bucket allocations by the cycle they were born in and
** compute survival = (objects still live at end of stream) / (allocated in
** that cycle) — separating churn (survival ~0) from retained/leaked
** (survival ~1). The field is APPENDED at the end of each record (after
** src_id) so v1 readers halt cleanly on the new version byte and v2 readers
** default cycle=0 for v1 streams. PODFREE carries no cycle (it is an
** aggregate bulk-sweep event with no per-object addr; survival analysis
** matches FREEs by addr and does not consume PODFREE).
**
** v3 (multi-frame stack attribution): every ALLOC / REALLOC / FREE record
**   appends, AFTER the v2 gc_cycle field, a frame stack:
**     uleb(nframes)  then nframes × (byte kind, uleb id)   leaf..root
**   `kind` is one of MP_SRC_{INT,LFUNC,CFUNC,TRACE} (same encoding as the
**   header's src_kind nibble); `id` is the same per-kind identifier the
**   single-frame v1/v2 emitter wrote into the header's src_id field
**   (LFUNC: proto pointer, CFUNC: ffid, TRACE: traceno, INT: 0).
**
**   The header's src_kind nibble and the leading uleb(src_id) field STILL
**   carry the LEAF frame (frames[0]) for v1/v2-shape familiarity and so a
**   v2-only reader that does not know about nframes still finds the leaf
**   attribution in the same place. The v3 suffix is purely additive: a v3
**   record == v2 record + (uleb(nframes), nframes × (byte,uleb)).
**
**   Frames are collected by walking lj_debug_frame(L, level, &size) for
**   level = 0 .. depth-1 (leaf at index 0), mirroring lj_debug_dumpstack's
**   frame-walk. `depth` is the memprof.start{depth=N} knob (clamped to
**   MP_MAX_DEPTH=32), stored in mps->depth. The TRACE whole-event case
**   (vmstate >= 0) has no Lua frame stack to walk and emits nframes=1 with
**   a single TRACE frame. The GC/EXIT/etc. case emits nframes=1 with a
**   single INT frame. FREE records are emitted from the sweep path with no
**   lua_State available; they always emit nframes=1 (single INT frame) so
**   the v3 record shape is uniform across ALLOC/REALLOC/FREE. PODFREE is
**   unchanged (aggregate, no per-object stack). v1/v2 streams synthesize a
**   1-element leaf stack in the parser for back-compat.
**
** v4 (line-precise attribution): every frame in the v3 suffix gains a
**   trailing uleb128 `line` field, so the frame suffix becomes
**     nframes × (byte kind, uleb id, uleb line)
**   For LFUNC frames `line` is the ACTUAL source line of the currently
**   executing bytecode instruction (via lj_debug_frameline, which derives
**   the live PC and maps it through the proto's line info), NOT the
**   function's definition line (pt->firstline). CFUNC/TRACE/INT frames
**   carry line=0 (no bytecode line). The offline tool renders
**   chunkname:ACTUAL_line for v4 streams and falls back to the symtab's
**   firstline for v1/v2/v3 streams. v1/v2/v3 stream parsing is unchanged.
**
** v5 (sampling mode): a byte-accumulator gate in lj_memprof_emit_alloc
**   keeps a running `accum += size`; the ALLOC is emitted ONLY when
**   accum >= interval (the sample period, set via memprof.start{interval=N}).
**   `interval == 0` is EXACT mode = emit every alloc (the DEFAULT), which is
**   byte-for-byte semantically identical to v4 (the weight field is present
**   but carries weight = size, so weight/size = 1 and the aggregator applies
**   no scaling). When interval > 0, each emitted ALLOC appends a trailing
**     uleb(weight)
**   at the END of the ALLOC record (after the v3/v4 frame stack). `weight`
**   is the number of bytes this sample REPRESENTS (= accum at the crossing,
**   then accum resets to 0), so sum(weights) over all sampled ALLOCs equals
**   the true total GC-object allocation bytes. The offline tool scales
**   alloc_objects by weight/size and alloc_space by weight to produce
**   POPULATION ESTIMATES (the Go pprof / V8 allocation-sampling model).
**
**   THE CORRECTNESS TRAP: the profiler matches FREE by address. If only some
**   ALLOCs are emitted, a naive FREE would emit FREE for objects whose ALLOC
**   was never recorded -> phantom frees, negative inuse, broken survival.
**   FIX: a sampled-address hash set (MemprofState.sampled, C-managed via
**   g->allocf — NOT a Lua table; the FREE hook runs in the sweep path with no
**   lua_State) records every emitted (sampled) ALLOC address. emit_free and
**   emit_realloc CONSULT the set and emit ONLY when the address is present;
**   emit_free also REMOVES it. Un-sampled allocations produce no ALLOC, no
**   FREE, no REALLOC — fully invisible — so all downstream math stays
**   consistent. Raw allocf buffers (emit_realloc, cls=0xff) are never
**   inserted into the set (only GC objects via emit_alloc are), so in sampling
**   mode all raw-buffer events are suppressed — they are pure overhead noise.
**
**   REALLOC/FREE/PODFREE records carry NO weight field (they reference an
**   already-weighted sampled object, or are aggregate). The v5 addition is
**   purely the trailing uleb(weight) on ALLOC. v1–v4 streams default weight =
**   size in the parser (weight-per-object = 1, no scaling), so all existing
**   subcommands keep working unchanged for old streams.
** ======================================================================== **/

/* -- Wire format constants ------------------------------------------------ */

enum {
  MP_OP_EPILOGUE = 0,
  MP_OP_ALLOC = 1,
  MP_OP_REALLOC = 2,
  MP_OP_FREE = 3,
  MP_OP_PODFREE = 4,
  MP_OP_SYMTAB_LFUNC = 5,
  MP_OP_SYMTAB_TRACE = 6,
  MP_OP_SYMTAB_CFUNC = 7,
  /* 8 is reserved as the epilogue nibble (0x80). */
  MP_OP_LABELDICT = 9,	/* v6: label-id -> string dictionary (dumped at stop). */
  MP_OP__END = 10
};
/* The epilogue byte: top nibble 8, bottom nibble 0. */
#define MP_EPILOGUE_BYTE	0x80

enum {
  MP_SRC_INT = 0,
  MP_SRC_LFUNC = 1,
  MP_SRC_CFUNC = 2,
  MP_SRC_TRACE = 3
};

#define MP_STREAM_VERSION	6
#define MP_PROLOGUE_MAGIC	"ljm"
#define MP_MAX_DEPTH		32	/* Cap on per-event stack walk depth. */

/* -- Profiler state (single-VM owner, mirrors lj_profile.c ProfileState) -- */

/* Sampled-address hash set (v5 sampling mode). Open-addressing uintptr_t set
** with backward-shift deletion (no tombstones), power-of-two capacity, resized
** at ~70% load. Slot value 0 (NULL) denotes an empty slot — NULL is never a
** valid GCobj address, so it is a safe empty sentinel. Allocated/resized/freed
** via g->allocf (NOT a Lua table): the FREE hook runs in the GC sweep path
** with no lua_State and must not allocate GC objects. The set ops run inside
** mps->in_emit=1; the set's own allocf calls are raw memory and do not recur
** into the profiler (allocf is not a GC-object alloc hook), and the in_emit
** guard covers any lj_mem_realloc-based SBuf growth regardless. */
typedef struct {
  uintptr_t *slots;	/* Power-of-two array, 0 = empty. NULL when inactive. */
  uint32_t mask;	/* capacity - 1 (0 when slots == NULL). */
  uint32_t count;	/* Live entries (== used; backward-shift has no tombstones). */
} SampleSet;

/* v6 label dictionary: a string-interning store mapping small 1-based ids to
** copied label bytes. Managed via g->allocf (raw memory, NOT Lua tables —
** consistent with the sampled-address set above), because the store must not
** hold GCstr pointers (a label string may be collected before stop) and must
** be safe to touch from the stop path. Interndedded on the setlabel
** (Lua-call) path only; the emit hook just writes the current id.
** current_label_id == 0 means "no label" (the default for unlabeled allocs). */
typedef struct {
  char *bytes;		/* Copied label bytes (allocf-managed; NOT NUL-terminated). */
  uint32_t len;		/* Number of bytes in `bytes`. */
} LabelEntry;

typedef struct {
  LabelEntry *entries;	/* Array of length `cap`; ids are 1-based (entries[id-1]). */
  uint32_t count;	/* Number of live labels (ids 1..count). */
  uint32_t cap;		/* Array capacity (doubled on grow). */
} LabelDict;

typedef struct MemprofState {
  global_State *g;	/* Owning VM, or NULL when inactive. */
  SBuf sb;		/* In-memory event stream. */
  FILE *fp;		/* Output file handle. */
  int in_emit;		/* Re-entrancy guard (SBuf growth triggers realloc). */
  int depth;		/* Stack walk depth cap (1..MP_MAX_DEPTH). */
  uint64_t interval;	/* Sampling period in bytes; 0 = EXACT mode (default). */
  uint64_t accum;	/* Byte accumulator (sampling mode only). */
  SampleSet sampled;	/* Sampled-address set (sampling mode only). */
  LabelDict labels;	/* v6: interned label strings (ids 1..count). */
  uint32_t current_label_id;	/* v6: active label (0 = none). Set by setlabel. */
} MemprofState;

static MemprofState memprof_state;

/* -- Sampled-address set (raw allocf, open-addressing, backward-shift) ----- */

/* Fibonacci-multiply the cell-aligned address bits. GCobj addresses are at
** least 16-byte aligned (CellSize = 16), so shift away the low zero bits
** before mixing to spread sequential arena addresses across the table. */
static uint32_t sampleset_hash(uintptr_t a)
{
  uintptr_t x = a >> 4;
  x ^= x >> 16;
  return (uint32_t)((x * (uintptr_t)2654435761u) & 0xffffffffu);
}

/* Allocate / resize / free the slot array via the VM's raw allocator. */
static uintptr_t *sampleset_allocf(global_State *g, uintptr_t *old,
				   size_t oldsz, size_t newsz)
{
  return (uintptr_t *)g->allocf(g->allocd, old, oldsz, newsz);
}

/* Insert `addr` into the set, growing (doubling) if load factor > 70%.
** No-op (returns) if already present. */
static void sampleset_add(SampleSet *s, global_State *g, uintptr_t addr)
{
  uint32_t cap, mask, i;
  uintptr_t *slots;
  if (addr == 0) return;	/* NULL is the empty sentinel — never stored. */
  cap = s->mask ? s->mask + 1 : 0;
  /* Grow at 70% load (or on first insert: start at 256). */
  if (cap == 0 || (uint64_t)s->count * 10 >= (uint64_t)cap * 7) {
    uint32_t newcap = cap ? cap << 1 : 256;
    uintptr_t *newslots;
    uint32_t newmask = newcap - 1, j;
    newslots = sampleset_allocf(g, s->slots, (size_t)cap * sizeof(uintptr_t),
				(size_t)newcap * sizeof(uintptr_t));
    if (newslots == NULL) return;	/* OOM: skip insert (rare; no crash). */
    /* Rehash all live entries into the new table. */
    memset(newslots, 0, (size_t)newcap * sizeof(uintptr_t));
    for (j = 0; j < cap; j++) {
      uintptr_t v = s->slots[j];
      uint32_t k;
      if (v == 0) continue;
      k = sampleset_hash(v) & newmask;
      while (newslots[k] != 0) k = (k + 1) & newmask;
      newslots[k] = v;
    }
    s->slots = newslots;
    s->mask = newmask;
    cap = newcap;
  }
  slots = s->slots;
  mask = s->mask;
  i = sampleset_hash(addr) & mask;
  while (slots[i] != 0) {
    if (slots[i] == addr) return;	/* already present */
    i = (i + 1) & mask;
  }
  slots[i] = addr;
  s->count++;
}

/* Return 1 if `addr` is in the set, 0 otherwise. */
static int sampleset_has(SampleSet *s, uintptr_t addr)
{
  uintptr_t *slots;
  uint32_t mask, i;
  if (s->slots == NULL || addr == 0) return 0;
  slots = s->slots;
  mask = s->mask;
  i = sampleset_hash(addr) & mask;
  while (slots[i] != 0) {
    if (slots[i] == addr) return 1;
    i = (i + 1) & mask;
  }
  return 0;
}

/* Remove `addr` from the set using backward-shift deletion (maintains probe
** sequences without tombstones). Returns 1 if removed, 0 if not found. */
static int sampleset_remove(SampleSet *s, global_State *g, uintptr_t addr)
{
  uintptr_t *slots;
  uint32_t mask, i, j, k, dgap, dcur;
  if (s->slots == NULL || addr == 0) return 0;
  slots = s->slots;
  mask = s->mask;
  i = sampleset_hash(addr) & mask;
  while (slots[i] != 0) {
    if (slots[i] == addr) goto found;
    i = (i + 1) & mask;
  }
  return 0;
found:
  /* Backward-shift: for each subsequent occupied slot j, if its natural home
  ** k is at or before the gap i in probe order (the gap lies within the run
  ** the entry probed from home), move it back to fill the gap. */
  j = i;
  while (1) {
    j = (j + 1) & mask;
    if (slots[j] == 0) break;
    k = sampleset_hash(slots[j]) & mask;
    dgap = (i - k) & mask;	/* distance from home to the gap */
    dcur = (j - k) & mask;	/* distance from home to current slot */
    if (dgap <= dcur) {		/* gap is within the entry's probe run */
      slots[i] = slots[j];
      i = j;
    }
  }
  slots[i] = 0;
  s->count--;
  return 1;
}

/* Free the slot array (called at stop). */
static void sampleset_free(SampleSet *s, global_State *g)
{
  if (s->slots != NULL) {
    sampleset_allocf(g, s->slots, (size_t)(s->mask + 1) * sizeof(uintptr_t), 0);
    s->slots = NULL;
    s->mask = 0;
    s->count = 0;
  }
}

/* -- Label dictionary (raw allocf, linear-scan dedup) -------------------- */

/* v6 label interning: dedup `str` (len bytes) against the live label dict,
** returning the existing 1-based id if seen, or a new id (copying the bytes
** via g->allocf) on first sight. `str == NULL` or `len == 0` returns 0
** (the "no label" sentinel). Called ONLY on the setlabel Lua-call path
** (has L, safe to allocate); the emit hook just reads current_label_id.
** Label counts are expected to be small (tens), so a linear scan is fine. */
static uint32_t label_intern(MemprofState *mps, global_State *g,
			     const char *str, size_t len)
{
  uint32_t i, newid;
  LabelEntry *e;
  char *copy;
  if (str == NULL || len == 0 || len > 0xffffffffu)
    return 0;
  /* Dedup: linear scan for an identical (bytes,len) entry. */
  for (i = 0; i < mps->labels.count; i++) {
    LabelEntry *p = &mps->labels.entries[i];
    if (p->len == (uint32_t)len && memcmp(p->bytes, str, len) == 0)
      return i + 1;	/* 1-based id. */
  }
  /* Grow the entries array (doubling) if full. Start at 8 on first insert. */
  if (mps->labels.count >= mps->labels.cap) {
    uint32_t newcap = mps->labels.cap ? mps->labels.cap << 1 : 8;
    LabelEntry *newe = (LabelEntry *)g->allocf(g->allocd,
			mps->labels.entries,
			(size_t)mps->labels.cap * sizeof(LabelEntry),
			(size_t)newcap * sizeof(LabelEntry));
    if (newe == NULL) return 0;	/* OOM: leave current_label_id unchanged. */
    mps->labels.entries = newe;
    mps->labels.cap = newcap;
  }
  /* Copy the label bytes via allocf (do NOT hold the GCstr pointer — the
  ** string may be collected before stop). */
  copy = (char *)g->allocf(g->allocd, NULL, 0, len);
  if (copy == NULL) return 0;	/* OOM: leave label unset. */
  memcpy(copy, str, len);
  newid = ++mps->labels.count;
  e = &mps->labels.entries[newid - 1];
  e->bytes = copy;
  e->len = (uint32_t)len;
  return newid;
}

/* Free every copied label string + the entries array (called at stop). */
static void labeldict_free(LabelDict *ld, global_State *g)
{
  if (ld->entries != NULL) {
    uint32_t i;
    for (i = 0; i < ld->count; i++) {
      if (ld->entries[i].bytes != NULL)
	g->allocf(g->allocd, ld->entries[i].bytes, ld->entries[i].len, 0);
    }
    g->allocf(g->allocd, ld->entries,
	      (size_t)ld->cap * sizeof(LabelEntry), 0);
    ld->entries = NULL;
    ld->count = 0;
    ld->cap = 0;
  }
}

/* -- ULEB128 writer ------------------------------------------------------- */

static void memprof_put_uleb128(SBuf *sb, uint64_t v)
{
  do {
    uint8_t b = (uint8_t)(v & 0x7f);
    v >>= 7;
    if (v) b |= 0x80;
    lj_buf_putb(sb, b);
  } while (v);
}

/* -- Attribution ---------------------------------------------------------- */

/* A single resolved frame in the per-allocation call stack (leaf..root).
** kind is one of MP_SRC_{INT,LFUNC,CFUNC,TRACE}; id is the per-kind
** identifier (LFUNC: proto pointer, CFUNC: ffid, TRACE: traceno, INT: 0).
** line is the ACTUAL source line for LFUNC frames (v4 line-precise
** attribution via lj_debug_frameline), or 0 for CFUNC/TRACE/INT frames
** (no bytecode line). For v1/v2/v3 streams the parser sets line=0 and
** falls back to the symtab's firstline for display. */
typedef struct MemprofFrame {
  uint8_t kind;
  uint64_t id;
  BCLine line;
} MemprofFrame;

/* Fill `frames` (leaf at index 0) by walking the Lua call stack, returning
** the frame count (1..maxdepth). Mirrors lj_debug_dumpstack's per-level
** lj_debug_frame walk. vmstate>=0 (a trace) and non-INTERP/C vmstates have
** no walkable Lua frame stack and yield a single TRACE / INT frame. */
static int memprof_attribution(global_State *g, lua_State *L,
			       MemprofFrame *frames, int maxdepth)
{
  int32_t vmstate = g->vmstate;
  int n = 0;
  int cap = maxdepth;
  if (cap < 1) cap = 1;
  if (cap > MP_MAX_DEPTH) cap = MP_MAX_DEPTH;
  if (vmstate >= 0) {
    frames[0].kind = MP_SRC_TRACE;
    frames[0].id = (uint64_t)vmstate;
    frames[0].line = 0;
    return 1;
  }
  {
    int vs = ~vmstate;
    if (vs != LJ_VMST_INTERP && vs != LJ_VMST_C) {
      frames[0].kind = MP_SRC_INT;
      frames[0].id = 0;
      frames[0].line = 0;
      return 1;
    }
  }
  {
    int level;
    for (level = 0; level < cap; level++) {
      int size;
      cTValue *frame = lj_debug_frame(L, level, &size);
      cTValue *nextframe;
      GCfunc *fn;
      if (frame == NULL) break;  /* past the outermost frame */
      nextframe = size ? frame+size : NULL;
      fn = frame_func(frame);
      if (isluafunc(fn)) {
        frames[n].kind = MP_SRC_LFUNC;
        frames[n].id = (uint64_t)(uintptr_t)funcproto(fn);
        {
          BCLine ln = lj_debug_frameline(L, fn, nextframe);
          if (ln == (BCLine)-1)
            ln = funcproto(fn)->firstline;
          frames[n].line = ln;
        }
      } else if (iscfunc(fn) || isffunc(fn)) {
        frames[n].kind = MP_SRC_CFUNC;
        frames[n].id = (uint64_t)fn->c.ffid;
        frames[n].line = 0;
      } else {
        frames[n].kind = MP_SRC_INT;
        frames[n].id = 0;
        frames[n].line = 0;
      }
      n++;
    }
  }
  if (n == 0) {
    frames[0].kind = MP_SRC_INT;
    frames[0].id = 0;
    frames[0].line = 0;
    n = 1;
  }
  return n;
}

/* Emit the v4 frame-stack suffix: uleb(nframes) then
** nframes × (byte kind, uleb id, uleb line). The per-frame `line` field
** (v4) carries the actual source line for LFUNC frames; v1/v2/v3 streams
** do not have this field (the parser defaults line=0 / firstline). */
static void memprof_emit_frames(SBuf *sb, const MemprofFrame *frames, int n)
{
  int i;
  memprof_put_uleb128(sb, (uint64_t)n);
  for (i = 0; i < n; i++) {
    lj_buf_putb(sb, frames[i].kind);
    memprof_put_uleb128(sb, frames[i].id);
    memprof_put_uleb128(sb, (uint64_t)frames[i].line);
  }
}

/* -- Emit functions (out-of-line, called from the hot inlines) ------------- */

LJ_FUNC void lj_memprof_emit_alloc(lua_State *L, void *o, GCSize size,
				   int cls, int link)
{
  MemprofState *mps = &memprof_state;
  global_State *g;
  SBuf *sb;
  MemprofFrame frames[MP_MAX_DEPTH];
  int nframes;
  uint64_t weight;	/* v5: bytes this sample represents (== size in exact). */
  UNUSED(link);
  if (mps->in_emit) return;
  g = mps->g;
  if (g == NULL) return;
  /* v5 sampling gate: accumulate bytes; emit only on threshold crossing.
  ** interval == 0 is EXACT mode — no accumulator math, no set ops. */
  if (mps->interval) {
    mps->accum += (uint64_t)size;
    if (mps->accum < mps->interval)
      return;	/* not sampled — fully invisible (no event, no set entry) */
    weight = mps->accum;	/* bytes accumulated since the last sample */
    mps->accum = 0;
  } else {
    weight = (uint64_t)size;	/* exact mode: weight == size (no scaling) */
  }
  mps->in_emit = 1;
  sb = &mps->sb;
  nframes = memprof_attribution(g, L, frames, mps->depth);
  lj_buf_putb(sb, (uint8_t)((MP_OP_ALLOC << 4) | (frames[0].kind & 0xf)));
  memprof_put_uleb128(sb, (uint64_t)(uintptr_t)o);
  memprof_put_uleb128(sb, (uint64_t)size);
  lj_buf_putb(sb, 0);		/* gct pending (caller sets after return) */
  lj_buf_putb(sb, (uint8_t)cls);	/* arena-class type axis */
  lj_buf_putb(sb, (uint8_t)g->gc.state);
  memprof_put_uleb128(sb, frames[0].id);  /* leaf src_id (v1/v2 shape) */
  memprof_put_uleb128(sb, (uint64_t)g->gc.stats.cycles);  /* v2: cycle */
  memprof_emit_frames(sb, frames, nframes);  /* v3: leaf..root stack */
  memprof_put_uleb128(sb, weight);  /* v5: trailing weight (end-of-record) */
  memprof_put_uleb128(sb, mps->current_label_id);  /* v6: label id (0=none) */
  if (mps->interval)
    sampleset_add(&mps->sampled, g, (uintptr_t)o);  /* record sampled addr */
  mps->in_emit = 0;
}

LJ_FUNC void lj_memprof_emit_realloc(lua_State *L, void *p,
				     GCSize osz, GCSize nsize)
{
  MemprofState *mps = &memprof_state;
  global_State *g;
  SBuf *sb;
  MemprofFrame frames[MP_MAX_DEPTH];
  int nframes;
  uint8_t op;
  if (mps->in_emit) return;
  g = mps->g;
  if (g == NULL) return;
  /* v5 sampling mode: raw allocf buffers (cls=0xff) are never inserted into
  ** the sampled-address set (only GC objects via emit_alloc are), so this
  ** consultation never matches for raw buffers and all raw-buffer events are
  ** suppressed — they are pure overhead noise. If the address happens to be
  ** in the set (defensive; cannot occur for raw buffers vs GC objects), emit
  ** so a sampled object is not lost. */
  if (mps->interval) {
    if (!sampleset_has(&mps->sampled, (uintptr_t)p))
      return;
  }
  mps->in_emit = 1;
  sb = &mps->sb;
  nframes = memprof_attribution(g, L, frames, mps->depth);
  op = (osz == 0) ? MP_OP_ALLOC : (nsize == 0) ? MP_OP_FREE : MP_OP_REALLOC;
  lj_buf_putb(sb, (uint8_t)((op << 4) | (frames[0].kind & 0xf)));
  memprof_put_uleb128(sb, (uint64_t)(uintptr_t)p);
  if (op == MP_OP_ALLOC) {
    memprof_put_uleb128(sb, (uint64_t)nsize);
    lj_buf_putb(sb, 0);	/* no gct for raw buffers */
    lj_buf_putb(sb, 0xff);	/* cls sentinel: raw allocf memory */
    lj_buf_putb(sb, (uint8_t)g->gc.state);
  } else if (op == MP_OP_FREE) {
    memprof_put_uleb128(sb, (uint64_t)osz);
    lj_buf_putb(sb, 0);
  } else {
    memprof_put_uleb128(sb, (uint64_t)osz);
    memprof_put_uleb128(sb, (uint64_t)nsize);
  }
  memprof_put_uleb128(sb, frames[0].id);  /* leaf src_id */
  memprof_put_uleb128(sb, (uint64_t)g->gc.stats.cycles);  /* v2: cycle */
  memprof_emit_frames(sb, frames, nframes);  /* v3: leaf..root stack */
  if (op == MP_OP_ALLOC) {
    /* v5: ALLOC records (including raw-buffer ALLOC) carry a trailing
    ** weight. Exact mode: weight == nsize (no scaling). Sampling mode only
    ** reaches here for set members (see the gate above). */
    memprof_put_uleb128(sb, (uint64_t)nsize);
    memprof_put_uleb128(sb, mps->current_label_id);  /* v6: label id (0=none) */
  }
  if (mps->interval && op == MP_OP_FREE)
    sampleset_remove(&mps->sampled, g, (uintptr_t)p);
  mps->in_emit = 0;
}

LJ_FUNC void lj_memprof_emit_free(global_State *g, void *o,
				  size_t osize, uint32_t gct)
{
  MemprofState *mps = &memprof_state;
  SBuf *sb;
  if (mps->in_emit) return;
  if (mps->g == NULL) return;
  /* v5 sampling mode: emit FREE only for previously-sampled objects, so a
  ** FREE is never emitted for an object whose ALLOC was not recorded (the
  ** phantom-free trap). Remove the address so a second FREE is a no-op. */
  if (mps->interval) {
    if (!sampleset_has(&mps->sampled, (uintptr_t)o))
      return;
  }
  mps->in_emit = 1;
  sb = &mps->sb;
  lj_buf_putb(sb, (uint8_t)((MP_OP_FREE << 4) | MP_SRC_INT));
  memprof_put_uleb128(sb, (uint64_t)(uintptr_t)o);
  memprof_put_uleb128(sb, (uint64_t)osize);
  lj_buf_putb(sb, (uint8_t)gct);
  memprof_put_uleb128(sb, 0);	/* INT: no source id */
  memprof_put_uleb128(sb, (uint64_t)mps->g->gc.stats.cycles);  /* v2: cycle */
  /* v3: FREE has no walkable Lua stack (emitted from sweep); single INT frame. */
  {
    MemprofFrame fint = { MP_SRC_INT, 0, 0 };
    memprof_emit_frames(sb, &fint, 1);
  }
  if (mps->interval)
    sampleset_remove(&mps->sampled, g, (uintptr_t)o);
  mps->in_emit = 0;
}

LJ_FUNC void lj_memprof_emit_podfree(global_State *g, uint32_t cellcount,
				     size_t bytes)
{
  MemprofState *mps = &memprof_state;
  SBuf *sb;
  UNUSED(g);
  if (mps->in_emit) return;
  if (mps->g == NULL) return;
  mps->in_emit = 1;
  sb = &mps->sb;
  lj_buf_putb(sb, (uint8_t)((MP_OP_PODFREE << 4) | MP_SRC_INT));
  memprof_put_uleb128(sb, (uint64_t)cellcount);
  memprof_put_uleb128(sb, (uint64_t)bytes);
  mps->in_emit = 0;
}

/* -- Symtab scan-and-dump (at stop, zero hot-path cost) ------------------- */

static uint64_t memprof_read_uleb128(const char **pp, const char *e)
{
  const char *p = *pp;
  uint64_t v = 0;
  int s = 0;
  while (p < e) {
    uint8_t b = (uint8_t)*p++;
    v |= (uint64_t)(b & 0x7f) << s;
    s += 7;
    if (!(b & 0x80)) break;
  }
  *pp = p;
  return v;
}

/* Collect unique LFUNC proto pointers and TRACE ids from the event stream,
** then append SYMTAB_* records. Dedup via a file-local array (unique protos
** are typically O(hundreds) in a session; cap 4096, overflow skips). v3
** streams carry a frame stack per record; every LFUNC/TRACE frame id in
** every stack is collected so the symtab covers all frames, not just the
** leaf. */
#define MP_SYMTAB_CAP	4096

static void symtab_note_proto(uintptr_t pp, uintptr_t *protos, int *nprotos)
{
  int i;
  for (i = 0; i < *nprotos; i++) if (protos[i] == pp) return;
  if (*nprotos < MP_SYMTAB_CAP) protos[(*nprotos)++] = pp;
}

static void symtab_note_trace(uintptr_t tt, uintptr_t *traces, int *ntraces)
{
  int i;
  for (i = 0; i < *ntraces; i++) if (traces[i] == tt) return;
  if (*ntraces < MP_SYMTAB_CAP) traces[(*ntraces)++] = tt;
}

/* Read and discard a v3/v4 frame stack, collecting unique LFUNC/TRACE ids.
** v4+ streams carry an extra uleb128 `line` field per frame (the actual
** source line for LFUNC frames); v3 streams stop after `id`. The `has_line`
** flag selects which shape to read so the scanner does not desync. */
static void symtab_read_frames(const char **pp, const char *e, int has_line,
			       uintptr_t *protos, int *nprotos,
			       uintptr_t *traces, int *ntraces)
{
  const char *p = *pp;
  uint64_t n = memprof_read_uleb128(&p, e);
  uint64_t i;
  for (i = 0; i < n; i++) {
    uint8_t kind;
    uint64_t id;
    if (p >= e) break;
    kind = (uint8_t)*p++;
    id = memprof_read_uleb128(&p, e);
    if (has_line) (void)memprof_read_uleb128(&p, e);
    if (kind == MP_SRC_LFUNC)
      symtab_note_proto((uintptr_t)id, protos, nprotos);
    else if (kind == MP_SRC_TRACE)
      symtab_note_trace((uintptr_t)id, traces, ntraces);
  }
  *pp = p;
}

static void memprof_dump_symtab(global_State *g, SBuf *sb)
{
  const char *p = sb->b;
  const char *e = sb->w;
  static uintptr_t protos[MP_SYMTAB_CAP];
  static uintptr_t traces[MP_SYMTAB_CAP];
  int nprotos = 0, ntraces = 0;
  int i;
  memset(protos, 0, sizeof(protos));
  memset(traces, 0, sizeof(traces));

  /* Walk the event stream (skip the 5-byte prologue). */
  int has_cycle = 0, has_frames = 0, has_line = 0, has_weight = 0, has_label = 0;
  if (p + 5 <= e) {
    uint8_t ver = (uint8_t)p[3];
    has_cycle = (ver >= 2);   /* v2+ appends a gc_cycle uleb128 */
    has_frames = (ver >= 3); /* v3+ appends a frame stack */
    has_line = (ver >= 4);   /* v4+ appends a per-frame uleb line */
    has_weight = (ver >= 5); /* v5+ appends a trailing uleb weight on ALLOC */
    has_label = (ver >= 6);  /* v6+ appends a trailing uleb label_id on ALLOC */
    p += 5;
  }
  while (p < e) {
    uint8_t hdr = (uint8_t)*p++;
    uint8_t op = hdr >> 4;
    uint8_t sk = hdr & 0xf;
    if (hdr == MP_EPILOGUE_BYTE) break;
    switch (op) {
    case MP_OP_ALLOC:
      memprof_read_uleb128(&p, e);	/* addr */
      memprof_read_uleb128(&p, e);	/* size */
      p++;					/* gct */
      p++;					/* cls */
      p++;					/* gcstate */
      {
	uint64_t id = memprof_read_uleb128(&p, e);  /* leaf src_id */
	if (sk == MP_SRC_LFUNC)
	  symtab_note_proto((uintptr_t)id, protos, &nprotos);
	else if (sk == MP_SRC_TRACE)
	  symtab_note_trace((uintptr_t)id, traces, &ntraces);
      }
      if (has_cycle) memprof_read_uleb128(&p, e);  /* v2: gc_cycle */
      if (has_frames)
	symtab_read_frames(&p, e, has_line, protos, &nprotos, traces, &ntraces);
      if (has_weight) memprof_read_uleb128(&p, e);  /* v5: trailing weight */
      if (has_label) memprof_read_uleb128(&p, e);  /* v6: trailing label_id */
      break;
    case MP_OP_REALLOC:
      memprof_read_uleb128(&p, e);	/* addr */
      memprof_read_uleb128(&p, e);	/* osize */
      memprof_read_uleb128(&p, e);	/* nsize */
      {
	uint64_t id = memprof_read_uleb128(&p, e);  /* leaf src_id */
	if (sk == MP_SRC_LFUNC)
	  symtab_note_proto((uintptr_t)id, protos, &nprotos);
	else if (sk == MP_SRC_TRACE)
	  symtab_note_trace((uintptr_t)id, traces, &ntraces);
      }
      if (has_cycle) memprof_read_uleb128(&p, e);  /* v2: gc_cycle */
      if (has_frames)
	symtab_read_frames(&p, e, has_line, protos, &nprotos, traces, &ntraces);
      break;
    case MP_OP_FREE:
      memprof_read_uleb128(&p, e);	/* addr */
      memprof_read_uleb128(&p, e);	/* osize */
      p++;					/* gct */
      memprof_read_uleb128(&p, e);	/* src_id (always INT/0) */
      if (has_cycle) memprof_read_uleb128(&p, e);  /* v2: gc_cycle */
      if (has_frames)
	symtab_read_frames(&p, e, has_line, protos, &nprotos, traces, &ntraces);
      break;
    case MP_OP_PODFREE:
      memprof_read_uleb128(&p, e);	/* cellcount */
      memprof_read_uleb128(&p, e);	/* bytes */
      break;
    default:
      goto done;	/* unknown opcode: stop scanning */
    }
  }
done:
  /* Emit SYMTAB_LFUNC records. */
  for (i = 0; i < nprotos; i++) {
    GCproto *pt = (GCproto *)(void *)protos[i];
    GCstr *cn;
    const char *name;
    MSize len;
    if (pt == NULL) continue;
    cn = proto_chunkname(pt);
    name = strdata(cn);
    len = cn->len;
    lj_buf_putb(sb, (uint8_t)((MP_OP_SYMTAB_LFUNC << 4) | 0));
    memprof_put_uleb128(sb, (uint64_t)protos[i]);
    memprof_put_uleb128(sb, (uint64_t)len);
    lj_buf_putmem(sb, name, len);
    memprof_put_uleb128(sb, (uint64_t)pt->firstline);
  }
  /* Emit SYMTAB_TRACE records (also pick up live traces not seen in events). */
#if LJ_HASJIT
  {
    jit_State *J = G2J(g);
    MSize tn;
    for (tn = 1; tn < J->sizetrace; tn++) {
      GCtrace *tr = (GCtrace *)gcref(J->trace[tn]);
      if (tr == NULL) continue;
      GCproto *tpt = (GCproto *)gcrefp(tr->startpt, GCproto);
      lj_buf_putb(sb, (uint8_t)((MP_OP_SYMTAB_TRACE << 4) | 0));
      memprof_put_uleb128(sb, (uint64_t)tn);
      memprof_put_uleb128(sb, (uint64_t)(uintptr_t)tpt);
      memprof_put_uleb128(sb, (uint64_t)(tpt ? tpt->firstline : 0));
    }
  }
#endif
  UNUSED(g);
}

/* v6: dump the LABELDICT section — one record per interned label id, so the
** offline tool can resolve per-ALLOC label ids back to their strings. Each
** record: header byte (MP_OP_LABELDICT<<4 | 0), uleb(id), uleb(len), len bytes.
** Emitted at stop AFTER the symtab section and BEFORE the epilogue. The
** header high nibble is 9 (0x90), which cannot collide with the 0x80 epilogue
** byte (the scanner checks `hdr == 0x80` first). ids are 1-based; id 0 is the
** "no label" sentinel and is never emitted. */
static void memprof_dump_labeldict(MemprofState *mps, SBuf *sb)
{
  uint32_t i;
  for (i = 0; i < mps->labels.count; i++) {
    lj_buf_putb(sb, (uint8_t)((MP_OP_LABELDICT << 4) | 0));
    memprof_put_uleb128(sb, (uint64_t)(i + 1));		/* 1-based id */
    memprof_put_uleb128(sb, (uint64_t)mps->labels.entries[i].len);
    lj_buf_putmem(sb, mps->labels.entries[i].bytes,
		  (MSize)mps->labels.entries[i].len);
  }
}

/* -- Start / Stop --------------------------------------------------------- */

LJ_FUNC int lj_memprof_start(lua_State *L, const char *outpath, int depth,
			     uint64_t interval)
{
  MemprofState *mps = &memprof_state;
  global_State *g = G(L);
  if (mps->g != NULL)
    return 1;	/* Already active (possibly on another VM). */
  if (outpath == NULL)
    return 2;
  mps->fp = fopen(outpath, "wb");
  if (mps->fp == NULL)
    return 2;
  mps->g = g;
  mps->in_emit = 0;
  mps->depth = depth > 0 ? depth : 1;
  mps->interval = interval;	/* 0 = EXACT mode (default). */
  mps->accum = 0;
  mps->sampled.slots = NULL;
  mps->sampled.mask = 0;
  mps->sampled.count = 0;
  mps->labels.entries = NULL;
  mps->labels.count = 0;
  mps->labels.cap = 0;
  mps->current_label_id = 0;
  lj_buf_init(L, &mps->sb);
  lj_buf_need(&mps->sb, 4096);
  lj_buf_reset(&mps->sb);
  lj_buf_putmem(&mps->sb, MP_PROLOGUE_MAGIC, 3);
  lj_buf_putb(&mps->sb, MP_STREAM_VERSION);
  lj_buf_putb(&mps->sb, 0);	/* reserved */
  g->gc.gcmarkflags |= GCF_MEMPROF;
  return 0;
}

LJ_FUNC void lj_memprof_stop(lua_State *L)
{
  MemprofState *mps = &memprof_state;
  global_State *g;
  SBuf *sb;
  if (mps->g != G(L))
    return;	/* Not the owning VM. */
  g = mps->g;
  g->gc.gcmarkflags &= ~GCF_MEMPROF;
  sb = &mps->sb;
  memprof_dump_symtab(g, sb);
  memprof_dump_labeldict(mps, sb);  /* v6: label-id -> string dictionary. */
  lj_buf_putb(sb, MP_EPILOGUE_BYTE);
  if (mps->fp != NULL) {
    fwrite(sb->b, 1, sbuflen(sb), mps->fp);
    fclose(mps->fp);
    mps->fp = NULL;
  }
  lj_buf_free(g, sb);
  sampleset_free(&mps->sampled, g);  /* v5: release the sampled-address set. */
  labeldict_free(&mps->labels, g);  /* v6: release the label dictionary. */
  mps->current_label_id = 0;
  mps->interval = 0;
  mps->accum = 0;
  mps->g = NULL;
}

/* v6: set the current allocation label. Called from the memprof.setlabel
** Lua function (the Lua-call path — has L, safe to intern via g->allocf).
** str==NULL or len==0 clears the label (current_label_id = 0). When the
** profiler is inactive (mps->g == NULL) this is a documented no-op. */
LJ_FUNC void lj_memprof_setlabel(lua_State *L, const char *str, size_t len)
{
  MemprofState *mps = &memprof_state;
  global_State *g;
  if (mps->g == NULL) return;	/* inactive: no-op (documented). */
  if (mps->g != G(L)) return;	/* not the owning VM. */
  g = mps->g;
  if (str == NULL || len == 0) {
    mps->current_label_id = 0;
    return;
  }
  mps->current_label_id = label_intern(mps, g, str, len);
}

#endif /* LJ_HASGCMARK && defined(LUAJIT_ENABLE_MEMPROF) */

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

#include <string.h>
#include <stdlib.h>

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

#endif /* LJ_HASGCMARK && defined(LUAJIT_ENABLE_MEMPROF) */

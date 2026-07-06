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
** v1: Event-stream mode (Capability B, design doc 2.2/3/9).
**
** While active (GCF_MEMPROF set in g->gc.gcmarkflags), every GC-object
** ALLOC/REALLOC/FREE emits a compact ULEB128 record into an in-memory SBuf.
** At stop the symtab sideband (proto/trace -> file:line) is appended, then the
** epilogue, and the whole buffer is flushed to the output file.
**
** Attribution (design 3.2, option B — proto/trace-id + offline symtab):
**   vmstate >= 0          -> TRACE, src_id = trace number
**   vmstate ~INTERP/~C    -> read level-0 frame: LFUNC(proto*) or CFUNC(fn*)
**   else (GC/EXIT/...)    -> INT, src_id = 0
** ALLOC records carry the arena-class (cls) as the type axis and gct=0
** (pending: the caller sets gct after the inline returns). FREE records carry
** the precise gct (valid at free time). POD sweep is aggregate (PODFREE).
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
  MP_OP__END = 9
};
/* The epilogue byte: top nibble 8, bottom nibble 0. */
#define MP_EPILOGUE_BYTE	0x80

enum {
  MP_SRC_INT = 0,
  MP_SRC_LFUNC = 1,
  MP_SRC_CFUNC = 2,
  MP_SRC_TRACE = 3
};

#define MP_STREAM_VERSION	1
#define MP_PROLOGUE_MAGIC	"ljm"

/* -- Profiler state (single-VM owner, mirrors lj_profile.c ProfileState) -- */

typedef struct MemprofState {
  global_State *g;	/* Owning VM, or NULL when inactive. */
  SBuf sb;		/* In-memory event stream. */
  FILE *fp;		/* Output file handle. */
  int in_emit;		/* Re-entrancy guard (SBuf growth triggers realloc). */
  int depth;		/* Stack read depth (reserved, currently 1). */
} MemprofState;

static MemprofState memprof_state;

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

static void memprof_attribution(global_State *g, lua_State *L,
				uint8_t *src_kind, uint64_t *src_id)
{
  int32_t vmstate = g->vmstate;
  if (vmstate >= 0) {
    *src_kind = MP_SRC_TRACE;
    *src_id = (uint64_t)vmstate;
    return;
  }
  *src_kind = MP_SRC_INT;
  *src_id = 0;
  {
    int vs = ~vmstate;
    if (vs == LJ_VMST_INTERP || vs == LJ_VMST_C) {
      int size;
      cTValue *frame = lj_debug_frame(L, 0, &size);
      if (frame) {
	GCfunc *fn = frame_func(frame);
	if (isluafunc(fn)) {
	  *src_kind = MP_SRC_LFUNC;
	  *src_id = (uint64_t)(uintptr_t)funcproto(fn);
	} else if (iscfunc(fn) || isffunc(fn)) {
	  *src_kind = MP_SRC_CFUNC;
	  *src_id = (uint64_t)(uintptr_t)fn;
	}
      }
    }
  }
}

/* -- Emit functions (out-of-line, called from the hot inlines) ------------- */

LJ_FUNC void lj_memprof_emit_alloc(lua_State *L, void *o, GCSize size,
				   int cls, int link)
{
  MemprofState *mps = &memprof_state;
  global_State *g;
  SBuf *sb;
  uint8_t src_kind = 0;
  uint64_t src_id = 0;
  UNUSED(link);
  if (mps->in_emit) return;
  g = mps->g;
  if (g == NULL) return;
  mps->in_emit = 1;
  sb = &mps->sb;
  memprof_attribution(g, L, &src_kind, &src_id);
  lj_buf_putb(sb, (uint8_t)((MP_OP_ALLOC << 4) | (src_kind & 0xf)));
  memprof_put_uleb128(sb, (uint64_t)(uintptr_t)o);
  memprof_put_uleb128(sb, (uint64_t)size);
  lj_buf_putb(sb, 0);		/* gct pending (caller sets after return) */
  lj_buf_putb(sb, (uint8_t)cls);	/* arena-class type axis */
  lj_buf_putb(sb, (uint8_t)g->gc.state);
  memprof_put_uleb128(sb, src_id);
  mps->in_emit = 0;
}

LJ_FUNC void lj_memprof_emit_realloc(lua_State *L, void *p,
				     GCSize osz, GCSize nsize)
{
  MemprofState *mps = &memprof_state;
  global_State *g;
  SBuf *sb;
  uint8_t src_kind = 0, op;
  uint64_t src_id = 0;
  if (mps->in_emit) return;
  g = mps->g;
  if (g == NULL) return;
  mps->in_emit = 1;
  sb = &mps->sb;
  memprof_attribution(g, L, &src_kind, &src_id);
  op = (osz == 0) ? MP_OP_ALLOC : (nsize == 0) ? MP_OP_FREE : MP_OP_REALLOC;
  lj_buf_putb(sb, (uint8_t)((op << 4) | (src_kind & 0xf)));
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
  memprof_put_uleb128(sb, src_id);
  mps->in_emit = 0;
}

LJ_FUNC void lj_memprof_emit_free(global_State *g, void *o,
				  size_t osize, uint32_t gct)
{
  MemprofState *mps = &memprof_state;
  SBuf *sb;
  UNUSED(g);
  if (mps->in_emit) return;
  if (mps->g == NULL) return;
  mps->in_emit = 1;
  sb = &mps->sb;
  lj_buf_putb(sb, (uint8_t)((MP_OP_FREE << 4) | MP_SRC_INT));
  memprof_put_uleb128(sb, (uint64_t)(uintptr_t)o);
  memprof_put_uleb128(sb, (uint64_t)osize);
  lj_buf_putb(sb, (uint8_t)gct);
  memprof_put_uleb128(sb, 0);	/* INT: no source id */
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
** are typically O(hundreds) in a session; cap 4096, overflow skips). */
#define MP_SYMTAB_CAP	4096

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
  if (p + 5 <= e) p += 5;
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
      if (sk == MP_SRC_LFUNC) {
	uint64_t id = memprof_read_uleb128(&p, e);
	uintptr_t pp = (uintptr_t)id;
	for (i = 0; i < nprotos; i++) if (protos[i] == pp) break;
	if (i == nprotos && nprotos < MP_SYMTAB_CAP) protos[nprotos++] = pp;
      } else if (sk == MP_SRC_TRACE) {
	uint64_t id = memprof_read_uleb128(&p, e);
	uintptr_t pp = (uintptr_t)id;
	for (i = 0; i < ntraces; i++) if (traces[i] == pp) break;
	if (i == ntraces && ntraces < MP_SYMTAB_CAP) traces[ntraces++] = pp;
      } else {
	memprof_read_uleb128(&p, e);
      }
      break;
    case MP_OP_REALLOC:
      memprof_read_uleb128(&p, e);	/* addr */
      memprof_read_uleb128(&p, e);	/* osize */
      memprof_read_uleb128(&p, e);	/* nsize */
      memprof_read_uleb128(&p, e);	/* src_id */
      break;
    case MP_OP_FREE:
      memprof_read_uleb128(&p, e);	/* addr */
      memprof_read_uleb128(&p, e);	/* osize */
      p++;					/* gct */
      memprof_read_uleb128(&p, e);	/* src_id */
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

/* -- Start / Stop --------------------------------------------------------- */

LJ_FUNC int lj_memprof_start(lua_State *L, const char *outpath, int depth)
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
  lj_buf_putb(sb, MP_EPILOGUE_BYTE);
  if (mps->fp != NULL) {
    fwrite(sb->b, 1, sbuflen(sb), mps->fp);
    fclose(mps->fp);
    mps->fp = NULL;
  }
  lj_buf_free(g, sb);
  mps->g = NULL;
}

#endif /* LJ_HASGCMARK && defined(LUAJIT_ENABLE_MEMPROF) */

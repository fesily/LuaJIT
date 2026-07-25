/*
** String handling.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_str_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_str.h"
#include "lj_char.h"
#include "lj_prng.h"

/* -- String helpers ------------------------------------------------------ */

/* Ordered compare of strings. Assumes string data is 4-byte aligned. */
int32_t LJ_FASTCALL lj_str_cmp(GCstr *a, GCstr *b)
{
  MSize i, n = a->len > b->len ? b->len : a->len;
  for (i = 0; i < n; i += 4) {
    /* Note: innocuous access up to end of string + 3. */
    uint32_t va = *(const uint32_t *)(strdata(a)+i);
    uint32_t vb = *(const uint32_t *)(strdata(b)+i);
    if (va != vb) {
#if LJ_LE
      va = lj_bswap(va); vb = lj_bswap(vb);
#endif
      i -= n;
      if ((int32_t)i >= -3) {
	va >>= 32+(i<<3); vb >>= 32+(i<<3);
	if (va == vb) break;
      }
      return va < vb ? -1 : 1;
    }
  }
  return (int32_t)(a->len - b->len);
}

/* Find fixed string p inside string s. */
const char *lj_str_find(const char *s, const char *p, MSize slen, MSize plen)
{
  if (plen <= slen) {
    if (plen == 0) {
      return s;
    } else {
      int c = *(const uint8_t *)p++;
      plen--; slen -= plen;
      while (slen) {
	const char *q = (const char *)memchr(s, c, slen);
	if (!q) break;
	if (memcmp(q+1, p, plen) == 0) return q;
	q++; slen -= (MSize)(q-s); s = q;
      }
    }
  }
  return NULL;
}

/* Check whether a string has a pattern matching character. */
int lj_str_haspattern(GCstr *s)
{
  const char *p = strdata(s), *q = p + s->len;
  while (p < q) {
    int c = *(const uint8_t *)p++;
    if (lj_char_ispunct(c) && strchr("^$*+?.([%-", c))
      return 1;  /* Found a pattern matching char. */
  }
  return 0;  /* No pattern matching chars found. */
}

/* -- String hashing ------------------------------------------------------ */

/* Keyed sparse ARX string hash. Constant time. */
static StrHash hash_sparse(uint64_t seed, const char *str, MSize len)
{
  /* Constants taken from lookup3 hash by Bob Jenkins. */
  StrHash a, b, h = len ^ (StrHash)seed;
  if (len >= 4) {  /* Caveat: unaligned access! */
    a = lj_getu32(str);
    h ^= lj_getu32(str+len-4);
    b = lj_getu32(str+(len>>1)-2);
    h ^= b; h -= lj_rol(b, 14);
    b += lj_getu32(str+(len>>2)-1);
  } else {
    a = *(const uint8_t *)str;
    h ^= *(const uint8_t *)(str+len-1);
    b = *(const uint8_t *)(str+(len>>1));
    h ^= b; h -= lj_rol(b, 14);
  }
  a ^= h; a -= lj_rol(h, 11);
  b ^= a; b -= lj_rol(a, 25);
  h ^= b; h -= lj_rol(b, 16);
  return h;
}

#if LUAJIT_SECURITY_STRHASH
#if !LJ_HASGCMARK
/* Keyed dense ARX string hash. Linear time. */
static LJ_NOINLINE StrHash hash_dense(uint64_t seed, StrHash h,
				      const char *str, MSize len)
{
  StrHash b = lj_bswap(lj_rol(h ^ (StrHash)(seed >> 32), 4));
  if (len > 12) {
    StrHash a = (StrHash)seed;
    const char *pe = str+len-12, *p = pe, *q = str;
    do {
      a += lj_getu32(p);
      b += lj_getu32(p+4);
      h += lj_getu32(p+8);
      p = q; q += 12;
      h ^= b; h -= lj_rol(b, 14);
      a ^= h; a -= lj_rol(h, 11);
      b ^= a; b -= lj_rol(a, 25);
    } while (p < pe);
    h ^= b; h -= lj_rol(b, 16);
    a ^= h; a -= lj_rol(h, 4);
    b ^= a; b -= lj_rol(a, 14);
  }
  return b;
}
#endif
#endif

/* -- String interning ---------------------------------------------------- */

#define LJ_STR_MAXCOLL		32

#if !LJ_HASGCMARK
/* ===== Chain-path intern table (classic GC only, !LJ_HASGCMARK) ===== */

/* Resize the string interning hash table (grow and shrink). */
void lj_str_resize(lua_State *L, MSize newmask)
{
  global_State *g = G(L);
  GCRef *newtab, *oldtab = g->str.tab;
  MSize i;

  /* No resizing during GC traversal or if already too big. */
  if (g->gc.state == GCSsweepstring || newmask >= LJ_MAX_STRTAB-1)
    return;

  newtab = lj_mem_newvec(L, newmask+1, GCRef);
  memset(newtab, 0, (newmask+1)*sizeof(GCRef));

#if LUAJIT_SECURITY_STRHASH
  /* Check which chains need secondary hashes. */
  if (g->str.second) {
    int newsecond = 0;
    /* Compute primary chain lengths. */
    for (i = g->str.mask; i != ~(MSize)0; i--) {
      GCobj *o = (GCobj *)(gcrefu(oldtab[i]) & ~(uintptr_t)1);
      while (o) {
	GCstr *s = gco2str(o);
	MSize hash = s->hashalg ? hash_sparse(g->str.seed, strdata(s), s->len) :
				  s->hash;
	hash &= newmask;
	setgcrefp(newtab[hash], gcrefu(newtab[hash]) + 1);
	o = gcnext(o);
      }
    }
    /* Mark secondary chains. */
    for (i = newmask; i != ~(MSize)0; i--) {
      int secondary = gcrefu(newtab[i]) > LJ_STR_MAXCOLL;
      newsecond |= secondary;
      setgcrefp(newtab[i], secondary);
    }
    g->str.second = newsecond;
  }
#endif

  /* Reinsert all strings from the old table into the new table. */
  for (i = g->str.mask; i != ~(MSize)0; i--) {
    GCobj *o = (GCobj *)(gcrefu(oldtab[i]) & ~(uintptr_t)1);
    while (o) {
      GCobj *next = gcnext(o);
      GCstr *s = gco2str(o);
      MSize hash = s->hash;
#if LUAJIT_SECURITY_STRHASH
      uintptr_t u;
      if (LJ_LIKELY(!s->hashalg)) {  /* String hashed with primary hash. */
	hash &= newmask;
	u = gcrefu(newtab[hash]);
	if (LJ_UNLIKELY(u & 1)) {  /* Switch string to secondary hash. */
	  s->hash = hash = hash_dense(g->str.seed, s->hash, strdata(s), s->len);
	  s->hashalg = 1;
	  hash &= newmask;
	  u = gcrefu(newtab[hash]);
	}
      } else {  /* String hashed with secondary hash. */
	MSize shash = hash_sparse(g->str.seed, strdata(s), s->len);
	u = gcrefu(newtab[shash & newmask]);
	if (u & 1) {
	  hash &= newmask;
	  u = gcrefu(newtab[hash]);
	} else {  /* Revert string back to primary hash. */
	  s->hash = shash;
	  s->hashalg = 0;
	  hash = (shash & newmask);
	}
      }
      /* NOBARRIER: The string table is a GC root. */
      setgcrefp(o->gch.nextgc, (u & ~(uintptr_t)1));
      setgcrefp(newtab[hash], ((uintptr_t)o | (u & 1)));
#else
      hash &= newmask;
      /* NOBARRIER: The string table is a GC root. */
      setgcrefr(o->gch.nextgc, newtab[hash]);
      setgcref(newtab[hash], o);
#endif
      o = next;
    }
  }

  /* Free old table and replace with new table. */
  lj_str_freetab(g);
  g->str.tab = newtab;
  g->str.mask = newmask;
}

#if LUAJIT_SECURITY_STRHASH
#if defined(LUA_USE_ASSERT)
/* Classic chain rehash sweep hit counter (assert builds only). */
static uint32_t lj_str_rehash_sweep_hits_counter;
#endif

/* Rehash and rechain all strings in a chain. */
static LJ_NOINLINE GCstr *lj_str_rehash_chain(lua_State *L, StrHash hashc,
					      const char *str, MSize len)
{
  global_State *g = G(L);
  int ow = g->gc.state == GCSsweepstring ? otherwhite(g) : 0;  /* Sweeping? */
  GCRef *strtab = g->str.tab;
  MSize strmask = g->str.mask;
  GCobj *o = gcref(strtab[hashc & strmask]);
  setgcrefp(strtab[hashc & strmask], (void *)((uintptr_t)1));
  g->str.second = 1;
  while (o) {
    uintptr_t u;
    GCobj *next = gcnext(o);
    GCstr *s = gco2str(o);
    StrHash hash;
    if (ow) {  /* Must sweep while rechaining. */
      if (((o->gch.marked ^ LJ_GC_WHITES) & ow)) {  /* String alive? */
	lj_assertG(!isdead(g, o) || (o->gch.marked & LJ_GC_FIXED),
		   "sweep of undead string");
	makewhite(g, o);
      } else {  /* Free dead string. */
	lj_assertG(isdead(g, o) || ow == LJ_GC_SFIXED,
		   "sweep of unlive string");
	lj_str_free(g, s);
	o = next;
	continue;
      }
    }
    hash = s->hash;
    if (!s->hashalg) {  /* Rehash with secondary hash. */
      hash = hash_dense(g->str.seed, hash, strdata(s), s->len);
      s->hash = hash;
      s->hashalg = 1;
    }
    /* Rechain. */
    hash &= strmask;
    u = gcrefu(strtab[hash]);
    setgcrefp(o->gch.nextgc, (u & ~(uintptr_t)1));
    setgcrefp(strtab[hash], ((uintptr_t)o | (u & 1)));
    o = next;
  }
  /* Try to insert the pending string again. */
  return lj_str_new(L, str, len);
}

#ifdef LUA_USE_ASSERT
/* Default-visibility (declared in lj_str.h) so ffi.C/dlsym can resolve it. */
uint32_t lj_str_rehash_sweep_hits(void)
{
  return lj_str_rehash_sweep_hits_counter;
}
#endif
#endif

/* Reseed String ID from PRNG after random interval < 2^bits. */
#if LUAJIT_SECURITY_STRID == 1
#define STRID_RESEED_INTERVAL	8
#elif LUAJIT_SECURITY_STRID == 2
#define STRID_RESEED_INTERVAL	4
#elif LUAJIT_SECURITY_STRID >= 3
#define STRID_RESEED_INTERVAL	0
#endif

/* Allocate a new string and add to string interning table. */
static GCstr *lj_str_alloc(lua_State *L, const char *str, MSize len,
			   StrHash hash, int hashalg)
{
  GCstr *s = (GCstr *)lj_mem_newagco(L, lj_str_size(len), 0);
  global_State *g = G(L);
  uintptr_t u;
  newwhite(g, s);
  s->gct = ~LJ_TSTR;
  s->len = len;
  s->hash = hash;
#ifndef STRID_RESEED_INTERVAL
  s->sid = g->str.id++;
#elif STRID_RESEED_INTERVAL
  if (!g->str.idreseed--) {
    uint64_t r = lj_prng_u64(&g->prng);
    g->str.id = (StrID)r;
    g->str.idreseed = (uint8_t)(r >> (64 - STRID_RESEED_INTERVAL));
  }
  s->sid = g->str.id++;
#else
  s->sid = (StrID)lj_prng_u64(&g->prng);
#endif
  s->reserved = 0;
  s->hashalg = (uint8_t)hashalg;
  /* Clear last 4 bytes of allocated memory. Implies zero-termination, too. */
  *(uint32_t *)(strdatawr(s)+(len & ~(MSize)3)) = 0;
  memcpy(strdatawr(s), str, len);
  /* Add to string hash table. */
  hash &= g->str.mask;
  u = gcrefu(g->str.tab[hash]);
  setgcrefp(s->nextgc, (u & ~(uintptr_t)1));
  /* NOBARRIER: The string table is a GC root. */
  setgcrefp(g->str.tab[hash], ((uintptr_t)s | (u & 1)));
  if (g->str.num++ > g->str.mask)  /* Allow a 100% load factor. */
    lj_str_resize(L, (g->str.mask<<1)+1);  /* Grow string table. */
  return s;  /* Return newly interned string. */
}

/* Intern a string and return string object. */
GCstr *lj_str_new(lua_State *L, const char *str, size_t lenx)
{
  global_State *g = G(L);
  if (lenx-1 < LJ_MAX_STR-1) {
    MSize len = (MSize)lenx;
    StrHash hash = hash_sparse(g->str.seed, str, len);
    MSize coll = 0;
    int hashalg = 0;
    /* Check if the string has already been interned. */
    GCobj *o = gcref(g->str.tab[hash & g->str.mask]);
#if LUAJIT_SECURITY_STRHASH
    if (LJ_UNLIKELY((uintptr_t)o & 1)) {  /* Secondary hash for this chain? */
      hashalg = 1;
      hash = hash_dense(g->str.seed, hash, str, len);
      o = (GCobj *)(gcrefu(g->str.tab[hash & g->str.mask]) & ~(uintptr_t)1);
    }
#endif
    while (o != NULL) {
      GCstr *sx = gco2str(o);
      if (sx->hash == hash && sx->len == len) {
	if (memcmp(str, strdata(sx), len) == 0) {
	  if (gc_obj_isdead(g, o))
	    gc_obj_resurrect(g, o);  /* Resurrect if dead. */
	  return sx;  /* Return existing string. */
	}
	coll++;
      }
      coll++;
      o = gcnext(o);
    }
#if LUAJIT_SECURITY_STRHASH
    /* Rehash chain if there are too many collisions. */
    if (LJ_UNLIKELY(coll > LJ_STR_MAXCOLL) && !hashalg) {
      return lj_str_rehash_chain(L, hash, str, len);
    }
#endif
    /* Otherwise allocate a new string. */
    return lj_str_alloc(L, str, len, hash, hashalg);
  } else {
    if (lenx)
      lj_err_msg(L, LJ_ERR_STROV);
    return &g->strempty;
  }
}

#else
/* ===== Open-addressing intern table (arena GC / LJ_HASGCMARK) ===== */
/*
** Robin Hood open-addressing hash set of GCstr* (P4). Replaces the P1 plain
** linear-probe + tombstone scheme with Robin Hood displacement + probe-cap
** reseed (§3.6, I7) and backward-shift deletion (no tombstones).
**
** Slot encoding:
**   0              = EMPTY
**   (uintptr_t)1   = TOMBSTONE sentinel (reserved; NEVER written under P4 --
**                    backward-shift deletion fills gaps instead of tombstoning,
**                    so the sentinel checks below are dead code kept defensive)
**   otherwise      = GCstr* pointer
**
** Robin Hood invariant: on insert, if the candidate's probe distance exceeds
** the incumbent's, they swap and the evicted incumbent continues displacing.
** This bounds probe-length variance. Probe distance is recomputed from the
** stored GCstr->hash on the fly:  dist = (i - (s->hash & mask)) & mask
** No per-slot metadata, no new GCstr field (§3.1, G3).
**
** Backward-shift deletion (P4 decision): removing a slot shifts the following
** cluster entries back by one until an EMPTY or in-ideal-slot entry is hit.
** This eliminates tombstones entirely -> no tombstone clustering (kills the P2
** B7 +17% regression) and makes the Robin Hood early-exit unconditionally
** valid (no tombstone-through rule, simplifying §3.3 must-guard-3).
**
** Incremental-sweep safety (the flagged case): a backward shift preserves the
** Robin Hood invariant (contiguous clusters, every entry reachable by linear
** probe from its ideal slot). A mutator insert/lookup between sweep steps sees
** a valid table. lj_strtab_remove re-probes from s->hash against the CURRENT
** table, so entries relocated by a prior backward shift (or a resize) are still
** found (blocker-2). A mutator Robin Hood insert between sweep steps may
** displace entries; that too preserves the invariant, so the next sweep remove
** re-probes and locates its target wherever it landed. No torn-slot window.
**
** Probe-cap reseed (I7, anti-DoS): if an incremental insert's probe distance
** exceeds LJ_STR_OA_PROBE_CAP, the table is rehashed with a fresh PRNG seed.
** The new seed scrambles bucket assignments so an attacker who pinned a long
** cluster against the old seed cannot keep it pinned. This is the open-addr
** replacement for the chain path's hash_dense/LJ_STR_MAXCOLL defense (§3.6).
** The reseed-rehash recomputes every string's stored hash with the new seed
** and rebuilds with Robin Hood insertion (no cap check during rebuild -- a
** full rebuild at the current load factor yields normal ~O(log n) probe
** distances for non-adversarial inputs; an adaptive adversary is bounded by
** the reseed rate since each reseed forces re-computation against a fresh
** secret seed). The chain path already mutates s->hash during hash_dense
** rehash, so s->hash is not assumed immutable elsewhere.
*/

/* Probe-distance cap before a reseed+rehash is forced (I7). Chosen well above
** the ~O(log n) probe distance Robin Hood exhibits at 75% load, so it only
** fires under adversarial collision pinning. */
#define LJ_STR_OA_PROBE_CAP	128

/* Debug/test hooks (default visibility so test/lua resolves them via ffi.C).
** oa_probecap_override: 0 = use LJ_STR_OA_PROBE_CAP; >0 = lower cap for tests.
** oa_reseed_counter: incremented each time a reseed+rehash fires. */
static MSize oa_probecap_override;
static uint32_t oa_reseed_counter;

#if defined(_WIN32)
__declspec(dllexport) uint32_t lj_str_oa_reseed_count(void);
__declspec(dllexport) void lj_str_oa_set_probecap(uint32_t cap);
#elif defined(__ELF__) || defined(__MACH__)
extern __attribute__((visibility("default"))) uint32_t lj_str_oa_reseed_count(void);
extern __attribute__((visibility("default"))) void lj_str_oa_set_probecap(uint32_t cap);
#else
extern uint32_t lj_str_oa_reseed_count(void);
extern void lj_str_oa_set_probecap(uint32_t cap);
#endif
uint32_t lj_str_oa_reseed_count(void) { return oa_reseed_counter; }
void lj_str_oa_set_probecap(uint32_t cap) { oa_probecap_override = cap; }

static LJ_AINLINE uintptr_t strtab_oa_slot(global_State *g, MSize i)
{
  return gcrefu(g->str.tab[i]);
}

static LJ_AINLINE void strtab_oa_set(global_State *g, MSize i, uintptr_t v)
{
  setgcrefp(g->str.tab[i], (void *)v);
}

static LJ_AINLINE MSize strtab_oa_probecap(void)
{
  return oa_probecap_override ? oa_probecap_override : LJ_STR_OA_PROBE_CAP;
}

/* Robin Hood insert into an explicit table array (used by lj_str_resize and
** lj_str_reseed_rehash rebuilds, and by lj_str_alloc into g->str.tab).
** Displaces incumbents whose probe distance is less than the candidate's.
** Returns the max probe distance the candidate reached (caller checks cap).
** No tombstone handling: backward-shift keeps the table tombstone-free, so
** every non-zero slot is a GCstr*. */
static MSize strtab_oa_rh_insert(GCRef *tab, MSize mask, GCstr *s, StrHash hash)
{
  MSize i = hash & mask;
  MSize dist = 0;
  MSize maxdist = 0;
  GCstr *cur = s;
  for (;;) {
    uintptr_t v = gcrefu(tab[i]);
    if (v == 0) {  /* EMPTY */
      setgcrefp(tab[i], (void *)cur);
      return maxdist;
    }
    GCstr *occ = (GCstr *)(void *)v;
    MSize occdist = (i - (occ->hash & mask)) & mask;
    if (dist > occdist) {
      setgcrefp(tab[i], (void *)cur);  /* evict occ, place cur */
      cur = occ;
      dist = occdist;  /* evicted occ resumes from occ's old distance */
    }
    i = (i + 1) & mask;
    dist++;
    if (dist > maxdist) maxdist = dist;
  }
}

/* Lookup with Robin Hood early-exit. No tombstones (backward-shift), so the
** early-exit is unconditionally valid: if the incumbent's probe distance is
** less than ours, the key cannot be present further along the probe run. */
static GCstr *strtab_oa_lookup(global_State *g, const char *str, MSize len,
			       StrHash hash)
{
  MSize mask = g->str.mask;
  MSize i = hash & mask;
  MSize dist = 0;
  for (;;) {
    uintptr_t v = strtab_oa_slot(g, i);
    if (v == 0) return NULL;  /* EMPTY -> miss */
    GCstr *s = (GCstr *)(void *)v;
    if (s->hash == hash && s->len == len && memcmp(str, strdata(s), len) == 0)
      return s;  /* hit */
    MSize occdist = (i - (s->hash & mask)) & mask;
    if (dist > occdist) return NULL;  /* Robin Hood early-exit -> miss */
    i = (i + 1) & mask;
    dist++;
  }
}

/* Rehash: new slot array, re-insert every occupied non-empty slot whose
** object is still allocated REGARDLESS of mark bit (blocker-2: unmarked-but-
** not-yet-reclaimed strings are still valid interned objects). Robin Hood
** insertion preserves the invariant the lookup/remove early-exit relies on.
** Tombstones do not exist under backward-shift; tombs stays 0. */
void lj_str_resize(lua_State *L, MSize newmask)
{
  global_State *g = G(L);
  GCRef *oldtab = g->str.tab;
  MSize oldmask = g->str.mask;
  MSize i;
  if (g->gc.state == GCSsweepstring || newmask >= LJ_MAX_STRTAB-1)
    return;
  GCRef *newtab = lj_mem_newvec(L, newmask+1, GCRef);
  memset(newtab, 0, (newmask+1)*sizeof(GCRef));
  g->str.tab = newtab;
  g->str.mask = newmask;
  g->str.tombs = 0;
  for (i = oldmask; i != ~(MSize)0; i--) {
    uintptr_t v = gcrefu(oldtab[i]);
    GCstr *s;
    if (v == 0 || v == STRTAB_OA_TOMB) continue;  /* TOMB: dead (no tombs) */
    s = (GCstr *)(void *)v;
    strtab_oa_rh_insert(newtab, newmask, s, s->hash);
  }
  lj_mem_freevec(g, oldtab, oldmask+1, GCRef);
}

/* Probe-cap reseed + rehash (I7, anti-DoS). Triggered when an incremental
** insert's probe distance exceeds the cap. Reseeds g->str.seed via the PRNG,
** recomputes every string's stored hash with the new seed, and rebuilds the
** table (same size) with Robin Hood insertion. The new seed scrambles bucket
** assignments so an attacker cannot pin a long cluster. Re-computing s->hash
** is safe: strdata(s) is valid for every still-allocated string (blocker-2).
** After reseed, lj_str_new's hash_sparse(current seed) matches the recomputed
** s->hash. The rebuild has no cap check (full rebuild -> normal distances). */
static void lj_str_reseed_rehash(lua_State *L)
{
  global_State *g = G(L);
  GCRef *oldtab = g->str.tab;
  MSize oldmask = g->str.mask;
  MSize i;
  g->str.seed = lj_prng_u64(&g->prng);
  GCRef *newtab = lj_mem_newvec(L, oldmask+1, GCRef);
  memset(newtab, 0, (oldmask+1)*sizeof(GCRef));
  g->str.tab = newtab;
  for (i = oldmask; i != ~(MSize)0; i--) {
    uintptr_t v = gcrefu(oldtab[i]);
    GCstr *s;
    if (v == 0 || v == STRTAB_OA_TOMB) continue;
    s = (GCstr *)(void *)v;
    s->hash = hash_sparse(g->str.seed, strdata(s), s->len);
    strtab_oa_rh_insert(newtab, oldmask, s, s->hash);
  }
  lj_mem_freevec(g, oldtab, oldmask+1, GCRef);
  oa_reseed_counter++;
}

/* Remove s from the open-addressing intern table: re-probe from s->hash to
** find the slot whose value == s, then backward-shift the subsequent cluster
** entries back by one slot to fill the gap (no tombstones). Returns the probe
** + shift cost for sweep-budget billing.
**
** MUST be called STRICTLY before lj_str_free: it reads s->hash, and after
** lj_str_free the cell head may be overwritten by freelist metadata (small
** cells, lj_gc.h:485) or the huge mapping unmapped (lj_arena.c:1097).
**
** Re-probe walks the CURRENT table, so entries relocated by a prior resize or
** backward shift are still found (blocker-2). Backward-shift preserves the
** Robin Hood invariant, so subsequent lookups and removes remain correct. */
MSize lj_strtab_remove(global_State *g, GCstr *s)
{
  MSize mask = g->str.mask;
  MSize i = s->hash & mask;
  MSize probes = 0;
  MSize found = ~(MSize)0;
  for (;;) {
    uintptr_t v = strtab_oa_slot(g, i);
    if (v == 0) break;  /* EMPTY: s not present */
    if (v != STRTAB_OA_TOMB) {
      GCstr *t = (GCstr *)(void *)v;
      if (t == s) { found = i; break; }
      MSize occdist = (i - (t->hash & mask)) & mask;
      if (probes > occdist) break;  /* Robin Hood early-exit: s not in table */
    }
    i = (i + 1) & mask;
    probes++;
  }
  lj_assertG(found != ~(MSize)0,
	     "lj_strtab_remove: string not in intern table (ptr=%p hash=%u)",
	     (void *)s, (unsigned)s->hash);
  if (found == ~(MSize)0) return probes;  /* defensive: not found */
  /* Backward-shift deletion: fill the gap at `found`. */
  strtab_oa_set(g, found, 0);
  i = (found + 1) & mask;
  for (;;) {
    uintptr_t v = strtab_oa_slot(g, i);
    GCstr *occ;
    MSize occdist;
    if (v == 0) break;  /* EMPTY: cluster ends */
    occ = (GCstr *)(void *)v;
    occdist = (i - (occ->hash & mask)) & mask;
    if (occdist == 0) break;  /* occ in its ideal slot: stop */
    strtab_oa_set(g, found, (uintptr_t)occ);
    strtab_oa_set(g, i, 0);
    found = i;
    i = (i + 1) & mask;
    probes++;
  }
  return probes;
}

static GCstr *lj_str_alloc(lua_State *L, const char *str, MSize len,
			   StrHash hash)
{
  GCstr *s = (GCstr *)lj_mem_newagco(L, lj_str_size(len), 0);
  global_State *g = G(L);
  MSize maxdist;
  newwhite(g, s);
  s->gct = ~LJ_TSTR;
  s->len = len;
  s->hash = hash;
#ifndef STRID_RESEED_INTERVAL
  s->sid = g->str.id++;
#elif STRID_RESEED_INTERVAL
  if (!g->str.idreseed--) {
    uint64_t r = lj_prng_u64(&g->prng);
    g->str.id = (StrID)r;
    g->str.idreseed = (uint8_t)(r >> (64 - STRID_RESEED_INTERVAL));
  }
  s->sid = g->str.id++;
#else
  s->sid = (StrID)lj_prng_u64(&g->prng);
#endif
  s->reserved = 0;
  s->hashalg = 0;
  *(uint32_t *)(strdatawr(s)+(len & ~(MSize)3)) = 0;
  memcpy(strdatawr(s), str, len);
  maxdist = strtab_oa_rh_insert(g->str.tab, g->str.mask, s, hash);
  g->str.num++;
  if (maxdist > strtab_oa_probecap()) {
    /* Adversarial collision pin (I7): reseed seed + rehash. */
    lj_str_reseed_rehash(L);
  }
  if (g->str.num * 4 > (g->str.mask + 1) * 3)
    lj_str_resize(L, (g->str.mask << 1) + 1);
  return s;
}

GCstr *lj_str_new(lua_State *L, const char *str, size_t lenx)
{
  global_State *g = G(L);
  if (lenx-1 < LJ_MAX_STR-1) {
    MSize len = (MSize)lenx;
    StrHash hash = hash_sparse(g->str.seed, str, len);
    GCstr *s = strtab_oa_lookup(g, str, len, hash);
    if (s) {
      if (gc_obj_isdead(g, obj2gco(s)))
	gc_obj_resurrect(g, obj2gco(s));
      return s;
    }
    return lj_str_alloc(L, str, len, hash);
  } else {
    if (lenx)
      lj_err_msg(L, LJ_ERR_STROV);
    return &g->strempty;
  }
}

#endif /* LJ_HASGCMARK open-addressing strtab */

void LJ_FASTCALL lj_str_free(global_State *g, GCstr *s)
{
  g->str.num--;
  lj_mem_freegco(g, s, lj_str_size(s->len));
}

void LJ_FASTCALL lj_str_init(lua_State *L)
{
  global_State *g = G(L);
  g->str.seed = lj_prng_u64(&g->prng);
  lj_str_resize(L, LJ_MIN_STRTAB-1);
}


# Memory Profiler — Design Doc (grounded to the arena-GC fork)

Status: DESIGN ONLY (no code). Scope of this doc: map the cross-VM requirements
report (Go/pprof, V8/DevTools, Tarantool memprof) onto **this fork's actual hook
points, GC phases, and arena structures**. MVP is **Lua heap only** (JIT-trace
attribution kept but coarse; FFI-backing / external malloc explicitly out).

Worktree: `/home/fesily/luajit/.claude/worktrees/arenagc`  HEAD `d563d1d2`
Reference impl studied: Tarantool `tarantool/luajit` @ `712e6d85` (memprof/symtab/wbuf).

> **REV 2 (HEAD d563d1d2):** revised for the new **open-addressing string intern
> table** (`LUAJIT_STRTAB_OPENADDR`, commits `d6a73a09`/`740e1be1`/`ec243f9b`/
> `d563d1d2`). Strings now live in arena `block`/`mark` bitmaps and are swept in
> the bitmap pass — this SIMPLIFIES the profiler (uniform bitmap coverage,
> precise string FREE). See §1.1a, §1.2, §1.8. Original REV 1 was written against
> `488206f3` (chained strtab).

---

## 0. TL;DR — the one fact that reshapes the whole design

The requirements report assumes the **Tarantool model: wrap `lua_setallocf`**
(`memprof_allocf`) and use a `global_State.mem_L` back-reference for attribution.

**That model is BLIND in this fork.** In stock LuaJIT and Tarantool,
`lj_mem_newgco()` calls `g->allocf(...)` directly, so an allocf wrapper sees
every GC object. In this arena fork, GC-object allocation is **bump-allocated
inside arenas** and never touches `g->allocf`:

- `lj_mem_newgco` → `lj_mem_newgco_arena` (inline) → `arena_alloc()` bump pointer
  — `src/lj_gc.h:430`, `src/lj_arena.h:302`
- overflow → `lj_mem_newgco_slow` → `lj_arena_findspace` / `lj_hugeblock_alloc`
  — `src/lj_gc_arena.c:2785`

`g->allocf` in this fork is only reached by: freelist/registry metadata,
huge-block registry, arena chunk reservation, and non-GC `lj_mem_realloc`
buffers. **The dominant object stream (string/table/func/proto/cdata/udata) is
invisible to an allocf wrapper.**

Two consequences drive every decision below:

1. **Event hook must move** from `allocf` to the `lj_mem_newgco*` / `lj_mem_freegco_`
   / sweep chokepoints. Upside: those take `lua_State *L` directly, so we do NOT
   need Tarantool's `mem_L` global — attribution context is already in scope.
2. **Snapshot mode is unusually cheap here.** The arena already maintains
   per-cell `block[]`/`mark[]` bitmaps and an arena registry. A V8-style heap
   walk iterates cells via `arena_cellstate()` instead of chasing the
   `gc.root` linked list. The arena inverts the usual difficulty:
   **event mode is slightly harder, snapshot mode is much easier.**

---

## 1. Fork surface inventory (verified, with file:line)

### 1.1 Allocation chokepoints (event ALLOC/REALLOC source)
| Path | Location | Notes |
|---|---|---|
| `lj_mem_newgco(L, size)` | `src/lj_gc_arena.c:2775` | generic (NonTrav class), has `L` |
| `lj_mem_newgco_arena(L,size,cls,link)` inline fast path | `src/lj_gc.h:430` | **hot inline**; already has a `GCF_MARKALLOC` flag check we can mirror |
| `lj_mem_newgco_slow(L,size,cls,link)` | `src/lj_gc_arena.c:2785` | out-of-line refill + huge |
| `lj_mem_newgcot(L,s)` macro (Trav) | `src/lj_gc.h:506` | class-specialized newgco entry |
| `lj_mem_realloc(L,p,osz,nsz)` | `src/lj_gc_arena.c:2760` | vector/buffer resize → REALLOC |
| `lj_mem_grow(...)` | `src/lj_gc_arena.c:2822` | wraps `lj_mem_realloc` |

`g->gc.total` accounting is already updated at every one of these — the profiler
piggybacks on the exact same points.

### 1.1a Strings: open-addressing intern table (REV 2 — new arch)
Strings are NOT allocated by `lj_mem_newgco`. They use a distinct entry point:
- `lj_str_alloc` → **`lj_mem_newagco(L, lj_str_size(len), 0)`** — `src/lj_str.c:663`,
  macro at `src/lj_gc.h:510`. The `a` = "unlinked": `link=0`, class
  `ArenaClass_NonTrav` (`src/lj_arena.h:119`). **The string is NOT on the
  `gc.root`/`nextgc` chain** (asserted by commit `740e1be1`).
- Intern table state `g->str` = `StrInternState` (`src/lj_obj.h:753`):
  `tab` (`GCRef*` flat slot array), `mask`, `num`, `id`, `tombs`, `seed`.
  Robin Hood insert `strtab_oa_rh_insert` + backward-shift remove
  `lj_strtab_remove` (`src/lj_str.c:501/618`); no chains, no tombstones in the
  live path.
- **Profiler consequence:** the ALLOC hook set must include `lj_mem_newagco`
  (one extra site). For attribution, `lj_str_new`/`lj_str_alloc` carry `L`, same
  as the other chokepoints. `g->str.num` is a free live-string counter; a
  string-dedup / intern-pressure view can read `g->str.tab` directly (report §D
  "interned string / duplicated-looking payload").

`g->gc.total` accounting is already updated at every one of these — the profiler
piggybacks on the exact same points.

### 1.2 Free chokepoints (event FREE source)
| Path | Location | Notes |
|---|---|---|
| `gc_bitmap_sweep` → `gc_freefunc[gct-~LJ_TSTR](g,o)` | `src/lj_gc_arena.c:1170` | bulk incremental sweep (Trav arenas) |
| `gc_bitmap_sweep` NonTrav-string branch (REV 2) | `src/lj_gc_arena.c:~1170` | word-parallel `dead=block&~mark`; `lj_strtab_remove` **strictly before** `lj_str_free`; **precise per-string FREE**, billed against `GCSWEEPMAX` |
| `lj_arena_podsweep` (POD word-parallel) | called `src/lj_gc_arena.c:1126` | frees closures/protos in bulk; **no per-object callback** — accounting is cell-delta only |
| `rebuild_prologue_cdatav` → `gc_freefunc[...]` | `src/lj_gc_arena.c:1264` | dead VLA cdata |
| `lj_mem_freegco_(g,p,osize)` | `src/lj_gc.h:464` | per-object free (explicit, non-sweep) |
| `lj_hugeblock_free(g,p,size)` | `src/lj_arena.c:1091` | huge objects |
| `gc_sweepstr_oa(g,start,count)` | `src/lj_gc_arena.c:~1094` | **shutdown-only** string freeall (not incremental) |

**REV 2 — strings are now precise, POD is still the only gap.** Under
`LUAJIT_STRTAB_OPENADDR`, incremental string reclaim is folded into
`gc_bitmap_sweep`'s NonTrav branch, which visits each dead string cell and calls
`lj_strtab_remove`+`lj_str_free` — a clean per-object FREE hook point, same
quality as the Trav-arena `gc_freefunc` dispatch. The chained `gc_sweepstr` is
`#if`'d out under the flag. **The only remaining aggregate-FREE gap is POD**
(`lj_arena_podsweep`, closures/protos).

**Design note (FREE granularity):** the POD word-parallel sweep
(`lj_arena_podsweep`) reclaims many protos/closures with **no per-object hook** —
it only knows the freed *cell count*. Precise per-object FREE events for POD
objects would require either (a) a bitmap diff pass, or (b) instrumenting the
inner loop of `lj_arena_podsweep`. For MVP we accept **aggregate FREE for POD
arenas** (bytes + object count, no per-site FREE) and precise per-object FREE for
the Trav-arena bitmap sweep AND the NonTrav string sweep (REV 2), where the
per-cell loop already visits each object. This is a fork-specific limitation the
report does not anticipate.

### 1.3 Object typing & sizing from a raw cell
- Type tag: `o->gch.gct`, dispatched as `gc_freefunc[o->gch.gct - ~LJ_TSTR]`.
  Tags (`src/lj_obj.h:264-274`): `LJ_TSTR ~4`, `LJ_TUPVAL ~5`, `LJ_TTHREAD ~6`,
  `LJ_TPROTO ~7`, `LJ_TFUNC ~8`, `LJ_TTRACE ~9`, `LJ_TCDATA ~10`, `LJ_TTAB ~11`,
  `LJ_TUDATA ~12`.
- Type name arrays already exist: `lj_obj_typename[]`, `lj_obj_itypename[]`
  (`src/lj_obj.c:12-20`) — reuse for report labels, no new table.
- Size from a cell: `arena_roundcells(size)` and the bitmap; a snapshot walk
  derives object extent from the run of `CellState_Extent` cells following an
  allocated head (`arena_cellstate`, `src/lj_arena.h:211`).

### 1.4 Arena structures for snapshot mode
- Registry: `g->gc.arenas[0 .. g->gc.arenastop)` (`GCArena **`).
- Per-arena bitmaps: `a->block[]` (allocated) + `a->mark[]` (reachable),
  cell states Extent/Free/White/Black (`src/lj_arena.h:72-77`).
- `arena_cellstate(a,c)`, `arena_obj_ismarked(a,c)` give liveness without
  touching object payloads — ideal for a low-disturbance walk.
- Arena classes carry semantic hints already: `ArenaFlag_TravObjs`,
  `ArenaFlag_PODOnly`, `ArenaFlag_UdataOnly`, `ArenaFlag_CdataVOnly`
  (`src/lj_arena.c:512-520`) → coarse type bucketing for free.
  **REV 2:** `ArenaClass_NonTrav` arenas (flags == 0) are de-facto **strings-only**
  under `LUAJIT_STRTAB_OPENADDR` — so the snapshot walk covers interned strings
  through the same bitmap scan, no special-case path (contrast REV 1, where the
  chained strtab forced a separate string walk).
- Huge objects: `g->gc.hugeset` registry + `g->gc.hugemem`/`hugenum`.

### 1.5 GC phase / state context (for the report's "GC cycle" attribution)
- States (`src/lj_gc.h:16`): `GCSpause=0, GCSpropagate=1, GCSatomic=2,
  GCSsweepstring=3, GCSsweep=4, GCSfinalize=5`. Read `g->gc.state`.
- Mark-window flags: `GCF_MARKALLOC`, `GCF_BITMAPSWEEP` (`g->gc.gcmarkflags`) —
  let the profiler tag "allocated during sweep" for survival analysis.

### 1.6 Attribution machinery (reuse, do not reinvent)
- `lj_debug_dumpstack(L, sb, fmt, depth)` — `src/lj_debug.c:587` — compact stack
  formatter (`f`/`F`/`l`/`p`/`Z` verbs), already the backbone of `jit.p`.
- `luaJIT_profile_dumpstack(L, fmt, depth, &len)` — `src/lj_profile.c:359` —
  public wrapper we can mirror for the memprof stack sideband.
- `debug_putchunkname`, `lj_debug_frameline`, `proto_chunkname` for file:line.
- `vmstate` (`src/lj_obj.h:713`): `~LJ_VMST_*` when in VM code, else a **trace
  number** during trace execution — same trace-attribution signal Tarantool
  reads. **No `mem_L` field exists here and none is needed** (hook has `L`).

### 1.7 Existing profiler infra to align with (not fight)
- `src/lj_profile.c` — sampling CPU profiler: OS timer thread, `HOOK_PROFILE`
  bit, per-VM `ps->g` single-owner guard, `luaJIT_profile_start/stop`.
- `src/jit/p.lua`, `jit.profile` — Lua-side consumer patterns.
- **Naming/ownership convention to copy:** single-VM ownership guard, `-j`
  loadable module, `LUAJIT_*FILE` env output override.

---

## 2. MVP architecture (Lua heap only)

Two independently shippable capabilities. Recommend snapshot first (read-only,
zero hot-path cost), then event mode.

### 2.1 Capability A — Heap snapshot (arena bitmap walk)  [SHIP FIRST]
Read-only walker in a new `src/lj_memprof.c` (isolated; no hot-path edits).

- Enumerate `g->gc.arenas[0..arenastop)`; for each, scan `block[]`, and for each
  allocated head cell read `o->gch.gct` → type bucket, derive size from cell run.
  **REV 2:** this uniformly covers interned strings too (they now live in
  `ArenaClass_NonTrav` arena bitmaps); no separate strtab walk needed for the
  live-set histogram. For intern-specific analysis (dedup pressure, table load
  factor) additionally read `g->str.num` / `g->str.mask` / `g->str.tab`.
- Bucket by: object type, arena class, live vs dead (`arena_obj_ismarked`),
  size class. Emit `inuse_objects`, `inuse_space`, per-type histograms.
- Huge set: walk `g->gc.hugeset`.
- **Snapshot diff / leak suspects:** two walks + stable object id (cell address
  is stable within a session between snapshots as long as the object lives).
  Growth-between-snapshots = leak suspect set. This satisfies report §E items
  object-id / shallow-size / snapshot-diff / leak-suspects **cheaply**.
- `gc=full|step|none` option before a snapshot → call `lj_gc_fullgc` to avoid
  the incremental-GC "false leak" caveat (report §G, Tarantool caveat).
- **Deferred to P2:** retained size, dominator tree, retaining path (these need a
  full object-graph traversal — reuse `gc_traverse_*`; heavier, separate slice).

Cost: O(total cells) bitmap scan, no payload reads, no allocation-path change.
This is the fork's comparative advantage over stock LuaJIT.

### 2.2 Capability B — Allocation event stream (chokepoint instrumentation)
Gated by a new `GCF_MEMPROF`-style flag in `g->gc.gcmarkflags` (or a dedicated
`g->memprof.active` byte) checked in the hot inline. Mirror the existing
`GCF_MARKALLOC` branch in `lj_mem_newgco_arena` (`src/lj_gc.h:452`) — proven
idiomatic, one predictable-not-taken branch when profiling is off.

- ALLOC at `lj_mem_newgco*`: record {addr, size, type=gct, vmstate, L-frame}.
- REALLOC at `lj_mem_realloc`.
- FREE: precise at `gc_bitmap_sweep`/`rebuild_prologue_cdatav`/`lj_mem_freegco_`;
  **aggregate** for `lj_arena_podsweep` (see §1.2 limitation).
- Attribution resolved lazily via `lj_debug_dumpstack(L,...)` at event time
  (depth-limited, default 1 like Tarantool's frame attribution), or a
  proto/trace-id sideband resolved against a symtab dump (§3.2).
- Metrics derived offline from the stream: `alloc_space`, `alloc_objects`,
  `freed_*`, `realloc_*`, `peak_live`, `allocation_rate`, `survival_rate`
  (survival = allocated-then-still-marked across a GC cycle; `GCF_*` flags give
  the cycle boundary).

### 2.3 Object classification for MVP (report §D, trimmed to Lua heap)
Map `gct` → report categories directly; no FFI-backing / external malloc:
`GCstr / GCtab / GCfunc(+proto,upval) / GCthread / GCudata / GCcdata / GCtrace`.
Arena-class flag gives an extra axis (POD vs Trav vs Udata vs CdataV).

---

## 3. Wire format (adapt Tarantool, keep pprof export offline)

### 3.1 Event stream (borrow Tarantool's compact ULEB128 design)
- Prologue `ljm` + version + reserved; epilogue `0x80`.
- Event header bitfield {event opcode ALLOC/FREE/REALLOC, source kind
  INT/LFUNC/CFUNC/TRACE}; ULEB128 addr/size payloads (ref: Tarantool
  `lj_memprof.h`, `lj_wbuf.c`).
- **Fork delta:** add object-`gct` to the ALLOC record (Tarantool infers type
  differently); add an `arena-class` nibble; add a `gc.state`/flags byte for
  survival analysis.
- Reuse `lj_wbuf`-style buffered writer, or the existing `SBuf` (`lj_buf.c`) —
  prefer `SBuf` to avoid importing a second buffer abstraction.

### 3.2 Symtab sideband (proto/trace/C → file:line)
- Mirror Tarantool `lj_symtab`: dump proto pointer → chunkname+firstline,
  trace-id → proto+startline. Source data already reachable via
  `proto_chunkname` / trace table. C-symbol resolution (`dl_iterate_phdr`) is
  **out of MVP** (Lua heap only).

### 3.3 Output formats (report §3 "output")
- MVP: binary event log + a Lua-side parser producing **text top / collapsed
  stacks (flamegraph)**.
- pprof-proto + `.heapsnapshot` export: **P1/P2**, done offline in the Lua
  tooling, not in the C core. Keep the C core format-agnostic.

---

## 4. Public API surface (MVP)

Lua module `memprof` (loadable via `-j` like `jit.p`):
```
memprof.snapshot{ name=, gc="full"|"step"|"none", out= }   -- Capability A
memprof.start{ mode="event", depth=1, out= }               -- Capability B
memprof.stop()
```
C API (embedding host), mirroring `luaJIT_profile_*` ownership model:
```
int  luaJIT_memprof_snapshot(lua_State *L, const char *opts, ...);
int  luaJIT_memprof_start(lua_State *L, const MemprofOpts *o);
void luaJIT_memprof_stop(lua_State *L);
```
Single-VM owner guard exactly like `ProfileState.g` (`src/lj_profile.c:326-330`).

HTTP endpoints / labels / timeline (report §3, §F): **P1**, layered in Lua/host,
no C-core dependency.

---

## 5. What changes vs Tarantool, and why (decision log)

| Tarantool | This fork | Reason |
|---|---|---|
| Wrap `lua_setallocf` | Hook `lj_mem_newgco*` + **`lj_mem_newagco`** (strings) / `lj_mem_freegco_` / sweep | allocf bypassed by bump alloc; strings use a separate `newagco` entry |
| `global_State.mem_L` back-ref | none — use `L` param at hook | chokepoints carry `L` |
| Per-object FREE via allocf(size 0) | precise for Trav sweep **and NonTrav string sweep (REV 2)**, **aggregate for POD sweep** | string reclaim folded into `gc_bitmap_sweep`; only `lj_arena_podsweep` lacks a per-obj callback |
| Heap snapshot: N/A (root-chain walk) | **arena bitmap walk** (cheap), strings included | arena maintains `block`/`mark` bitmaps; strings now in arenas too |
| Strings on `gc.root` chain, chained strtab | **open-addressing `g->str.tab`, strings off-chain** (REV 2) | commits `d6a73a09`/`740e1be1`: snapshot must not rely on `nextgc` for strings |
| Type inferred at write | `o->gch.gct` directly | tag already on every object header |

---

## 6. Scope boundaries (MVP)
- **IN:** GC objects (str/tab/func/proto/upval/thread/udata/cdata/trace), Lua
  stack + trace-id attribution (coarse), heap snapshot + diff + leak suspects,
  forced-GC-before-snapshot.
- **OUT (documented, not silent):** FFI cdata *backing* memory, raw `malloc`/mmap
  outside arenas, C-symbol resolution, retained-size/dominator/retaining-path
  (P2), pprof-proto & HTTP & timeline & labels (P1), per-object POD FREE sites.
- **Known caveats to surface in output** (report §G, §H): trace-time allocs →
  `INTERNAL`/trace-id only; incremental GC means "unfreed at stop ≠ leak" (offer
  `gc=full`); POD FREE is aggregate.

---

## 7. Proposed files & rough size
| File | Role | Est. LOC |
|---|---|---|
| `src/lj_memprof.c` | snapshot walker + event core | ~350 |
| `src/lj_memprof.h` | opts, record/format defs, flag | ~120 |
| `src/lj_symtab.c/.h` | proto/trace symtab dump (adapt Tarantool) | ~250 |
| hot-path edits | `lj_gc.h:430` inline + `lj_gc_arena.c` newgco/free/sweep | ~40 (guarded) |
| `src/lib_memprof.c` or `lib_misc` | Lua binding | ~120 |
| `tools/memprof/*.lua` | parser, text/flamegraph, diff | ~400 |

Reuse (no new code): `lj_buf`/`SBuf`, `lj_debug_dumpstack`, `lj_obj_typename`,
arena bitmap primitives, `lj_gc_fullgc`.

---

## 8. Risks
1. **Hot-path edit in `lj_gc.h:430`** — the allocation fast path is the most
   perf-sensitive code in the VM. Mitigation: single predictable-not-taken flag
   branch mirroring `GCF_MARKALLOC`; benchmark against the existing
   `inc_pause_assert` / bench harness before/after; keep the whole feature behind
   a build flag (`-DLUAJIT_ENABLE_MEMPROF`) so release default is byte-identical.
2. **POD FREE aggregation gap** — may confuse users expecting per-site frees for
   closures/protos. Mitigation: document; optionally instrument `lj_arena_podsweep`
   inner loop in P1 if demand exists.
3. **Snapshot vs concurrent mutation** — a walk during incremental GC sees a
   half-swept heap. Mitigation: `gc=full` (or run at `GCSpause`) for consistent
   snapshots; document the `none` mode as "may include soon-dead objects".
4. **Classic `!LJ_HASGCMARK` build** — feature is arena-specific. Must compile
   out cleanly when arena GC is disabled (guard with `LJ_HASGCMARK`).
5. **Interaction with `feature/concurrent-gc` (future)** — a live-heap walk under
   a concurrent marker needs its own synchronization; keep the snapshot walker's
   arena-iteration isolated so a future concurrent variant can add a barrier.

---

## 9. Phased roadmap (mapped to the report's P0/P1/P2)
- **v0 (MVP-A, report P0/P2-snapshot subset):** arena bitmap snapshot, type/size
  histograms, `inuse_*`, diff, leak suspects, `gc=full`. Read-only, isolated
  file, zero hot-path cost. **Lowest risk, highest early value.**
- **v1 (MVP-B, report P0-event):** ALLOC/REALLOC/FREE stream + stack/trace
  attribution + symtab + `alloc_*`/`freed_*`/survival. Introduces the guarded
  hot-path edit.
- **P1:** pprof-proto export, labels/tags, timeline + GC-cycle events, HTTP
  endpoints, multi-VM/worker aggregation (all offline/host-side).
- **P2:** retained size, dominator tree, retaining paths (object-graph traversal
  via `gc_traverse_*`), FFI-backing + external-malloc layer, DevTools UI export.

---

## 10. Open questions for the next decision point
1. Ship order: confirm **snapshot-first (v0)** vs event-first.
2. Build gating: dedicated `-DLUAJIT_ENABLE_MEMPROF` (keeps release default
   byte-identical) vs always-compiled-guarded-by-runtime-flag. Recommend the
   former given the "classic GC byte-for-byte unchanged" project constraint.
3. Buffer choice: reuse `SBuf` vs port Tarantool `lj_wbuf`. Recommend `SBuf`.
4. Event attribution: eager `lj_debug_dumpstack` per event (simpler, higher
   overhead) vs proto/trace-id + offline symtab resolution (Tarantool's, lower
   overhead). Recommend the latter for v1.
5. Do we need per-object POD FREE precision in v1, or is aggregate acceptable?

---

# REV 3 — AS-BUILT (shipped). Sections 1–10 above are the original PLAN (REV 1/2); this section records what was actually implemented and where it deviated.

Baseline at start of implementation: HEAD `d563d1d2`. Feature landed in **10 commits** `ecd78090` … `92b09409`. All §10 open questions are resolved below.

## As-built commit map
| Commit | Slice | Stream ver |
|---|---|---|
| `ecd78090` | v0 — read-only arena-bitmap snapshot + diff + leak suspects | (snapshot, no stream) |
| `4159dc65` | v1 — event-mode ALLOC/REALLOC/FREE stream, guarded hot-path hook | v1 |
| `21f046dd` | offline tooling — parser + top/collapsed/summary/leak + **pprof export** | — |
| `5cac6ec4` | C/builtin symbolization (ffid→name) + **GC survival-rate** | v2 |
| `aa3578b5` | **multi-frame** stack attribution | v3 |
| `78f1303c` | **line-precise** attribution | v4 |
| `c4c6c873` | **low-overhead sampling** (byte-accumulator + sampled-address set) | v5 |
| `08f2b988` | **labels/tags** (memprof.setlabel) | v6 |
| `504956bb` | **retained-size + retaining-path** (dominator tree, read-only) | (snapshot) |
| `92b09409` | **Go-style timeline** (memprof.mark) | v7 |

## §10 open questions — RESOLVED
1. Ship order → **snapshot-first (v0)**, as recommended. ✅
2. Build gating → dedicated **`-DLUAJIT_ENABLE_MEMPROF`**; release default byte-identical (sha256-verified every C-touching slice). The whole of `lj_memprof.c`/`lib_memprof.c` is `#if LJ_HASGCMARK && defined(LUAJIT_ENABLE_MEMPROF)`. ✅
3. Buffer → **`SBuf`** (no `lj_wbuf` port). ✅
4. Attribution → **proto/trace-id + offline symtab** (low overhead), as recommended. ✅
5. POD FREE → **aggregate** (single PODFREE record with cell-count); precise per-object FREE for Trav + NonTrav-string sweeps. Documented, not closed. ✅

## As-built wire format (v7, all fields ULEB128 unless noted)
Prologue: `"ljm"` + version byte + 1 reserved (5 bytes). Event header = `(opcode<<4)|src_kind`. Epilogue byte `0x80`. Opcodes: EPILOGUE=0, ALLOC=1, REALLOC=2, FREE=3, PODFREE=4, SYMTAB_LFUNC=5, SYMTAB_TRACE=6, SYMTAB_CFUNC=7, (8 reserved = epilogue nibble), LABELDICT=9, MARK=10 (0xA0-safe: scanner checks epilogue 0x80 first). src_kind: INT=0, LFUNC=1, CFUNC=2, TRACE=3.

**ALLOC (v7):** hdr, uleb(addr), uleb(size), byte(gct), byte(cls), byte(gcstate), uleb(leaf_src_id), uleb(gc_cycle)[v2], frame-stack[v3] = uleb(nframes)+nframes×(byte kind, uleb id, uleb line[v4]), uleb(weight)[v5], uleb(label_id)[v6].
**REALLOC/FREE:** as ALLOC minus type-axis; carry gc_cycle + a 1-frame stack; no weight/label (reference an already-recorded object).
**PODFREE:** uleb(cellcount), uleb(bytes). Aggregate.
**MARK[v7]:** uleb(ts_ns via CLOCK_MONOTONIC), uleb(gc_total live bytes), uleb(name_len), name bytes. Emitted on the Lua `mark()` path only — zero hot-path cost.
**Sidebands dumped at stop (after events, before epilogue):** SYMTAB_* (proto/trace→chunkname:firstline), LABELDICT (id→string).
Back-compat: each version is a strict additive superset; the parser version-gates every added field, and the at-stop C symtab scanner version-gates its skips (the one recurring bug class — a scanner desync on each new field — fixed at every bump).

## Attribution axis (as-built, richer than the REV 1/2 plan)
- **Multi-frame** leaf→root call stack (depth cap 32), wiring the previously-dormant `memprof.start{depth=N}` knob. Walks `lj_debug_frame(L,level,&size)` (read-only mirror of `lj_debug_dumpstack`).
- **Line-precise**: per-frame actual allocating line via a `#if MEMPROF`-guarded `lj_debug_frameline` wrapper exported from `lj_debug.c` (delegates to the static `debug_frameline`; flag-off byte-identical). Not `firstline`.
- **CFUNC symbolization**: records `fn->c.ffid` (from the live frame, no stale pointer), resolved offline via `jit.vmdef.ffnames` — so `string.format` etc. render as names, not `C:0x…`.

## Sampling (as-built, the correctness-critical slice)
Byte-accumulator gate (`interval` bytes; 0 = exact/default). THE TRAP: FREE is matched by address, so a **sampled-address set** (open-addressing, `g->allocf`-managed, freed at stop) gates FREE/REALLOC to emit only for sampled objects — no phantom frees, no negative inuse. Per-ALLOC `weight` = bytes represented; offline tool scales to population estimates. Verified: ~100% byte accuracy, 99.7% object estimate, events cut to 0.16%.

## Retained-size (as-built, the P2 heavyweight)
Read-only **object-reference-graph** twin of the GC mark traversal: mirrors `gc_traverse_tab/func/proto/thread`/udata/upval/trace ref-enumeration but replaces every `gc_markobj`/`gc_marktv` with an edge-record callback — **never touches a color/mark bit or gray list** (read-only proven by running a full GC after the snapshot with no corruption). Node set = live arena cells + huge slots + roots under a synthetic super-root. CSR adjacency + **Cooper-Harvey-Kennedy iterative dominators**; retained[n] = shallow + Σ retained(dom-children). Retaining path = reverse-BFS to super-root. All scratch via `g->allocf`. APIs: `memprof.retained{top=N}`, `memprof.retainers(addr)`.

## Public API (as-built)
Lua module `memprof`: `snapshot{gc=}`, `diff(s1,s2)`, `start{mode, depth, interval, out}`, `stop()`, `setlabel(str|nil)`, `mark(name)`, `retained{top,gc}`, `retainers(addr,gc)`. CLI `tools/memprof.lua`: `top`/`top-leaf`, `collapsed`, `summary`, `leak`, `survival`/`survival-leaf`, `labels` + `--label=` filter, `pprof`, `timeline`. Files: `src/lj_memprof.{c,h}`, `src/lib_memprof.c`, guarded wrapper in `src/lj_debug.c/.h`; `tools/memprof/{parse,aggregate,pprof}.lua` + `tools/memprof.lua`; 12 tests `test/gc/memprof_*_assert.lua`. NOTE: no separate `lj_symtab.c/.h` (§7 plan) — folded into `lj_memprof.c`.

## Invariants held (verified every slice)
- `src/lj_gc.c` byte-for-byte unchanged across all 10 commits (`git diff ecd78090~1 HEAD -- src/lj_gc.c` empty).
- Flag-off build byte-identical (sha256/md5 proven per slice); default binary carries 0 memprof symbols.
- One guarded `GCF_MEMPROF` branch is the only hot-path addition; `inc_pause_assert` worst-ms unregressed (<5ms).
- 12 regression tests + GC invariants (tricolor, inc_pause) green.

## Deviations from the REV 1/2 plan
- Attribution went **beyond** plan: plan was depth-1 proto/trace-id; shipped multi-frame + line-precise + CFUNC-name resolution.
- `lj_debug.c` was touched (guarded wrapper) — the plan assumed only `lj_memprof.c`/`lib_memprof.c`/`lj_memprof.h`. Still flag-off byte-identical.
- Symtab folded into `lj_memprof.c` (no separate `lj_symtab.*`).
- Timeline chose the **Go window-delta / mark model** (zero hot-path cost) over V8's periodic-full-snapshot UI.

## Not implemented (out of scope / future)
FFI cdata *backing* memory + external `malloc`/mmap layer (§I); HTTP `/debug` endpoints; multi-VM/worker aggregation; DevTools `.heapsnapshot` UI export; multi-key label SETS + per-coroutine labels; per-object POD FREE precision.

(End of REV 3 — as-built)

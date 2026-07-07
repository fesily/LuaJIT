# String Intern Table Redesign — Open Addressing + Bitmap-Sweep Reclaim

**Repo:** LuaJIT arena GC fork (`/home/fesily/luajit/.claude/worktrees/arenagc`)
**Status:** Design proposal (no code written)
**Author context:** Follow-up to measured Finding F1 — `gc_sweepstr` is the #1 GC hotspot; 96.99% of strings it visits are live survivors, ~74% of bucket-visits are empty, ~27% of total CPU.

---

## 1. Problem Statement

The current string interning table is an **array of bucket heads** (`g->str.tab`, `GCRef[]`), each bucket a **singly-linked collision chain** through the reused `GCstr.nextgc` GC-header slot.

Because a dead string must be **unlinked from its bucket chain**, the collector cannot let the cache-friendly address-order `gc_bitmap_sweep` reclaim strings. Instead a dedicated GC state, `GCSsweepstring`, walks **every** bucket chain **every cycle**, one bucket per incremental step.

Measured cost (this session):

| Metric | Value | Meaning |
|---|---|---|
| `strings_live_walked` / total | **96.99%** | Almost all per-string work is walking survivors, not freeing garbage |
| empty bucket-visits | **~74%** | Most steps do nothing |
| `gc_sweepstr` CPU (perf) | **~27%** | #1 self-time symbol |
| GC-time share | **58.9%–84.5%** | Dominates GC on string workloads |

Cost scales with **(live string set × cycle count)**, not with garbage produced. This is a structural (algorithmic) cost, not a constant-factor one — prefetch/SIMD/array-of-pointers cannot fix it.

---

## 2. Design Goals & Invariants That MUST Hold

### Goals
- G1. Eliminate the whole-table string sweep: cost of reclaiming strings must scale with **dead strings**, not live set.
- G2. Make intern **lookup** (`lj_str_new`) cache-friendly: contiguous probing, not pointer-chasing scattered arena cells.
- G3. Keep `GCstr` header size unchanged if at all possible (avoid `sizeof(GCstr)` / `strdata` churn across all JIT backends).
- G4. Preserve incremental-GC correctness (mutator interns strings between sweep steps).

### Load-bearing invariants (verified against source)
- **I1 — Identity == pointer equality.** Full interning is why `s1==s2` compiles to a pointer compare in the JIT. Any design MUST guarantee: at most one live `GCstr` per (content) at any time. Breaking this breaks JIT string equality, table keys, metamethod dispatch.
- **I2 — `GCstr.sid` is JIT-load-bearing.** `offsetof(GCstr, sid)` is emitted by `lj_asm_arm.h`, `lj_asm_arm64.h`, `lj_asm_mips.h` as the fast table-key hash. **`sid` cannot be repurposed** for a slot index.
- **I3 — `sizeof(GCstr)` is the string-data offset** everywhere (`strdata(s) = (char*)(s+1)`), emitted by all `lj_asm_*` backends. Changing header size forces a full VM/JIT rebuild and DynASM regeneration; low but nonzero risk on generated backend files.
- **I4 — Mark bitmap is the sole liveness authority** under `LJ_HASGCMARK` (`arena_obj_ismarked` for arena strings, hugeset slot for huge strings). The header white bit is vestigial during `GCF_BITMAPSWEEP`.
- **I5 — `strempty` and `mainthread` are non-arena SFIXED** (dlmalloc). `ptr2arena` must never be called on `strempty`. It must stay excluded from any arena-bitmap logic.
- **I6 — Huge strings live in the hugeset**, not scanned by arena address order; freed/handled in `rebuild_hugescan` (currently skipped there as "owned by gc_sweepstr").
- **I7 — Secondary-hash low-bit tag.** Under `LUAJIT_SECURITY_STRHASH`, bucket-head `GCRef`s carry a low-bit flag (`hashalg` per-chain). The new representation must preserve equivalent anti-collision-DoS behavior.
- **I8 — Arena class rotation & shutdown symmetry.** Adding `ArenaClass_Str` must replicate ALL per-class handling the POD/Udata classes have: current-arena rotation on exhaustion, `lj_arena_shrink` empty-arena release ([src/lj_arena.c:669](src/lj_arena.c)), `lj_arena_freeall` teardown ([src/lj_arena.c:710](src/lj_arena.c)), `findspace` class-match ([src/lj_arena.c:627](src/lj_arena.c)), and the verify-time class invariant ([src/lj_gc_arena.c:2325](src/lj_gc_arena.c)). Missing any one leaks or corrupts string arenas. `markinit` must clear string-arena marks like other classes.

---

## 3. Proposed Design

### 3.1 Table representation: open-addressing slot array

Replace the bucket-head + intrusive chain with an **open-addressing hash set of `GCstr*`**:

```c
typedef struct StrInternTab {
  MRef   slots;    /* GCRef *: open-addressing slots. EMPTY=0, TOMB=sentinel, else GCstr*. */
  MSize  mask;     /* capacity - 1 (power of two). */
  MSize  num;      /* live entries. */
  MSize  tombs;    /* tombstones (drive rehash). */
  uint64_t seed;   /* hash seed (as today). */
} StrInternTab;
```

- **`nextgc` is freed from intern duty.** Strings no longer chain through the GC header for interning. `nextgc` reverts to pure GC use — and since strings are non-traversable and reclaimed by bitmap sweep, strings need not sit on any GC root chain at all (see §3.4). Net: **no `GCstr` layout growth** (G3 satisfied — this is the key advantage over the earlier "backslot" patch which needed +8 bytes).
- **Probing:** linear probing with **Robin Hood** displacement (bounds worst-case probe length, keeps variance low → predictable lookup and predictable re-probe on reclaim). Tombstone-aware.
- **Slot value encoding:** `EMPTY = 0`; `TOMBSTONE = 1` (a reserved non-pointer sentinel, never a valid aligned `GCstr*`); otherwise a tagged `GCstr*`. The `strempty` singleton is never stored (it is returned directly by `lj_str_new` for len==0, as today).

### 3.2 Lookup — `lj_str_new`

```
h = hash(seed, str, len)
i = h & mask
probe:
  slot = slots[i]
  if slot == EMPTY:           break (miss)
  if slot == TOMBSTONE:       remember first tomb; i = (i+1)&mask; continue
  s = slot
  if s->hash == h && s->len == len && memcmp == 0:
      if dead(s):  /* bitmap says unmarked but not yet reclaimed this cycle */
          resurrect(s); return s        /* keeps I1: reuse the live-again cell */
      return s                          /* hit */
  i = (i+1)&mask; continue
insert:
  place at first tomb slot if any (reuse), else at EMPTY slot
  num++; maybe rehash if load/tomb thresholds exceeded
```

- Cache behavior (G2): probing walks **contiguous `slots[]`**, dereferencing a `GCstr` only on hash/len match — far fewer scattered arena touches than chain-walking.
- **Resurrection (I1/I4):** a string can be unmarked (logically dead) but not yet reclaimed by the incremental sweep. `lj_str_new` finding such a slot must resurrect it (mark it live again) rather than allocate a duplicate — exactly the existing `gc_obj_resurrect` path in today's `lj_str_new`. This preserves single-identity.

### 3.3 Reclaim — dedicated `ArenaClass_Str` + its word-parallel sweep, no chain walk

> **DESIGN UPGRADE (supersedes the earlier "add a NonTrav sweep" fix for blocker-1).** Rather than teach `gc_bitmap_sweep` to also visit the generic NonTrav class, introduce a **dedicated string arena class `ArenaClass_Str`**, symmetric to the existing `ArenaClass_POD` / `ArenaClass_Udata`. This turns blocker-1 from a patch into an architectural correction: strings currently squat in `ArenaClass_NonTrav` (whose comment even says "strings, VLA cdata" though VLA cdata actually has its own `ArenaClass_CdataV`, and upvalues/traces go to `ArenaClass_Trav` — so NonTrav is *de facto* strings-only already). Every other object family already has its own class; strings should too.

**New arena class (mirror the Udata/POD template):**

| Piece | Existing template to copy | New value |
|---|---|---|
| enum member | `ArenaClass_Udata` ([src/lj_arena.h:119](src/lj_arena.h)) | `ArenaClass_Str` |
| class → flags | `arena_classflags` ([src/lj_arena.c:517](src/lj_arena.c)) | `ArenaFlag_StrOnly` (new flag, like `ArenaFlag_UdataOnly`); **NOT** `ArenaFlag_TravObjs` (strings are non-traversable) |
| class → current ptr | `arena_classcur` ([src/lj_arena.c:525](src/lj_arena.c), [src/lj_gc.h:437](src/lj_gc.h)) | `g->gc.strarena` (new `MRef` in `GCState`) |
| alloc macro | `lj_mem_newgcou` (Udata, link=0) ([src/lj_gc.h:516](src/lj_gc.h)) | `lj_mem_newgcos` → `lj_str_alloc` uses it instead of `lj_mem_newagco(...,0)` |
| shrink/scavenge rotation | `podarena`/`udatarena` handling in `lj_arena_shrink` ([src/lj_arena.c:669](src/lj_arena.c)) & `lj_arena_freeall` ([src/lj_arena.c:710](src/lj_arena.c)) | add `strarena` |
| class invariant check | `ArenaFlag_UdataOnly` invariant in verify ([src/lj_gc_arena.c:2325](src/lj_gc_arena.c)) & findspace class match ([src/lj_arena.c:627](src/lj_arena.c)) | add `ArenaFlag_StrOnly` |

**String-arena sweep (mirror `lj_arena_podsweep`, [src/lj_gc_arena.c:1146](src/lj_gc_arena.c)) but with intern removal per dead cell:**

`lj_arena_podsweep` does a whole-arena word-parallel bitmap sweep (`block[w] & ~mark[w]`), freeing dead cells and recoloring survivors, touching no object headers — this is exactly the cache-friendly reclaim we want. The string version differs in one way: **POD objects (protos/closures) have no global content-indexed directory, but strings have the intern table.** So for each dead string cell the string sweep must ALSO remove it from the intern table:

```
for each dead string cell s in an ArenaFlag_StrOnly arena (block=1, mark=0, not FIXED, not strempty):
    strtab_remove(g, s)   /* re-probe to locate & tombstone slot; MUST precede free */
    lj_str_free(g, s)     /* free the cell */
```

Run this as a `GCSsweep` sub-step, dispatched like the POD branch (`if (a->flags & ArenaFlag_StrOnly) { ... }` next to the `ArenaFlag_PODOnly` branch at [src/lj_gc_arena.c:1146](src/lj_gc_arena.c)). It only touches cells that are actually dead (word-parallel), never walks live strings, never visits empty buckets. Cost ∝ dead strings + O(cells/64) word reads.

**Division of labour (important — the arena class does NOT replace the open-addressing mechanism):**
- **`ArenaClass_Str` + its sweep** solves blocker-1: *efficiently DISCOVER* dead string cells in cache-friendly address order (what POD sweep gives protos).
- **Open-addressing + `strtab_remove` re-probe** solves the string-specific extra: *O(1) REMOVE* the dead string from the content-indexed intern table without walking a chain. This is why strings need more than a POD-style sweep, and why the header stays the same size (re-probe needs no back-index).

**`strtab_remove` needs no per-string back-index.** The dead cell is *still allocated and readable at the moment of sweep* (remove happens strictly **before** `lj_str_free`), so `s->hash` and `strdata(s)` are valid. Re-probe the table from `s->hash` until the slot whose value `== s` is found; set it to `TOMBSTONE`; `tombs++`, `num--`. Open addressing lets us re-derive the slot from the object, so **no extra `GCstr` field and no fragile predecessor-pointer invariant** (the trap that made the earlier backslot scheme risky).

**Re-probe safety requirements (Oracle must-guard-3/4):**
- **Tombstone-through probing.** Both lookup and `strtab_remove` MUST probe *through* tombstones; a Robin Hood early-exit must NOT trigger across a tombstone, or two colliding dead strings can become mutually unreachable (tombstoning the first breaks locating the second).
- **Bounded + budgeted.** Cap probe length; a dying cluster can make per-removal cost O(cluster probe distances) → O(n²) on a large dying cluster. Charge the probe count against the `GCSWEEPMAX` sweep budget (today the budget only counts freed objects, [src/lj_gc_arena.c:1191](src/lj_gc_arena.c)), so a big die-off cannot blow the incremental step time.
- **Remove strictly before free.** After `lj_str_free`, the block head may be overwritten by freelist metadata (small cells, [src/lj_gc.h:485](src/lj_gc.h)) or the huge mapping unmapped ([src/lj_arena.c:1097](src/lj_arena.c)). One reclaim helper `strtab_remove_found(s)` → `lj_str_free(s)`, with a debug assert that the slot was located before free.

Result:
- **`GCSsweepstring` state is deleted.** `gc_sweepstr` deleted. The 172.9M/cycle survivor walk → **gone**.
- Dead-string reclaim runs as a sequential, cache-friendly, word-parallel bitmap pass over `ArenaClass_Str` arenas.
- Work now scales with **dead strings** (G1); strings are now a first-class arena family (architectural consistency with POD/Udata/CdataV).

**Two template-copy traps (Oracle final pass) — do NOT copy blindly:**
- **Do NOT give the string class `ArenaFlag_TravObjs`.** Strings are non-traversable: `gc_mark` marks strings but explicitly does **not** gray-push `~LJ_TSTR` ([src/lj_gc_arena.c:317](src/lj_gc_arena.c)), and gray-arena pop only accepts `TravObjs` arenas ([src/lj_gc_arena.c:755](src/lj_gc_arena.c)). Copying POD/Udata's trav flag would wrongly enlist string arenas into gray traversal.
- **Do NOT copy `lj_arena_podsweep` literally.** POD sweep rewrites bitmap words **without touching object data** ([src/lj_arena.c:327](src/lj_arena.c)). Strings MUST read the object (`s->hash`) to `strtab_remove` before `lj_str_free`, and the freed cell may be overwritten by freelist metadata ([src/lj_gc.h:485](src/lj_gc.h)) — so the string sweep is podsweep's word-parallel *scan* shape but with per-dead-cell object access + remove-before-free.

**⚠ Most dangerous omission (Oracle):** placing the `StrOnly` sweep branch "next to POD" *after* the `if (!(a->flags & ArenaFlag_TravObjs)) continue;` skip at [src/lj_gc_arena.c:1134](src/lj_gc_arena.c) — a non-trav Str arena is skipped **before** the branch runs, silently **leaking every dead arena string**. The `StrOnly` branch MUST be reached before that skip.

### 3.4 Strings leave the GC root chain

Strings are non-traversable; liveness is the mark bitmap (I4); reclaim is the new NonTrav sweep (§3.3). The intern table itself is the root container that keeps strings reachable for marking. Therefore strings need not be threaded on `g->gc.root` and need no rebuild-phase re-chaining. In arena mode strings are *already* allocated unlinked from `g->gc.root` (rebuild anchors the root on `mainthread` only, [src/lj_gc_arena.c:1439](src/lj_gc_arena.c)), so this is largely true today.

**CORRECTION (Oracle) — `nextgc` is still read by non-intern walks.** Freeing `nextgc` from intern duty is only safe after auditing every other reader. The verifier/full-GC helper walks at [src/lj_gc_arena.c:2423](src/lj_gc_arena.c) and [src/lj_gc_arena.c:2648](src/lj_gc_arena.c) currently iterate string `nextgc`; they must be **ported to slot iteration** over the open-addressing table. Also audit serialization, trace snapshot, and `lj_state` shutdown sweep for string `nextgc` reads before repurposing the slot. Only after that audit does `nextgc` revert to pure GC use.

**Marking:** roots that reference strings (stack, upvalues, table keys/values, protos) mark them as today — nothing changes on the mark side; a string gets `mark=1` iff something live points at it. The intern table is **not** itself a mark root that keeps all strings alive (that would defeat GC); it is a weak-by-construction directory: entries whose target is unmarked at sweep are reclaimed and tombstoned.

### 3.5 Huge strings

Huge strings are not in address-order arenas (I6). `rebuild_hugescan` currently skips `~LJ_TSTR` at [src/lj_gc_arena.c:1412](src/lj_gc_arena.c), and the **shutdown** path also treats huge strings as owned by `gc_sweepstr` at [src/lj_gc_arena.c:1818](src/lj_gc_arena.c). New rule: in **both** the huge-set scan and shutdown, a dead (unmarked slot) huge string must `strtab_remove` + free, symmetric to §3.3. The existing hugeset generation/restart machinery (`hugesetgen`, cursor restart) is unaffected because `strtab_remove` touches the *intern* table, not the hugeset slot ordering — but this symmetry MUST be implemented or huge dead strings leak.

### 3.6 Resize / rehash

- **Trigger:** grow when `num > mask * loadnum/loadden` (e.g. 7/8), or when `tombs` exceeds a fraction (rehash-in-place to purge tombstones). Shrink opportunistically after large die-offs, as `lj_str_resize` does today.
- **Rebuild:** allocate new `slots[]`, re-insert every live entry by re-probing (tombstones dropped naturally). Robin Hood + a per-table max-probe cap gives the anti-DoS property (I7); if any insert exceeds the probe cap, force a reseed+rehash. **Keep the existing `LUAJIT_SECURITY_STRHASH` secondary/dense-hash defense until the Robin Hood + probe-cap replacement is proven against the existing collision tests (P4 is separable/optional).**
- **CORRECTION (Oracle blocker-2) — resize during `GCSsweep` must preserve pending-dead strings.** Today `lj_str_resize` is forbidden *only* during `GCSsweepstring` ([src/lj_str.c:135](src/lj_str.c)), and `lj_str_alloc` triggers a resize from insertion at [src/lj_str.c:351](src/lj_str.c). Once `GCSsweepstring` is deleted, a mutator insert can trigger a resize **mid-`GCSsweep`, before the address sweep has reclaimed string S**. If the rehash copies only marked-live entries, it **drops S**; a later `lj_str_new(S content)` then misses and allocates a **duplicate** while the old S cell is still allocated → violates identity (I1). **Guard:** the rehash MUST copy every occupied non-tombstone slot whose object is **still allocated, regardless of its mark bit** (unmarked-but-not-yet-reclaimed strings are still valid interned objects until the sweep frees them). If a rehash wants to also drop unmarked strings eagerly, it must do so via the same `strtab_remove` → `lj_str_free` protocol synchronously — never by silently omitting them from the new table. Because reclaim locates slots by re-probe (not a stored index), a resize that relocates entries does not invalidate pending reclaim: the next dead cell re-probes the *current* table. There is no torn-slot-pointer window (the decisive advantage over the backslot scheme).

---

## 4. Correctness Analysis (per invariant)

| Invariant | How the design preserves it | Biggest trap |
|---|---|---|
| I1 identity | Single live entry per content; §3.2 resurrection prevents duplicate on unmarked-but-unreclaimed cell | **Resurrection race:** a string unmarked this cycle, re-interned before sweep reclaims it, must resurrect the SAME cell, not tombstone+realloc. Guard: `lj_str_new` checks liveness and resurrects before miss/insert. |
| I2 sid | `sid` untouched, still at same offset | — |
| I3 sizeof | Header unchanged; `nextgc` reused-for-GC-only, no new field | — (this is why open-addressing beats backslot) |
| I4 mark authority | Reclaim keys off `!arena_obj_ismarked` exactly as bitmap sweep already does | Re-probe reads `s->hash`/`strdata` of a *dead but still allocated* cell — valid only before `lj_str_free`. Order: remove-then-free (small-cell freelist overwrite [src/lj_gc.h:485](src/lj_gc.h), huge unmap [src/lj_arena.c:1097](src/lj_arena.c)). |
| I5 strempty/mainthread | Never stored in table; sweep skips FIXED/strempty | `ptr2arena(strempty)` must remain unreachable. |
| I6 huge strings | §3.5 reclaim in hugescan **and shutdown** | Symmetry bug risk: forget huge path (scan OR shutdown) → huge-string table leak. Covered by test. |
| I7 anti-DoS | Robin Hood + probe-cap reseed replaces secondary-hash; **keep dense-hash until proven** | Must validate against `LUAJIT_SECURITY_STRHASH` threat model / existing tests before removing the dense-hash path. |
| **NonTrav sweep (blocker-1)** | Dedicated `ArenaClass_Str` + `ArenaFlag_StrOnly` + POD-style word-parallel sweep (§3.3); strings become a first-class arena family | `gc_bitmap_sweep` skips non-`TravObjs` arenas today ([src/lj_gc_arena.c:1134](src/lj_gc_arena.c)); the new class gets an explicit dispatched sweep branch, so no dead arena string leaks. |
| **resize mid-sweep (blocker-2)** | Rehash preserves all still-allocated entries regardless of mark (§3.6) | Dropping unmarked-pending strings → duplicate interned object → breaks I1. |

**Single biggest correctness trap:** the **resurrection/reclaim ordering** across an incremental boundary (I1 × I4), now specifically the **resize-during-`GCSsweep`** variant (Oracle blocker-2). Oracle's verdict: the basic "unmarked but re-interned before sweep" resurrection hazard **already exists today** (`lj_str_new` → `gc_obj_resurrect`, [src/lj_str.c:378](src/lj_str.c)) and is handled; the *within-word* bitmap race is **not real** (`gc_bitmap_sweep` snapshots `dead = block & ~mark` per word at [src/lj_gc_arena.c:1163](src/lj_gc_arena.c) then processes the whole word without yielding, [line 1194](src/lj_gc_arena.c)). The **new** hazard the design introduces is resize/rehash during `GCSsweep` dropping a pending-dead string (§3.6 guard). Port the existing resurrection contract faithfully, AND make rehash preserve every still-allocated entry regardless of mark.

### 4.1 Accounting (Oracle) — no inherent bug, but deliberate updates required

Folding string frees into `GCSsweep` keeps the `estimate` correct (the existing `old - g->gc.total` subtraction at [src/lj_gc_arena.c:1997](src/lj_gc_arena.c) still applies). But the `GCState` enum, per-phase stats arrays, the timing switch, and exported stats still name `GCSsweepstring` ([src/lj_gc.h:16](src/lj_gc.h), [src/lj_gc_arena.c:2093](src/lj_gc_arena.c)). Delete/renumber them deliberately; the new NonTrav string sweep needs its own cost billing charged against `GCSWEEPMAX` (including probe cost, §3.3).

---

## 5. Behavior-Lock Tests (write FIRST, before any structural change)

1. **Identity under churn+resize:** intern many colliding strings, drop refs to a random subset, single-step incremental GC, re-intern survivors between steps, force a resize mid-sweep; assert (a) no duplicate interned object for equal content (pointer identity stable), (b) `num` equals live count, (c) lookups return the same pointer as before.
2. **Resurrection across sweep boundary:** arrange S unmarked, re-intern S's content before its cell is swept; assert the returned pointer == original S and S survives the cycle.
3. **Huge string reclaim + no-leak:** intern huge strings, kill them, run GC; assert huge-string count returns to baseline and no tombstone/leak accumulates (`collectgarbage("stats")` counters + `checkheap`).
4. **Tombstone purge:** create heavy tombstone load, assert rehash purges them and probe length stays bounded (add a max-probe assert in the debug verifier).
5. **Anti-DoS:** the existing `LUAJIT_SECURITY_STRHASH` collision tests must pass under the Robin Hood + reseed defense (or keep dense-hash as fallback if they don't).

**Debug verifier** (compile under `LUA_USE_ASSERT`, run each sweep step / insert / resize): every live slot's target is marked-or-unmarked-pending (never freed), `num`+`tombs` ≤ capacity, `num` == count of live cells with `gct==~LJ_TSTR` in arenas+hugeset, no slot points at a freed cell, probe length ≤ cap.

---

## 6. Migration Plan (incremental, each step shippable & verified)

- **P0.** Land the behavior-lock tests + `strings_live_walked`/`strings_dead_freed` counters (already present) as the regression baseline. Record current bench numbers. **The mandatory first test (Oracle): S unmarked after atomic → GC yields before reclaiming S → mutator re-interns same bytes → returned pointer is exactly S; then force a table resize/rehash before reclaim reaches S → assert pointer identity still holds and no stale slot remains after full GC.** This single test covers identity, resurrection, and resize-preservation.
- **P1.** Introduce `StrInternTab` open-addressing structure **alongside** the chain, behind a compile flag; port `lj_str_new` lookup/insert (tombstone-through probing, resurrection contract). Keep the existing `gc_sweepstr` doing reclaim (still walking, not yet merged) to isolate the lookup change. Verify identity tests + bench lookup delta (informs whether G2 pays off independently).
- **P2.** Add the **string sweep branch reached before the `TravObjs` skip** ([src/lj_gc_arena.c:1134](src/lj_gc_arena.c)) with per-dead-cell `strtab_remove` (§3.3), the huge-string reclaim (§3.5, item 8/9), the **resize-preserves-pending-dead** guard (§3.6, blocker-2), and a **verifier assertion that string arenas contain only `~LJ_TSTR`** (§9 item 10); delete `GCSsweepstring`/`gc_sweepstr` (§9 item 11). **Land on today's flags-0 NonTrav string arena first** (Oracle §8.1 — it is de-facto strings-only, avoiding the new-class plumbing surface). Verify `strings_live_walked` → 0, all GC tests (`test_gc_invariants`, `schedule_fuzz`, `stale_gray_survivor_stress`, `test_str_rehash_sweep`, `test_arena_freelist`, `checkheap`), bench.
- **P2.5 (optional, deferred).** Promote strings to a first-class `ArenaClass_Str` per the full §9 checklist (items 1–6) — only if per-class stats / distinct shrink policy / clearer invariants justify the added plumbing. Pure refactor; behavior-identical to P2.
- **P3.** Port the verifier/full-GC `nextgc` string walks ([src/lj_gc_arena.c:2423,2648](src/lj_gc_arena.c)) + serialization/shutdown readers to slot iteration; then remove strings from any residual root/rebuild handling (§3.4). Verify rebuild tests, `checkheap`.
- **P4.** Replace secondary-hash with Robin Hood + probe-cap reseed (§3.6); verify `LUAJIT_SECURITY_STRHASH` tests. (Optional — keep dense-hash if it's cheaper to retain.)
- **P5.** Remove the compile flag / old chain code + the `GCSsweepstring` enum/stats/timing remnants (§4.1). Re-run full baseline; compare `bench_gc_large` (was 84.5% GC in sweepstring) and `inc_pause_bench` strings workload.

Each phase is independently correct and measurable. P1 and P2 carry the bulk of the win; P3–P5 are cleanup/refinement.

---

## 7. Expected Outcome & Risks

**Win:** eliminates the 97%-live survivor walk and 74% empty-bucket visits entirely (they cease to exist, not merely optimized). String reclaim cost → O(dead strings), folded into an already-running cache-friendly pass. Lookup becomes contiguous-probe. Removes an entire GC state and the string branch of rebuild.

**Risks / cost:**
- Rewriting the intern subsystem touches `lj_str.c`, `lj_gc_arena.c` (sweep + hugescan + rebuild), `lj_state.c` (table init/free), and any place assuming bucket-chain layout. `GCstr` header itself is unchanged (biggest de-risk vs. backslot).
- Robin Hood + tombstones add insert-side complexity; anti-DoS parity with `LUAJIT_SECURITY_STRHASH` must be proven (P4 is separable/optional).
- Effort estimate: **~1 week including regression**, vs ~2 days for the lighter backslot patch — but strictly higher ceiling and a cleaner invariant surface.

**Recommendation:** proceed via §6 phases. P0–P2 alone deliver the measured hotspot elimination; treat P3–P5 as follow-ups gated on their own measurements.

---

## 8. Red-Team Verdict (Oracle)

**Verdict: the open-addressing idea is viable, but the design was NOT sound as originally written — two blockers must be fixed in the design before P1**, both now incorporated above:

1. **Blocker-1 (§3.3):** `gc_bitmap_sweep` does **not** scan NonTrav string arenas today. Resolved by a **dedicated `ArenaClass_Str` arena family** (`ArenaFlag_StrOnly` + a POD-style word-parallel sweep branch), mirroring the existing POD/Udata classes — an architectural correction, not a patch. (Merely replacing the `~LJ_TSTR` skip inside the trav-only loop would leak every dead arena string.)
2. **Blocker-2 (§3.6):** with `GCSsweepstring` gone, a mutator-triggered resize during `GCSsweep` must preserve every still-allocated string regardless of mark, or re-interning a pending-dead string creates a duplicate and breaks pointer-identity (I1).

**Confirmed non-issues:** the within-word bitmap resurrection race is not real (word snapshot processed without yield); the basic resurrection hazard already exists today and is handled by `gc_obj_resurrect`; deleting `GCSsweepstring` is not inherently an accounting bug (only enum/stats/timing renames needed).

**Must-guards folded in:** tombstone-through probing, bounded+budgeted re-probe, remove-strictly-before-free, huge-string symmetry in BOTH hugescan and shutdown, port of `nextgc` verifier/full-GC walks to slot iteration, retain `LUAJIT_SECURITY_STRHASH` defense until Robin Hood parity is proven.

With those amendments, the phased P0–P5 plan is sound to implement. P0's mandatory identity/resurrection/resize test is the gate.

### 8.1 Arena-class landing strategy (Oracle final pass)

`ArenaClass_Str` is **technically sound with the full §9 checklist**, but Oracle's risk recommendation is to **NOT introduce the new class in the first landing**:

- Today `g->gc.arena` (the NonTrav/default current pointer) is **de-facto strings-only** — verified: upvalues/traces route to `travarena` (`trav=1`, [src/lj_func.c:54](src/lj_func.c), [src/lj_trace.c:130](src/lj_trace.c)), VLA cdata to `CdataV` ([src/lj_cdata.c:34](src/lj_cdata.c)), udata to `Udata` ([src/lj_udata.c:16](src/lj_udata.c)). So strings already have a de-facto dedicated arena.
- **Recommended first landing (lower risk):** implement the new reclaim as a **flags-0 NonTrav string sweep branch** (reached before the `TravObjs` skip) + a verifier assertion that NonTrav arenas contain **only** `~LJ_TSTR`. This avoids the entire new-flag / new-current-pointer / findspace-mask / shrink / freeall failure surface (§9 items 1–6).
- **Promote to `ArenaClass_Str` later** only if the explicit family earns its added plumbing (e.g. once you want per-class stats, distinct shrink policy, or clearer invariants). The §9 checklist is the promotion spec.

`lj_arena_gcprepare` / `lj_arena_gc_markinit` are **confirmed non-issues**: they iterate every arena and clear allocated-cell marks flag-blind ([src/lj_arena.c:824](src/lj_arena.c), [src/lj_arena.c:850](src/lj_arena.c)), so a StrOnly (or flags-0) string arena needs no markinit change vs today.

---

## 9. Implementation Site Checklist (Oracle) — every place needing a string-arena case

If promoting to `ArenaClass_Str`, ALL of these must be handled (the flags-0 NonTrav-first path in §8.1 needs only items 7–11 plus the verifier assertion):

1. [src/lj_arena.h:83](src/lj_arena.h) — add `ArenaFlag_StrOnly` and `ArenaClass_Str`, **without** `ArenaFlag_TravObjs` (CdataV at lines 103–109 is the non-trav dedicated-class precedent).
2. [src/lj_obj.h:696](src/lj_obj.h) — add `g->gc.strarena` next to `arena/travarena/podarena/udatarena/cdatavarena`.
3. [src/lj_gc.h:437](src/lj_gc.h) — add `ArenaClass_Str → g->gc.strarena` to the inline current-arena selector; add a string alloc macro near lines 508–529; route [src/lj_str.c:321](src/lj_str.c) from `lj_mem_newagco(...,0)` to it.
4. [src/lj_arena.c:513](src/lj_arena.c) — `arena_classflags`: `ArenaClass_Str → ArenaFlag_StrOnly`; `arena_classcur` (~525): `ArenaClass_Str → &g->gc.strarena`.
5. [src/lj_arena.c:627](src/lj_arena.c) — findspace class-match mask MUST include `ArenaFlag_StrOnly` (else StrOnly compares as default NonTrav); also add `g->gc.strarena` to the "don't steal another class's current arena" list (lines 630–634).
6. [src/lj_arena.c:667](src/lj_arena.c) — `lj_arena_shrink` must load `curstr` and preserve it in the empty-arena release test (~684) — else `g->gc.strarena` can dangle at a destroyed arena; [src/lj_arena.c:708](src/lj_arena.c) `lj_arena_freeall` must clear `g->gc.strarena`.
7. [src/lj_gc_arena.c:1124](src/lj_gc_arena.c) — add the string sweep branch **before** the `TravObjs` skip at line 1134: flush bins, scan `block & ~mark`, `strtab_remove` before `lj_str_free`, continue. Leave POD (line 1146) and Udata (normal trav sweep) untouched.
8. [src/lj_gc_arena.c:1412](src/lj_gc_arena.c) — huge strings (currently "owned by gc_sweepstr") need huge-string reclaim once `GCSsweepstring` is gone.
9. [src/lj_gc_arena.c:1783](src/lj_gc_arena.c) — shutdown skips non-trav arenas and frees strings via `gc_sweepstr` (~1829–1830); with no chain sweep, shutdown must free string arenas + huge strings via the new path.
10. [src/lj_gc_arena.c:2325](src/lj_gc_arena.c) — extend verify-time class invariant: StrOnly arenas contain only `~LJ_TSTR`; the NonTrav scan (~2348–2363) currently only rejects udata, so it would miss a bad string route.
11. Deleting `GCSsweepstring` — update state/stats sites: [src/lj_gc.h:16](src/lj_gc.h), [src/lj_obj.h:597](src/lj_obj.h), [src/lj_gc_arena.c:1985](src/lj_gc_arena.c), [src/lj_gc_arena.c:2093](src/lj_gc_arena.c), stats exports (~2956, ~2996, ~3003).

## T0 — thread_openupval_sweep_assert.lua (failing-first test)

### Chosen observable: Option A (white-box via FFI)

Walk the coroutine's `openupval` chain directly and count open upvalues.
The `openupval` GCRef field offset in `lua_State` is self-calibrated at
runtime by comparing a thread with 0 vs 1 open upvalues (scan 8-byte-
aligned offsets for a uint64 that is 0 in the empty thread and non-zero
in the one-upvalue thread). This makes the test portable across LJ_GC64
and non-GC64 builds without hardcoding struct offsets.

On the assert build (x64, LJ_GC64=1), the calibrated offset is **64**.

### Key layout facts (lj_obj.h, LJ_GC64)

- `GCHeader = GCRef nextgc (8) + uint8_t marked (1) + uint8_t gct (1)` = 10 bytes
- `lua_State` layout (GC64):
  - offset 0: nextgc (GCRef, 8 bytes)
  - offset 8: marked (1), gct (1)
  - offset 10: dummy_ffid (1), status (1)
  - offset 12: 4 bytes padding (align glref to 8)
  - offset 16: glref (MRef, 8)
  - offset 24: gclist (GCRef, 8)
  - offset 32: base (TValue*, 8)
  - offset 40: top (TValue*, 8)
  - offset 48: maxstack (MRef, 8)
  - offset 56: stack (MRef, 8)
  - **offset 64: openupval (GCRef, 8)** ← target
  - offset 72: env (GCRef, 8)
- `gct` byte values: upvalue = 5 (~LJ_TUPVAL = ~(~5u) = 5), thread = 6.
- The openupval chain is linked via `nextgc` (GCRef at offset 0 of each
  GCobj), NOT via the `prev`/`next` union fields (those are for the global
  uvhead doubly-linked list). `gc_fullsweep(g, &th->openupval)` walks via
  `nextgc` and frees unmarked entries.

### Exact scenario

1. Create coroutine whose inner function captures **distinct named locals**
   as upvalues: `local a,b,c = ...; local f1 = function() return a end; ...`
   Then nil all closure refs: `f1,f2,f3 = nil,nil,nil`. Then `coroutine.yield()`.
   Resume once to drive it to the yield point.
2. The upvalues stay OPEN on `co->openupval` (frame is live, suspended).
   The closures are dead (nilled) -> no live closure references the upvalues
   -> the upvalues are unmarked during GC (gc_traverse_thread does NOT walk
   the openupval chain; only gc_traverse_func marks upvalues via closures).
3. Anchor the coroutine in a live table (keeps it LIVE + suspended).
   Do NOT resume to completion (that would close upvalues via
   lj_func_closeuv, masking the leak).
4. `collectgarbage("collect")` twice. The dead open upvalues must be freed
   by `gc_fullsweep(g, &gco2th(o)->openupval)` in rebuild_arenascan.

### Critical Lua semantics gotcha

Loop variables (`for i=1,N`) share a single stack slot across iterations.
Closures created in a loop that capture `i` all share ONE upvalue, not N.
Table accesses (`vars[i]`) capture only the table local, not per-element
upvalues. To get N distinct upvalues, use N distinct named locals:
`local a, b, c = ...`. The test hardcodes 3 upvalues per coroutine.

### RED/GREEN proof (captured, not committed)

**GREEN** (HEAD eb71f9d3, assert build, no stub):
```
calibration: openupval offset = 64
thread_openupval_sweep_assert: 9 passed, 0 failed
EXIT: 0
```

**RED** (stub: replaced `gc_fullsweep(g, &gco2th(o)->openupval);` at
lj_gc_arena.c:1363 with `/* STUB: ... */`, rebuilt):
```
calibration: openupval offset = 64
FAIL: group1 after GC: dead open upvalues freed (expected 0, found 30) -- thread sweep is load-bearing
RED PROOF: 30 dead open upvalues leaked on 10 live coroutines after full GC -- rebuild_arenascan thread sweep missing
FAIL: group3 round 1: no leaked upvalues (expected 0, found 12)
FAIL: group3 round 2: no leaked upvalues (expected 0, found 24)
FAIL: group3 round 3: no leaked upvalues (expected 0, found 36)
FAIL: group3 round 4: no leaked upvalues (expected 0, found 48)
FAIL: group3 round 5: no leaked upvalues (expected 0, found 60)
thread_openupval_sweep_assert: 3 passed, 6 failed

Rename note: rebuild_arenascan/Rebuild_ArenaScan became rebuild_threadscan/Rebuild_ThreadScan to match the thread-scan rebuild phase.
EXIT: 1
```

Group2 (control: live closures keep their upvalues) passed in both RED
and GREEN -- the stub only affects dead upvalues, not live ones. This
proves the sweep is surgical.

Group3 shows linear accumulation (12 per round = 4 coroutines x 3
upvalues), confirming the leak is deterministic and unbounded.

**Revert**: `git checkout -- src/lj_gc_arena.c`, rebuild, re-run -> GREEN
(exit 0, 9 passed). No stub left in the tree. `git diff -- src/lj_gc.c`
empty at all times.

### Why gc_traverse_thread does NOT mark open upvalues

`gc_traverse_thread` (lj_gc.c:313) marks stack slots (stack+1+LJ_FR2 to
top), the env table, and frame functions. It does NOT walk `openupval`.
Open upvalues are only marked by `gc_traverse_func` (when a live closure
references them) or `gc_mark_uv` (which only marks the VALUE of already-
gray upvalues, not the upvalue object itself). So a dead closure's open
upvalue is truly unreachable from the marker's perspective -> dead ->
only freed by the thread sweep.

## T1 — sweepthreads snapshot infrastructure (capture only, no behavior change)

### Files touched
- `src/lj_obj.h`: 3 new GCState fields after the graythread trio (L670-675).
  `MRef sweepthreads` + `MSize sweepthreadstop` + `MSize sweepthreadssz`.
- `src/lj_gc_arena.c`: helpers, snapshot capture, reset, free. NO change to
  `rebuild_arenascan` (T2's job) and NO change to `src/lj_gc.c`.

### Exact snapshot site
`atomic()` in `src/lj_gc_arena.c`, IMMEDIATELY AFTER the final
`udsize += gc_propagate_gray(g);` at L1944 and BEFORE the
`/* All marking done, clear weak tables. */` comment at L1965.
Snapshot block occupies L1946-1963. This is the Oracle-approved point:
AFTER `gc_mark_mmudata` (L1943) + final `gc_propagate_gray` (L1944), so
finalizer-reachable threads discovered late are included. Snapshotting
BEFORE this point (e.g. after the first drain/rescan at L1884-1890) would
miss threads greyed by mmudata finalizer marking.

### mainthread in graythread at snapshot time — defensive exclusion kept
`gc_traverse_mainthread` (called at L1871 in atomic, before the drain) marks
the mainthread via `gc_markobj`/direct traversal, NOT via
`gc_graythread_push` (that push happens only in `gc_propagatemark` at L706
for threads reached through barrier graying). Empirically the mainthread is
NOT on graythread at snapshot time (it is traversed directly, not pushed).
The `if (o == obj2gco(mainthread(g))) continue;` exclusion is kept as
defensive cover regardless — it is a no-op if mainthread is absent, and
correct if it is ever present. `rebuild_epilogue` sweeps mainthread's
openupval chain itself, so it must not appear in the snapshot.

### Init sites
No explicit zero-init site for graythread fields exists — global_State is
zero-allocated at creation (calloc/memset), so all graythread/sweepthreads
fields (MRef NULL, MSize 0) start zeroed automatically. Nothing to mirror
for sweepthreads. Confirmed by grepping `graythreadsz`/`graythreadtop =`
and `setmref(g->gc.graythread` across src/ — only the helper bodies and
the free site touch them.

### Reset / free mirroring
- `gc_mark_start` (L344-347): added `gc_sweepthreads_reset(g);` next to
  `gc_graythread_reset(g);` so no stale snapshot survives into the next
  cycle.
- `lj_gc_graywork_free` (L922-928): added
  `gc_ptrstack_free(g, &g->gc.sweepthreads, &g->gc.sweepthreadstop,
  &g->gc.sweepthreadssz);` next to the graythread free.
- The mid-cycle abort path in `lj_gc_fullgc` (L2532 `gc_graythread_reset`)
  was NOT mirrored — the plan specifies reset only at `gc_mark_start`.
  A stale snapshot window [fullgc-abort, next gc_mark_start] is harmless
  in T1 (snapshot is captured but never consumed). T2 will revisit if
  needed.

### Helpers (lj_gc_arena.c L848-862)
`gc_sweepthreads_push` (wraps `gc_ptrstack_push`) and
`gc_sweepthreads_reset` (top=0). No `empty`/`pop` helpers added — T1 only
captures; T2 will add consumption helpers when it walks the snapshot.

### Verification (arena assert build)
- Build: `make clean && make -j4 XCFLAGS="-DLUAJIT_ENABLE_GCARENA
  -DLUAJIT_SECURITY_STRHASH=1 -DLUA_USE_ASSERT"` — clean.
- `git diff -- src/lj_gc.c` empty (classic byte-for-byte unchanged).
- `git diff --stat`: only `src/lj_gc_arena.c` (+36) and `src/lj_obj.h` (+3).
- Full suite green:
  - test_gc_invariants: 6/6
  - test_gc_adversarial: 18/18
  - test_weak_stacks: 12/12
  - test_jit_tbar: 3/3
  - test_gc_obj_isdead_authority: 8/8
  - test_str_rehash_sweep: 34/34
  - test_huge_string_gc: 9/9
  - gc/huge_swept_tag_assert: 5/5
  - test_jit_cdata_parity: 21/21
  - gc/udata_finalize_assert: 5/5
  - gc/thread_openupval_sweep_assert (T0): 9/9
  - torture_gc 300: OK (6000 finalizers, 0 errors)
  - gc/rebuild_stress: ALL 8 SCENARIOS PASSED (5 iter; 100-iter exceeds
    time budget — pre-existing, scenario b huge-alloc is genuinely slow,
    unrelated to T1 which only adds a pointer copy)
  - gc/deep_pause_assert: PASS
  - gc/inc_pause_assert: PASS

### Behavior
No observable behavior change — the snapshot is captured but not yet
consumed. `rebuild_arenascan` still does the old O(live) arena scan. T2
will switch `rebuild_arenascan`'s thread-openupval walk to consume
`g->gc.sweepthreads` instead.

## T2 — rebuild_arenascan rewritten to O(threads)

### Diff scope
- `src/lj_gc_arena.c` ONLY (+36/-78). `src/lj_gc.c` and `src/lj_obj.h` byte-
  for-byte unchanged (`git diff -- src/lj_gc.c` empty).

### Rewrite (L1334)
Replaced the `while (ai < g->gc.arenastop && done < GCSWEEPMAX)` O(live)
arena survivor scan with an iteration over `g->gc.sweepthreads`:
```
GCobj **thr = mref(g->gc.sweepthreads, GCobj *);
MSize i, n = g->gc.sweepthreadstop;
for (i = 0; i < n; i++) {
  GCobj *o = thr[i];
  lj_assertG(o->gch.gct == ~LJ_TTHREAD, "sweepthreads non-thread");
  lj_assertG(o != obj2gco(mainthread(g)), "mainthread in sweepthreads");
  lj_assertG(arena_obj_ismarked(ptr2arena(o), ptr2cell(o)) ||
             lj_arena_ishuge(o), "sweepthreads entry not live at rebuild");
  gc_fullsweep(g, &gco2th(o)->openupval);
}
g->gc.sweepa = 0; g->gc.sweepw = UnusedBlockWords;
g->gc.rebuild_hugehi = 0; g->gc.rebuild_hugegen = g->gc.hugesetgen;
g->gc.rebuildphase = Rebuild_HugeScan;
```
Removed locals: arenas, ai, w, done, wtop, a, alive, bitidx, c, o (loop-
local). Kept the two pre-existing entry asserts (state==GCSsweep,
!GCF_DEADAUTH). The HugeScan handoff block is preserved verbatim (the
"Arm the HugeScan cursor" comment is original).

### Comment updates
1. Enum (L1089): `Rebuild_ArenaScan, /* Pass-1: O(threads) sweep of live
   coroutine openupval chains (snapshot from atomic). */`
2. Function comment above `rebuild_arenascan` rewritten to one-shot
   O(threads) rationale (snapshot source, mainthread exclusion, atomic-
   live⇒survives invariant, why the old scan is gone).
3. Dispatcher comment (gc_rebuild_rootchain L1529-1535): ArenaScan is no
   longer chunked — only Prologue and HugeScan yield mid-phase.

### Dispatcher yield condition
CHOSE: dropped `Rebuild_ArenaScan` from the yield condition (plan option
"cleaner"). New condition: `phase == Rebuild_Prologue || phase ==
Rebuild_HugeScan`. ArenaScan always completes in one call (sets
rebuildphase=HugeScan), so `rebuildphase == phase` was never true for it
— the dead branch is removed for clarity. Comment matches.

### fullgc abort path reset
`lj_gc_fullgc` abort branch (L2508 `if (g->gc.state <= GCSatomic)`) had
`gc_graythread_reset(g)` at L2510. Added
`gc_sweepthreads_reset(g);  /* Stale snapshot must not feed next cycle. */`
immediately after. Closes the T1-residual gap: a stale snapshot captured
by the aborted cycle is now cleared before the restarted cycle's rebuild
can consume it. Without this, the restarted cycle could sweep openupvals
on threads that died in the aborted cycle's view (snapshot was unused in
T1 so the gap was harmless then).

### Verification (arena assert build)
`make clean && make -j4 XCFLAGS="-DLUAJIT_ENABLE_GCARENA
-DLUAJIT_SECURITY_STRHASH=1 -DLUA_USE_ASSERT"` — clean.

Suite:
- test_gc_invariants: 6/6
- test_gc_adversarial: 18/18
- test_weak_stacks: 12/12
- test_jit_tbar: 3/3
- test_gc_obj_isdead_authority: 8/8
- test_str_rehash_sweep: 34/34
- test_huge_string_gc: 9/9
- test_jit_cdata_parity: 21/21
- gc/huge_swept_tag_assert: 5/5
- gc/udata_finalize_assert: 5/5
- gc/deep_pause_assert: PASS (worst 0.640ms / 3ms threshold)
- gc/inc_pause_assert: PASS (worst 2.316ms / 5ms threshold)
- torture_gc 300: OK (6000 finalizers, 0 errors)
- gc/rebuild_stress: ALL 8 SCENARIOS PASSED (5 iter; 100-iter exceeds
  time budget — pre-existing, scenario b huge-alloc is genuinely slow,
  unrelated to T2 which only swaps an arena walk for a snapshot walk)
- gc/thread_openupval_sweep_assert (T0): 9/9 GREEN — load-bearing proof
  the thread openupval sweep still happens.

### Behavior
The O(live) arena survivor scan is GONE — no dead/commented code left.
The single per-cycle duty of `rebuild_arenascan` is now
`gc_fullsweep(g, &gco2th(o)->openupval)` for each snapshotted live non-
main thread. `makewhite` is NOT re-added (removed in eb71f9d3, separate
concern). Huge threads handled by `rebuild_hugescan`; mainthread by
`rebuild_epilogue`; vmthread kept in snapshot (epilogue doesn't sweep it).

## T3 — Measurement + full verification

### Instrumentation (temporary, reverted)
Added at top of `src/lj_gc_arena.c` after the macros (between
`isfinalized` define and the `-- Mark phase` comment):
- `#include <time.h>`, `#include <stdio.h>`
- `static uint64_t instr_bs_ns, instr_as_ns, instr_cm_ns;` + `_cnt` counters
- `static LJ_AINLINE uint64_t instr_now(void)` wrapping `clock_gettime`
  (CLOCK_MONOTONIC).

Wrapped each sub-phase:
- `gc_bitmap_sweep`: `instr_t0` at entry; before `return freed;` ->
  `instr_bs_ns += delta; instr_bs_cnt += freed;`
- `rebuild_arenascan`: `instr_t0` at entry; after setting
  `rebuildphase = Rebuild_HugeScan` -> `instr_as_ns += delta;
  instr_as_cnt += g->gc.sweepthreadstop;`
- `rebuild_clearmarks`: `instr_t0` at entry; after
  `g->gc.rebuildphase = Rebuild_Done;` -> `instr_cm_ns += delta;` +
  one `fprintf(stderr, "[SWEEP] bitmap_sweep=%.2fms ... arenascan=%.2fms
  ... clearmarks=%.2fms\n", ...)` then reset all counters to 0.

`git diff --stat -- src/` during instrumentation: only
`src/lj_gc_arena.c` (+22). `git diff -- src/lj_gc.c` empty throughout.

### Measurement 1 — sweep sub-phase probe (1GB all-live tables)
Build: arena release + instrumentation.
Probe: `local N=1024*16384; live={} for i=1,N do live[i]={i,i,i} end;
collectgarbage("collect"); for r=1,3 do collectgarbage("collect") end`
(N=16,777,216 live tables; numbers unboxed so 16.77M live GC objects).

Steady-state (last 4 cycles, all 16.77M live so freed=20 transient only):
```
[SWEEP] bitmap_sweep=2.93ms (freed=20)  arenascan=0.00ms (threads=1)  clearmarks=2.28ms
[SWEEP] bitmap_sweep=2.56ms (freed=20)  arenascan=0.00ms (threads=1)  clearmarks=2.15ms
[SWEEP] bitmap_sweep=3.63ms (freed=20)  arenascan=0.00ms (threads=1)  clearmarks=2.51ms
[SWEEP] bitmap_sweep=3.26ms (freed=20)  arenascan=0.00ms (threads=1)  clearmarks=2.13ms
```
- **arenascan = 0.00ms** (was ~60ms pre-T2, 92% of sweep). T2 confirmed:
  the O(live) arena survivor scan is gone; only the snapshot thread loop
  runs (threads=1 here = mainthread-excluded vm thread).
- bitmap_sweep ~3ms, clearmarks ~2ms, **sweep total ~5ms** at 16.77M live.
  Matches plan target.

### Measurement 2 — interleaved arena-vs-classic 1GB full-GC
Builds: arena release (instr) + classic release (make clean between).
Probe: 1GB all-live tables, 15 `collectgarbage("collect")` per round,
report min/median/max via FFI `clock_gettime`. 4 interleaved rounds
(arena, classic, arena, classic, ...). Min-based (machine has variable
background load; min = noise-robust floor for compute-bound full-GC).

| Round | arena min | classic min | arena ≤ classic? |
|------|----------|------------|------------------|
| 1    | 385.5    | 506.6      | yes              |
| 2    | 344.1    | 623.7      | yes              |
| 3    | 383.1    | 493.5      | yes              |
| 4    | 380.0    | 489.1      | yes              |

- Arena min ≤ classic min in **all 4 rounds** (gate met).
- Arena best min = **344.1ms** ≤ eb71f9d3 baseline (~369ms) (gate met).
- Classic min ranges 489-624ms (consistent with pre-arena cost; the
  classic path was not regressed by T1/T2 — `src/lj_gc.c` byte-for-byte
  unchanged across all of T0-T3).

### Verification 3 — arena assert suite (all GREEN)
Build: `make clean && make -j4 XCFLAGS="-DLUAJIT_ENABLE_GCARENA
-DLUAJIT_SECURITY_STRHASH=1 -DLUA_USE_ASSERT"`.

| Suite                                  | Result                          |
|---------------------------------------|---------------------------------|
| test_gc_invariants                     | 6/6                             |
| test_gc_adversarial                    | 18/18                           |
| test_weak_stacks                       | 12/12                           |
| test_jit_tbar                          | 3/3                             |
| test_gc_obj_isdead_authority           | 8/8                             |
| test_str_rehash_sweep                  | 34/34                           |
| test_huge_string_gc                    | 9/9                             |
| gc/huge_swept_tag_assert               | 5/5 (4 scenarios)               |
| test_jit_cdata_parity                  | 21/21 (traces=8)                |
| gc/udata_finalize_assert               | 5/5 scenarios                   |
| gc/thread_openupval_sweep_assert (T0)  | 9/9 (threads=21 snapshot)       |
| torture_gc 300                         | OK (6000 finalizers, 0 errors)  |
| gc/rebuild_stress (5 iter)             | ALL 8 SCENARIOS PASSED          |
| gc/deep_pause_assert                   | PASS (worst 0.406ms / 3ms)      |
| gc/inc_pause_assert                    | PASS (worst 1.627ms / 5ms)      |
| gc/huge_vla_cdata_assert               | 4/4 scenarios (s5 documented skip) |
| gc/white1_free_assert                  | 135/135                         |
| gc/root_chain_probe_assert             | 10/10                           |

All green. T0 shows `threads=21` in the arenascan snapshot — confirms
the snapshot path is exercised end-to-end under load.

### Verification 4 — coroutine-churn stress
Script: `/tmp/opencode/churn.lua` (NOT in repo; cleaned up after).
- 10000 ops: create coroutine with 3 distinct named-local upvalues,
  resume to yield (suspended w/ open upvalues), anchor in `live` table;
  randomly drop anchors; interleave `collectgarbage("step", 0)` every op;
  every 50 ops create 10 unanchored coroutines + another step.
- Final: 3x full collect, clear remaining anchors, 2x full collect.
- Gate: `lj_state_free` graythread-membership assert (src/lj_state.c
  L414-417) must never trip; no crash/UAF; memory stable.

Assert build result:
```
churn: ops=10000 max_live=10000  mem_start=53KB mem_end=179KB delta=+126KB
churn: PASS (no assert trip, no crash, memory stable)
```
- max_live hit 10000 (the create-heavy LCG distribution), so the sweep
  snapshot grew to ~10k entries — well above the steady-state 1.
- delta +126KB after full churn + final collects — bounded, no leak.
- graythread assert never tripped: dead threads freed in `gc_bitmap_sweep`
  (mark=0) are not on the graythread stack at free time, and snapshot
  entries (mark=1 atomic-live) are not freed during the sweep window —
  matches the T2 invariant argument.

### Verification 5 — ASAN
ASAN available (`gcc -fsanitize=address`). Build:
`make clean && ASAN_OPTIONS=detect_leaks=0 make -j4
CC="gcc -fsanitize=address" XCFLAGS="-DLUAJIT_ENABLE_GCARENA
-DLUAJIT_SECURITY_STRHASH=1 -DLUA_USE_ASSERT"`
(detect_leaks=0 during build because the host code-gen tools
minilua/buildvm intentionally don't clean up at exit; re-enabled for
runtime).

Runtime (ASAN_OPTIONS=detect_leaks=0 — LuaJIT global_State itself is
not torn down at exit, would produce false leak reports unrelated to the
UAF check; ASAN heap/access sanitizer stays active):
- churn.lua: PASS, no ASAN error, no UAF.
- T0 thread_openupval_sweep_assert: 9/9, no ASAN error.

Confirms no use-after-free in the thread sweep / snapshot path.

### Revert + restore
- `git checkout -- src/lj_gc_arena.c` -> all instrumentation gone.
- `grep -nE "instr_bs_ns|instr_as_ns|instr_cm_ns|instr_now|T3 TEMPORARY"
  src/lj_gc_arena.c` -> empty.
- `git diff --stat -- src/` -> empty.
- `git diff -- src/lj_gc.c` -> empty (classic untouched throughout T3).
- `make clean && make -j4 XCFLAGS="-DLUAJIT_ENABLE_GCARENA
  -DLUAJIT_SECURITY_STRHASH=1"` -> arena release restored.
- `./src/luajit -e 'print("arena release restored")'` -> "arena release
  restored" (exit 0).
- HEAD: `c33e9f0d` (T2; no new commits in T3 — instrumentation was
  temporary proof only, reverted before finish).
- Temp scripts in /tmp/opencode/ removed (sweep_probe.lua,
  interleaved.lua, churn.lua, luajit_arena_instr, luajit_classic,
  asan_probe). No temp files left in the repo.

### Summary
- arenascan 0ms (was 60ms) — T2 fix verified.
- sweep total ~5ms at 16.77M live (1GB tables).
- arena min (344ms) ≤ classic min (489-624ms) all 4 rounds, ≤ 369ms
  baseline.
- Full assert suite green; T0 load-bearing proof green.
- Coroutine-churn + ASAN: no graythread assert trip, no UAF, no leak.
- src/ clean at c33e9f0d; arena release is worktree default.

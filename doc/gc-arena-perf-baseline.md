# Arena GC — Performance Baseline & Hotspot Report

**Repo:** `/home/fesily/luajit/.claude/worktrees/arenagc` (LuaJIT fork, arena bitmap GC)
**Git HEAD:** `488206f3` (uncommitted: GC stats module in `lib_base.c`, `lj_arena.c`, `lj_gc.h`, `lj_gc_arena.c`, `lj_obj.h`, `test/bench_gc_focused.lua`)
**Date:** 2026-07-04
**Mode:** MEASUREMENT AND ANALYSIS ONLY — no source code was modified. Only `/tmp/gcbaseline/` and this report file were written.

---

## 1. Environment

| Item | Value |
|---|---|
| CPU model | AMD Ryzen 9 7945HX with Radeon Graphics |
| nproc | 32 |
| Kernel | Linux 6.6.87.2-microsoft-standard-WSL2 (WSL2, x86_64) |
| `perf_event_paranoid` | 2 (user-space `perf record` allowed; no sudo used) |
| Load avg at start | 1.08 / 1.62 / 3.58 (32 cores → effectively idle) |
| `perf` binary | `/home/fesily/.local/bin/perf` |

### Build configurations used

All builds: `BUILDMODE=static` (shared lib fails to link due to a **known pre-existing** `lj_gc_barrierback_arena` symbol clash — out of scope, ignored per task constraints).

| Build | Flags | Purpose |
|---|---|---|
| **default** | `XCFLAGS="-DLUAJIT_ENABLE_GCARENA"` | T1 baseline timing + T3 perf profile (no timing probes polluting the profile) |
| **timing** | `XCFLAGS="-DLUAJIT_ENABLE_GCARENA -DLUAJIT_ENABLE_GCSTATS_TIMING"` | T2 per-phase time attribution |
| **perf** | `XCFLAGS="-DLUAJIT_ENABLE_GCARENA" CCDEBUG=-g` | T3 perf profile with symbols (default codegen `-O2`, `-g` only adds debug info) |

`./src/luajit -e 'print(collectgarbage("stats").timing)'` was verified `false` for default/perf builds and `true` for the timing build.

All benchmarks invoked with `-joff` (per the script headers' own recommendation, so the timed signal is GC work, not trace compilation). Benches run **sequentially**, never in parallel.

---

## 2. Baseline Wall Times + Bench-Reported Metrics (default build, no timing)

3 runs each. Wall time from `/usr/bin/time -v` "Elapsed". Each bench reports its own per-case `median/min/max` over RUNS=7 internal repetitions; the table below reports the **median of the 3 runs' reported medians** (and the min/max across the 3 runs).

### 2.1 bench_gc_focused (wall: 10.52 / 11.10 / 10.62 s — median **10.62 s**)

| Case | median (s) | min (s) | max (s) |
|---|---|---|---|
| B1: 200 fullgc, 50K live tables | 0.4029 | 0.4010 | 0.4146 |
| B2: sweep 100K dead tables | 0.0011 | 0.0009 | 0.0013 |
| B3: 100 fullgc, 10K deep chain | 0.0172 | 0.0171 | 0.0183 |
| B4: alloc 1M tables, auto GC | 0.0399 | 0.0389 | 0.0437 |
| B5: 500K barrier writes | 0.0837 | 0.0785 | 0.0975 |
| B6: 500K unique strings + collect | 0.0352 | 0.0349 | 0.0359 |
| B7: 500K mixed (50% survive) | 0.0567 | 0.0531 | 0.0682 |
| B8: collect 100K weak entries | 0.0000 | 0.0000 | 0.0000 |
| B9: 200K cdata alloc + collect | 0.0355 | 0.0330 | 0.0362 |
| B10: 10K incremental steps, 20K live | 0.0100 | 0.0097 | 0.0106 |
| B11: fullgc on 10 tables × 10K keys | 0.5446 | 0.4994 | 0.6307 |

**GC stats counters (default build, run 1 — bench_gc_focused self-reports):**

| Counter | Value | Counter | Value |
|---|---|---|---|
| cycles | 7,250 | steps_propagate | 196,267,051 |
| steps_atomic | 8,066 | steps_sweepstring | **224,729,088** |
| steps_sweep_bitmap | 303,766 | steps_sweep_rebuild | 0 |
| strings_chains_swept | **224,729,088** | sweep_cells | 12,293,799 |
| mark_calls | 199,201,081 | mark_cost | 53,228,857,516 |
| grayarena_pops | 196,275,690 | findspace_calls | 434,285 |
| barrierback | 157,457 | gray_notify | 1,194,568 |
| arenas_created / destroyed / shrunk | 187 / 171 / 171 | arenastop | 20 |

The **224.7M `strings_chains_swept`** (vs 196M mark calls, 12.3M sweep_cells) is the headline counter anomaly — see Finding F1.

### 2.2 bench_gc_large (wall: 43.12 / 43.51 / 44.85 s — median **43.51 s**)

| Workload | lua (MB) | rss (MB) | over (rss/lua) | fullgc (ms) | incWorst (ms) | incP99 (ms) | steps |
|---|---|---|---|---|---|---|---|
| tables  512MB | 832.1 | 846.5 | 1.02x | 139.35 | 1.41 | 0.003 | 408,566 |
| strings 512MB | 801.7 | 956.0 | 1.19x | 1,557.45 | 50.25 | 0.027 | 82,882 |
| deep    512MB | 701.1 | 760.4 | 1.08x | 124.04 | 1.41 | 0.003 | 370,873 |
| tables 1024MB | 1,664.2 | 1,814.1 | 1.09x | 279.91 | 105.58 | 0.003 | 817,147 |
| strings 1024MB | 1,603.7 | 1,909.9 | 1.19x | **3,601.59** | **101.20** | 0.031 | 165,745 |
| deep   1024MB | 1,402.1 | 1,518.0 | 1.08x | 250.83 | 5.04 | 0.003 | 741,727 |

(Each row is the median of the 3 runs.) **strings 1024MB fullgc = 3.6 s** with **incWorst = 101 ms** — 12× slower than the tables/deep shapes at the same live-set size, and the worst single-step pause of any bench. The `over=1.19x` for strings is the arena reserve overhead.

### 2.3 bench_gc_huge (wall: 13.46 / 13.71 / 16.19 s — median **13.71 s**)

| Case | median (s) | min | max | spread |
|---|---|---|---|---|
| H1: sweep 400 dead huge cdata | 0.0397 | 0.0339 | 0.0475 | 6–22% |
| H2: 200 fullgc, 200 live huge cdata | 0.0025 | 0.0023 | 0.0028 | 9–17% |
| H3: 800 mixed huge churn, 50% survive | 0.3591 | 0.3307 | 0.3856 | 2–7% |
| H4: build 2000 live huge, force resizes | 0.7457 | 0.6977 | 0.9411 | 3–12% |
| H5: sweep 400 dead huge strings | 0.0074 | 0.0064 | 0.0122 | 21–33% |

### 2.4 bench_gc_pod (wall: 12.69 / 13.41 / 14.00 s — median **13.41 s**)

| Case | median (s) | min | max | spread |
|---|---|---|---|---|
| P1: 20x sweep 30K dead protos | 0.4040 | 0.2733 | 0.5074 | 26–55% |
| P2: 20x sweep 50K dead closures | 0.0740 | 0.0717 | 0.1145 | 34–46% |
| P3: 200 fullgc, 40K live closures | 0.1815 | 0.1690 | 0.2519 | 9–46% |
| P4: 600K mixed proto+closure churn | 0.2855 | 0.2650 | 0.4839 | 14–43% |
| P5: alloc 1.5M closures, auto GC | 0.0958 | 0.0804 | 0.1125 | 10–21% |

P1/P4 have very high run-to-run spread (26–55%) — the POD sweep delta is a few percent and per-collect scheduling is noisy (acknowledged in the bench header).

### 2.5 test/gc/inc_pause_bench (3 runs × 3 workloads, default stepmul=200, scale=128MB)

| Workload | wall (s) | worst_ms | p99_ms | p50_ms | mean_ms | cyc_ms (one full cycle) | objects | steps |
|---|---|---|---|---|---|---|---|---|
| tables  | 0.49/0.50/0.50 | 0.602 | 0.007 | 0.001 | 0.001 | 156.46 | 2,097,152 | 102,158 |
| strings | 4.21/3.92/3.66 | 1.888 | 0.126 | 0.018 | 0.026 | **542.40** | 2,752,512 | 20,734 |
| deep    | 0.44/0.46/0.71 | 0.606 | 0.008 | 0.001 | 0.001 | 156.76 | 1,622,016 | 92,080 |

**strings cyc_ms = 542 ms** per full GC cycle vs 156 ms for tables/deep — a 3.5× per-cycle slowdown driven entirely by the string sweep path (the strings workload takes only 20,734 steps but each step is ~10× heavier: p99 = 0.126 ms vs 0.007 ms).

---

## 3. Per-Phase Time Attribution (timing build, `-DLUAJIT_ENABLE_GCSTATS_TIMING`)

**Methodology note (read before interpreting):** the timing build instruments every `gc_onestep_raw` phase entry/exit with `clock_gettime`-style reads. This adds substantial overhead — e.g. bench_gc_focused wall went from 10.6 s (default) to 41.2 s (timing). The timing probes also slow each GC step, which makes the scheduler fire **more** cycles (focused: 7,250 → 19,301 cycles). Therefore **absolute** time numbers below are inflated and not comparable to §2; **relative phase shares and maxpause values are the valid signal**. Raw outputs: `/tmp/gcbaseline/timing_*.txt`.

Throughputs: `mark_throughput = mark_cost / time_propagate_ns` (cost units per ns; `mark_cost` is the collector's internal work-cost counter, roughly proportional to marked bytes), `sweep_throughput = sweep_cells / time_sweep_bitmap_ns` (cells per ns).

### 3.1 bench_gc_focused (timing wall 41.2 s)

| Phase | time (ns) | share | maxpause (ns) |
|---|---|---|---|
| propagate | 10,101,598,062 | 37.21% | 7,422,534 |
| **sweepstring** | **15,987,085,361** | **58.89%** | 997,245 |
| atomic | 319,734,255 | 1.18% | **13,686,808** |
| sweep_bitmap | 678,520,338 | 2.50% | 10,080,736 |
| sweep_rebuild | 0 | 0.00% | 0 |
| finalize | 0 | 0.00% | 0 |
| pause | 61,383,407 | 0.23% | 546,935 |
| **total** | **27.148 s** | | |

mark_throughput = 5.345 cost/ns · sweep_throughput = 0.0334 cells/ns · cycles=19,301 · strings_chains_swept=241,213,440 (12,497 chains/cycle).

### 3.2 bench_gc_large (timing wall 76.0 s)

| Phase | time (ns) | share | maxpause (ns) |
|---|---|---|---|
| propagate | 3,475,524,691 | 9.80% | **205,116,544** |
| **sweepstring** | **29,947,080,642** | **84.46%** | 397,951 |
| atomic | 2,685,444 | 0.01% | 532,885 |
| sweep_bitmap | 2,006,532,248 | 5.66% | **275,845,900** |
| sweep_rebuild | 0 | 0.00% | 0 |
| finalize | 29,112 | 0.00% | 13,734 |
| pause | 27,008,980 | 0.08% | 2,717,175 |
| **total** | **35.459 s** | | |

mark_throughput = 6.025 cost/ns · sweep_throughput = 0.0326 cells/ns · cycles=37 · strings_chains_swept=201,199,616 (**5,437,827 chains/cycle** — string hash table is ~2²³ wide for the 1.6 GB string set) · findspace_calls/cycle=1,330 · sweep_cells/cycle=1,765,578.

### 3.3 bench_gc_huge (timing wall 12.5 s)

| Phase | time (ns) | share | maxpause (ns) |
|---|---|---|---|
| propagate | 24,188,255 | 1.04% | 224,109 |
| sweepstring | 68,226,782 | 2.93% | 245,168 |
| atomic | 6,092,893 | 0.26% | 266,583 |
| sweep_bitmap | 61,998,981 | 2.66% | 2,028,085 |
| **sweep_rebuild** | **2,163,827,379** | **93.00%** | 3,065,902 |
| finalize | 0 | 0.00% | 0 |
| pause | 2,473,096 | 0.11% | 50,751 |
| **total** | **2.327 s** | | |

mark_throughput = 4.839 cost/ns · sweep_throughput = 0.0006 cells/ns (huge objects aren't bitmap cells) · cycles=1,930 · rebuild_hugescan=39,043 (20.2 hugescan slices/cycle) · steps_sweep_rebuild=37,113.

**Caveat:** the timing probes disproportionately inflate `sweep_rebuild` (the chunked `rebuild_hugescan` dispatcher re-enters `gc_onestep_raw` once per GCSWEEPMAX-slot slice, so timing-probe overhead is paid per slice). The §3.3 default-build perf profile shows total GC is <3% of huge's wall — so under the real (no-timing) build the rebuild path is **not** a 93% cost; the 93% is an artifact of per-slice probe overhead multiplied by the 39K hugescan slices. The valid signal here is that `rebuild_hugescan` is the most probe-dense path (many short slices), not that it dominates real runtime.

### 3.4 bench_gc_pod (timing wall 17.9 s)

| Phase | time (ns) | share | maxpause (ns) |
|---|---|---|---|
| **propagate** | **4,240,265,868** | **56.59%** | 2,174,158 |
| sweepstring | 1,453,419,208 | 19.40% | 308,770 |
| atomic | 142,577,273 | 1.90% | 1,343,790 |
| sweep_bitmap | 1,576,649,297 | 21.04% | 3,163,526 |
| sweep_rebuild | 0 | 0.00% | 0 |
| finalize | 0 | 0.00% | 0 |
| pause | 80,303,009 | 1.07% | 122,742 |
| **total** | **7.493 s** | | |

mark_throughput = 4.660 cost/ns · sweep_throughput = 0.0298 cells/ns · cycles=34,332 · **findspace_calls=9,905,762 (288.5/cycle)** · sweep_cells/cycle=1,369.5 · strings_chains_swept/cycle=1,815.

### 3.5 test/gc/inc_pause_bench (timing build, per workload)

| Workload | total GC (s) | propagate | sweepstring | sweep_bitmap | atomic | maxpause (worst, ns) |
|---|---|---|---|---|---|---|
| strings | 1.357 | 1.09% | **93.64%** | 5.23% | 0.00% | propagate 14,702,578 · sweep_bitmap 43,157,602 |
| tables  | 0.132 | 56.87% | 0.06% | 41.91% | 0.06% | propagate 22,058,610 · sweep_bitmap 26,588,955 |
| deep    | 0.102 | 49.68% | 0.08% | 49.82% | 0.07% | sweep_bitmap 20,064,890 |

`incpause_strings` confirms §2.5: the strings workload spends **93.6%** of GC time in `sweepstring`, and a single `sweep_bitmap` step paused for **43 ms** (the bitmap sweep runs after sweepstring finishes all chains, so this is the post-sweepstring bitmap pass on the surviving cells).

---

## 4. Perf Hotspots (default build + `-g`, dwarf call-graph)

`perf record -g --call-graph dwarf` on the **default (no-timing)** build so timing probes don't pollute the profile. Raw: `/tmp/gcbaseline/perf_*.data`, reports: `/tmp/gcbaseline/perf_*_report.txt`.

### 4.1 bench_gc_focused (63,045 samples, 99.7% in luajit symbols)

| Rank | Symbol | self % | kind |
|---|---|---|---|
| 1 | `gc_sweepstr` | **27.73%** | GC — string sweep |
| 2 | `propagatemark` | **25.70%** | GC — mark propagation |
| 3 | `lj_gc_fullgc` | 13.67% | GC — full-cycle driver |
| 4 | `gc_onestep_raw` | 8.76% | GC — step dispatcher |
| 5 | `gc_mark` | 8.76% | GC — mark |
| 6 | `gc_grayarena_pop` | 2.76% | GC — gray arena heap pop |
| 7 | `gc_onestep` | 2.22% | GC — step wrapper |
| 8 | `lj_alloc_free` | 1.07% | allocator |
| 9 | `hash_sparse` | 0.61% | string table |
| 10 | `arena_scavenge` | 0.43% | arena |

**GC-function self % sum ≈ 91%.** `gc_sweepstr` alone is 27.73% — the single biggest hotspot, consistent with the 224M `strings_chains_swept` counter. The `propagatemark` 25.70% splits mainly into `gc_traverse_tab` (8.99%, inlined) and `propagatemark` self-recursive (8.86%).

### 4.2 bench_gc_large (221,971 samples, 98.4% in luajit symbols)

| Rank | Symbol | self % | kind |
|---|---|---|---|
| 1 | `gc_sweepstr` | **26.67%** | GC — string sweep |
| 2 | `lj_str_resize` | 17.52% | string table grow/shrink |
| 3 | `lj_str_new` | 10.76% | string intern |
| 4 | `nd_mul2k` | 9.94% | number→string (mutator) |
| 5 | `gc_onestep_raw` | 7.57% | GC — step dispatcher |
| 6 | `lj_gc_fullgc` | 5.91% | GC — full-cycle driver |
| 7 | `lj_strfmt_wfnum` | 4.78% | number formatting (mutator) |
| 8 | `propagatemark` | 2.00% | GC — mark |
| 9 | `lj_str_free` | 1.88% | string free |
| 10 | `gc_mark` | 0.99% | GC — mark |

GC-related self % sum ≈ 45%; the rest is the string-building mutator (`lj_str_new`, `lj_str_resize`, `nd_mul2k`, `lj_strfmt_wfnum`). Even so, **`gc_sweepstr` is the #1 single symbol** at 26.67% — larger than any mutator function. `lj_str_resize` at 17.52% is suspicious (see F3): the string table is being resized repeatedly.

### 4.3 bench_gc_huge (47,595 samples; 68.7% luajit, 31.1% libc)

| Rank | Symbol | self % | kind |
|---|---|---|---|
| 1 | `__memset_avx512_unaligned_erms` (libc) | **20.27%** | huge-block zeroing |
| 2–13 | `[unknown] [k] 0xffffffff...` | ~62% combined | kernel: page-fault / mmap handling |
| 14 | `gc_onestep_raw` | 1.28% | GC |
| 15 | `propagatemark` | 0.93% | GC |
| 16 | `lj_gc_fullgc` | 0.34% | GC |
| 17 | `gc_sweepstr` | 0.27% | GC |
| 18 | `lj_gc_separateudata` | 0.21% | GC — udata finalize separate |
| 19 | `lj_hugeblock_free` | 0.17% | GC — huge free |
| 20 | `huge_obj_ismarked` | 0.15% | GC — huge mark check |

**GC-function self % sum ≈ 3.4%.** The huge workload is **not GC-bound** — it is dominated by libc `memset`/`memcmp`/`memmove` (31%) and kernel page-fault handling for the 512 KB–16 MB mmap'd huge blocks (~62% in `[unknown] [k]` frames). This is the OS/allocation cost, not the collector. The arena GC's huge path (`lj_hugeblock_free`, `huge_obj_ismarked`, `lj_arena_gc_markinit`, `hugeset_resize`) is collectively <0.5%. **Conclusion: huge-block GC is efficient; the huge-bench bottleneck is allocation/zeroing, which is out of scope for the GC.**

---

## 5. Findings — Ranked Suspected Performance Problems

### F1. `gc_sweepstr` walks every string hash chain every cycle — dominant GC cost
**Evidence:**
- `gc_sweepstr` is the #1 self-hotspot in both GC-heavy benches: **27.73%** (focused) and **26.67%** (large). It is larger than `propagatemark` despite mark doing more "real" work (196M mark calls vs the sweepstring counter).
- `strings_chains_swept`: **224,729,088** (focused, default) / **201,199,616** (large, timing). At 30,997 chains/cycle (focused) the string hash table is ~2¹⁵ wide; most chains are empty but `gc_sweepstr` is still called once per chain per cycle.
- Timing attribution: `sweepstring` is **58.89%** of GC time (focused) and **84.46%** (large). For `incpause_strings` it is **93.64%**.
- `strings 1024MB fullgc = 3.6 s` vs `tables 1024MB = 0.28 s` and `deep = 0.25 s` — a 12–14× penalty that scales with hash-table width, not with live-string count.

**Source location:** `src/lj_gc_arena.c:1044` (`gc_sweepstr`) driven from `src/lj_gc_arena.c:1988` — `case GCSsweepstring: gc_sweepstr(g, &g->str.tab[g->gc.sweepstr++]);` sweeps **one chain per `gc_onestep_raw` call**, looping over `g->str.mask + 1` chains (the full hash table) every cycle.

**Hypothesis:** the sweepstring phase pays a fixed O(hash-table-width) per-cycle cost regardless of how many strings are actually live or dead. For workloads with a wide string table but sparse occupancy (the common case after the table grows then shrinks), the vast majority of `gc_sweepstr` calls iterate zero or one entry. Batching multiple chains per onestep, or skipping empty chains via a free-list/occupancy bitmap, would cut both the 27% self-time and the 224M call count substantially. **Top suspect.**

### F2. `atomic` pause scales with live-set size — worst single-step stop-the-world
**Evidence:**
- `maxpause_atomic_ns`: **13.69 ms** (focused), and under the timing build `large` showed `maxpause_propagate_ns = 205 ms` and `maxpause_sweep_bitmap_ns = 276 ms` — but those are timing-inflated. The bench's own (default-build, untimed) `incWorst` is the trustworthy pause figure: **101 ms** for `strings 1024MB`, **105 ms** for `tables 1024MB`, **57 ms** for `strings 512MB`.
- `atomic` itself is indivisible (the whole point of the pause), and `time_atomic_ns` is small in share (1.18% focused, 0.01% large) — but its **maxpause** is the largest single-chunk pause in focused (13.69 ms > propagate 7.4 ms > sweep_bitmap 10.1 ms).
- The 100 ms+ `incWorst` for 1024MB workloads is the real product risk: this is the pause a latency-sensitive mutator feels.

**Source location:** `src/lj_gc_arena.c:1834` (`atomic`) — runs `gc_mark_uv`, `gc_propagate_gray`, `gc_weak_redirect_all`, `gc_traverse_mainthread`, `gc_traverse_curtrace`, `gc_mark_gcroot`, `lj_gc_ssb_flush`, the graythread drain, `gc_propagate_gray` again, and `lj_gc_separateudata` all without yielding.

**Hypothesis:** `atomic` does all gray draining + udata separation in one non-yielding slice; for large heaps the second `gc_propagate_gray` and `lj_gc_separateudata` walk O(live-udata) and O(residual-gray) with no incremental chunking. The 100 ms pauses at 1 GB suggest `separateudata` or the second propagate is the long pole. Candidate: chunk the udata separation or pre-drain more gray before atomic.

### F3. `lj_str_resize` at 17.52% of `bench_gc_large` — repeated string-table rehashing
**Evidence:**
- `perf` (large, default build): `lj_str_resize` is the **#2 hotspot at 17.52% self**, behind only `gc_sweepstr`. It is more expensive than the actual string allocator (`lj_str_new` 10.76%).
- `bench_gc_large` builds strings up to 1.6 GB live; the string hash table grows through several powers of two and then `gc_onestep_raw` shrinks it again at sweep end (`src/lj_gc_arena.c:2011–2012` and `:2032–2033`: `if (g->str.num <= (g->str.mask >> 2) ... lj_str_resize(L, g->str.mask >> 1)`).
- The `over=1.19x` rss overhead for the strings workload (vs 1.02–1.09x for tables/deep) is consistent with a bloated hash table.

**Source location:** `src/lj_gc_arena.c:2011` (shrink-at-sweep-end) and `src/lj_gc_arena.c:2032` (shrink after fullgc); the grow path is in `lj_str.c` (`lj_str_new` → `lj_str_resize` when load factor exceeds threshold).

**Hypothesis:** the large-strings workload oscillates the string table between wide (during fill) and half-wide (after sweep shrinks it 2×), paying repeated O(width) rehash + re-link cost each cycle. With ~165K GC steps and 37 cycles in the timing build, each cycle both grows and shrinks the table. A wider hysteresis band (shrink only when occupancy drops below 1/8, not 1/4) or growing in larger strides would cut rehash churn.

### F4. `lj_arena_findspace` O(arenastop) scan per freelist miss — high in POD churn
**Evidence:**
- `bench_gc_pod` (timing): `findspace_calls = 9,905,762` (288.5/cycle), with `arenastop` reaching 365 arenas. Each `lj_arena_findspace` call iterates `for (i = 0; i < g->gc.arenastop; i++)` → up to 365 arena-pointer dereferences + flag checks per call. Worst case ≈ 9.9M × 365 ≈ 3.6B arena iterations.
- POD's `time_propagate` share (56.59%) is much higher than focused's (37.21%) despite POD doing less marking work per object — consistent with extra per-allocation overhead on the alloc path (POD allocates 1.5M closures + 600K mixed churn).
- `perf` did not cover `bench_gc_pod` (task scope: focused/large/huge only), so the self-% is not directly measured; this finding is from the **counter** evidence + source inspection. (perf for pod is a recommended follow-up.)

**Source location:** `src/lj_arena.c:613` (`lj_arena_findspace`) — the `for (i = 0; i < g->gc.arenastop; i++)` loop scans all arenas linearly on every freelist miss, with no per-class arena index.

**Hypothesis:** with hundreds of arenas and per-class free-list misses occurring ~288×/cycle, the linear scan is a hidden allocation tax. A per-class free-list (or a per-class arena vector indexed separately from the full `arenas[]` array) would turn this into O(arenas-of-class). **Suspect #4 — counter-evidence only, perf follow-up recommended.**

### F5. `gc_grayarena_pop` / gray-arena heap maintenance — 2.76% in focused
**Evidence:**
- `perf` (focused): `gc_grayarena_pop` = **2.76% self**, ranking #6 — notable for a "pop" primitive. `grayarena_pops = 200,072,003` in the timing build (196M in default), i.e. ~one pop per mark call.
- `propagatemark` (25.70%) calls `gc_traverse_*` which pushes children onto per-arena gray stacks; the next mark pops from the highest-priority arena via a sift-down heap (`grayheap_siftdown`, `src/lj_gc_arena.c:730`).
- The heap is maintained per-pop with `grayarena_prio` re-reading `greytop - greybase` for both children (lines 738, 740) — two pointer subtractions + dereferences per pop.

**Source location:** `src/lj_gc_arena.c:748` (`gc_grayarena_pop`) using `grayheap_siftdown` (line 730) and `grayarena_prio` (line 706).

**Hypothesis:** a binary heap of arenas with per-pop sift-down is reasonable for tens of arenas but pays a constant ~2× child-priority recomputation per pop; with 200M pops this is 5–10% of mark-path overhead. A cheaper priority scheme (e.g. a tiered bucket queue keyed by gray-stack depth buckets, or caching prio in the heap slot) would cut this. **Suspect #5 — smaller than F1–F3 but a clean 2.76% with a clear source.**

---

## 6. Reproduction Commands

```sh
cd /home/fesily/luajit/.claude/worktrees/arenagc

# Default build (baseline + perf)
make -C src clean && make -C src -j8 BUILDMODE=static XCFLAGS="-DLUAJIT_ENABLE_GCARENA"
# For perf symbols, add CCDEBUG=-g:
make -C src clean && make -C src -j8 BUILDMODE=static CCDEBUG=-g XCFLAGS="-DLUAJIT_ENABLE_GCARENA"

# Timing build (per-phase attribution)
make -C src clean && make -C src -j8 BUILDMODE=static XCFLAGS="-DLUAJIT_ENABLE_GCARENA -DLUAJIT_ENABLE_GCSTATS_TIMING"

# Verify build flags
./src/luajit -e 'print(collectgarbage("stats").timing)'   # false for default/perf, true for timing

# T1 baseline (3 runs each, sequential)
/usr/bin/time -v ./src/luajit -joff test/bench_gc_focused.lua
/usr/bin/time -v ./src/luajit -joff test/bench_gc_large.lua
/usr/bin/time -v ./src/luajit -joff test/bench_gc_huge.lua
/usr/bin/time -v ./src/luajit -joff test/bench_gc_pod.lua
/usr/bin/time -v ./src/luajit -joff test/gc/inc_pause_bench.lua tables
/usr/bin/time -v ./src/luajit -joff test/gc/inc_pause_bench.lua strings
/usr/bin/time -v ./src/luajit -joff test/gc/inc_pause_bench.lua deep

# T2 timing attribution (uses the wrapper at /tmp/gcbaseline/wrap.lua)
./src/luajit -joff /tmp/gcbaseline/wrap.lua test/bench_gc_focused.lua
./src/luajit -joff /tmp/gcbaseline/wrap.lua test/bench_gc_large.lua
./src/luajit -joff /tmp/gcbaseline/wrap.lua test/bench_gc_huge.lua
./src/luajit -joff /tmp/gcbaseline/wrap.lua test/bench_gc_pod.lua
./src/luajit -joff /tmp/gcbaseline/wrap.lua test/gc/inc_pause_bench.lua strings
./src/luajit -joff /tmp/gcbaseline/wrap.lua test/gc/inc_pause_bench.lua tables
./src/luajit -joff /tmp/gcbaseline/wrap.lua test/gc/inc_pause_bench.lua deep

# T3 perf profile (default build + -g)
perf record -g --call-graph dwarf -o /tmp/gcbaseline/perf_focused.data ./src/luajit -joff test/bench_gc_focused.lua
perf record -g --call-graph dwarf -o /tmp/gcbaseline/perf_large.data   ./src/luajit -joff test/bench_gc_large.lua
perf record -g --call-graph dwarf -o /tmp/gcbaseline/perf_huge.data    ./src/luajit -joff test/bench_gc_huge.lua
perf report -i /tmp/gcbaseline/perf_focused.data --stdio --no-children | head -60
perf report -i /tmp/gcbaseline/perf_focused.data --stdio 2>/dev/null | grep -iE "lj_gc|lj_arena|gc_|arena_|propagate|sweep|atomic|barrier" | head -40
```

### Raw data location
All raw bench outputs, timing-build stats dumps, perf.data files, perf reports, and build logs are under **`/tmp/gcbaseline/`**:
- `focused_run{1,2,3}.txt`, `large_run{1,2,3}.txt`, `huge_run{1,2,3}.txt`, `pod_run{1,2,3}.txt`, `incpause_{tables,strings,deep}_run{1,2,3}.txt` — default-build bench output + `/usr/bin/time -v` footer.
- `timing_{focused,large,huge,pod,incpause_tables,incpause_strings,incpause_deep}.txt` — timing-build wrapper output with the full sorted stats dump.
- `perf_{focused,large,huge}.data` + `perf_{focused,large,huge}_report.txt` — perf record data + `perf report --stdio --no-children` text.
- `build_*.txt` — build logs for the three configurations.
- `wrap.lua` — the statsreset/dofile/print-stats wrapper used for T2.

### Deviations from the task spec
- **bench_gc_large under the timing build took 76 s** (1:16 wall). Handled by running it alone with a 600 s timeout; completed successfully.
- **`bench_gc_large` arg plumbing through the wrapper**: the original `wrap.lua` passed the bench path as `arg[1]`, which broke `bench_gc_large`'s own `arg[1]` parsing (it expected `512|1024|both`). The wrapper was fixed to strip the bench path from `arg` before `dofile` so the bench sees only its own args. This is a harness fix in `/tmp/gcbaseline/wrap.lua`, not a source change.
- **Perf for `bench_gc_pod` was not captured** (task scope limited perf to focused/large/huge). F4 (findspace) is therefore counter-evidence + source-inspection only; a pod perf run is the recommended follow-up.
- **`perf record` on huge emitted "Check IO/CPU overload!"** warnings (dwarf call-graph on 47K–222K samples is heavy) but completed successfully with valid samples; no fallback to `--call-graph fp` was needed.
- The **known pre-existing `lj_gc_barrierback_arena` shared-lib link bug** was respected — all builds used `BUILDMODE=static` and the bug was not chased.
- The uncommitted GC stats changes were left untouched; no git operations were performed.

---

*End of report. Measurement only — all findings are hypotheses for later optimization work, not applied fixes.*

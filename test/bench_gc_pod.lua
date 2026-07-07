-- POD-arena GC benchmark: isolates the proto/closure sweep path that the
-- word-parallel sweep optimizes (lj_arena_podsweep). Protos and closures are
-- the only object types routed into ArenaFlag_PODOnly arenas, where the sweep
-- frees dead objects and recolors survivors with a pure bitmap word transform
-- (block'=block&mark; mark'=block^mark) instead of a per-object free loop.
--
-- The existing bench_gc_focused.lua covers tables/strings/cdata but never
-- allocates protos or closures, so it does not exercise the POD arena at all.
--
-- Run: luajit -joff test/bench_gc_pod.lua
--   -joff matters: with the JIT on, hot loops compile to traces (GCtrace, NOT
--   a POD type) and the interpreted proto/closure churn shrinks, muddying the
--   sweep signal. Keep the JIT off to measure the GC paths directly.
--
-- Design notes (learned the hard way):
--   * Keep each benchmark to ONE object type and ONE GC phase. Mixing a table
--     constant into a "proto" workload makes every full GC also mark tables,
--     burying the sweep signal under unrelated work.
--   * The POD sweep delta is a few percent and per-collect scheduling is
--     noisy, so favour batched, steady-state shapes over a single timed
--     collectgarbage(); report the run-to-run spread so noisy rows are visible.
--
-- Representative result (x86-64, -joff, 4 aggregated runs, median of medians),
-- comparing v2.1 mainline default GC against the final arena/POD word-sweep
-- build. Negative = arena/POD faster:
--   P1 20x sweep 30K dead protos      -44%  (run-to-run noise ~33%: noisy)
--   P2 20x sweep 50K dead closures    -43%  (noise ~25%: noisy but consistent)
--   P3 200 fullgc, 40K live closures  +43%  (noise ~4%: arena/POD slower)
--   P4 600K mixed proto+closure churn +76%  (noise ~6%: arena/POD slower)
--   P5 alloc 1.5M closures, auto GC   +30%  (noise ~66%: inconclusive)
-- Takeaway: relative to v2.1 mainline, the POD word-sweep does make the pure
-- sweep phase faster, but the arena/bitmap collector is still much slower on
-- live-marking and mixed mutator+GC workloads. This benchmark is therefore a
-- phase diagnostic, not an end-to-end claim of superiority.

local clock = os.clock
local RUNS = 11

local function stats(t)
  local s = {}
  for i = 1, #t do s[i] = t[i] end
  table.sort(s)
  local n = #s
  local med = (n % 2 == 1) and s[(n + 1) / 2] or (s[n / 2] + s[n / 2 + 1]) / 2
  return med, s[1], s[n]
end

local function bench(name, fn)
  collectgarbage("collect"); collectgarbage("collect"); fn()  -- warm-up
  local times = {}
  for r = 1, RUNS do
    collectgarbage("collect")
    collectgarbage("collect")
    times[r] = fn()
  end
  local med, lo, hi = stats(times)
  local spread = med > 0 and (hi - lo) / med * 100 or 0
  io.write(string.format("%-42s  median=%.4fs  min=%.4fs  max=%.4fs  spread=%.0f%%\n",
    name, med, lo, hi, spread))
end

-- Distinct chunk source -> a fresh GCproto per load(). Kept deliberately simple
-- and POD-only: no table constant (would add GCtab marking), no nested closure
-- (keeps it one proto + the outer function proto). The returned value is a
-- number, so loading never allocates a survivor table.
local function protochunk(i)
  return "return " .. i .. " + 1"
end

local function maker(n)  -- one fresh closure capturing one upvalue per call
  return function(x) return x + n end
end

io.write("=== POD-arena GC Benchmark (protos + closures) ===\n\n")

-- P1: proto sweep, batched. Repeatedly fill a buffer with N dead protos and
-- collect, so the timed region is many sweep phases back to back (amortizing
-- the per-collect scheduling noise of a single timed collect).
bench("P1: 20x sweep 30K dead protos", function()
  local t0 = clock()
  for batch = 1, 20 do
    local hold = {}
    for i = 1, 30000 do hold[i] = load(protochunk(i)) end
    hold = nil
    collectgarbage("collect")   -- sweep the 30K dead protos
  end
  return clock() - t0
end)

-- P2: closure sweep, batched (same shape, closures instead of protos).
bench("P2: 20x sweep 50K dead closures", function()
  local t0 = clock()
  for batch = 1, 20 do
    local hold = {}
    for i = 1, 50000 do hold[i] = maker(i) end
    hold = nil
    collectgarbage("collect")
  end
  return clock() - t0
end)

-- P3: closure mark/recolor with a pure-POD live set. N live closures, M full
-- GCs. Each cycle marks the survivors and the POD sweep recolors them
-- black->white via the word transform (vs the per-object makewhite pass).
bench("P3: 200 fullgc, 40K live closures", function()
  local live = {}
  for i = 1, 40000 do live[i] = maker(i) end
  collectgarbage("collect")
  local t0 = clock()
  for c = 1, 200 do collectgarbage("collect") end
  local dt = clock() - t0
  live = nil
  return dt
end)

-- P4: steady-state mixed proto+closure churn driven by incremental steps
-- (mutator + collector overlap, the common real-world shape). A fraction
-- survives each pass to keep the arenas non-trivially populated.
bench("P4: 600K mixed proto+closure churn", function()
  local live = {}
  collectgarbage("restart")
  local t0 = clock()
  for i = 1, 600000 do
    if i % 2 == 0 then
      local c = maker(i)
      if i % 64 == 0 then live[#live + 1] = c end
    else
      local f = load(protochunk(i))
      if i % 128 == 0 then live[#live + 1] = f end
    end
    if i % 50000 == 0 then collectgarbage("collect") end
  end
  local dt = clock() - t0
  collectgarbage("stop")
  return dt
end)

-- P5: closure allocation throughput with the collector running (auto-GC).
-- Sensitive to the cell-space accounting branch added on the POD alloc path.
bench("P5: alloc 1.5M closures, auto GC", function()
  collectgarbage("restart")
  local t0 = clock()
  for i = 1, 1500000 do local _ = maker(i) end
  local dt = clock() - t0
  collectgarbage("stop")
  return dt
end)

io.write("\nDone.\n")

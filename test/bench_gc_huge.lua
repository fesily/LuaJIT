-- Huge-object GC benchmark: exercises the hugeset path that bench_gc_focused,
-- bench_gc_compare and bench_gc_pod never touch. Any GC object >= 512KB
-- (ArenaHugeThreshold = ArenaSize>>1) is routed to lj_hugeblock_alloc and lives
-- in the open-addressed hugeset registry, NOT in a bitmap arena. Its GC color
-- and the rebuild restart-tag live in the hugeset slot word (HUGESET_MARK /
-- HUGESET_SWEPT), so this is the only benchmark that measures:
--   * huge sweep (freeing dead huge blocks + tombstoning their slots)
--   * huge mark + the per-cycle lj_arena_gc_markinit pass that clears
--     HUGESET_MARK|HUGESET_SWEPT across the whole registry
--   * mixed live/dead rebuild, where survivors carry HUGESET_SWEPT
--   * hugeset_resize when the registry outgrows its load factor
--
-- Run: luajit -joff test/bench_gc_huge.lua
--   -joff keeps the mutator interpreted so the timed signal is GC work, not
--   trace compilation. Wall-clock (clock_gettime MONOTONIC) is used instead of
--   os.clock so GC pauses -- a wall-clock phenomenon -- are measured correctly.

local ffi = require("ffi")
ffi.cdef[[
typedef struct timespec { long tv_sec; long tv_nsec; } timespec;
int clock_gettime(int clk_id, struct timespec *tp);
]]
local CLOCK_MONOTONIC = 1
local ts = ffi.new("timespec[1]")
local function now()
  ffi.C.clock_gettime(CLOCK_MONOTONIC, ts)
  return tonumber(ts[0].tv_sec) + tonumber(ts[0].tv_nsec) * 1e-9
end

-- 600KB > 512KB threshold: safely huge, minimal memory waste. A char[?] cdata
-- of this size is a single huge block; string.rep of this length interns one
-- huge GCstr.
local HUGE_BYTES = 600 * 1024
local RUNS = 7

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
  io.write(string.format("%-46s  median=%.4fs  min=%.4fs  max=%.4fs  spread=%.0f%%\n",
    name, med, lo, hi, spread))
end

local function new_huge_cdata(i)
  local b = ffi.new("char[?]", HUGE_BYTES)
  b[0] = i % 128            -- touch a byte so the alloc is not optimized away
  return b
end

local function new_huge_string(i)
  -- Distinct length per call defeats interning so each is a fresh huge GCstr.
  return string.rep("x", HUGE_BYTES + (i % 64))
end

io.write("=== Huge-object GC Benchmark (>512KB hugeset path) ===\n\n")

-- H1: pure huge sweep. Fill the hugeset with N dead huge blocks, then one
-- collect frees them all and tombstones their slots. Isolates the sweep +
-- slot-tombstone path.
bench("H1: sweep 400 dead huge cdata", function()
  local hold = {}
  for i = 1, 400 do hold[i] = new_huge_cdata(i) end
  hold = nil
  local t0 = now()
  collectgarbage("collect")
  return now() - t0
end)

-- H2: huge mark + per-cycle markinit clear. N live huge blocks, M full GCs.
-- Every cycle marks all survivors (setting HUGESET_MARK) and the next
-- markinit clears HUGESET_MARK|HUGESET_SWEPT across the whole registry --
-- the pass that absorbed the old Rebuild_HugeClear sub-phase.
bench("H2: 200 fullgc, 200 live huge cdata", function()
  local live = {}
  for i = 1, 200 do live[i] = new_huge_cdata(i) end
  collectgarbage("collect")
  local t0 = now()
  for c = 1, 200 do collectgarbage("collect") end
  local dt = now() - t0
  live = nil
  return dt
end)

-- H3: mixed live/dead huge churn (50% survive), incremental steps interleaved
-- so the collector and mutator overlap. This is the shape that drives a rebuild
-- with survivors, where each survivor's slot carries HUGESET_SWEPT across the
-- rebuild -- the exact tag migrated from the header LJ_GC_BLACK bit.
bench("H3: 800 mixed huge churn, 50% survive", function()
  local live = {}
  collectgarbage("restart")
  local t0 = now()
  for i = 1, 800 do
    if i % 2 == 0 then live[i / 2] = new_huge_cdata(i)
    else local _ = new_huge_cdata(i) end
    if i % 100 == 0 then collectgarbage("collect") end
  end
  local dt = now() - t0
  collectgarbage("stop")
  live = nil
  return dt
end)

-- H4: hugeset resize stress. Grow a live set large enough to cross the load
-- factor several times, forcing hugeset_resize to rehash every live slot
-- (which must carry the full slot word, including HUGESET_SWEPT, intact).
bench("H4: build 2000 live huge, force resizes", function()
  local t0 = now()
  local live = {}
  for i = 1, 2000 do live[i] = new_huge_cdata(i) end
  collectgarbage("collect")
  local dt = now() - t0
  live = nil
  return dt
end)

-- H5: huge string sweep. Same as H1 but GCstr instead of cdata, so the huge
-- path is exercised for the string type (separate hugeset entry kind).
bench("H5: sweep 400 dead huge strings", function()
  local hold = {}
  for i = 1, 400 do hold[i] = new_huge_string(i) end
  hold = nil
  local t0 = now()
  collectgarbage("collect")
  return now() - t0
end)

io.write("\nDone.\n")

-- Focused GC comparison benchmark: pure GC timings only
-- Run: luajit -joff test/bench_gc_focused.lua
--
-- Cross-collector caveat: B5 and B10 drive the collector with
-- collectgarbage("step", N). The N argument is a scheduler credit, not a fixed
-- work unit -- the arena bitmap GC and the classic tri-color GC convert the
-- same N into different amounts of actual marking/sweeping. Treat B5/B10 as
-- within-build trend rows, NOT as apples-to-apples arena-vs-classic numbers;
-- the full-cycle rows (B1/B2/B3/B7/B11) are the fair cross-collector compare.
local ffi = require("ffi")
ffi.cdef[[
typedef struct timespec { long tv_sec; long tv_nsec; } timespec;
int clock_gettime(int clk_id, struct timespec *tp);
typedef struct { int x; int y; int z; } Vec3f;
]]

-- Wall-clock (clock_gettime MONOTONIC), not os.clock: GC pauses are a
-- wall-clock phenomenon and os.clock's process-CPU time (~2us granularity,
-- excludes blocking) both under-reports pauses and floors small workloads to
-- 0.0000s. clock() below is this monotonic reader, so each bench body's
-- clock()-t0 measures real elapsed time.
local CLOCK_MONOTONIC = 1
local ts = ffi.new("timespec[1]")
local function clock()
  ffi.C.clock_gettime(CLOCK_MONOTONIC, ts)
  return tonumber(ts[0].tv_sec) + tonumber(ts[0].tv_nsec) * 1e-9
end

local RUNS = 7

local function median(t)
  local s = {}
  for i = 1, #t do s[i] = t[i] end
  table.sort(s)
  local n = #s
  return (n % 2 == 1) and s[(n + 1) / 2] or (s[n / 2] + s[n / 2 + 1]) / 2
end

local function bench(name, fn)
  collectgarbage("collect"); collectgarbage("collect"); fn()  -- warm-up
  local times = {}
  for r = 1, RUNS do
    collectgarbage("collect")
    collectgarbage("collect")
    times[r] = fn()
  end
  local med = median(times)
  io.write(string.format("%-45s  median=%.4fs  min=%.4fs  max=%.4fs\n",
    name, med, math.min(unpack(times)), math.max(unpack(times))))
end

io.write("=== Focused GC Benchmark ===\n\n")

-- 1. Mark: 200 full GC over 50K live tables
bench("B1: 200 fullgc, 50K live tables", function()
  local live = {}
  for i = 1, 50000 do live[i] = { name = "obj" .. i, data = i } end
  collectgarbage("collect")
  local t0 = clock()
  for c = 1, 200 do collectgarbage("collect") end
  local dt = clock() - t0
  live = nil
  return dt
end)

-- 2. Sweep: collect 100K dead tables
bench("B2: sweep 100K dead tables", function()
  local objs = {}
  for i = 1, 100000 do objs[i] = { val = i } end
  collectgarbage("collect")
  objs = nil
  local t0 = clock()
  collectgarbage("collect")
  return clock() - t0
end)

-- 3. Deep chain mark
bench("B3: 100 fullgc, 10K deep chain", function()
  local root = {}
  local cur = root
  for i = 1, 10000 do cur.next = { val = i }; cur = cur.next end
  collectgarbage("collect")
  local t0 = clock()
  for c = 1, 100 do collectgarbage("collect") end
  local dt = clock() - t0
  root = nil
  return dt
end)

-- 4. Alloc throughput with auto GC
bench("B4: alloc 1M tables, auto GC", function()
  collectgarbage("restart")
  local t0 = clock()
  for i = 1, 1000000 do local _ = { key = i } end
  local dt = clock() - t0
  collectgarbage("stop")
  return dt
end)

-- 5. Barrier stress
bench("B5: 500K barrier writes", function()
  local tabs = {}
  for i = 1, 1000 do tabs[i] = {} end
  collectgarbage("collect")
  collectgarbage("restart")
  local t0 = clock()
  for round = 1, 500 do
    for i = 1, 1000 do tabs[i][round] = { val = round } end
    collectgarbage("step", 5)
  end
  local dt = clock() - t0
  collectgarbage("stop")
  return dt
end)

-- 6. String churn
bench("B6: 500K unique strings + collect", function()
  collectgarbage("restart")
  local t0 = clock()
  for i = 1, 500000 do
    local _ = "str_" .. i
    if i % 50000 == 0 then collectgarbage("collect") end
  end
  local dt = clock() - t0
  collectgarbage("stop")
  return dt
end)

-- 7. Mixed live/dead
bench("B7: 500K mixed (50%% survive)", function()
  local live = {}
  collectgarbage("restart")
  local t0 = clock()
  for i = 1, 500000 do
    if i % 2 == 0 then live[i / 2] = { val = i }
    else local _ = { val = i } end
    if i % 50000 == 0 then collectgarbage("collect") end
  end
  local dt = clock() - t0
  collectgarbage("stop")
  return dt
end)

-- 8. Weak table clearing
bench("B8: collect 100K weak entries", function()
  local wt = setmetatable({}, { __mode = "v" })
  for i = 1, 100000 do wt[i] = { val = i } end
  local t0 = clock()
  collectgarbage("collect")
  return clock() - t0
end)

-- 9. FFI cdata churn
bench("B9: 200K cdata alloc + collect", function()
  collectgarbage("restart")
  local t0 = clock()
  for i = 1, 200000 do
    local _ = ffi.new("Vec3f", { x = i, y = i, z = i })
    if i % 20000 == 0 then collectgarbage("collect") end
  end
  local dt = clock() - t0
  collectgarbage("stop")
  return dt
end)

-- 10. Incremental step latency
bench("B10: 10K incremental steps, 20K live", function()
  local live = {}
  for i = 1, 20000 do live[i] = { name = "obj" .. i } end
  collectgarbage("collect")
  collectgarbage("restart")
  local t0 = clock()
  for s = 1, 10000 do collectgarbage("step", 1) end
  local dt = clock() - t0
  collectgarbage("stop")
  return dt
end)

-- 11. Large table with many keys (hash part stress)
bench("B11: fullgc on 10 tables × 10K keys", function()
  local tabs = {}
  for t = 1, 10 do
    local tb = {}
    for i = 1, 10000 do tb["key_" .. t .. "_" .. i] = { val = i } end
    tabs[t] = tb
  end
  collectgarbage("collect")
  local t0 = clock()
  for c = 1, 100 do collectgarbage("collect") end
  local dt = clock() - t0
  tabs = nil
  return dt
end)

io.write("\nDone.\n")

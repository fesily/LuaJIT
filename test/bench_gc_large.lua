-- Large-heap GC benchmark: 512MB / 1GB live sets. The other benchmarks top out
-- around a few hundred MB, so they never expose how the collector behaves at a
-- large resident footprint -- which is exactly where the arena bitmap GC and the
-- classic tri-color GC diverge most (arena reserves whole 1MB arenas and frees
-- at arena granularity, so its real OS footprint can far exceed the Lua-tracked
-- byte count; the classic per-object collector tracks the two closely).
--
-- Headline metric is rss_mb (actual resident memory from /proc/self/statm), not
-- lua_mb (collectgarbage("count"), which counts only Lua-tracked bytes and can
-- hide arena reserve overhead). The gap between the two columns IS the arena
-- overhead this benchmark exists to measure.
--
-- Run: luajit -joff test/bench_gc_large.lua [512|1024|both] [tables|strings|deep|all]

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

-- Resident set size in MB, read from the OS (field 2 of statm = resident pages).
local PAGE_MB = 4096 / 1048576
local function rss_mb()
  local f = io.open("/proc/self/statm")
  local s = f:read("*a")
  f:close()
  return tonumber(s:match("%d+%s+(%d+)")) * PAGE_MB
end

-- Per-MB object counts calibrated so count*scale_mb yields ~scale_mb of
-- classic-tracked live bytes (shared with test/gc/inc_pause_bench.lua).
local function build_tables(mb)
  local n = mb * 16384
  local keep = {}
  for i = 1, n do keep[i] = { i, i, i } end
  return keep
end
local function build_strings(mb)
  local n = mb * 21504
  local keep = {}
  for i = 1, n do keep[i] = "large_heap_string_" .. i end
  return keep
end
local function build_deep(mb)
  local n = mb * 12672
  local keep = nil
  for i = 1, n do keep = { next = keep, v = i } end
  return keep
end
local builders = { tables = build_tables, strings = build_strings, deep = build_deep }

local function percentile(sorted, pct)
  if #sorted == 0 then return 0 end
  local idx = math.ceil(#sorted * pct)
  if idx < 1 then idx = 1 elseif idx > #sorted then idx = #sorted end
  return sorted[idx]
end

local function measure(shape, mb)
  collectgarbage("collect"); collectgarbage("collect"); collectgarbage("stop")
  local keep = builders[shape](mb)

  local lua_mb = collectgarbage("count") / 1024
  local resident = rss_mb()

  -- Full-GC pause: time one full mark+sweep over the whole live set.
  local t0 = now()
  collectgarbage("collect")
  local fullgc_ms = (now() - t0) * 1000

  -- Incremental pause distribution: drive one full cycle in steps and record
  -- per-step wall-clock, so worst/p99 reflect real stop-the-world spikes.
  collectgarbage("restart")
  local samples = {}
  local worst = 0
  while true do
    local s0 = now()
    local done = collectgarbage("step")
    local dt = now() - s0
    samples[#samples + 1] = dt
    if dt > worst then worst = dt end
    if done then break end
  end
  table.sort(samples)

  io.write(string.format("%-8s %5dMB  lua=%8.1fMB  rss=%8.1fMB  over=%5.2fx  fullgc=%8.2fms  incWorst=%7.3fms  incP99=%7.3fms  steps=%d\n",
    shape, mb, lua_mb, resident, resident / lua_mb,
    fullgc_ms, worst * 1000, percentile(samples, 0.99) * 1000, #samples))

  keep = nil
  collectgarbage("stop"); collectgarbage("collect"); collectgarbage("collect")
end

local size_arg = arg[1] or "both"
local shape_arg = arg[2] or "all"
local sizes = (size_arg == "both") and { 512, 1024 } or { tonumber(size_arg) }
local shapes = (shape_arg == "all") and { "tables", "strings", "deep" } or { shape_arg }

io.write("=== Large-heap GC Benchmark (512MB / 1GB live sets) ===\n")
io.write("over = rss/lua ratio (arena reserve overhead); higher = more OS memory held beyond Lua-tracked bytes\n\n")
for _, mb in ipairs(sizes) do
  for _, shape in ipairs(shapes) do
    measure(shape, mb)
  end
end
io.write("\nDone.\n")

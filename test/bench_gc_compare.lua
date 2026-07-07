-- GC Performance Comparison Benchmark
-- Run with: luajit -joff test/bench_gc_compare.lua
-- Compare arena bitmap GC vs tri-color GC

local clock = os.clock
local WARM = 2  -- warmup iterations
local ITER = 5  -- measurement iterations

local function measure(name, setup_fn, fn)
  local results = {}
  local mem_peaks = {}
  if setup_fn then setup_fn() end
  for i = 1, WARM + ITER do
    collectgarbage("collect")
    collectgarbage("collect")
    collectgarbage("stop")
    local t0 = clock()
    local mem_peak = fn()
    local t1 = clock()
    collectgarbage("restart")
    if i > WARM then
      results[#results + 1] = t1 - t0
      mem_peaks[#mem_peaks + 1] = mem_peak or 0
    end
  end
  table.sort(results)
  local n = #results
  local median = (n % 2 == 1) and results[(n + 1) / 2]
    or (results[n / 2] + results[n / 2 + 1]) / 2
  local min_t = results[1]
  local max_t = results[#results]
  local avg_mem = 0
  for _, m in ipairs(mem_peaks) do avg_mem = avg_mem + m end
  avg_mem = avg_mem / #mem_peaks
  io.write(string.format("  %-40s median=%7.4fs  min=%7.4fs  max=%7.4fs  mem=%dKB\n",
    name, median, min_t, max_t, math.floor(avg_mem)))
  return median
end

io.write("=== GC Performance Comparison ===\n\n")

-- B1: Pure mark throughput (many live objects, measure fullgc)
io.write("[B1] Mark throughput (large live set)\n")
measure("mark 50K live tables", nil, function()
  local live = {}
  for i = 1, 50000 do
    live[i] = { name = "obj" .. i, data = i }
  end
  local peak = collectgarbage("count")
  collectgarbage("restart")
  local t0 = clock()
  for c = 1, 200 do collectgarbage("collect") end
  local t1 = clock()
  collectgarbage("stop")
  io.write(string.format("    200 fullgc over 50K live: %.4fs\n", t1 - t0))
  return peak
end)

-- B2: Pure sweep throughput (kill many objects, measure collection)
io.write("[B2] Sweep throughput (kill 100K objects)\n")
measure("sweep 100K dead tables", nil, function()
  local objs = {}
  for i = 1, 100000 do objs[i] = { val = i } end
  local peak = collectgarbage("count")
  objs = nil -- kill all
  collectgarbage("restart")
  local t0 = clock()
  collectgarbage("collect")
  collectgarbage("collect")
  local t1 = clock()
  collectgarbage("stop")
  io.write(string.format("    collect 100K dead: %.4fs\n", t1 - t0))
  return peak
end)

-- B3: Mark throughput with deep reference chain
io.write("[B3] Mark depth stress (10K deep chain × 100 cycles)\n")
measure("deep chain mark", nil, function()
  local root = {}
  local cur = root
  for i = 1, 10000 do
    cur.next = { val = i }
    cur = cur.next
  end
  collectgarbage("restart")
  local t0 = clock()
  for c = 1, 100 do collectgarbage("collect") end
  local t1 = clock()
  collectgarbage("stop")
  io.write(string.format("    100 fullgc on 10K chain: %.4fs\n", t1 - t0))
  return collectgarbage("count")
end)

-- B4: Allocation throughput (GC running concurrently)
io.write("[B4] Allocation throughput with GC\n")
measure("alloc 1M tables, auto GC", nil, function()
  collectgarbage("restart")
  local t0 = clock()
  for i = 1, 1000000 do
    local _ = { key = i }
  end
  local t1 = clock()
  collectgarbage("stop")
  io.write(string.format("    1M alloc with GC: %.4fs\n", t1 - t0))
  return collectgarbage("count")
end)

-- B5: Barrier stress (write white into black, force backward barrier)
io.write("[B5] Write barrier stress\n")
measure("barrier: 500K writes to black tables", nil, function()
  local tabs = {}
  for i = 1, 1000 do tabs[i] = {} end
  collectgarbage("restart")
  local t0 = clock()
  for round = 1, 500 do
    for i = 1, 1000 do
      tabs[i][round] = { val = round }
    end
    collectgarbage("step", 5)
  end
  local t1 = clock()
  collectgarbage("stop")
  io.write(string.format("    500K barrier writes: %.4fs\n", t1 - t0))
  return collectgarbage("count")
end)

-- B6: String interning + sweep
io.write("[B6] String churn (intern + sweep)\n")
measure("create+collect 500K unique strings", nil, function()
  collectgarbage("restart")
  local t0 = clock()
  for i = 1, 500000 do
    local _ = "str_" .. i
    if i % 50000 == 0 then collectgarbage("collect") end
  end
  local t1 = clock()
  collectgarbage("stop")
  io.write(string.format("    500K strings: %.4fs\n", t1 - t0))
  return collectgarbage("count")
end)

-- B7: Mixed live/dead ratio (realistic)
io.write("[B7] Mixed live/dead (50%% survival)\n")
measure("churn 500K, 50% survive", nil, function()
  local live = {}
  collectgarbage("restart")
  local t0 = clock()
  for i = 1, 500000 do
    if i % 2 == 0 then
      live[i / 2] = { val = i }
    else
      local _ = { val = i }
    end
    if i % 50000 == 0 then collectgarbage("collect") end
  end
  local t1 = clock()
  collectgarbage("stop")
  io.write(string.format("    500K mixed: %.4fs\n", t1 - t0))
  return collectgarbage("count")
end)

-- B8: Weak table clearance speed
io.write("[B8] Weak table clearing\n")
measure("weak table 100K entries + collect", nil, function()
  local wt = setmetatable({}, { __mode = "v" })
  for i = 1, 100000 do wt[i] = { val = i } end
  collectgarbage("restart")
  local t0 = clock()
  collectgarbage("collect")
  collectgarbage("collect")
  local t1 = clock()
  collectgarbage("stop")
  io.write(string.format("    collect 100K weak: %.4fs\n", t1 - t0))
  return collectgarbage("count")
end)

-- B9: FFI cdata allocation + GC
io.write("[B9] FFI cdata alloc + GC\n")
local ffi = require("ffi")
ffi.cdef[[ typedef struct { int x; int y; int z; } Vec3; ]]
measure("alloc+collect 200K cdata", nil, function()
  collectgarbage("restart")
  local t0 = clock()
  for i = 1, 200000 do
    local _ = ffi.new("Vec3", { x = i, y = i, z = i })
    if i % 20000 == 0 then collectgarbage("collect") end
  end
  local t1 = clock()
  collectgarbage("stop")
  io.write(string.format("    200K cdata: %.4fs\n", t1 - t0))
  return collectgarbage("count")
end)

-- B10: Incremental GC latency (per-step cost)
io.write("[B10] Incremental step latency\n")
measure("10K steps over 20K live objects", nil, function()
  local live = {}
  for i = 1, 20000 do live[i] = { name = "obj" .. i } end
  collectgarbage("restart")
  local t0 = clock()
  for s = 1, 10000 do collectgarbage("step", 1) end
  local t1 = clock()
  collectgarbage("stop")
  io.write(string.format("    10K steps: %.4fs\n", t1 - t0))
  return collectgarbage("count")
end)

io.write("\nDone.\n")

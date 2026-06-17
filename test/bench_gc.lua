-- GC Performance Benchmark
-- Measures: GC time, total time, peak memory, GC cycles
-- Run with: luajit -joff bench_gc.lua

local clock = os.clock

local function bench(name, fn)
  collectgarbage("collect")
  collectgarbage("collect")
  local mem_before = collectgarbage("count")
  local gc_before = 0 -- can't measure GC time directly, measure total
  local t0 = clock()
  local result = fn()
  local t1 = clock()
  collectgarbage("collect")
  collectgarbage("collect")
  local mem_after = collectgarbage("count")
  io.write(string.format("  %-35s %7.3fs  mem_peak=%6dKB  mem_final=%6dKB\n",
    name, t1 - t0, result or 0, math.floor(mem_after)))
end

io.write("=== GC Performance Benchmark ===\n\n")

-- Benchmark 1: Allocation throughput (tables + strings)
io.write("[1] Allocation throughput\n")
bench("alloc 2M tables+strings", function()
  local peak = collectgarbage("count")
  for i = 1, 2000000 do
    local t = { key = "k" .. (i % 10000), val = i }
    if collectgarbage("count") > peak then peak = collectgarbage("count") end
  end
  return math.floor(peak)
end)

-- Benchmark 2: Linked list (GC mark depth stress)
io.write("[2] Mark propagation depth\n")
bench("build+collect 10K deep chain", function()
  local peak = collectgarbage("count")
  for round = 1, 100 do
    local root = {}
    local cur = root
    for i = 1, 10000 do
      cur.next = { val = i }
      cur = cur.next
    end
    if collectgarbage("count") > peak then peak = collectgarbage("count") end
    root = nil
    collectgarbage("collect")
  end
  return math.floor(peak)
end)

-- Benchmark 3: Table churn (alloc/free interleaved with GC)
io.write("[3] Table churn with incremental GC\n")
bench("churn 1M tables, step every 100", function()
  local peak = collectgarbage("count")
  local live = {}
  for i = 1, 1000000 do
    live[i % 1000 + 1] = { "data" .. (i % 5000), i }
    if i % 100 == 0 then collectgarbage("step", 1) end
    if i % 10000 == 0 then
      local m = collectgarbage("count")
      if m > peak then peak = m end
    end
  end
  return math.floor(peak)
end)

-- Benchmark 4: String interning churn
io.write("[4] String interning\n")
bench("create+discard 1M unique strings", function()
  local peak = collectgarbage("count")
  for i = 1, 1000000 do
    local _ = "str_" .. i .. "_" .. (i % 100)
    if i % 50000 == 0 then
      collectgarbage("collect")
      local m = collectgarbage("count")
      if m > peak then peak = m end
    end
  end
  return math.floor(peak)
end)

-- Benchmark 5: Weak table churn
io.write("[5] Weak table churn\n")
bench("weak table 500K insertions", function()
  local peak = collectgarbage("count")
  local wt = setmetatable({}, { __mode = "v" })
  for i = 1, 500000 do
    wt[i] = { data = i }
    if i % 10000 == 0 then
      collectgarbage("collect")
      local m = collectgarbage("count")
      if m > peak then peak = m end
    end
  end
  return math.floor(peak)
end)

-- Benchmark 6: Full GC cycle throughput
io.write("[6] Full GC cycle throughput\n")
bench("1000 full GC cycles (with 10K live objects)", function()
  local live = {}
  for i = 1, 10000 do
    live[i] = { name = "obj" .. i, data = string.rep("x", 50) }
  end
  local peak = collectgarbage("count")
  local t0 = clock()
  for i = 1, 1000 do
    collectgarbage("collect")
  end
  local gc_time = clock() - t0
  io.write(string.format("    (pure GC time for 1000 cycles: %.3fs)\n", gc_time))
  return math.floor(peak)
end)

-- Benchmark 7: Mixed workload (realistic)
io.write("[7] Mixed workload\n")
bench("mixed ops 500K iterations", function()
  local peak = collectgarbage("count")
  local cache = {}
  local results = {}
  for i = 1, 500000 do
    local key = "item_" .. (i % 2000)
    if not cache[key] then
      cache[key] = { name = key, count = 0 }
    end
    cache[key].count = cache[key].count + 1
    if i % 5000 == 0 then
      results[#results + 1] = { snapshot = i, size = #cache }
      if #results > 50 then
        for j = 1, 25 do table.remove(results, 1) end
      end
    end
    if i % 20000 == 0 then
      collectgarbage("collect")
      local m = collectgarbage("count")
      if m > peak then peak = m end
    end
  end
  return math.floor(peak)
end)

io.write("\nDone.\n")

-- Run: luajit test/bench_jit_realistic.lua (JIT ON)

local ffi = require("ffi")

ffi.cdef[[
typedef struct timespec { long tv_sec; long tv_nsec; } timespec;
int clock_gettime(int clk_id, struct timespec *tp);
typedef struct { float x; float y; float z; } Vec3f;
]]

local CLOCK_MONOTONIC = 1
local ts = ffi.new("timespec[1]")

local W1_N, W1_K = 2000000, 1000
local W2_N = 1000000
local W3_N = 2000000
local W4_N = 1000000
local W4_KEYS = 10000
local W5_REPEATS, W5_DEPTH = 10000, 100
local RUNS = 7

local function now_ns()
  ffi.C.clock_gettime(CLOCK_MONOTONIC, ts)
  return tonumber(ts[0].tv_sec) * 1000000000 + tonumber(ts[0].tv_nsec)
end

local function median(values)
  local copy = {}
  for i = 1, #values do
    copy[i] = values[i]
  end
  table.sort(copy)
  local n = #copy
  return (n % 2 == 1) and copy[(n + 1) / 2] or (copy[n / 2] + copy[n / 2 + 1]) / 2
end

local function percentile(sorted, pct)
  if #sorted == 0 then
    return 0
  end
  local idx = math.ceil(#sorted * pct)
  if idx < 1 then
    idx = 1
  elseif idx > #sorted then
    idx = #sorted
  end
  return sorted[idx]
end

local function gc_probe(samples)
  local start_ns = now_ns()
  collectgarbage("step")
  local elapsed_ns = now_ns() - start_ns
  samples[#samples + 1] = elapsed_ns
  return elapsed_ns
end

local function finish_stats(result)
  table.sort(result.gc_samples)
  result.worst_gc_ms = (result.worst_gc_ns or 0) / 1000000
  result.p99_gc_ms = percentile(result.gc_samples, 0.99) / 1000000
  result.peak_kb = result.peak_kb or 0
  result.gc_samples = nil
  result.worst_gc_ns = nil
  return result
end

local function prepare_gc()
  collectgarbage("collect")
  collectgarbage("collect")
  collectgarbage("restart")
end

local function median_result(results)
  local row = {
    total_time_s = {},
    throughput = {},
    worst_gc_ms = {},
    p99_gc_ms = {},
    peak_kb = {},
  }
  for i = 1, #results do
    row.total_time_s[i] = results[i].total_time_s
    row.throughput[i] = results[i].throughput
    row.worst_gc_ms[i] = results[i].worst_gc_ms
    row.p99_gc_ms[i] = results[i].p99_gc_ms
    row.peak_kb[i] = results[i].peak_kb
  end
  return {
    total_time_s = median(row.total_time_s),
    throughput = median(row.throughput),
    worst_gc_ms = median(row.worst_gc_ms),
    p99_gc_ms = median(row.p99_gc_ms),
    peak_kb = median(row.peak_kb),
  }
end

local function run_with_steps(total_iters, step_every, body)
  local gc_samples = {}
  local peak_kb = collectgarbage("count")
  local worst_gc_ns = 0
  local t0 = os.clock()
  for i = 1, total_iters do
    body(i)
    if i % step_every == 0 then
      local kb = collectgarbage("count")
      if kb > peak_kb then
        peak_kb = kb
      end
      local elapsed_ns = gc_probe(gc_samples)
      if elapsed_ns > worst_gc_ns then
        worst_gc_ns = elapsed_ns
      end
    end
  end
  local total_time_s = os.clock() - t0
  return {
    total_time_s = total_time_s,
    gc_samples = gc_samples,
    worst_gc_ns = worst_gc_ns,
    peak_kb = peak_kb,
  }
end

local function run_w1()
  local results = {}
  for _ = 1, RUNS do
    prepare_gc()
    local t = {}
    local raw = run_with_steps(W1_N, 100, function(i)
      t[(i - 1) % W1_K + 1] = { i, i * 2, i * 3 }
    end)
    raw.peak_kb = math.max(raw.peak_kb, collectgarbage("count"))
    results[#results + 1] = finish_stats({
      total_time_s = raw.total_time_s,
      throughput = W1_N / raw.total_time_s,
      gc_samples = raw.gc_samples,
      worst_gc_ns = raw.worst_gc_ns,
      peak_kb = raw.peak_kb,
    })
    t = nil
    collectgarbage("stop")
  end
  return median_result(results)
end

local function run_w2()
  local results = {}
  for _ = 1, RUNS do
    prepare_gc()
    local raw = run_with_steps(W2_N, 100, function(i)
      local _ = ffi.new("Vec3f", i, i * 2, i * 3)
    end)
    raw.peak_kb = math.max(raw.peak_kb, collectgarbage("count"))
    results[#results + 1] = finish_stats({
      total_time_s = raw.total_time_s,
      throughput = W2_N / raw.total_time_s,
      gc_samples = raw.gc_samples,
      worst_gc_ns = raw.worst_gc_ns,
      peak_kb = raw.peak_kb,
    })
    collectgarbage("stop")
  end
  return median_result(results)
end

local function run_w3()
  local results = {}
  for _ = 1, RUNS do
    prepare_gc()
    local sink = 0
    local raw = run_with_steps(W3_N, 100, function(i)
      local s = tostring(i) .. "_" .. tostring(i * 2)
      sink = sink + #s
    end)
    raw.peak_kb = math.max(raw.peak_kb, collectgarbage("count"))
    results[#results + 1] = finish_stats({
      total_time_s = raw.total_time_s,
      throughput = W3_N / raw.total_time_s,
      gc_samples = raw.gc_samples,
      worst_gc_ns = raw.worst_gc_ns,
      peak_kb = raw.peak_kb,
    })
    if sink == 0 then
      io.write("")
    end
    collectgarbage("stop")
  end
  return median_result(results)
end

local function run_w4()
  local results = {}
  for _ = 1, RUNS do
    prepare_gc()
    local buckets = {}
    local intern = {}
    local raw = run_with_steps(W4_N, 100, function(i)
      -- Bounded key pool so the intern cache actually hits (the realistic
      -- shape being modeled). A per-iteration-unique key (e.g. i*7) would
      -- make every lookup a miss and grow intern/buckets without bound,
      -- turning this into an allocation-storm micro-bench instead.
      local key = "k" .. (i % W4_KEYS)
      intern[key] = intern[key] or key
      buckets[(i - 1) % 2048 + 1] = {
        idx = i,
        key = intern[key],
        vec = ffi.new("Vec3f", i, i + 1, i + 2),
      }
      if i % 2 == 0 then
        buckets[key] = buckets[(i - 1) % 2048 + 1]
      else
        buckets[key] = nil
      end
    end)
    raw.peak_kb = math.max(raw.peak_kb, collectgarbage("count"))
    results[#results + 1] = finish_stats({
      total_time_s = raw.total_time_s,
      throughput = W4_N / raw.total_time_s,
      gc_samples = raw.gc_samples,
      worst_gc_ns = raw.worst_gc_ns,
      peak_kb = raw.peak_kb,
    })
    buckets = nil
    intern = nil
    collectgarbage("stop")
  end
  return median_result(results)
end

local function make_recursor()
  local function rec(level, limit)
    local node = { level, level * 2, level * 3 }
    if level < limit then
      node.next = rec(level + 1, limit)
    end
    return node
  end
  return rec
end

local function run_w5()
  local results = {}
  for _ = 1, RUNS do
    prepare_gc()
    local rec = make_recursor()
    local keep = {}
    local raw = run_with_steps(W5_REPEATS, 100, function(i)
      keep[i] = rec(1, W5_DEPTH)
      if i % 2 == 0 then
        keep[i - 1] = nil
      end
    end)
    raw.peak_kb = math.max(raw.peak_kb, collectgarbage("count"))
    results[#results + 1] = finish_stats({
      total_time_s = raw.total_time_s,
      throughput = W5_REPEATS / raw.total_time_s,
      gc_samples = raw.gc_samples,
      worst_gc_ns = raw.worst_gc_ns,
      peak_kb = raw.peak_kb,
    })
    keep = nil
    collectgarbage("stop")
  end
  return median_result(results)
end

local workloads = {
  { name = "W1: Hot loop table churn", unit = "ops/sec", run = run_w1 },
  { name = "W2: FFI cdata churn in hot loop", unit = "ops/sec", run = run_w2 },
  { name = "W3: String building in hot loop", unit = "ops/sec", run = run_w3 },
  { name = "W4: Mixed realistic workload", unit = "iterations/sec", run = run_w4 },
  { name = "W5: Deep recursion with allocation", unit = "iterations/sec", run = run_w5 },
}

io.write("JIT realistic benchmark\n")
io.write(string.format("%-34s %12s %18s %18s %12s %12s %12s\n",
  "workload", "total_s", "throughput", "unit", "worst_gc_ms", "p99_ms", "peak_kb"))

for i = 1, #workloads do
  local item = workloads[i]
  local result = item.run()
  io.write(string.format("%-34s %12.4f %13.2f %18s %12.4f %12.4f %12.1f\n",
    item.name,
    result.total_time_s,
    result.throughput,
    item.unit,
    result.worst_gc_ms,
    result.p99_gc_ms,
    result.peak_kb))
end

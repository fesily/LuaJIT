local ffi = require("ffi")

ffi.cdef[[
struct timespec { long tv_sec; long tv_nsec; };
int clock_gettime(int clk_id, struct timespec *tp);
]]

local CLOCK_MONOTONIC = 1
local DEFAULT_SCALE_MB = 128
local DEFAULT_STEPMUL = 200

local ts = ffi.new("struct timespec[1]")

local function now_ns()
  ffi.C.clock_gettime(CLOCK_MONOTONIC, ts)
  return tonumber(ts[0].tv_sec) * 1000000000 + tonumber(ts[0].tv_nsec)
end

local function usage()
  io.stderr:write("usage: luajit -joff test/gc/inc_pause_bench.lua [tables|strings|deep] [scale_mb] [stepmul ...]\n")
  os.exit(2)
end

local function parse_positive_int(value, name)
  local parsed = tonumber(value)
  if not parsed or parsed < 1 or parsed % 1 ~= 0 then
    io.stderr:write(string.format("invalid %s: %s\n", name, tostring(value)))
    usage()
  end
  return parsed
end

local function build_tables(scale_mb)
  local n = scale_mb * 16384
  local keep = {}
  for i = 1, n do
    keep[i] = { i, i, i }
  end
  return keep, n
end

local function build_strings(scale_mb)
  local n = scale_mb * 21504
  local keep = {}
  for i = 1, n do
    keep[i] = "inc_pause_string_" .. i
  end
  return keep, n
end

local function build_deep(scale_mb)
  local n = scale_mb * 12672
  local keep = nil
  for i = 1, n do
    keep = { next = keep, v = i }
  end
  return keep, n
end

local builders = {
  tables = build_tables,
  strings = build_strings,
  deep = build_deep,
}

local function percentile(sorted, pct)
  if #sorted == 0 then
    return 0
  end
  local index = math.ceil(#sorted * pct)
  if index < 1 then
    index = 1
  elseif index > #sorted then
    index = #sorted
  end
  return sorted[index]
end

local function measure_cycle(workload, scale_mb, stepmul)
  collectgarbage("collect")
  collectgarbage("collect")
  collectgarbage("stop")

  local keep, objects = builders[workload](scale_mb)
  collectgarbage("setstepmul", stepmul)
  collectgarbage("restart")

  local samples = {}
  local total_ns = 0
  local worst_ns = 0
  local cycle_start_ns = now_ns()

  while true do
    local start_ns = now_ns()
    local done = collectgarbage("step")
    local elapsed_ns = now_ns() - start_ns
    samples[#samples + 1] = elapsed_ns
    total_ns = total_ns + elapsed_ns
    if elapsed_ns > worst_ns then
      worst_ns = elapsed_ns
    end
    if done then
      break
    end
  end

  local cycle_ns = now_ns() - cycle_start_ns
  table.sort(samples)

  local result = {
    objects = objects,
    steps = #samples,
    worst_ms = worst_ns / 1000000,
    p99_ms = percentile(samples, 0.99) / 1000000,
    p50_ms = percentile(samples, 0.50) / 1000000,
    mean_ms = (total_ns / #samples) / 1000000,
    cycle_ms = cycle_ns / 1000000,
  }

  keep = nil
  collectgarbage("stop")
  collectgarbage("collect")
  collectgarbage("collect")

  return result
end

local workload = arg[1] or "tables"
if not builders[workload] then
  usage()
end

local scale_mb = arg[2] and parse_positive_int(arg[2], "scale_mb") or DEFAULT_SCALE_MB
local stepmuls = {}
if arg[3] then
  for i = 3, #arg do
    stepmuls[#stepmuls + 1] = parse_positive_int(arg[i], "stepmul")
  end
else
  stepmuls[1] = DEFAULT_STEPMUL
end

io.write(string.format("workload=%s scale_mb=%d\n", workload, scale_mb))
io.write("stepmul    objects     steps   worst_ms     p99_ms     p50_ms    mean_ms     cyc_ms\n")

for i = 1, #stepmuls do
  local stepmul = stepmuls[i]
  local result = measure_cycle(workload, scale_mb, stepmul)
  io.write(string.format("%7d %10d %9d %10.3f %10.3f %10.3f %10.3f %10.3f\n",
    stepmul,
    result.objects,
    result.steps,
    result.worst_ms,
    result.p99_ms,
    result.p50_ms,
    result.mean_ms,
    result.cycle_ms))
end

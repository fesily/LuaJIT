local ffi = require("ffi")

ffi.cdef[[
struct timespec { long tv_sec; long tv_nsec; };
int clock_gettime(int clk_id, struct timespec *tp);
]]

local CLOCK_MONOTONIC = 1
local THRESHOLD_MS = 3
local OBJECTS = 3244032

local ts = ffi.new("struct timespec[1]")

local function now_ns()
  ffi.C.clock_gettime(CLOCK_MONOTONIC, ts)
  return tonumber(ts[0].tv_sec) * 1000000000 + tonumber(ts[0].tv_nsec)
end

local function build_deep(n)
  local keep = nil
  for i = 1, n do
    keep = { next = keep, v = i }
  end
  return keep
end

collectgarbage("stop")
local keep = build_deep(OBJECTS)
collectgarbage("setstepmul", 200)
collectgarbage("restart")

local worst_ns = 0
local steps = 0
local cycle_start_ns = now_ns()

while true do
  local start_ns = now_ns()
  local done = collectgarbage("step")
  local elapsed_ns = now_ns() - start_ns
  steps = steps + 1
  if elapsed_ns > worst_ns then
    worst_ns = elapsed_ns
  end
  if done then
    break
  end
end

local cycle_ms = (now_ns() - cycle_start_ns) / 1000000
local worst_ms = worst_ns / 1000000

io.write(string.format(
  "deep_pause_assert objects=%d stepmul=200 steps=%d worst_ms=%.3f cycle_ms=%.3f threshold_ms=%.3f\n",
  OBJECTS, steps, worst_ms, cycle_ms, THRESHOLD_MS))

assert(worst_ms < THRESHOLD_MS,
  string.format("deep-list incremental GC worst-step pause %.3fms >= %.3fms", worst_ms, THRESHOLD_MS))

keep = keep

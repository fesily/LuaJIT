local ffi = require("ffi")

ffi.cdef[[
struct timespec { long tv_sec; long tv_nsec; };
int clock_gettime(int clk_id, struct timespec *tp);
]]

local CLOCK_MONOTONIC = 1
local THRESHOLD_MS = 3
local OBJECTS = 3244032

-- collectgarbage("step") without a size is not incremental under LuaJIT
-- (data=0 -> threshold=total -> whole cycle per call); step(1) is required.
-- The collect before stop guarantees the measured cycle is fresh: stopping
-- mid-cycle and restarting resumes the old cycle, whose newborns survive
-- via current-white without ever being marked (nothing to measure).

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

collectgarbage("collect")
collectgarbage("stop")
local keep = build_deep(OBJECTS)
collectgarbage("setstepmul", 200)
collectgarbage("restart")

local worst_ns = 0
local steps = 0
local samples = {}
local cycle_start_ns = now_ns()

while true do
  local start_ns = now_ns()
  local done = collectgarbage("step", 1)
  local elapsed_ns = now_ns() - start_ns
  steps = steps + 1
  samples[steps] = elapsed_ns
  if elapsed_ns > worst_ns then
    worst_ns = elapsed_ns
  end
  if done then
    break
  end
end

local cycle_ms = (now_ns() - cycle_start_ns) / 1000000
local worst_ms = worst_ns / 1000000
table.sort(samples)
local p50_ms = samples[math.floor((#samples + 1) / 2)] / 1000000

io.write(string.format(
  "deep_pause_assert objects=%d stepmul=200 steps=%d worst_ms=%.3f p50_ms=%.3f cycle_ms=%.3f threshold_ms=%.3f\n",
  OBJECTS, steps, worst_ms, p50_ms, cycle_ms, THRESHOLD_MS))

-- Assert builds run a full-heap checkheap after every onestep
-- (lj_gc_arena.c, LUA_USE_ASSERT && !LJ_GC_NOSTEPVERIFY), inflating every
-- step to O(heap); pause assertions are only meaningful on release builds.
if p50_ms > 0.5 then
  io.write("SKIP: verify build (per-step checkheap dominates step time)\n")
  return
end

assert(worst_ms < THRESHOLD_MS,
  string.format("deep-list incremental GC worst-step pause %.3fms >= %.3fms", worst_ms, THRESHOLD_MS))

keep = keep

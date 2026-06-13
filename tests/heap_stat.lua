-- Large-heap concurrent-GC validation workload.
-- usage: luajit heap_stat.lua <conc|inc> <target_gb>
-- Builds a pointer-rich retained graph sized to target_gb, then runs a
-- steady-state phase (rotate-window mutation + incremental GC steps) and
-- reports phase-level GC stats. Concurrent marking SHOULD win here: each GC
-- cycle traverses the whole GB-scale graph while the mutator keeps running.

local u = require("jit.util")
local mode, gb = ...
gb = tonumber(gb) or 2
local conc = (mode == "conc")
if conc then assert(collectgarbage("concurrent") ~= -1, "concgc enable failed") end

local KB_PER_GB = 1024 * 1024
local target_kb = gb * KB_PER_GB

-- Build phase: grow a pointer-rich graph until heap ~ target.
-- Node = small table with 3 back-edges (real pointers to chase) + a tiny
-- numeric payload. Back-edges make the live graph deeply connected so the
-- marker has substantial pointer-traversal work, not just byte volume.
local live = {}
local n = 0
local t_build = os.clock()
while collectgarbage("count") < target_kb * 0.94 do
  n = n + 1
  live[n] = {
    id = n,
    a = live[n - 1],                 -- chain back-edge
    b = live[(n >= 4) and (n - (n % 4) - 1) or 0] or false,  -- skip back-edge
    c = live[(n % 1024) + 1] or false,                       -- locality edge
    data = { n, n + 1, n + 2 },      -- nested table (more objects to mark)
  }
end
collectgarbage("collect")  -- settle to true live size
local built_gb = collectgarbage("count") / KB_PER_GB
io.write(string.format("[%s %gGB] built %d nodes, live=%.2f GB, build=%.1fs\n",
  mode, gb, n, built_gb, os.clock() - t_build))
io.flush()

-- Steady-state: rotate a window. Each round replaces a fraction of slots
-- (drops old node -> garbage, allocs new -> same live size) then drives GC
-- incrementally. Keeps the live set ~constant while generating GC pressure.
local FRACTION = 0.05            -- mutate 5% of the graph per round
local per_round = math.floor(n * FRACTION)
local ROUNDS = 40
u.gcstat_reset()
local t0 = os.clock()
local seq = n
for round = 1, ROUNDS do
  local base = (round * per_round) % n
  for j = 1, per_round do
    seq = seq + 1
    local slot = ((base + j) % n) + 1
    live[slot] = { id = seq, a = live[(slot % n) + 1], data = { seq } }
  end
  for _ = 1, 8 do collectgarbage("step") end
end
local wall = os.clock() - t0
collectgarbage("collect")

local s = u.gcstat()
io.write(string.format("[%s %gGB] steady wall=%.3fs  rounds=%d mutated=%d/round\n",
  mode, gb, wall, ROUNDS, per_round))
if conc then
  local mut = s.phases.mark_start.ns_total + s.phases.drainlog.ns_total
            + s.phases.conc_park.ns_total + s.phases.conc_finish.ns_total
            + s.phases.atomic.ns_total
  local bg = s.phases.gcthread_mark.ns_total
  io.write(string.format(
    "  cycles=%d drains=%d bursts=%d peak=%.2fGB\n",
    s.cycle_count, s.drain_rounds, s.gcthread_bursts, s.bytes_peak/1024/1024/1024))
  io.write(string.format(
    "  bg_mark=%.1fms  mutator-side=%.1fms (mark_start=%.1f drainlog=%.1f park=%.1f finish=%.1f atomic=%.1f)\n",
    bg/1e6, mut/1e6,
    s.phases.mark_start.ns_total/1e6, s.phases.drainlog.ns_total/1e6,
    s.phases.conc_park.ns_total/1e6, s.phases.conc_finish.ns_total/1e6,
    s.phases.atomic.ns_total/1e6))
  io.write(string.format("  OFFLOAD RATIO = %.3f  (bg_mark / mutator-side)\n",
    mut > 0 and bg/mut or 0))
end
io.flush()

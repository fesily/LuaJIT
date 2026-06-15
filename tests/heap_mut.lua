-- Large-heap concurrent-GC validation workload, parameterized mutation rate.
-- usage: luajit heap_mut.lua <conc|inc> <target_gb> <fraction>
-- fraction = portion of the live graph rewritten per round (e.g. 0.05, 0.001, 0).
-- Builds a pointer-rich retained graph, runs a steady-state phase, reports
-- the mutator-visible steady wall-time (the metric that actually matters).

local u = require("jit.util")
local mode, gb, frac = ...
gb = tonumber(gb) or 1
frac = tonumber(frac) or 0.05
local conc = (mode == "conc")
if conc then assert(collectgarbage("concurrent") ~= -1, "concgc enable failed") end

local KB_PER_GB = 1024 * 1024
local target_kb = gb * KB_PER_GB

local live = {}
local n = 0
while collectgarbage("count") < target_kb * 0.94 do
  n = n + 1
  live[n] = {
    id = n,
    a = live[n - 1],
    b = live[(n >= 4) and (n - (n % 4) - 1) or 0] or false,
    c = live[(n % 1024) + 1] or false,
    data = { n, n + 1, n + 2 },
  }
end
collectgarbage("collect")
local built_gb = collectgarbage("count") / KB_PER_GB
io.write(string.format("[%s %gGB f=%.4f] built %d nodes, live=%.2f GB\n",
  mode, gb, frac, n, built_gb))
io.flush()

local per_round = math.floor(n * frac)
local ROUNDS = 40
u.gcstat_reset()
local t0 = os.clock()
local seq = n
for round = 1, ROUNDS do
  if per_round > 0 then
    local base = (round * per_round) % n
    for j = 1, per_round do
      seq = seq + 1
      local slot = ((base + j) % n) + 1
      live[slot] = { id = seq, a = live[(slot % n) + 1], data = { seq } }
    end
  end
  for _ = 1, 8 do collectgarbage("step") end
end
local wall = os.clock() - t0
collectgarbage("collect")

local s = u.gcstat()
io.write(string.format("  steady wall=%.3fs  mutated=%d/round\n", wall, per_round))
if conc then
  local mut = s.phases.mark_start.ns_total + s.phases.drainlog.ns_total
            + s.phases.conc_park.ns_total + s.phases.conc_finish.ns_total
            + s.phases.atomic.ns_total
  local bg = s.phases.gcthread_mark.ns_total
  io.write(string.format("  cycles=%d park=%.1fms finish=%.1fms atomic=%.1fms bg_mark=%.1fms offload=%.3f\n",
    s.cycle_count, s.phases.conc_park.ns_total/1e6, s.phases.conc_finish.ns_total/1e6,
    s.phases.atomic.ns_total/1e6, bg/1e6, mut>0 and bg/mut or 0))
end
io.flush()

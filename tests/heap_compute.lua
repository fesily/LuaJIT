-- Mutator-bound concurrent-GC test: heavy NON-GC compute between allocations.
-- usage: luajit heap_compute.lua <conc|inc> <gb> <compute_iters>
-- The point: if the GC thread marks concurrently while the mutator does real
-- work (not just driving GC), wall-time should overlap and conc should win or
-- at least not lose. compute_iters scales the per-round non-allocating work.

local u = require("jit.util")
local mode, gb, citers = ...
gb = tonumber(gb) or 1
citers = tonumber(citers) or 2000000
local conc = (mode == "conc")
if conc then assert(collectgarbage("concurrent") ~= -1, "concgc enable failed") end

local KB_PER_GB = 1024 * 1024
local target_kb = gb * KB_PER_GB

-- Build a pointer-rich retained graph (~gb).
local live = {}
local n = 0
while collectgarbage("count") < target_kb * 0.94 do
  n = n + 1
  live[n] = {
    id = n, a = live[n - 1],
    b = live[(n >= 4) and (n - (n % 4) - 1) or 0] or false,
    c = live[(n % 1024) + 1] or false,
    data = { n, n + 1, n + 2 },
  }
end
collectgarbage("collect")
io.write(string.format("[%s %gGB citers=%d] built %d nodes, live=%.2f GB\n",
  mode, gb, citers, n, collectgarbage("count") / KB_PER_GB))
io.flush()

-- Pure-compute kernel: no allocation, no GC interaction. This is the "useful
-- work" the mutator does that SHOULD overlap with concurrent marking.
local function compute(iters, seed)
  local x = seed
  for i = 1, iters do
    x = (x * 1103515245 + 12345) % 2147483648
    x = x + (i % 7)
    if x > 2e9 then x = x - 1e9 end
  end
  return x
end

local ROUNDS = 40
local per_round = math.floor(n * 0.02)  -- modest 2% mutation to drive GC cycles
u.gcstat_reset()
local acc = 0
local t0 = os.clock()
local seq = n
for round = 1, ROUNDS do
  -- (a) real non-GC work
  acc = acc + compute(citers, round * 31 + 7)
  -- (b) some allocation/mutation to generate GC pressure
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
io.write(string.format("  steady wall=%.3fs  (acc=%d guard)\n", wall, acc % 1000))
io.write(string.format("  atomic: total=%.1fms max=%.1fms count=%d\n",
  s.phases.atomic.ns_total/1e6, s.phases.atomic.ns_max/1e6, s.phases.atomic.count))
if conc then
  local fin = s.phases.conc_finish
  local ms  = s.phases.mark_start
  local pk  = s.phases.conc_park
  local dl  = s.phases.drainlog
  local ncyc = s.cycle_count
  -- mark_start, conc_finish, atomic are top-level mutator STW phases.
  -- drainlog is nested inside conc_finish, so NOT added separately.
  -- conc_park is the ring-full inline drain; independent of the above.
  local total_stw = ms.ns_total + pk.ns_total + fin.ns_total + s.phases.atomic.ns_total
  io.write(string.format("  mark_start: total=%.1fms max=%.1fms count=%d\n",
    ms.ns_total/1e6, ms.ns_max/1e6, ms.count))
  io.write(string.format("  conc_park:  total=%.1fms max=%.1fms count=%d\n",
    pk.ns_total/1e6, pk.ns_max/1e6, pk.count))
  io.write(string.format("  conc_finish:total=%.1fms max=%.1fms count=%d\n",
    fin.ns_total/1e6, fin.ns_max/1e6, fin.count))
  io.write(string.format("  drainlog(nested): total=%.1fms max=%.1fms count=%d\n",
    dl.ns_total/1e6, dl.ns_max/1e6, dl.count))
  io.write(string.format("  total_STW=%.1fms  max_single_pause=%.1fms\n",
    total_stw/1e6,
    math.max(ms.ns_max, pk.ns_max, fin.ns_max, s.phases.atomic.ns_max)/1e6))
  io.write(string.format("  cycles=%d bg_mark=%.1fms\n",
    ncyc, s.phases.gcthread_mark.ns_total/1e6))
end
io.flush()

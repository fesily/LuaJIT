-- Concurrent GC stats smoke test.
-- Run with a build configured: make XCFLAGS="-DLUAJIT_ENABLE_CONCGC -DLUAJIT_GC_STAT=1"
--   ./src/luajit tests/concgc_stats_test.lua

local u = require("jit.util")

-- If the build has stats disabled, gcstat() returns nil. Skip gracefully.
if u.gcstat() == nil then
  print("SKIP: built without LUAJIT_GC_STAT=1")
  return
end

assert(collectgarbage("concurrent") ~= -1, "concurrent GC init failed")
u.gcstat_reset()

-- Churn workload guaranteed to trigger several concurrent cycles.
local roots = {}
for cycle = 1, 40 do
  for i = 1, 25000 do
    roots[i % 20000] = { x = i, name = "obj" .. i, list = { i, i + 1 } }
  end
end
collectgarbage("collect")

local s = u.gcstat()

-- Structural assertions.
assert(type(s) == "table", "gcstat must return a table")
assert(type(s.phases) == "table", "missing phases table")
for _, name in ipairs({"mark_start", "drainlog", "conc_park", "conc_finish",
                       "atomic", "gcthread_mark"}) do
  local p = s.phases[name]
  assert(type(p) == "table", "missing phase " .. name)
  assert(p.ns_total >= 0 and p.ns_max >= 0 and p.count >= 0,
         "negative stat in phase " .. name)
  assert(p.ns_max <= p.ns_total or p.count <= 1,
         "ns_max exceeds ns_total in phase " .. name)
end

-- Behavioral assertions: concurrency actually happened.
assert(s.cycle_count >= 1, "expected >=1 concurrent cycle, got " .. s.cycle_count)
assert(s.phases.gcthread_mark.ns_total > 0,
       "expected background marking time on the GC thread")
assert(s.phases.gcthread_mark.count >= 1, "GC thread never ran a mark interval")

-- The validation we care about: background mark time should be a meaningful
-- fraction of the mutator-visible GC time. Report the ratio rather than
-- asserting a threshold (it is workload/CPU dependent).
local mutator_gc = s.phases.mark_start.ns_total + s.phases.drainlog.ns_total
                 + s.phases.conc_park.ns_total + s.phases.conc_finish.ns_total
                 + s.phases.atomic.ns_total
local bg = s.phases.gcthread_mark.ns_total
print(string.format("cycles=%d drains=%d bursts=%d",
      s.cycle_count, s.drain_rounds, s.gcthread_bursts))
print(string.format("mutator-side GC = %.2f ms", mutator_gc / 1e6))
print(string.format("  park wait     = %.2f ms", s.phases.conc_park.ns_total / 1e6))
print(string.format("  drainlog      = %.2f ms", s.phases.drainlog.ns_total / 1e6))
print(string.format("  atomic        = %.2f ms", s.phases.atomic.ns_total / 1e6))
print(string.format("background mark = %.2f ms", bg / 1e6))
print(string.format("offload ratio   = %.2f (bg / mutator-side)",
      mutator_gc > 0 and bg / mutator_gc or 0))

-- Dump to CSV and confirm the file is written.
local ok = u.gcstat_dump("/tmp/concgc_stats_test.csv", {label = "smoketest"})
assert(ok, "gcstat_dump failed to write file")
local f = assert(io.open("/tmp/concgc_stats_test.csv", "r"))
local header = f:read("*l")
local row = f:read("*l")
f:close()
assert(header:find("gcthread_mark_ns", 1, true), "CSV header missing phase column")
assert(row:find("smoketest", 1, true), "CSV row missing label")
os.remove("/tmp/concgc_stats_test.csv")

print("PASS: concgc stats smoke test")

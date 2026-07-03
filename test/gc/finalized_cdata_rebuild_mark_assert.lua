-----------------------------------------------------------------------------
-- test/gc/finalized_cdata_rebuild_mark_assert.lua
--
-- T1 regression-lock for the finalized-cdata rebuild MARK fix.
--
-- Oracle audit (ses_0da327195ffeXSAd46pPjLcAUI) found that lj_cdata_free's
-- finalized branch (LJ_GC_CDATA_FIN) calls gc_obj_makewhite, clearing the
-- arena/huge MARK bit and leaving the pending-finalizer cdata at mark0 while
-- it sits on mmudata. Under "raw marks authoritative during rebuild" (the
-- T3/T4 state with GCF_DEADAUTH removed), a HugeScan restart after a hugeset
-- rehash mid-yield re-processes that mark0 slot and RE-LINKS the same cdata
-- onto mmudata -> double-link -> double-finalize and/or mmudata ring
-- corruption (use-after-free).
--
-- T1's fix: the finalized branch SETS the MARK (gc_obj_resurrect) instead of
-- clearing it (gc_obj_makewhite), so the pending-finalizer cdata stays
-- GC-live (mark=1) through every rebuild yield. A HugeScan restart then sees
-- it as a live survivor and skips it -- no double-link.
--
-- This test forces the exact interleaving:
--   1. Allocate a cohort of HUGE finalizable cdata (ffi.gc on a fixed huge
--      ctype -> lj_cdata_new safe-huge path -> hugeset-resident, with
--      LJ_GC_CDATA_FIN set).
--   2. Drop the cohort so the next sweep frees them via lj_cdata_free's
--      finalized branch -> linked onto mmudata.
--   3. Run incremental collectgarbage("step") slices interleaved with
--      bursts of huge cdata allocation that force hugeset resize/rehash
--      DURING the rebuild (HugeScan) window -- exactly the restart scenario.
--   4. Drop everything and run two full collectgarbage("collect") to drain
--      mmudata (run finalizers).
--   5. Assert each cohort's finalizer ran EXACTLY once (counter == cohort
--      size) and the process did not crash or assert-fail.
--
-- RED/GREEN STATUS:
--   On HEAD (a1d81817, with GCF_DEADAUTH + HUGESET_SWEPT) this test is GREEN:
--   DEADAUTH suppresses mark-based deadness during the rebuild window and
--   SWEPT prevents slot reprocessing, so the double-link never happens even
--   with the mark0 bug. This file is therefore a REGRESSION-LOCK: its true
--   RED appears only when DEADAUTH is removed (T3/T4) WITHOUT the T1 fix.
--   The throwaway local revert demonstration (makewhite restored, DEADAUTH
--   removed) confirmed the double-finalize; that revert was discarded.
--   The lock guarantees that after T1+T3+T4 land together, the cohort
--   finalizer count stays exact.
--
-- Build:
--   make clean && make -j4 \
--     XCFLAGS="-DLUAJIT_ENABLE_GCARENA -DLUAJIT_SECURITY_STRHASH=1 -DLUA_USE_ASSERT"
-- Run:
--   ./src/luajit test/gc/finalized_cdata_rebuild_mark_assert.lua
-----------------------------------------------------------------------------

local LUAJIT = "./luajit"

-- Fixed huge ctype: 600000 bytes > ArenaHugeThreshold (512KB = 524288).
-- Fixed ctype -> lj_cdata_new (NOT lj_cdata_newv) -> safe huge path with
-- the GCobj at the block base (hugeset/ishuge classify it correctly).
local HUGE_FIXED_SIZE = 600000

-- Substitute __SIZE__ in code templates (avoids string.format %d clashes
-- with the inner code's own format strings).
local function mkcode(template)
  return (string.gsub(template, "__SIZE__", tostring(HUGE_FIXED_SIZE)))
end

local scenarios = {}

-----------------------------------------------------------------------------
-- Scenario 1: basic -- a single cohort of finalizable huge cdata, dropped,
-- then full gc. Smoke test: finalizer runs exactly once, no crash.
-----------------------------------------------------------------------------
scenarios[#scenarios + 1] = {
  name = "basic_finalized_huge_once",
  code = mkcode([[
local ffi = require("ffi")
local counter = { n = 0 }
local N = 8
local keep = {}
for i = 1, N do
  keep[i] = ffi.gc(ffi.new("char[__SIZE__]"), function()
    counter.n = counter.n + 1
  end)
end
keep = nil
collectgarbage("collect")
collectgarbage("collect")
assert(counter.n == N,
  string.format("basic: expected %d finalizers, got %d", N, counter.n))
io.write(string.format("scenario 1 basic_finalized_huge_once: PASS count=%d\n", counter.n))
]]),
}

-----------------------------------------------------------------------------
-- Scenario 2: CORE -- cohort dropped, then incremental GC steps interleaved
-- with huge allocations that force hugeset resize/rehash during the rebuild
-- (HugeScan) window. This is the exact restart interleaving. Assert no
-- double-finalize and no crash.
-----------------------------------------------------------------------------
scenarios[#scenarios + 1] = {
  name = "hugeset_rehash_during_rebuild",
  code = mkcode([[
local ffi = require("ffi")
local counter = { n = 0 }
local N = 32
local keep = {}
for i = 1, N do
  keep[i] = ffi.gc(ffi.new("char[__SIZE__]"), function()
    counter.n = counter.n + 1
  end)
end
-- Drop the cohort: next sweep sends them through lj_cdata_free finalized
-- branch -> mmudata, pending finalizer.
keep = nil
collectgarbage("stop")
collectgarbage("setstepmul", 200)
collectgarbage("restart")
-- Interleave incremental steps with huge allocations to force hugeset
-- resize/rehash while HugeScan is mid-walk (the restart window).
local ring = {}
local ridx = 1
for _ = 1, 4000 do
  ring[ridx] = ffi.new("char[__SIZE__]")
  ridx = (ridx % 64) + 1
  collectgarbage("step")
end
ring = nil
collectgarbage("stop")
collectgarbage("collect")
collectgarbage("collect")
assert(counter.n == N,
  string.format("hugeset_rehash: expected %d finalizers, got %d", N, counter.n))
io.write(string.format("scenario 2 hugeset_rehash_during_rebuild: PASS count=%d\n", counter.n))
]]),
}

-----------------------------------------------------------------------------
-- Scenario 3: tombstone-clearing same-mask rehash during rebuild (Momus-3/B3
-- style). Alternate building tombstones (alloc+drop huges) then filling
-- slots to trigger resize -- with a pending finalizable huge cohort in
-- flight. Exercises the restart-skip path on a rehashed hugeset.
-----------------------------------------------------------------------------
scenarios[#scenarios + 1] = {
  name = "tombstone_rehash_during_rebuild",
  code = mkcode([[
local ffi = require("ffi")
local counter = { n = 0 }
local N = 24
local keep = {}
for i = 1, N do
  keep[i] = ffi.gc(ffi.new("char[__SIZE__]"), function()
    counter.n = counter.n + 1
  end)
end
keep = nil
collectgarbage("stop")
collectgarbage("setstepmul", 200)
collectgarbage("restart")
local phase, tick = 1, 0
local live, dropped = {}, {}
for _ = 1, 6000 do
  tick = tick + 1
  if phase == 1 then
    for i = 1, 8 do
      live[#live + 1] = ffi.new("char[__SIZE__]")
    end
    if tick % 4 == 0 then
      for i = 1, #live do dropped[i] = live[i] end
      live = {}
    end
    if tick >= 16 then phase, tick, dropped = 2, 0, {} end
  else
    live[#live + 1] = ffi.new("char[__SIZE__]")
    if tick >= 24 then phase, tick, live, dropped = 1, 0, {}, {} end
  end
  collectgarbage("step")
end
live, dropped = nil, nil
collectgarbage("stop")
collectgarbage("collect")
collectgarbage("collect")
assert(counter.n == N,
  string.format("tombstone_rehash: expected %d finalizers, got %d", N, counter.n))
io.write(string.format("scenario 3 tombstone_rehash_during_rebuild: PASS count=%d\n", counter.n))
]]),
}

-----------------------------------------------------------------------------
-- Scenario 4: large cohort across multiple cycle boundaries. 200 finalizable
-- huge cdata, dropped in batches, with sustained huge churn across cycles.
-- Catches double-link that only manifests after mmudata drains and a second
-- cycle re-encounters a stale slot.
-----------------------------------------------------------------------------
scenarios[#scenarios + 1] = {
  name = "large_cohort_multi_cycle",
  code = mkcode([[
local ffi = require("ffi")
local counter = { n = 0 }
local N = 200
local keep = {}
for i = 1, N do
  keep[i] = ffi.gc(ffi.new("char[__SIZE__]"), function()
    counter.n = counter.n + 1
  end)
end
keep = nil
collectgarbage("stop")
collectgarbage("setstepmul", 200)
collectgarbage("restart")
for c = 1, 4 do
  local ring = {}
  for j = 1, 400 do
    ring[j % 48 + 1] = ffi.new("char[__SIZE__]")
    collectgarbage("step")
  end
  collectgarbage("collect")
end
collectgarbage("stop")
collectgarbage("collect")
collectgarbage("collect")
assert(counter.n == N,
  string.format("large_cohort: expected %d finalizers, got %d", N, counter.n))
io.write(string.format("scenario 4 large_cohort_multi_cycle: PASS count=%d\n", counter.n))
]]),
}

-----------------------------------------------------------------------------
-- Driver: run each scenario in a child process for crash/assert isolation.
-----------------------------------------------------------------------------
local function write_temp(code)
  local path = assert(os.tmpname())
  local file = assert(io.open(path, "w"))
  assert(file:write(code))
  assert(file:close())
  return path
end

local function remove_temp(path)
  os.remove(path)
end

local function execute(command)
  local rc = os.execute(command)
  if type(rc) == "number" then
    return math.floor(rc / 256)
  end
  if rc == true then
    return 0
  end
  return 1
end

local function run_child(code)
  local path = write_temp(code)
  local rc = execute(string.format('%s -joff "%s"', LUAJIT, path))
  remove_temp(path)
  return rc
end

local failures, passes = 0, 0

for i = 1, #scenarios do
  local scenario = scenarios[i]
  local rc = run_child(scenario.code)
  if rc == 0 then
    passes = passes + 1
    io.write(string.format("scenario %d %s: PASS (exit %d)\n", i, scenario.name, rc))
  else
    failures = failures + 1
    io.write(string.format("scenario %d %s: FAIL (exit %d)\n", i, scenario.name, rc))
  end
end

if failures == 0 then
  print(string.format("finalized_cdata_rebuild_mark_assert: all %d scenarios PASS", passes))
  os.exit(0)
end

io.stderr:write(string.format(
  "finalized_cdata_rebuild_mark_assert: %d scenario(s) failed\n", failures))
os.exit(1)

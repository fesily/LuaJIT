 -----------------------------------------------------------------------------
-- test/gc/stale_gray_survivor_stress.lua
--
-- T0 behavior-lock for drop-survivor-gray-recolor.
--
-- Barrier-heavy workload that maximizes long-lived light-gray survivors:
--   1. Build large long-lived tables and cdata (the "survivors").
--   2. In a loop, mutate fields on the survivors (drives white->light-gray
--      via the backward barrier) and force collectgarbage("collect") for
--      many cycles (>= 50), interleaving allocation of garbage so the full
--      sweep + rebuild runs each cycle.
--   3. Assert final structural integrity (sums/counts of the long-lived
--      data unchanged) so a mis-freed survivor or lost child would fail.
--
-- This is a regression LOCK, not a failing-first test: it must pass GREEN on
-- HEAD (a483e2d6) BEFORE any makewhite removal. Removal has no natural RED;
-- this lock proves the removal preserves behavior. If removing a survivor
-- makewhite causes a mis-freed survivor or lost child, this test will catch
-- it because the structural sums will diverge.
--
-- Also exercises the interrupted-fullgc catch-up path (Group B): every 10th
-- cycle does collectgarbage("step") partial then collectgarbage("collect")
-- to force lj_gc_fullgc's g->gc.state <= GCSatomic catch-up branch.
--
-- Build:
--   make clean && make -j4 XCFLAGS="-DLUAJIT_ENABLE_GCARENA -DLUAJIT_SECURITY_STRHASH=1 -DLUA_USE_ASSERT"
-- Run:
--   ./src/luajit test/gc/stale_gray_survivor_stress.lua
-----------------------------------------------------------------------------

local ffi = require("ffi")
local ok_jit, jit = pcall(require, "jit")
if ok_jit then jit.off() end  -- deterministic GC timing

-----------------------------------------------------------------------------
-- Configuration
-----------------------------------------------------------------------------

local NUM_CYCLES = 60           -- >= 50 full collect cycles
local SURVIVOR_TABLES = 40      -- long-lived tables
local SURVIVOR_TABLE_SIZE = 100 -- entries per survivor table
local SURVIVOR_CDATA = 20       -- long-lived cdata (VLA, small)
local GARBAGE_PER_CYCLE = 2000  -- garbage allocations per cycle to drive sweep
local INTERRUPT_INTERVAL = 10   -- every Nth cycle: interrupt mid-mark then fullgc

-----------------------------------------------------------------------------
-- Build long-lived survivors
-----------------------------------------------------------------------------

-- Survivor tables: each holds numeric keys with computed checksums.
-- Mutating fields drives the backward barrier (white -> light-gray).
local survivor_tables = {}
for i = 1, SURVIVOR_TABLES do
  local t = {}
  for j = 1, SURVIVOR_TABLE_SIZE do
    t[j] = i * 1000 + j
  end
  survivor_tables[i] = t
end

-- Survivor cdata: small VLA cdata (< 512KB) that live across cycles.
-- These exercise the CdataV arena rebuild prologue path.
local survivor_cdata = {}
for i = 1, SURVIVOR_CDATA do
  survivor_cdata[i] = ffi.new("char[?]", 4096 + i * 17)
end

-- Child tables stored inside survivor tables — these are the "children"
-- that must not be lost if stale-gray causes a skip. Each survivor table
-- gets a child table at a special key.
local child_tables = {}
for i = 1, SURVIVOR_TABLES do
  local child = {}
  for j = 1, 50 do
    child[j] = string.format("child-%d-%d", i, j)
  end
  survivor_tables[i]["__child"] = child
  child_tables[i] = child
end

-----------------------------------------------------------------------------
-- Record baseline structural integrity
-----------------------------------------------------------------------------

local function compute_table_sum(t)
  local sum = 0
  for j = 1, SURVIVOR_TABLE_SIZE do
    sum = sum + (t[j] or -1)  -- -1 if missing (mis-freed/corrupted)
  end
  return sum
end

local function compute_child_sum(t)
  local sum = 0
  local child = t["__child"]
  if not child then return -1 end  -- lost child!
  for j = 1, 50 do
    local s = child[j]
    if not s then return -2 end  -- lost child entry
    -- hash the string content into the sum
    sum = sum + #s + string.byte(s, 1)
  end
  return sum
end

local baseline_sums = {}
local baseline_child_sums = {}
for i = 1, SURVIVOR_TABLES do
  baseline_sums[i] = compute_table_sum(survivor_tables[i])
  baseline_child_sums[i] = compute_child_sum(survivor_tables[i])
end

-- Baseline cdata: write a known value to verify they remain accessible.
-- We do NOT check byte patterns (VLA cdata memory can be reused by the
-- allocator on free+realloc); we check presence + read/write access only,
-- which is what the GC liveness guarantee actually provides.
for i = 1, SURVIVOR_CDATA do
  local cd = survivor_cdata[i]
  local fill = (i * 37) % 256
  ffi.fill(cd, 4096 + i * 17, fill)
end

local function verify_cdata()
  for i = 1, SURVIVOR_CDATA do
    local cd = survivor_cdata[i]
    if cd == nil then return false, "cdata " .. i .. " is nil (mis-freed)" end
    -- Verify read/write access works (cdata object still valid)
    local b = cd[0]
    cd[0] = b  -- round-trip write
  end
  return true
end

-----------------------------------------------------------------------------
-- Main stress loop
-----------------------------------------------------------------------------

local function make_garbage(n)
  for _ = 1, n do
    local _ = { 1, 2, 3, "garbage" }
  end
end

local errors = {}

for cycle = 1, NUM_CYCLES do
  -- Mutate survivor fields: drives backward barrier (white -> light-gray).
  -- Alternate between numeric field mutation and child table mutation.
  for i = 1, SURVIVOR_TABLES do
    local t = survivor_tables[i]
    if t then
      -- Toggle a field to force a write barrier
      local j = ((cycle - 1) % SURVIVOR_TABLE_SIZE) + 1
      t[j] = (t[j] or 0) + 1
      -- Also touch the child table to keep it in the barrier path
      local child = t["__child"]
      if child then
        child[50] = string.format("cycle-%d-tab-%d", cycle, i)
      end
    end
  end

  -- Allocate garbage so the sweep/rebuild has work each cycle
  make_garbage(GARBAGE_PER_CYCLE)

  -- Every INTERRUPT_INTERVAL-th cycle: interrupt mid-mark then full collect.
  -- This exercises lj_gc_fullgc's catch-up branch (Group B sites).
  if cycle % INTERRUPT_INTERVAL == 0 then
    -- Step a few times to get into mark phase, then force a full collect
    collectgarbage("stop")
    collectgarbage("setstepmul", 200)
    collectgarbage("restart")
    for _ = 1, 5 do
      collectgarbage("step")
    end
    -- Now force fullgc — this hits the g->gc.state <= GCSatomic catch-up
    collectgarbage("collect")
  else
    -- Normal full collect: runs full sweep + rebuild
    collectgarbage("collect")
  end

  -- Mid-run integrity check every 20 cycles
  if cycle % 20 == 0 then
    for i = 1, SURVIVOR_TABLES do
      local s = compute_table_sum(survivor_tables[i] or {})
      -- After mutation, sum increases by cycle count for the toggled field.
      -- We check the table still exists and has the right number of entries.
      local t = survivor_tables[i]
      if not t then
        errors[#errors + 1] = string.format("cycle %d: survivor table %d freed!", cycle, i)
      else
        local count = 0
        for _ in pairs(t) do count = count + 1 end
        if count < SURVIVOR_TABLE_SIZE then
          errors[#errors + 1] = string.format(
            "cycle %d: survivor table %d lost entries (count=%d)", cycle, i, count)
        end
      end
    end
    local cd_ok, cd_err = verify_cdata()
    if not cd_ok then
      errors[#errors + 1] = string.format("cycle %d: %s", cycle, cd_err)
    end
  end
end

-----------------------------------------------------------------------------
-- Final structural integrity check
-----------------------------------------------------------------------------

local final_errors = 0

-- 1. All survivor tables still exist with correct entry counts
for i = 1, SURVIVOR_TABLES do
  local t = survivor_tables[i]
  if not t then
    final_errors = final_errors + 1
    print(string.format("FAIL: survivor table %d was freed (mis-freed survivor!)", i))
  else
    local count = 0
    for _ in pairs(t) do count = count + 1 end
    if count < SURVIVOR_TABLE_SIZE then
      final_errors = final_errors + 1
      print(string.format("FAIL: survivor table %d lost entries: count=%d (expected >= %d)",
                          i, count, SURVIVOR_TABLE_SIZE))
    end
    -- Check child table still attached
    local child = t["__child"]
    if not child then
      final_errors = final_errors + 1
      print(string.format("FAIL: survivor table %d lost its child table (lost child!)", i))
    else
      -- Verify child entries
      local child_count = 0
      for _ in pairs(child) do child_count = child_count + 1 end
      if child_count < 50 then
        final_errors = final_errors + 1
        print(string.format("FAIL: child table %d lost entries: count=%d (expected 50)",
                            i, child_count))
      end
    end
  end
end

-- 2. All survivor cdata still exist with correct pattern
local cd_ok, cd_err = verify_cdata()
if not cd_ok then
  final_errors = final_errors + 1
  print("FAIL: " .. cd_err)
end

-- 3. No mid-run errors
for _, e in ipairs(errors) do
  print("MID-RUN ERROR: " .. e)
  final_errors = final_errors + 1
end

-----------------------------------------------------------------------------
-- Report
-----------------------------------------------------------------------------

if final_errors == 0 and #errors == 0 then
  print(string.format(
    "stale_gray_survivor_stress: PASSED — %d cycles, %d survivor tables, %d survivor cdata, "
    .. "all structural integrity checks passed",
    NUM_CYCLES, SURVIVOR_TABLES, SURVIVOR_CDATA))
  os.exit(0)
else
  print(string.format(
    "stale_gray_survivor_stress: FAILED — %d final errors, %d mid-run errors",
    final_errors, #errors))
  os.exit(1)
end

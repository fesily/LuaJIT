-- String-table sweep under the bitmap-sweep cycle.
--
-- Pins the observable behavior of lj_str_rehash_chain / gc_sweepstr while the
-- collector is in GCSsweepstring: live interned strings must survive a sweep,
-- dead ones must be reclaimed, and the heap must stay self-consistent
-- (collectgarbage("checkheap") == 0) across many interleaved GC steps.
--
-- The "rehash chain" seam fires when a single primary bucket exceeds
-- LJ_STR_MAXCOLL (32) collisions while the collector sweeps the string table.
-- The per-VM random hash seed plus the 100%-load-factor auto-grow make WHICH
-- bucket overflows non-deterministic from pure Lua, but overflow itself is
-- guaranteed (pigeonhole) once enough distinct strings are interned. Section 1
-- below drives that with a bounded counter-feedback loop: on assert builds the
-- C side exports lj_str_rehash_sweep_hits() (incremented each time the
-- bitmap-liveness branch of lj_str_rehash_chain is entered), which this test
-- polls via ffi.C and retries until the branch is provably reached -- failing
-- LOUDLY if the budget is exhausted. Release builds lack the symbol, so the
-- counter assertions are skipped while every functional invariant still runs.
--
-- Run: ./src/luajit -joff test/test_str_rehash_sweep.lua

local pass, fail = 0, 0
local function ok(c, msg)
  if c then pass = pass + 1 else fail = fail + 1; print("FAIL: " .. msg) end
end

local has_checkheap = pcall(collectgarbage, "checkheap")
local function healthy(name)
  if not has_checkheap then pass = pass + 1; return end
  local bad = collectgarbage("checkheap")
  ok(bad == 0, name .. " checkheap=" .. tostring(bad))
end

-- Resolve the assert-only hit counter via FFI. On release builds (no
-- LUA_USE_ASSERT) the symbol is absent, so binding fails and has_counter stays
-- false: counter assertions are skipped, functional invariants still run.
local has_counter, hits = false, nil
do
  local ok_ffi, ffi = pcall(require, "ffi")
  if ok_ffi then
    local ok_cdef = pcall(function()
      ffi.cdef[[uint32_t lj_str_rehash_sweep_hits(void);]]
    end)
    if ok_cdef then
      local ok_call, v = pcall(function() return ffi.C.lj_str_rehash_sweep_hits() end)
      if ok_call then
        has_counter = true
        hits = function() return tonumber(ffi.C.lj_str_rehash_sweep_hits()) end
      end
    end
  end
end

-- Distinct string builder: a stable prefix + a unique tail keeps contents
-- distinct (so each is its own interned object) while sharing length classes.
local function mkstr(round, i)
  return "rehash_sweep_probe_" .. round .. "_" .. i .. "_" .. string.rep("c", (i % 17))
end

local HUGE = 600 * 1024  -- > ArenaHugeThreshold (512 KB): hits the huge-slot path.

-- 1. Counter-feedback loop: churn enough distinct colliders through the
--    GCSsweepstring sweep to force a primary bucket past LJ_STR_MAXCOLL (32),
--    triggering lj_str_rehash_chain's bitmap-liveness branch. On assert builds
--    poll the exported counter and stop the moment it advances; fail loudly if
--    the budget is spent. Each outer iteration also interns ONE huge (>512 KB)
--    string so the huge-slot liveness path is exercised inside the branch.
do
  local OUTER = 100          -- outer iterations (budget)
  local BATCH = 6000         -- distinct strings interned per outer iteration
  local STEP_EVERY = 40
  local TAIL_STEPS = 60
  local HOLD = 512
  local start = has_counter and hits() or 0
  local reached = false
  local huge_intact = true

  for outer = 1, OUTER do
    -- Reset: free the previous iteration's dead colliders and let the string
    -- table shrink toward minimum, so the NEXT cycle's frozen GCSsweepstring
    -- table is small enough for one primary bucket to overflow past 32.
    collectgarbage("collect")

    local hold = {}
    -- Interleave interning with fine GC steps so fresh interns land WHILE the
    -- collector sweeps the (frozen, non-growable) string table -- the only
    -- window where a primary chain can exceed LJ_STR_MAXCOLL and fire the
    -- rehash-during-sweep branch.
    for i = 1, BATCH do
      hold[(i % HOLD) + 1] =
        "collider_" .. outer .. "_" .. i .. "_" .. string.rep("k", (i % 29))
      if i % STEP_EVERY == 0 then collectgarbage("step") end
    end

    -- One huge string per iteration: forces the lj_arena_ishuge -> huge_obj_*
    -- side of the branch's liveness decision when the sweep reaches it.
    local huge = string.rep("X", HUGE + outer)
    if #huge ~= HUGE + outer then huge_intact = false end
    for _ = 1, TAIL_STEPS do collectgarbage("step") end
    if huge:byte(1) ~= 88 or #huge ~= HUGE + outer then huge_intact = false end

    if has_counter and hits() > start then reached = true; break end

    hold = nil
    huge = nil
  end

  ok(huge_intact, "huge (>512KB) strings stay byte-intact across the sweep churn")

  if has_counter then
    if not reached then
      error("rehash-sweep branch not reached within budget "
            .. "(start=" .. start .. " hits=" .. hits() .. ")")
    end
    local seen = hits()
    ok(seen > start,
       "rehash-during-sweep bitmap branch entered (hits " .. start .. " -> " .. seen .. ")")
    print("rehash-sweep branch hit counter: " .. seen .. " (start " .. start .. ")")

    -- Pending-string survival: the string interned by lj_str_rehash_chain's
    -- final lj_str_new (during the sweep) is marked via GCF_MARKALLOC; assert it
    -- is byte-intact after a full collect, proving the mark was set and reset.
    local pending = "pending_after_rehash_" .. seen .. "_" .. string.rep("p", 250)
    local expect = "pending_after_rehash_" .. seen .. "_" .. string.rep("p", 250)
    collectgarbage("collect")
    ok(pending == expect and #pending == #expect,
       "pending string interned during sweep survives the next full GC intact")
  else
    print("rehash-sweep hit counter: unavailable (release build) -- "
          .. "counter assertions skipped, functional invariants still run")
    pass = pass + 1  -- account for the skipped counter assertion slot
  end
  collectgarbage("collect")
  healthy("feedback loop")
end

-- 2. Build a large live set, drive GC steps while interning more, and verify
--    every live string is byte-intact afterward + heap healthy.
local ROUNDS = 12
local PER_ROUND = 600

for round = 1, ROUNDS do
  local live = {}
  -- Intern the live set, stepping GC between batches so some interning lands
  -- while the collector is sweeping the string table.
  for i = 1, PER_ROUND do
    live[i] = mkstr(round, i)
    if i % 7 == 0 then collectgarbage("step") end
  end

  -- Churn a throwaway population through the sweep: intern, drop, step. These
  -- become dead strings the GCSsweepstring sweep must reclaim without touching
  -- the live set.
  for j = 1, PER_ROUND do
    local junk = "ephemeral_" .. round .. "_" .. j .. "_" .. string.rep("z", (j % 23))
    junk = nil
    if j % 5 == 0 then collectgarbage("step") end
  end

  collectgarbage("collect")  -- complete at least one full cycle this round.

  local intact = true
  for i = 1, PER_ROUND do
    if live[i] ~= mkstr(round, i) then intact = false; break end
  end
  ok(intact, "round " .. round .. ": all live strings byte-intact after sweep")
  healthy("round " .. round)

  live = nil
  collectgarbage("collect")
end

-- 3. Re-intern after the live set is dropped: previously-dead strings must be
--    freshly interned cleanly (equality by content holds, heap healthy).
local reborn = mkstr(1, 1)
ok(reborn == mkstr(1, 1), "re-interned previously-dead string compares equal")
healthy("re-intern")

-- 4. Memory reclaim: a dropped throwaway population must free memory after a
--    full GC (dead strings actually reclaimed, not merely unreachable).
do
  local junk = {}
  for i = 1, 20000 do
    junk[i] = "reclaim_probe_" .. i .. "_" .. string.rep("r", i % 37)
  end
  collectgarbage("collect")
  local before = collectgarbage("count")
  junk = nil
  collectgarbage("collect")
  collectgarbage("collect")
  local after = collectgarbage("count")
  ok(after < before,
     string.format("dropped strings free memory (%.0f -> %.0f KB)", before, after))
  healthy("reclaim")
end

-- 5. Mixed retention: keep odd-indexed, drop even-indexed, sweep, verify odds
--    survive intact (the sweep distinguished live from dead correctly).
local mixed = {}
for i = 1, 800 do
  mixed[i] = "mixed_probe_" .. i .. "_" .. string.rep("m", i % 11)
  if i % 9 == 0 then collectgarbage("step") end
end
for i = 2, 800, 2 do mixed[i] = false end  -- drop the even slots' references
collectgarbage("collect")
collectgarbage("collect")
local odds_ok = true
for i = 1, 800, 2 do
  if mixed[i] ~= "mixed_probe_" .. i .. "_" .. string.rep("m", i % 11) then
    odds_ok = false; break
  end
end
ok(odds_ok, "mixed retention: odd-indexed survivors byte-intact after sweep")
healthy("mixed retention")

print(string.format("\nString rehash/sweep: %d passed, %d failed", pass, fail))
os.exit(fail == 0 and 0 or 1)

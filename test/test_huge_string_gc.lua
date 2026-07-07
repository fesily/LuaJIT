-- Huge-string GC exercise: drives the migrated hugeset slot-mark path for
-- strings > ArenaHugeThreshold (512 KB). Forces:
--   * mark of a live huge string (gc_mark_str -> huge_obj_setmark)
--   * gc_sweepstr huge-string live branch (huge_obj_ismarked survivor)
--   * gc_rebuild_rootchain ~LJ_TSTR per-cycle clear (huge_obj_clearmark)
--   * gc_arena_verify GCSpause assert covering huge strings
--   * eventual free of a dropped huge string
-- Run: ./src/luajit -joff test/test_huge_string_gc.lua

local HUGE = 600 * 1024          -- ~614 KB > 512 KB ArenaHugeThreshold
local pass, fail = 0, 0
local function ok(c, msg) if c then pass = pass + 1 else fail = fail + 1; print("FAIL: "..msg) end end

-- 1. Allocate a huge string, keep it live across several full GCs.
local s = string.rep("x", HUGE)
ok(#s == HUGE, "huge string allocated at expected length")
for i = 1, 5 do
  collectgarbage("collect")          -- full cycle: mark survivor, sweep keeps, rebuild clears slot
  ok(#s == HUGE and s:byte(1) == 120, "huge string survives full GC round "..i)
end

-- 2. Allocate huge strings mid-churn so some land during the sweep window,
--    exercising the GCF_MARKALLOC huge slot-mark (FIX A) + already-swept bucket.
local keep = {}
for i = 1, 20 do
  keep[i] = string.rep(string.char(65 + (i % 26)), HUGE + i)  -- distinct contents/lengths
  collectgarbage("step")             -- interleave GC steps with huge-string allocs
end
collectgarbage("collect")
local allgood = true
for i = 1, 20 do
  if #keep[i] ~= HUGE + i then allgood = false end
end
ok(allgood, "20 churned huge strings all intact after full GC")

-- 3. Drop everything, full GC, ensure no crash/leak and memory reclaimed.
local before = collectgarbage("count")
s = nil
keep = nil
collectgarbage("collect")
collectgarbage("collect")
local after = collectgarbage("count")
ok(after < before, string.format("memory reclaimed after dropping huge strings (%.0f -> %.0f KB)", before, after))

-- 4. Re-allocate to confirm the freed huge slots are reusable.
local s2 = string.rep("z", HUGE)
ok(#s2 == HUGE, "huge string re-allocated after free")
s2 = nil
collectgarbage("collect")

print(string.format("\nHuge-string GC: %d passed, %d failed", pass, fail))
os.exit(fail == 0 and 0 or 1)

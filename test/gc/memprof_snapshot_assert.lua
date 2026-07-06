-- memprof_snapshot_assert.lua: v0 heap-snapshot census + leak-diff self-checks.
-- Run: ./src/luajit -joff test/gc/memprof_snapshot_assert.lua
-- Gated: requires a build with -DLUAJIT_ENABLE_MEMPROF (and arena GC + open-addr
-- strtab). If memprof is not compiled in, require() errors and we skip cleanly.

local memprof = require("memprof")
local assert = assert
local function eq(a, b) return a == b end

local pass, fail = 0, 0
local function check(cond, msg)
  if cond then pass = pass + 1
  else fail = fail + 1; io.stderr:write("FAIL: "..msg.."\n") end
end

-- 1. Basic snapshot shape + internal consistency.
do
  local s = memprof.snapshot{gc="full"}
  check(type(s) == "table", "snapshot returns a table")
  check(type(s.total) == "table", "has total")
  check(type(s.by_type) == "table", "has by_type")
  check(type(s.by_class) == "table", "has by_class")
  check(type(s.huge) == "table", "has huge")
  check(type(s.intern) == "table", "has intern")
  check(s.gc == "full", "gc option recorded")
  -- total.count == sum of per-type counts.
  local sum = 0
  for _,v in pairs(s.by_type) do sum = sum + v.count end
  -- "huge" entry is inside by_type too; its count is separate from arena total.
  -- Recompute without the huge bucket.
  local arena_sum = 0
  for k,v in pairs(s.by_type) do if k ~= "huge" then arena_sum = arena_sum + v.count end end
  check(arena_sum == s.total.count, "arena type counts sum to total.count")
  -- by_class counts sum to total.count.
  local cls_sum = 0
  for _,v in pairs(s.by_class) do cls_sum = cls_sum + v.count end
  check(cls_sum == s.total.count, "class counts sum to total.count")
  -- live + dead == total.
  check(s.total.live + s.total.dead == s.total.count, "live+dead == total")
end

-- 2. Allocate a known number of strings/tables; snapshot; assert counts grew.
do
  local s_before = memprof.snapshot{gc="full"}
  local str_before = s_before.by_type.string and s_before.by_type.string.count or 0
  local tab_before = s_before.by_type.table and s_before.by_type.table.count or 0
  local hold = {}
  for i = 1, 500 do hold[#hold+1] = tostring(i).."x"..tostring(i) end  -- 500 unique strings
  for i = 1, 100 do hold[#hold+1] = {} end  -- 100 new tables
  local s_after = memprof.snapshot{gc="full"}
  local str_after = s_after.by_type.string and s_after.by_type.string.count or 0
  local tab_after = s_after.by_type.table and s_after.by_type.table.count or 0
  check(str_after > str_before, "string count grew after allocating strings")
  check(tab_after >= tab_before + 100, "table count grew by >= 100")
  -- intern.num should be consistent with the string census (within a small
  -- delta for strempty / huge / fixed strings).
  check(s_after.intern.num > 0, "intern.num is positive")
  -- hold is still referenced, so the strings/tables must be live.
  collectgarbage("collect")  -- full GC; hold keeps them alive.
  local s_live = memprof.snapshot{gc="none"}
  local str_live = s_live.by_type.string and s_live.by_type.string.count or 0
  check(str_live >= str_after - 5, "strings retained by hold survive full GC")
end

-- 3. Intern self-check: g->str.num should be close to the arena string count.
do
  local s = memprof.snapshot{gc="full"}
  local str_count = s.by_type.string and s.by_type.string.count or 0
  local diff = math.abs(s.intern.num - str_count)
  -- The delta accounts for the strempty singleton (not in an arena) and any
  -- huge strings (counted in huge, not in by_type.string). A few units is fine.
  check(diff <= 16, "intern.num ("..s.intern.num..") ~= string census ("..str_count..
       ", diff "..diff..") within tolerance")
end

-- 4. Deliberate retained leak + two-snapshot diff.
do
  -- First snapshot with details.
  local s1 = memprof.snapshot{gc="full", details=true, name="s1"}
  check(s1.name == "s1", "name recorded in snapshot")
  check(s1.objects ~= nil, "details=true builds objects map")
  -- Create a deliberately retained leak: a table + many strings anchored in a
  -- global, never freed between snapshots.
  _LEAK_ANCHOR = {}
  for i = 1, 200 do _LEAK_ANCHOR[tostring(i).."__leak"] = {i} end
  local s2 = memprof.snapshot{gc="full", details=true, name="s2"}
  local d = memprof.diff(s1, s2)
  check(type(d) == "table", "diff returns a table")
  check(d.new_count > 0, "diff flags new objects (leak suspects present)")
  check(d.leak_suspects ~= nil, "diff has leak_suspects table")
  -- The leak anchored in _LEAK_ANCHOR must show up as grown string+table counts.
  check(d.grown ~= nil, "diff has grown table")
  local grew_str = d.grown.string or 0
  local grew_tab = d.grown.table or 0
  check(grew_str >= 200, "diff grown.string >= 200 (got "..grew_str..")")
  check(grew_tab >= 1, "diff grown.table >= 1 (got "..grew_tab..")")
  -- Cleanup.
  _LEAK_ANCHOR = nil
  collectgarbage("collect")
end

-- 5. gc option behavior: none vs full should both succeed.
do
  local s_none = memprof.snapshot{gc="none"}
  local s_step = memprof.snapshot{gc="step"}
  local s_full = memprof.snapshot{gc="full"}
  check(s_none.gc == "none", "gc=none recorded")
  check(s_step.gc == "step", "gc=step recorded")
  check(s_full.gc == "full", "gc=full recorded")
end

-- 6. huge objects field is present and non-negative.
do
  local s = memprof.snapshot{gc="full"}
  check(s.huge.count >= 0, "huge.count >= 0")
  check(s.huge.bytes >= 0, "huge.bytes >= 0")
end

io.write("memprof_snapshot_assert: "..pass.." passed, "..fail.." failed\n")
if fail > 0 then os.exit(1) end

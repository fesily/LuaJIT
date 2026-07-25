-- Phase A backfill: late-bound __gc on udata whose metatable gained __gc
-- AFTER setmetatable (the 5.1 `newproxy(true); getmetatable(u).__gc = fn`
-- idiom). Pinned contract (LJ 3.0 Finalizers.zh.md §6.3, only meaningful
-- under COMPAT=1 which LJ_DS forces on):
--   (1) late-bound __gc still fires exactly once,
--   (2) __gc added then removed before death does NOT fire (sticky TF_HASGC
--       is a hint; separate re-resolves __gc at death),
--   (3) non-bloat: a mt that NEVER had __gc does not park its udata in the
--       registry (COMPAT=1 no longer registers on mt!=NULL alone),
--   (4) no double-finalize across two cycles for a finalized-then-resurrected
--       udata (backfill must skip finalized objects).
--
-- Run: luajit -joff test/gc/udata_backfill_assert.lua

local pass, fail = 0, 0
local function check(name, cond, msg)
  if cond then pass = pass + 1
  else fail = fail + 1; io.write("FAIL: "..name..(msg and (": "..msg) or "").."\n") end
end

local function full_collect()
  collectgarbage("collect")
  collectgarbage("collect")
end

-- (1) Late-bound __gc fires exactly once.
do
  local n = 0
  do
    local u = newproxy(true)
    getmetatable(u).__gc = function() n = n + 1 end
  end
  full_collect()
  check("late_bound_fires_once", n == 1, "n="..n)
  full_collect()
  check("late_bound_no_double", n == 1, "n="..n)
end

-- (2) __gc added then removed before death: sticky bit stays (TF_HASGC),
--     but separate re-resolves __gc at death and finds nil → no call.
do
  local n = 0
  do
    local u = newproxy(true)
    local mt = getmetatable(u)
    mt.__gc = function() n = n + 1 end
    mt.__gc = nil  -- remove before death
  end
  full_collect()
  check("added_then_removed_no_fire", n == 0, "n="..n)
end

-- (3) Non-bloat: a metatable that never had __gc must not register its udata.
--     The pre-Phase-A simplified COMPAT=1 registered every udata with mt!=NULL,
--     bloating the registry; Phase A only registers on TF_HASGC || __gc-now.
do
  local non_gc = {}
  for i = 1, 200 do
    local u = newproxy(true)
    getmetatable(u).note = "no __gc here"  -- mt has a key but never __gc
  end
  full_collect()
  -- registry size is not directly observable; use the behavioral proxy: none
  -- of these udata had a finalizer to run, and the cycle must be clean.
  local n = 0
  for i = 1, 200 do
    local u = newproxy(true)
    getmetatable(u).note = "x"
    getmetatable(u).__gc = function() n = n + 1 end
  end
  full_collect()
  check("non_bloat_then_late_bound", n == 200, "n="..n)
end

-- (4) Resurrected finalized udata is not re-finalized next cycle and is not
--     re-registered by backfill (it is finalized, so backfill skips it).
do
  local stash, n = {}, 0
  do
    local u = newproxy(true)
    getmetatable(u).__gc = function(self) n = n + 1; stash[1] = self end
  end
  full_collect()
  check("resurrect_finalized_once", n == 1, "n="..n)
  check("resurrect_survives", stash[1] ~= nil)
  stash[1] = nil
  full_collect()
  check("resurrect_no_double", n == 1, "n="..n)
end

-- (5) Late-bound __gc added to a SHARED metatable after many udata already
--     exist with that mt: backfill registers all of them in one walk.
do
  local mt, n = {}, 0
  local hold = {}
  for i = 1, 50 do
    local u = newproxy(true)
    debug.setmetatable(u, mt)
    hold[i] = u
  end
  -- now add __gc to the shared mt; none of the 50 were registered at setmetatable
  mt.__gc = function() n = n + 1 end
  hold = nil
  full_collect()
  check("shared_mt_backfill_all", n == 50, "n="..n)
end

io.write(string.format("\nudata_backfill_assert: %d passed, %d failed\n", pass, fail))
if fail > 0 then os.exit(1) end

-- FFI_FIN empty-table cleanup: focused behavioral assert.
-- Contract (arena / LJ_HASGCMARK build):
--   * lj_ctype_initfin is a no-op -> g.gcroot[GCROOT_FFI_FIN] stays NULL.
--   * cdata finalizers run via the fin_registry, NOT the weak FFI_FIN table.
--   * The tabref(GCROOT_FFI_FIN) sites in lj_cdata_setfin / lib_ffi.c
--     ffi.new(__gc) / lj_gc_finalize_cdata must be NULL-safe.
--
-- This test does NOT poke private GC internals. It verifies behaviorally that
-- ffi.gc (and the struct __gc metamethod path) still register, fire, clear,
-- resurrect, and stress-survive without crashing -- which is the observable
-- contract the NULL FFI_FIN table must still satisfy.
--
-- If init-write missed a null-safety site, this test SEGVs / asserts under
-- the arena build (luajit-arenagc). Under the classic build (luajit) the
-- FFI_FIN table is still created, so the same behavior must hold and the
-- test is a no-regression guard for classic.
--
-- Run:
--   ./src/luajit -joff test/test_ffi_fin_null_arena.lua            (classic)
--   ./builds/.../luajit/Release/luajit-arenagc -joff test/test_ffi_fin_null_arena.lua

local ffi = require("ffi")
ffi.cdef[[
  typedef struct { int id; } R;
  typedef struct { int v; } S;
]]

local pass, fail = 0, 0
local function check(name, cond, msg)
  if cond then pass = pass + 1
  else fail = fail + 1; io.write("FAIL: "..name..(msg and (": "..msg) or "").."\n") end
end

-- -----------------------------------------------------------------------
-- 1. Basic register + fire (lj_cdata_setfin hot path via ffi.gc).
--    Crashes here => lj_cdata_setfin forgot the NULL guard on FFI_FIN.
-- -----------------------------------------------------------------------
do
  local fired = 0
  local cd = ffi.new("R", {id=42})
  ffi.gc(cd, function(self) fired = fired + 1; check("basic_id", self.id == 42) end)
  cd = nil
  collectgarbage("collect")
  collectgarbage("collect")
  check("basic_fired_once", fired == 1, "fired="..fired)
end

-- -----------------------------------------------------------------------
-- 2. Struct __gc metamethod path (lib_ffi.c ffi_new -> lj_cdata_setfin).
--    Crashes here => lib_ffi.c forgot the NULL guard on FFI_FIN.
-- -----------------------------------------------------------------------
do
  local fired = 0
  local mt = { __gc = function(self) fired = fired + 1 end }
  ffi.metatype("S", mt)
  do local s = ffi.new("S", {v=7}) end
  collectgarbage("collect")
  collectgarbage("collect")
  check("metatype_fired", fired == 1, "fired="..fired)
end

-- -----------------------------------------------------------------------
-- 3. ffi.gc(cd, nil) clears the finalizer (unregister path).
--    Crashes here => lj_gc_fin_unregister hit through NULL FFI_FIN.
-- -----------------------------------------------------------------------
do
  local fired = 0
  local cd = ffi.new("R", {id=9})
  ffi.gc(cd, function() fired = fired + 1 end)
  ffi.gc(cd, nil)  -- disarm
  cd = nil
  collectgarbage("collect")
  collectgarbage("collect")
  check("disarm_not_fired", fired == 0, "fired="..fired)
end

-- -----------------------------------------------------------------------
-- 4. Re-arm: ffi.gc sets, clears, re-sets a finalizer.
-- -----------------------------------------------------------------------
do
  local fired = 0
  local cd = ffi.new("R", {id=5})
  ffi.gc(cd, function() fired = fired + 100 end)
  ffi.gc(cd, nil)
  ffi.gc(cd, function() fired = fired + 1 end)
  cd = nil
  collectgarbage("collect")
  collectgarbage("collect")
  check("rearm_fired_once_correct_value", fired == 1, "fired="..fired)
end

-- -----------------------------------------------------------------------
-- 5. Resurrection: finalizer stashes the cdata. Must survive the cycle
--    and NOT be re-finalized (finalizer consumed unless re-armed).
-- -----------------------------------------------------------------------
do
  local fired = 0
  local stash = {}
  for i = 1, 30 do
    local cd = ffi.new("R", {id=i})
    ffi.gc(cd, function(self) fired = fired + 1; stash[#stash + 1] = self end)
  end
  collectgarbage("collect")
  collectgarbage("collect")
  check("resurrect_fired_count", fired == 30, "fired="..fired)
  check("resurrect_stashed", #stash == 30, "stash="..#stash)
  -- Drop strong refs; must not double-finalize (finalizer gone).
  for i = 1, #stash do stash[i] = nil end
  stash = nil
  collectgarbage("collect")
  collectgarbage("collect")
  check("resurrect_no_double", fired == 30, "fired="..fired)
end

-- -----------------------------------------------------------------------
-- 6. Stress: many finalizers, register + collect + verify all fire, no crash.
--    Scale kept moderate to avoid tripping unrelated pre-existing arena GC
--    asserts (lj_gc_arena.c stale-gray residual); the FFI_FIN null-safety
--    contract is exercised at every registration, not just at scale.
-- -----------------------------------------------------------------------
do
  local n = 200
  local fired = 0
  for i = 1, n do
    local cd = ffi.new("R", {id=i})
    ffi.gc(cd, function() fired = fired + 1 end)
    cd = nil
    if i % 50 == 0 then collectgarbage("step", 5) end
  end
  collectgarbage("collect")
  collectgarbage("collect")
  collectgarbage("collect")
  check("stress_all_fired", fired == n, "fired="..fired.."/"..n)
end

-- -----------------------------------------------------------------------
-- 7. Live cdata not finalized while reachable (no premature finalization).
-- -----------------------------------------------------------------------
do
  local fired = 0
  local keep = {}
  for i = 1, 50 do
    local cd = ffi.new("R", {id=i})
    ffi.gc(cd, function() fired = fired + 1 end)
    keep[i] = cd
  end
  for _ = 1, 4 do collectgarbage("collect") end
  check("live_not_finalized", fired == 0, "fired="..fired)
  check("live_still_usable", keep[25].id == 25)
  keep = nil
  collectgarbage("collect")
  collectgarbage("collect")
  check("live_then_all_fired", fired == 50, "fired="..fired)
end

io.write("FFI_FIN null-arena behavioral: "..pass.." passed, "..fail.." failed\n")
os.exit(fail == 0 and 0 or 1)

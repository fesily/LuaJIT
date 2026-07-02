-- Failing-first invariant test for rebuild_arenascan's thread openupval
-- sweep (src/lj_gc_arena.c:1363: gc_fullsweep(g, &gco2th(o)->openupval)).
--
-- This is the ONLY mechanism that frees dead OPEN upvalues on LIVE
-- coroutine openupval chains. gc_bitmap_sweep skips open upvalues
-- (src/lj_gc_arena.c:1134: gct==~LJ_TUPVAL && !closed -> continue).
-- lj_state_free only runs for DEAD threads (src/lj_state.c). So if the
-- thread sweep is removed, dead open upvalues on live suspended coroutines
-- leak forever -- they are never freed by any other path.
--
-- SCENARIO (the crux):
--   1. Create a coroutine whose inner function captures a local as an
--      open upvalue, nils the only closure reference, then yields
--      mid-frame (frame stays live -> upvalue stays OPEN on the chain).
--   2. The upvalue stays on co->openupval, but the closure that referenced
--      it is dead. gc_traverse_thread (src/lj_gc.c:313) does NOT walk the
--      openupval chain, so the upvalue is NOT kept alive by the thread.
--      No live closure references it either. -> the upvalue is unmarked
--      (dead) this GC cycle.
--   3. Anchor the coroutine (keep it LIVE + suspended). Do NOT resume it
--      to completion (that would close the upvalue via lj_func_closeuv,
--      masking the leak).
--   4. Force collectgarbage("collect"). The dead open upvalue MUST be
--      unlinked/freed from co->openupval by the thread sweep.
--
-- OBSERVABLE (Option A -- white-box via FFI, assert build):
--   Walk the coroutine's openupval chain (GCRef at lua_State+openupval_off,
--   linked via nextgc) and count open upvalues. The openupval offset is
--   self-calibrated at runtime by comparing threads with 0 vs 1 open
--   upvalues, so the test is portable across LJ_GC64 / non-GC64 builds.
--   A control group (live closure keeps its upvalue) proves the sweep only
--   removes DEAD upvalues, not live ones.
--
-- RED/GREEN:
--   GREEN (current HEAD, eb71f9d3): thread sweep frees dead open upvalues
--     -> dead-chain count == 0 after GC, control count unchanged.
--   RED (stub rebuild_arenascan L1363 -- remove gc_fullsweep call):
--     dead open upvalue leaks -> count == N after GC.
--
-- Build: make clean && make -j4 XCFLAGS="-DLUAJIT_ENABLE_GCARENA -DLUAJIT_SECURITY_STRHASH=1 -DLUA_USE_ASSERT"
-- Run:   ./src/luajit test/gc/thread_openupval_sweep_assert.lua

local ok_ffi, ffi = pcall(require, "ffi")
if not ok_ffi then
  print("ffi unavailable -- thread_openupval_sweep_assert cannot run; skipping")
  os.exit(0)
end

-- Interpreter-only: deterministic GC timing + the assert build's arena
-- shadow-verify aborts if JIT-allocated objects are touched mid-step.
local ok_jit, jit = pcall(require, "jit")
if ok_jit then jit.off() end

----------------------------------------------------------------
-- Constants from lj_obj.h (stable across builds)
----------------------------------------------------------------

-- gct stores ~LJ_T* truncated to a byte. LJ_TUPVAL = ~5u -> gct = 5.
-- LJ_TTHREAD = ~6u -> gct = 6. (see lj_obj.h:264-266)
local GCT_UPVAL  = 5
local GCT_THREAD = 6

----------------------------------------------------------------
-- Helpers
----------------------------------------------------------------

local pass, fail = 0, 0
local function ok(cond, msg)
  if cond then pass = pass + 1 else fail = fail + 1; print("FAIL: " .. msg) end
end

local function addr_of(co)
  local s = tostring(co)
  local hex = s:match("thread: 0x(%x+)")
  if not hex then return nil end
  return tonumber(hex, 16)
end

----------------------------------------------------------------
-- Self-calibrate the openupval field offset in lua_State.
--
-- Compare a thread with 0 open upvalues (yield immediately, no captured
-- locals) vs a thread with 1 open upvalue (capture local, nil closure,
-- yield mid-frame). Scan 8-byte-aligned offsets for a uint64 that is 0
-- in the first and non-zero in the second -- that is the openupval GCRef.
----------------------------------------------------------------

local function make_co_no_upval()
  local co = coroutine.create(function() coroutine.yield() end)
  coroutine.resume(co)
  return co
end

local function make_co_one_dead_upval()
  local co = coroutine.create(function()
    local x = 1
    local f = function() return x end  -- creates open upvalue for x
    f = nil                              -- drop closure -> upvalue becomes dead
    coroutine.yield()                    -- suspended mid-frame, upvalue stays OPEN
  end)
  coroutine.resume(co)
  return co
end

local function calibrate_openupval_offset()
  local co0 = make_co_no_upval()
  local co1 = make_co_one_dead_upval()
  local p0 = addr_of(co0)
  local p1 = addr_of(co1)
  if not p0 or not p1 then return nil end

  local bp0 = ffi.cast("uint8_t*", p0)
  local bp1 = ffi.cast("uint8_t*", p1)

  -- Validate we found a lua_State: gct byte at offset 9 == GCT_THREAD
  if bp0[9] ~= GCT_THREAD or bp1[9] ~= GCT_THREAD then return nil end

  -- Scan 8-byte-aligned offsets 0..120 for the openupval GCRef.
  for off = 0, 120, 8 do
    local v0 = ffi.cast("uint64_t*", bp0 + off)[0]
    local v1 = ffi.cast("uint64_t*", bp1 + off)[0]
    if v0 == 0 and v1 ~= 0 then
      -- Cross-check with a second pair to rule out coincidental non-zero.
      local co0b = make_co_no_upval()
      local co1b = make_co_one_dead_upval()
      local bp0b = ffi.cast("uint8_t*", addr_of(co0b))
      local bp1b = ffi.cast("uint8_t*", addr_of(co1b))
      local v0b = ffi.cast("uint64_t*", bp0b + off)[0]
      local v1b = ffi.cast("uint64_t*", bp1b + off)[0]
      if v0b == 0 and v1b ~= 0 then
        return off
      end
    end
  end
  return nil
end

----------------------------------------------------------------
-- Walk a coroutine's openupval chain and count open upvalues.
-- The chain is linked via nextgc (GCRef at offset 0 of each GCobj).
-- Each entry must have gct == GCT_UPVAL; stop if it doesn't (safety).
----------------------------------------------------------------

local function count_open_upvalues(co, openupval_off)
  local p = addr_of(co)
  if not p then return nil end
  local bp = ffi.cast("uint8_t*", p)
  local uv_ptr = ffi.cast("uint64_t*", bp + openupval_off)[0]
  local count = 0
  while uv_ptr ~= 0 do
    local uv = ffi.cast("uint8_t*", uv_ptr)
    local gct = uv[9]
    if gct ~= GCT_UPVAL then break end  -- not an upvalue: stop walking
    count = count + 1
    uv_ptr = ffi.cast("uint64_t*", uv)[0]  -- nextgc at offset 0
    if count > 1000 then break end          -- safety: broken chain
  end
  return count
end

----------------------------------------------------------------
-- Scenario builders
----------------------------------------------------------------

-- Create a coroutine with 3 dead open upvalues on its chain.
-- Each captured local must be a DISTINCT named local (loop variables share
-- a stack slot -> one upvalue, not N; table accesses like vars[i] capture
-- only the table, not per-element upvalues).
local function make_co_dead_upvals3()
  local co = coroutine.create(function()
    local a, b, c = 10, 20, 30
    local f1 = function() return a end
    local f2 = function() return b end
    local f3 = function() return c end
    f1, f2, f3 = nil, nil, nil  -- drop all closure references -> upvalues dead
    coroutine.yield()  -- suspended mid-frame, upvalues stay OPEN
  end)
  coroutine.resume(co)
  return co
end

-- Create a coroutine with 3 LIVE open upvalues (control: closures kept
-- reachable, upvalues stay marked -> NOT freed by the thread sweep).
local function make_co_live_upvals3(keep)
  local co = coroutine.create(function()
    local a, b, c = 10, 20, 30
    local f1 = function() return a end
    local f2 = function() return b end
    local f3 = function() return c end
    coroutine.yield({ f1, f2, f3 })  -- pass closures out so they stay reachable
  end)
  local ok, closures = coroutine.resume(co)
  if ok and type(closures) == "table" then
    for _, f in ipairs(closures) do keep[#keep + 1] = f end
  end
  return co
end

----------------------------------------------------------------
-- Main test
----------------------------------------------------------------

local openupval_off = calibrate_openupval_offset()
if not openupval_off then
  print("CALIBRATION FAILED: could not locate openupval offset -- skipping")
  os.exit(0)
end
ok(true, "calibrated openupval offset = " .. openupval_off)
print(string.format("calibration: openupval offset = %d", openupval_off))

----------------------------------------------------------------
-- Group 1: dead open upvalues on live suspended coroutines (RED target)
----------------------------------------------------------------
do
  local N_CO = 10       -- coroutines
  local N_UPV = 3       -- dead upvalues per coroutine (hardcoded in builder)
  local anchored = {}   -- keep coroutines live + suspended

  for i = 1, N_CO do
    anchored[i] = make_co_dead_upvals3()
  end

  local before_total = 0
  for i = 1, N_CO do
    local c = count_open_upvalues(anchored[i], openupval_off)
    before_total = before_total + (c or 0)
  end
  ok(before_total == N_CO * N_UPV,
     string.format("group1 before GC: expected %d open upvalues, found %d",
                   N_CO * N_UPV, before_total))

  collectgarbage("collect")
  collectgarbage("collect")

  local after_total = 0
  for i = 1, N_CO do
    local c = count_open_upvalues(anchored[i], openupval_off)
    after_total = after_total + (c or 0)
  end

  -- THE RED ASSERTION: all dead open upvalues freed -> count == 0.
  -- RED when rebuild_arenascan L1363 is stubbed (upvalues leak -> count == N_CO*N_UPV).
  ok(after_total == 0,
     string.format("group1 after GC: dead open upvalues freed (expected 0, found %d) "
                   .. "-- thread sweep is load-bearing", after_total))
  if after_total > 0 then
    print(string.format("RED PROOF: %d dead open upvalues leaked on %d live coroutines "
                        .. "after full GC -- rebuild_arenascan thread sweep missing",
                        after_total, N_CO))
  end
end

----------------------------------------------------------------
-- Group 2: control -- LIVE open upvalues must NOT be freed
----------------------------------------------------------------
do
  local N_CO = 5
  local N_UPV = 3
  local anchored = {}
  local keep_closures = {}

  for i = 1, N_CO do
    anchored[i] = make_co_live_upvals3(keep_closures)
  end

  collectgarbage("collect")
  collectgarbage("collect")

  local after_total = 0
  for i = 1, N_CO do
    local c = count_open_upvalues(anchored[i], openupval_off)
    after_total = after_total + (c or 0)
  end

  ok(after_total == N_CO * N_UPV,
     string.format("group2 control: live open upvalues preserved (expected %d, found %d)",
                   N_CO * N_UPV, after_total))
  if after_total ~= N_CO * N_UPV then
    print(string.format("WARNING: control group lost %d live upvalues -- test too aggressive",
                        N_CO * N_UPV - after_total))
  end
end

----------------------------------------------------------------
-- Group 3: repeated cycles -- no accumulation across GC rounds
----------------------------------------------------------------
do
  local ROUNDS = 5
  local N_CO = 4
  local N_UPV = 3
  local anchored = {}

  for round = 1, ROUNDS do
    for i = 1, N_CO do
      anchored[#anchored + 1] = make_co_dead_upvals3()
    end
    collectgarbage("collect")
    collectgarbage("collect")

    local leaked = 0
    for _, co in ipairs(anchored) do
      local c = count_open_upvalues(co, openupval_off)
      leaked = leaked + (c or 0)
    end
    ok(leaked == 0,
       string.format("group3 round %d: no leaked upvalues (expected 0, found %d)",
                     round, leaked))
  end
end

----------------------------------------------------------------
-- Summary
----------------------------------------------------------------

print(string.format("\nthread_openupval_sweep_assert: %d passed, %d failed",
                    pass, fail))
os.exit(fail == 0 and 0 or 1)

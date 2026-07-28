-- Permanent-gray THREAD residual allowlist assert suite (pure Lua + FFI).
--
-- Locks Wave 1 of the arenagc-thread-permgray plan (todos 1 & 2):
--   * Arena coroutine THREADs end mark∧GRAY after first propagatemark
--     (black2gray after shared gray2black) and stay gray for the whole
--     cycle until ClearMarks / next-cycle makewhite.
--   * Residual free-entry invariant (gc_assert_atomic_end) allows
--     mark∧GRAY for open UV AND arena THREAD only; mainthread stays
--     ¬GRAY; closed UV / tables / funcs / cdata must still be pure black.
--
-- Why the assert build is the oracle:
--   gc_assert_atomic_end (lj_gc_markalloc_debug.h) walks every arena +
--   huge cell at the atomic→sweep boundary and aborts on any mark∧GRAY
--   survivor that is NOT on the dual allowlist (open UV skip-edge-walk,
--   THREAD skip-assert-but-edge-walk). A live coroutine IS mark∧GRAY at
--   that instant (propagatemark set GRAY via black2gray and no makewhite
--   has run yet — sweep runs makewhite only after the assert). So:
--     * if the allowlist correctly includes THREAD -> full GC completes.
--     * if the allowlist reverts to open-UV-only while THREAD stays gray
--       -> the assert build aborts with "mark∧GRAY at atomic→sweep:
--       gct=6 ...". Running this test under the assert arena binary IS
--       the positive+negative proof (no abort = allowlist correct).
--   The transient mark∧GRAY window at atomic→sweep is not reliably
--   catchable by Lua-side polling (small heaps finish mark+atomic+sweep
--   inside one collectgarbage step). After free, permanent-gray THREAD may
--   still show header GRAY as light-gray residual (mark bitmap cleared).
--   The assert build no-abort is the stable residual oracle; survival of
--   anchored coroutines is the liveness oracle.
--
-- Cases:
--   P1  live coroutines through full GC under assert build: no residual
--       abort (THREAD mark∧GRAY allowlisted). Post-GC light-gray residual
--       (header GRAY without mark) is allowed for permanent-gray THREAD.
--   P2  open UV residual still allowlisted (closure keeps UV open on a
--       suspended anchored coroutine).
--   P3  closed UV + non-weak table NOT residual-gray: FFI post-GC header
--       GRAY bit clear (pure black after sweep makewhite); no abort.
--   P4  mainthread NOT gray: FFI post-GC header GRAY bit clear; the
--       separate mainthread assert in gc_assert_atomic_end would fire
--       if mainthread ever went gray.
--   P5  stack mutation after first prop still reachable via atomic rescan
--       (graythread re-scan) — reuse the G4 mutate-then-GC pattern from
--       openuv_dead_thread_mark_assert.lua.
--
-- Build: cmake --build builds/ninja-multi-vcpkg --target luajit-5.1 --preset ninja-vcpkg-debug
--        (produces luajit/Debug/luajit-arenagc with LUA_USE_ASSERT + arena GC)
-- Run:   builds/ninja-multi-vcpkg/luajit/Debug/luajit-arenagc \
--          luajit/test/gc/thread_permgray_residual_assert.lua

local ok_ffi, ffi = pcall(require, "ffi")
if not ok_ffi then
  print("ffi unavailable -- thread_permgray_residual_assert cannot run; skipping")
  os.exit(0)
end

-- Interpreter-only: deterministic GC timing + the assert build's arena
-- shadow-verify aborts if JIT-allocated objects are touched mid-step.
local ok_jit, jit = pcall(require, "jit")
if ok_jit then jit.off() end
local bit = require("bit")

----------------------------------------------------------------
-- Constants from lj_obj.h (stable across builds)
----------------------------------------------------------------

-- gct stores ~LJ_T* truncated to a byte. LJ_TUPVAL = ~5u -> gct = 5.
-- LJ_TTHREAD = ~6u -> gct = 6. LJ_TTAB = ~11u -> gct = 11.
-- (see lj_obj.h:264-266)
local GCT_UPVAL  = 5
local GCT_THREAD = 6
local GCT_TAB    = 11

-- LJ_GC_GRAY = 0x01 under arena GC (reuses WHITE0 slot; see lj_gc.h:32).
-- HASGCMARK GCHeader = marked:1, gct:1 at offsets 0,1 (no nextgc).
local MARKED_OFF = 0
local GCT_OFF    = 1
local LJ_GC_GRAY = 0x01

local SENTINEL_MARKER = 0xDEAD0A11

----------------------------------------------------------------
-- Helpers
----------------------------------------------------------------

local pass, fail = 0, 0
local function ok(cond, msg)
  if cond then pass = pass + 1 else fail = fail + 1; print("FAIL: " .. msg) end
end

local function addr_of(obj)
  local s = tostring(obj)
  local hex = s:match("0x(%x+)")
  if not hex then return nil end
  return tonumber(hex, 16)
end

local function marked_byte(obj)
  local p = addr_of(obj)
  if not p then return nil end
  return ffi.cast("uint8_t*", p)[MARKED_OFF]
end

local function gct_byte(obj)
  local p = addr_of(obj)
  if not p then return nil end
  return ffi.cast("uint8_t*", p)[GCT_OFF]
end

local function is_gray(obj)
  local m = marked_byte(obj)
  if not m then return nil end
  return bit.band(m, LJ_GC_GRAY) ~= 0
end

local function full_gc()
  collectgarbage("collect")
  collectgarbage("collect")
end

local function count_surviving(weak)
  local n = 0
  for k in pairs(weak) do
    if type(k) == "table" and k.marker == SENTINEL_MARKER then
      n = n + 1
    end
  end
  return n
end

----------------------------------------------------------------
-- P1: live coroutines through full GC under assert build (no abort)
----------------------------------------------------------------
-- A live anchored coroutine IS mark∧GRAY at the atomic→sweep boundary
-- (propagatemark black2gray'd it and sweep hasn't makewhite'd it yet).
-- If gc_assert_atomic_end's allowlist did NOT include THREAD, the assert
-- build would abort here. Completing this block = positive proof.
do
  local N = 16
  local anchored = {}
  for i = 1, N do
    local co = coroutine.create(function()
      local x = i
      local f = function() return x end
      coroutine.yield()
    end)
    coroutine.resume(co)
    anchored[i] = co
  end

  full_gc()

  -- All anchored coroutines survive (they are strong-referenced).
  local alive = 0
  for i = 1, N do
    if anchored[i] ~= nil and coroutine.status(anchored[i]) == "suspended" then
      alive = alive + 1
    end
  end
  ok(alive == N,
     string.format("p1: live coroutines survive full GC under assert build "
                 .. "(expected %d, found %d) -- THREAD mark∧GRAY residual allowlisted",
                 N, alive))

  -- Post-GC: permanent-gray THREAD may retain header GRAY as *light-gray*
  -- residual (bitmap mark cleared in ClearMarks; GRAY bit not swept). That is
  -- intentional — residual free-entry assert only cares about mark∧GRAY at the
  -- atomic→sweep boundary (oracle: no abort above). Live threads must remain
  -- suspended/callable; light-gray residual is not a leak.
  local gray_post = 0
  for i = 1, N do
    if is_gray(anchored[i]) then gray_post = gray_post + 1 end
  end
  ok(gray_post == N or gray_post == 0,
     string.format("p1: THREAD light-gray residual or pure-black after full GC "
                 .. "(expected %d or 0 gray headers, found %d)",
                 N, gray_post))
end

----------------------------------------------------------------
-- P2: open UV residual still allowlisted (no abort)
----------------------------------------------------------------
-- A suspended coroutine with a live closure holding an OPEN upvalue:
-- the open UV stays mark∧GRAY through atomic (P3a). gc_assert_atomic_end
-- must skip it; if the allowlist dropped open UV, the assert would abort.
do
  local N = 10
  local anchored = {}
  local keep = {}
  for i = 1, N do
    local sentinel = { marker = SENTINEL_MARKER, id = i }
    local co = coroutine.create(function()
      local x = sentinel
      sentinel = nil
      local f = function() return x end
      keep[#keep + 1] = f
      coroutine.yield()
    end)
    coroutine.resume(co)
    anchored[i] = co
  end

  full_gc()

  local call_ok = 0
  for i = 1, N do
    local v = keep[i]()
    if type(v) == "table" and v.marker == SENTINEL_MARKER and v.id == i then
      call_ok = call_ok + 1
    end
  end
  ok(call_ok == N,
     string.format("p2: open UV residual allowlisted, closures callable "
                 .. "(expected %d, found %d)", N, call_ok))
end

----------------------------------------------------------------
-- P3: closed UV + non-weak table NOT residual-gray (pure black after GC)
----------------------------------------------------------------
-- Closed UV and tables are NOT on the dual allowlist. After a full GC
-- cycle, sweep makewhite has cleared their header GRAY bit. If any had
-- been left mark∧GRAY at the atomic boundary, gc_assert_atomic_end would
-- have aborted (residual_gray++ branch). No abort + GRAY bit clear =
-- pure-black proof.
do
  local N = 8
  local keep_closed = {}
  for i = 1, N do
    local sentinel = { marker = SENTINEL_MARKER, id = i }
    -- factory returns a closure that closes over `sentinel` (closed UV
    -- once the factory frame returns).
    local function factory(id)
      local s = sentinel
      return function() return s end
    end
    keep_closed[i] = factory(i)
  end

  local t_strong = {}
  for i = 1, N do
    t_strong[i] = { marker = SENTINEL_MARKER, id = i, data = { i, i + 1 } }
  end

  full_gc()

  -- Closed-UV closures still return their sentinel (closed UV survived).
  local closed_ok = 0
  for i = 1, N do
    local v = keep_closed[i]()
    if type(v) == "table" and v.marker == SENTINEL_MARKER and v.id == i then
      closed_ok = closed_ok + 1
    end
  end
  ok(closed_ok == N,
     string.format("p3: closed-UV closures still callable (expected %d, found %d)",
                 N, closed_ok))

  -- Non-weak table: header GRAY bit must be clear after full GC.
  local gray_tables = 0
  for i = 1, N do
    if is_gray(t_strong[i]) then gray_tables = gray_tables + 1 end
  end
  ok(gray_tables == 0,
     string.format("p3: non-weak tables pure black after GC (expected 0 gray, found %d)",
                 gray_tables))

  -- Sanity: the tables are actually marked (gct == GCT_TAB).
  local tab_gct_ok = 0
  for i = 1, N do
    if gct_byte(t_strong[i]) == GCT_TAB then tab_gct_ok = tab_gct_ok + 1 end
  end
  ok(tab_gct_ok == N,
     string.format("p3: tables are LJ_TTAB (expected %d, found %d)", N, tab_gct_ok))
end

----------------------------------------------------------------
-- P4: mainthread NOT gray (no mainthread residual)
----------------------------------------------------------------
-- mainthread is SFIXED non-arena and is traversed via gc_traverse_mainthread
-- in mark_start/atomic. It must NEVER be mark∧GRAY at the atomic boundary;
-- gc_assert_atomic_end has a dedicated mainthread branch (~lj_gc_markalloc
-- _debug.h:499-504) that asserts on GRAY and would abort with
-- "mark∧GRAY mainthread at atomic→sweep". Running a full GC under the
-- assert build without abort IS the proof that mainthread stays ¬GRAY.
--
-- (The mainthread lua_State* is not exposed to pure Lua: coroutine.running()
-- returns nil at top level — only coroutines created by coroutine.create are
-- observable via tostring. So the assert-build no-abort is the oracle here,
-- matching how P1/P2 verify the THREAD/open-UV residual.)
do
  -- Drive several full cycles with live coroutines anchored so the atomic
  -- walk + mainthread branch actually runs each time. No abort = mainthread
  -- not gray.
  local anchored = {}
  for i = 1, 4 do
    local co = coroutine.create(function() local x = i coroutine.yield() end)
    coroutine.resume(co)
    anchored[i] = co
  end
  for _ = 1, 3 do full_gc() end
  local alive = 0
  for i = 1, 4 do
    if anchored[i] ~= nil and coroutine.status(anchored[i]) == "suspended" then
      alive = alive + 1
    end
  end
  ok(alive == 4,
     string.format("p4: mainthread ¬GRAY across full GC cycles under assert "
                 .. "build (no mainthread residual abort) -- %d/4 coroutines live",
                 alive))
end

----------------------------------------------------------------
-- P5: stack mutation after first prop still reachable via rescan
----------------------------------------------------------------
-- Reuse the G4 mutate-then-GC pattern from openuv_dead_thread_mark_assert.
-- While the coroutine is LIVE + suspended, the atomic graythread re-scan
-- (gc_atomic_rescan_threads) must re-mark the CURRENT stack slot value.
-- A live permanent-gray thread is exactly the object the rescan walks, so
-- this also proves permanent-gray does not break stack re-scan coverage.
do
  local N = 6
  local anchored = {}
  local keep = {}
  local weak_old = setmetatable({}, { __mode = "k" })
  local weak_new = setmetatable({}, { __mode = "k" })

  for i = 1, N do
    local old_s = { marker = SENTINEL_MARKER, id = i, gen = "old" }
    local new_s = { marker = SENTINEL_MARKER, id = i, gen = "new" }
    weak_old[old_s] = true
    weak_new[new_s] = true
    local co = coroutine.create(function()
      local x = old_s
      local f = function() return x end
      keep[#keep + 1] = f
      coroutine.yield()           -- open UV points at x == old_s
      x = new_s                   -- mutate stack slot while UV still open
      coroutine.yield()
    end)
    coroutine.resume(co)
    anchored[i] = co
  end

  -- First GC with old_s still in the slot.
  full_gc()
  ok(count_surviving(weak_old) == N,
     string.format("p5: old sentinels live before mutate (expected %d, found %d)",
                 N, count_surviving(weak_old)))

  -- Mutate each stack slot to new_s.
  for i = 1, N do
    coroutine.resume(anchored[i])
  end

  full_gc()

  ok(count_surviving(weak_new) == N,
     string.format("p5: new stack values survive rescan (expected %d, found %d)",
                 N, count_surviving(weak_new)))

  local call_ok = 0
  for i = 1, N do
    local v = keep[i]()
    if type(v) == "table" and v.gen == "new" and v.id == i then
      call_ok = call_ok + 1
    end
  end
  ok(call_ok == N,
     string.format("p5: closures see mutated value via rescan (expected %d, found %d)",
                 N, call_ok))

  -- Keep anchored so UVs stay open (proves permanent-gray thread + open UV
  -- coexist through multiple full cycles without residual abort).
  full_gc()
  local still_alive = 0
  for i = 1, N do
    if anchored[i] ~= nil and coroutine.status(anchored[i]) == "suspended" then
      still_alive = still_alive + 1
    end
  end
  ok(still_alive == N,
     string.format("p5: anchored coroutines still suspended after 3rd GC (expected %d, found %d)",
                 N, still_alive))
end

----------------------------------------------------------------
-- Summary
----------------------------------------------------------------

print(string.format("\nthread_permgray_residual_assert: %d passed, %d failed",
                     pass, fail))
os.exit(fail == 0 and 0 or 1)

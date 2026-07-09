-- MARK-side invariant test for open-upvalue sentinel survival.
--
-- Guards the invariant that a heap object reachable ONLY through a live
-- suspended coroutine's stack slot (captured as an OPEN upvalue) survives
-- GC -- independent of the global uvhead DLL and the gc_mark_uv atomic
-- remark (both removed by the arenagc openupval GC hygiene plan: P1 drops
-- gc_mark_uv, P2 drops g->uvhead). After those waves the stack scan in
-- gc_traverse_thread (driven by the atomic graythread re-scan) is the sole
-- mechanism keeping such sentinels alive; this test must stay GREEN.
--
-- SCENARIO (closure-dead group -- the guard):
--   1. A coroutine's inner frame binds `x = sentinel` then nils the only
--      Lua reference (`sentinel = nil`) and the only closure reference
--      (`f = nil`), then yields mid-frame. The stack slot for `x` is the
--      ONLY strong reference to the sentinel; the upvalue is dead (no live
--      closure) and will be freed by the rebuild thread scan, but the slot
--      stays live because the coroutine is suspended + anchored.
--   2. A weak-valued table holds a non-strong reference to the sentinel so
--      we can observe survival without keeping it alive.
--   3. collectgarbage("collect") x2. The sentinel MUST survive: marking the
--      anchored coroutine -> gc_traverse_thread -> gc_marktv(stack slot).
--      If the stack scan (the gray drain) is broken, the sentinel is not
--      marked -> collected -> the weak ref goes nil -> FAIL.
--
-- SCENARIO (control group -- live closure):
--   The closure referencing the open upvalue is kept reachable, so the
--   upvalue stays gray and its value is remarked by gc_mark_uv (pre-P1) /
--   reached via the closure (post-P1). The sentinel survives; proves the
--   test only fails when ALL keeping mechanisms are broken, not spuriously.
--
-- SCENARIO (repeated-cycle group):
--   Repeated create/drop/collect rounds assert no accumulation/loss across
--   GC cycles.
--
-- RED/GREEN:
--   GREEN (clean tree): stack scan keeps the sentinel alive -> weak ref
--     stays non-nil and the marker field is intact.
--   RED (throwaway stub -- early-return gc_traverse_thread so the stack is
--     not marked): the sentinel is unreachable from any marked root ->
--     collected -> weak ref goes nil. (Verified during P0; stub reverted
--     before commit. After P1/P2 the test stays GREEN because the stack
--     scan is untouched by those waves.)
--
-- Build: make clean && make -j4 XCFLAGS="-DLUAJIT_ENABLE_GCARENA -DLUAJIT_SECURITY_STRHASH=1 -DLUA_USE_ASSERT"
-- Run:   ./src/luajit test/gc/uvhead_mark_demoted_assert.lua

-- Interpreter-only: deterministic GC timing + the assert build's arena
-- shadow-verify aborts if JIT-allocated objects are touched mid-step.
local ok_jit, jit = pcall(require, "jit")
if ok_jit then jit.off() end

local pass, fail = 0, 0
local function ok(cond, msg)
  if cond then pass = pass + 1 else fail = fail + 1; print("FAIL: " .. msg) end
end

local SENTINEL_MARKER = 0xDEAD

----------------------------------------------------------------
-- Builders
----------------------------------------------------------------

-- Closure-DEAD group: the open upvalue has no live closure; the sentinel
-- survives ONLY via the suspended coroutine's stack slot (the stack scan).
-- `weak` is a weak-valued table used to observe survival; it does NOT keep
-- the sentinel alive.
local function make_co_dead_closure(weak)
  local sentinel = { marker = SENTINEL_MARKER }
  weak[sentinel] = true
  local co = coroutine.create(function()
    local x = sentinel        -- x's stack slot is the only strong ref
    sentinel = nil            -- drop the upvalue-source local Lua ref
    local f = function() return x end  -- closure capturing x as open upvalue
    f = nil                   -- drop closure -> upvalue dead, stack slot live
    coroutine.yield()         -- suspended mid-frame; x's slot stays live
  end)
  coroutine.resume(co)
  return co
end

-- Control group: the closure referencing the open upvalue is kept reachable
-- so the upvalue stays gray and its value is reached via the closure (and
-- remarked by gc_mark_uv on pre-P1 trees). Sentinel survives.
local function make_co_live_closure(weak, keep_closures)
  local sentinel = { marker = SENTINEL_MARKER }
  weak[sentinel] = true
  local co = coroutine.create(function()
    local x = sentinel
    sentinel = nil
    local f = function() return x end
    keep_closures[#keep_closures + 1] = f  -- keep f reachable
    coroutine.yield()
  end)
  coroutine.resume(co)
  return co
end

-- Count surviving sentinel weak refs in `weak` (keys are the sentinel
-- tables; a collected sentinel's key is removed by the weak table logic).
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
-- Group 1: closure-dead -- sentinel survives via the stack scan only
----------------------------------------------------------------
do
  local N_CO = 10
  local anchored = {}
  local weak = setmetatable({}, { __mode = "kv" })

  for i = 1, N_CO do
    anchored[i] = make_co_dead_closure(weak)
  end

  -- Sanity: before GC all sentinels are present.
  local before = 0
  for _ in pairs(weak) do before = before + 1 end
  ok(before == N_CO,
     string.format("group1 before GC: expected %d weak entries, found %d",
                   N_CO, before))

  collectgarbage("collect")
  collectgarbage("collect")

  local after = count_surviving(weak)
  -- THE ASSERTION: every sentinel survives via its anchored coroutine's
  -- stack slot, even though the capturing closure is dead and the open
  -- upvalue itself is freed by the rebuild thread scan.
  ok(after == N_CO,
     string.format("group1 after GC: sentinel survived via stack scan "
                   .. "(expected %d, found %d)", N_CO, after))
  if after ~= N_CO then
    print(string.format("RED PROOF: %d/%d sentinels lost -- stack scan "
      .. "did not keep closure-dead open-upvalue values alive", N_CO - after, N_CO))
  end
end

----------------------------------------------------------------
-- Group 2: control -- live closure keeps the sentinel (always GREEN)
----------------------------------------------------------------
do
  local N_CO = 5
  local anchored = {}
  local keep_closures = {}
  local weak = setmetatable({}, { __mode = "kv" })

  for i = 1, N_CO do
    anchored[i] = make_co_live_closure(weak, keep_closures)
  end

  collectgarbage("collect")
  collectgarbage("collect")

  local after = count_surviving(weak)
  ok(after == N_CO,
     string.format("group2 control: live-closure sentinels preserved "
                   .. "(expected %d, found %d)", N_CO, after))
end

----------------------------------------------------------------
-- Group 3: repeated cycles -- no accumulation/loss across rounds
----------------------------------------------------------------
do
  local ROUNDS = 5
  local N_CO = 4
  local anchored = {}
  local weak = setmetatable({}, { __mode = "kv" })

  for round = 1, ROUNDS do
    for i = 1, N_CO do
      anchored[#anchored + 1] = make_co_dead_closure(weak)
    end
    collectgarbage("collect")
    collectgarbage("collect")

    local surviving = count_surviving(weak)
    -- Every anchored coroutine keeps its sentinel; none should be lost.
    local expected = round * N_CO
    ok(surviving == expected,
       string.format("group3 round %d: surviving sentinels %d (expected %d)",
                     round, surviving, expected))
  end
end

----------------------------------------------------------------
-- Summary
----------------------------------------------------------------

print(string.format("\nuvhead_mark_demoted_assert: %d passed, %d failed",
                    pass, fail))
os.exit(fail == 0 and 0 or 1)

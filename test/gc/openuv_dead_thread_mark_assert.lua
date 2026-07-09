-- Open-upvalue mark/close invariant suite (pure Lua; no FFI required).
--
-- Complements:
--   * test/gc/uvhead_mark_demoted_assert.lua  -- live suspended co, dead
--     closure: sentinel survives ONLY via stack scan
--   * test/gc/thread_openupval_sweep_assert.lua -- dead open UVs freed from
--     LIVE co chains (FFI white-box)
--
-- This file locks the classic corner that justified Lua's remarkupvals /
-- LuaJIT's former gc_mark_uv, and that P1 removed in favor of once-mark
-- (gc_mark -> uvval) + thread re-scan:
--
--   G1  dead-thread + live-closure: drop the coroutine, KEEP the closure
--       that still holds an OPEN upvalue into that thread's stack. Full GC
--       must (a) collect the thread, (b) close live open UVs via
--       lj_state_free -> lj_func_closeuv, (c) keep the sentinel alive so
--       f() still returns it. Without marking uvval when the UV is gray,
--       the sentinel can die while still only reachable through a white
--       thread's stack slot.
--
-- Also covers pure-Lua control paths that need no C:
--   G2  shared open UV (two closures, one local)
--   G3  close-on-return factory (closed UV holds value)
--   G4  live-thread mutate-then-GC (stack re-scan / closed value)
--   G5  multi-round dead-thread stress
--
-- Reverse / negative controls (must observe COLLECTION -- harness RED proof):
--   R1  dead-thread + dead-closure: drop co AND f -> sentinel MUST die.
--       Proves weak-table observation is not a false-GREEN sensor.
--   R2  unanchored co + dead-closure (stack-scan dual of G1 in
--       uvhead_mark_demoted_assert): no stack root, no UV root -> die.
--   R3  factory result dropped: closed UV + closure both unreachable -> die.
--
-- Note: a true "break once-mark / break stack scan" RED (mechanism fault)
-- still needs a throwaway C stub (see comments in uvhead_mark_demoted and
-- thread_openupval_sweep). Pure Lua can only strip *roots*, not disable
-- the collector's mark steps. R1-R3 are the root-stripping duals of G*.
--
-- Build: make -j4 XCFLAGS="-DLUAJIT_ENABLE_GCARENA -DLUAJIT_SECURITY_STRHASH=1 -DLUA_USE_ASSERT"
--        (classic assert build is fine too; no arena-only APIs used)
-- Run:   ./src/luajit test/gc/openuv_dead_thread_mark_assert.lua

local ok_jit, jit = pcall(require, "jit")
if ok_jit then jit.off() end

local pass, fail = 0, 0
local function ok(cond, msg)
  if cond then
    pass = pass + 1
  else
    fail = fail + 1
    print("FAIL: " .. msg)
  end
end

local SENTINEL_MARKER = 0xDEAD0A11

local function count_surviving(weak)
  local n = 0
  for k in pairs(weak) do
    if type(k) == "table" and k.marker == SENTINEL_MARKER then
      n = n + 1
    end
  end
  return n
end

local function full_gc()
  collectgarbage("collect")
  collectgarbage("collect")
end

----------------------------------------------------------------
-- G1: dead thread + live open-upvalue closure (THE mark-side lock)
----------------------------------------------------------------
-- Open UV does NOT GC-reference its thread. Thread can go white while the
-- UV stays gray via a kept closure. Atomic no longer walks g->uvhead;
-- survival depends on gc_mark marking uvval once, then lj_state_free
-- closing the UV when the dead thread is freed.
do
  local N = 12
  local keep = {}
  local weak_sent = setmetatable({}, { __mode = "k" })
  local weak_co = setmetatable({}, { __mode = "v" })

  for i = 1, N do
    local sentinel = { marker = SENTINEL_MARKER, id = i }
    weak_sent[sentinel] = true
    local co = coroutine.create(function()
      local x = sentinel
      sentinel = nil
      local f = function() return x end
      keep[#keep + 1] = f
      coroutine.yield()  -- frame stays live -> UV stays OPEN until free
    end)
    local rok = coroutine.resume(co)
    ok(rok, string.format("g1 resume co %d", i))
    weak_co[i] = co
    -- drop strong ref to co; only weak_co observes collection
  end

  full_gc()

  local threads_left = 0
  for i = 1, N do
    if weak_co[i] ~= nil then threads_left = threads_left + 1 end
  end
  ok(threads_left == 0,
     string.format("g1: dead threads collected (expected 0 live, found %d)",
                   threads_left))

  local survived = count_surviving(weak_sent)
  ok(survived == N,
     string.format("g1: sentinels survive via open-UV once-mark/close "
                   .. "(expected %d, found %d)", N, survived))

  local call_ok = 0
  for i = 1, N do
    local f = keep[i]
    if type(f) == "function" then
      local v = f()
      if type(v) == "table" and v.marker == SENTINEL_MARKER and v.id == i then
        call_ok = call_ok + 1
      end
    end
  end
  ok(call_ok == N,
     string.format("g1: closed/open UV still returns sentinel "
                   .. "(expected %d, found %d)", N, call_ok))
end

----------------------------------------------------------------
-- G2: two closures share one open upvalue; drop thread; both work
----------------------------------------------------------------
do
  local N = 6
  local keep_a, keep_b = {}, {}
  local weak_sent = setmetatable({}, { __mode = "k" })
  local weak_co = setmetatable({}, { __mode = "v" })

  for i = 1, N do
    local sentinel = { marker = SENTINEL_MARKER, id = i }
    weak_sent[sentinel] = true
    local co = coroutine.create(function()
      local x = sentinel
      sentinel = nil
      local a = function() return x end
      local b = function() return x end
      keep_a[#keep_a + 1] = a
      keep_b[#keep_b + 1] = b
      coroutine.yield()
    end)
    ok(coroutine.resume(co), string.format("g2 resume %d", i))
    weak_co[i] = co
  end

  full_gc()

  local threads_left = 0
  for i = 1, N do
    if weak_co[i] ~= nil then threads_left = threads_left + 1 end
  end
  ok(threads_left == 0, "g2: threads collected")

  ok(count_surviving(weak_sent) == N, "g2: shared-UV sentinels survive")

  local both = 0
  for i = 1, N do
    local va, vb = keep_a[i](), keep_b[i]()
    if va == vb and type(va) == "table" and va.marker == SENTINEL_MARKER
        and va.id == i then
      both = both + 1
    end
  end
  ok(both == N,
     string.format("g2: both shared closures agree (expected %d, found %d)",
                   N, both))
end

----------------------------------------------------------------
-- G3: close-on-return factory (closed UV; no open chain)
----------------------------------------------------------------
do
  local N = 8
  local keep = {}
  local weak_sent = setmetatable({}, { __mode = "k" })

  local function factory(id)
    local sentinel = { marker = SENTINEL_MARKER, id = id }
    weak_sent[sentinel] = true
    return function() return sentinel end  -- closes on return
  end

  for i = 1, N do
    keep[i] = factory(i)
  end

  full_gc()
  ok(count_surviving(weak_sent) == N, "g3: closed-UV sentinels survive")

  local call_ok = 0
  for i = 1, N do
    local v = keep[i]()
    if type(v) == "table" and v.marker == SENTINEL_MARKER and v.id == i then
      call_ok = call_ok + 1
    end
  end
  ok(call_ok == N, "g3: closed UV callable after GC")
end

----------------------------------------------------------------
-- G4: live anchored thread; mutate stack after capture; GC; observe
----------------------------------------------------------------
-- While the co is live, grayagain/graythread stack re-scan (not uvhead)
-- must keep the *current* stack value alive across incremental mark.
do
  local N = 5
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
    ok(coroutine.resume(co), string.format("g4 resume1 %d", i))
    anchored[i] = co
  end

  -- First GC with old_s still in the slot.
  full_gc()
  ok(count_surviving(weak_old) == N, "g4: old sentinels live before mutate")

  -- Mutate each stack slot to new_s; drop old_s as the only strong ref
  -- besides whatever the open UV / stack still holds.
  for i = 1, N do
    ok(coroutine.resume(anchored[i]), string.format("g4 resume2 %d", i))
  end

  full_gc()

  -- new_s must survive (current open-UV / stack value).
  ok(count_surviving(weak_new) == N,
     string.format("g4: new stack values survive re-scan (found %d/%d)",
                   count_surviving(weak_new), N))

  local call_ok = 0
  for i = 1, N do
    local v = keep[i]()
    if type(v) == "table" and v.gen == "new" and v.id == i then
      call_ok = call_ok + 1
    end
  end
  ok(call_ok == N,
     string.format("g4: closures see mutated value (expected %d, found %d)",
                   N, call_ok))

  -- old_s may or may not still be reachable depending on barriers; only
  -- require that new is correct. Keep anchored so UVs stay open.
  for i = 1, N do ok(anchored[i] ~= nil, "g4 anchor") end
end

----------------------------------------------------------------
-- G5: multi-round dead-thread + live-closure stress
----------------------------------------------------------------
do
  local ROUNDS = 5
  local N = 4
  local keep = {}
  local weak_sent = setmetatable({}, { __mode = "k" })

  for round = 1, ROUNDS do
    local weak_co = setmetatable({}, { __mode = "v" })
    for i = 1, N do
      local id = (round - 1) * N + i
      local sentinel = { marker = SENTINEL_MARKER, id = id }
      weak_sent[sentinel] = true
      local co = coroutine.create(function()
        local x = sentinel
        sentinel = nil
        local f = function() return x end
        keep[#keep + 1] = f
        coroutine.yield()
      end)
      ok(coroutine.resume(co),
         string.format("g5 r%d resume %d", round, i))
      weak_co[i] = co
    end
    full_gc()

    local threads_left = 0
    for i = 1, N do
      if weak_co[i] ~= nil then threads_left = threads_left + 1 end
    end
    ok(threads_left == 0,
       string.format("g5 round %d: threads collected", round))

    local expected = round * N
    local survived = count_surviving(weak_sent)
    ok(survived == expected,
       string.format("g5 round %d: surviving sentinels %d (expected %d)",
                     round, survived, expected))
  end

  local call_ok = 0
  for i = 1, #keep do
    local v = keep[i]()
    if type(v) == "table" and v.marker == SENTINEL_MARKER and v.id == i then
      call_ok = call_ok + 1
    end
  end
  ok(call_ok == #keep,
     string.format("g5: all accumulated closures callable (%d/%d)",
                   call_ok, #keep))
end

----------------------------------------------------------------
-- R1: reverse of G1 -- strip both roots (co + f) -> sentinel MUST die
----------------------------------------------------------------
-- If this stays GREEN (survives), the weak-table harness is broken and
-- every G* "survives" assertion is untrustworthy.
do
  local N = 10
  local weak_sent = setmetatable({}, { __mode = "k" })
  local weak_co = setmetatable({}, { __mode = "v" })

  for i = 1, N do
    local sentinel = { marker = SENTINEL_MARKER, id = i }
    weak_sent[sentinel] = true
    local co = coroutine.create(function()
      local x = sentinel
      sentinel = nil
      local f = function() return x end
      f = nil  -- drop closure: open UV becomes dead (no gray UV root)
      coroutine.yield()
    end)
    ok(coroutine.resume(co), string.format("r1 resume %d", i))
    weak_co[i] = co
    -- no strong ref to co, no keep[] for f
  end

  full_gc()

  local threads_left = 0
  for i = 1, N do
    if weak_co[i] ~= nil then threads_left = threads_left + 1 end
  end
  ok(threads_left == 0, "r1: threads collected")

  local survived = count_surviving(weak_sent)
  ok(survived == 0,
     string.format("r1 REVERSE: no roots -> sentinels MUST die "
                   .. "(expected 0, found %d) -- weak harness RED proof",
                   survived))
  if survived > 0 then
    print(string.format("HARNESS BROKEN: %d/%d sentinels survived with zero "
      .. "roots -- positive G* tests cannot be trusted", survived, N))
  end
end

----------------------------------------------------------------
-- R2: reverse of stack-scan lock -- unanchored co + dead closure
----------------------------------------------------------------
-- Dual of uvhead_mark_demoted G1: that test anchors co so the stack scan
-- keeps the sentinel. Here co is dropped; stack is not a root -> die.
do
  local N = 10
  local weak_sent = setmetatable({}, { __mode = "k" })

  for i = 1, N do
    local sentinel = { marker = SENTINEL_MARKER, id = i }
    weak_sent[sentinel] = true
    local co = coroutine.create(function()
      local x = sentinel
      sentinel = nil
      local f = function() return x end
      f = nil
      coroutine.yield()
    end)
    ok(coroutine.resume(co), string.format("r2 resume %d", i))
    -- deliberately do not anchor co
  end

  full_gc()

  local survived = count_surviving(weak_sent)
  ok(survived == 0,
     string.format("r2 REVERSE: unanchored co + dead closure -> die "
                   .. "(expected 0, found %d)", survived))
end

----------------------------------------------------------------
-- R3: reverse of G3 -- drop factory closure -> closed UV dies with it
----------------------------------------------------------------
do
  local N = 8
  local weak_sent = setmetatable({}, { __mode = "k" })

  local function factory(id)
    local sentinel = { marker = SENTINEL_MARKER, id = id }
    weak_sent[sentinel] = true
    return function() return sentinel end
  end

  for i = 1, N do
    local f = factory(i)
    f = nil  -- drop immediately; closed UV only reachable via f
  end

  full_gc()

  local survived = count_surviving(weak_sent)
  ok(survived == 0,
     string.format("r3 REVERSE: dropped closed-UV closure -> die "
                   .. "(expected 0, found %d)", survived))
end

----------------------------------------------------------------
-- Summary
----------------------------------------------------------------

print(string.format("\nopenuv_dead_thread_mark_assert: %d passed, %d failed",
                    pass, fail))
os.exit(fail == 0 and 0 or 1)

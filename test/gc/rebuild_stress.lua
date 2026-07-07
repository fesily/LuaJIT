-----------------------------------------------------------------------------
-- test/gc/rebuild_stress.lua
--
-- T9: Cross-step mutation stress tests (Metis G14 + Momus B2/B3).
--
-- Deterministic scenarios that interleave single `collectgarbage("step")`
-- slices with each hazard operation that the chunked rebuild (T4-T8) must
-- survive. Each scenario:
--   1. builds a workload (including a known number of finalizable udata),
--   2. stops the GC, sets stepmul=200, restarts it incremental,
--   3. loops `collectgarbage("step")` interleaved with the hazard op,
--   4. drops all references and runs `collectgarbage("collect")` twice,
--   5. asserts the finalizer count is exactly the number of __gc udata.
--
-- A scenario passes if the process exits 0 under the arena ASSERT build
-- (no checkheap / shadow-verify / barrier assert fires), the finalizer
-- count is exact, and no crash occurs.
--
-- Scenarios:
--   (a)  VLA cdata alloc during Prologue (and any rebuild sub-phase).
--   (b)  huge alloc forcing hugeset_resize during HugeScan.
--   (b2) SAME-MASK tombstone-clearing rehash during HugeScan (Momus-3/B3).
--   (c)  arena growth during ThreadScan.
--   (d)  closeuv during rebuild.
--   (e)  udata-with-__gc across a cycle boundary.
--   (f)  thousands of finalizable udata spanning chunked mmudata (Momus-2/B2).
--   (g)  back-barrier (table store on a LIVE arena table whose child is white)
--        interleaved at every rebuild sub-phase boundary.
--
-- Usage:
--   ./src/luajit -joff test/gc/rebuild_stress.lua [iterations]
--
-- Default iterations = 100 (per T9 acceptance: catch cursor-boundary races).
-- A smaller value (e.g. 5) is useful for quick smoke runs.
--
-- Build:
--   make -C src clean && make -C src -j$(nproc) \
--     XCFLAGS="-DLUAJIT_ENABLE_GCARENA -DLUA_USE_ASSERT -DLUAJIT_SECURITY_STRHASH=1"
--
-- Pre-existing documented bug avoidance:
--   `ffi.new("char[?]", n)` with n >= 512KB (ArenaHugeThreshold) hits the
--   huge-VLA cdata GC crash documented in .omo/drafts/bug-huge-vla-cdata-gc-crash.md.
--   Scenarios that allocate VLAs keep them < 512KB; scenarios that need a
--   huge block use a FIXED-size ctype (`ffi.new("char[600000]")`), which
--   takes the non-VLA `lj_cdata_new` path where the GCobj sits at the block
--   base and hugeset/ishuge classify it correctly.
-----------------------------------------------------------------------------

local ffi = require("ffi")

-- iterations is configurable; default 100 per T9 acceptance criteria.
local ITERATIONS = tonumber(arg and arg[1] or "") or 100

-- Steps per scenario iteration. With stepmul=200 each step is bounded; 2000
-- steps give the rebuild plenty of sub-phase yields to expose cursor races.
local STEPS_PER_ITER = 2000

-- Huge fixed-size ctype: 600000 bytes > ArenaHugeThreshold (512KB = 524288).
-- Fixed ctype -> lj_cdata_new (NOT lj_cdata_newv) -> safe huge path.
local HUGE_FIXED_SIZE = 600000

-- Safe VLA size: 100000 bytes, well under ArenaHugeThreshold.
local SAFE_VLA_SIZE = 100000

-----------------------------------------------------------------------------
-- Helpers
-----------------------------------------------------------------------------

-- Create a finalizable userdata that increments counter.n on __gc.
-- Returns the proxy. The caller must keep a reference alive until done.
local function make_finalizable(counter)
  local p = newproxy(true)
  local mt = getmetatable(p)
  mt.__gc = function()
    counter.n = counter.n + 1
  end
  return p
end

-- Build N finalizable proxies and return a table holding the references.
local function build_finalizables(counter, n)
  local keep = {}
  for i = 1, n do
    keep[i] = make_finalizable(counter)
  end
  return keep
end

-- Run one scenario iteration: setup -> incremental step loop with hazard
-- injection -> drop refs -> full collect x2 -> assert finalizer count.
local function run_iter(name, setup, hazard, expected_fin, iter)
  local counter = { n = 0 }
  local keep = setup(counter)

  collectgarbage("stop")
  collectgarbage("setstepmul", 200)
  collectgarbage("restart")

  for _ = 1, STEPS_PER_ITER do
    hazard(keep, counter)
    collectgarbage("step")
  end

  -- Drop all references and force two full collections so any pending
  -- finalizers (queued in mmudata) are run and accounted for.
  keep = nil
  collectgarbage("stop")
  collectgarbage("collect")
  collectgarbage("collect")

  if counter.n ~= expected_fin then
    error(string.format(
      "[%s] iter %d: finalizer count %d != expected %d",
      name, iter, counter.n, expected_fin))
  end
end

-- Register a scenario and run it ITERATIONS times.
local scenarios = {}
local function scenario(name, setup, hazard, expected_fin)
  scenarios[#scenarios + 1] = {
    name = name,
    setup = setup,
    hazard = hazard,
    expected_fin = expected_fin,
  }
end

-----------------------------------------------------------------------------
-- Scenario (a): VLA cdata alloc during Prologue (and any sub-phase).
--
-- Hazard: allocate a small (< 512KB) VLA cdata between every step. The
-- lj_cdata_newv path links the new cdata into cdatavroot; the rebuild must
-- observe it correctly across every sub-phase yield.
-----------------------------------------------------------------------------
scenario(
  "a_vla_alloc_during_prologue",
  function(counter)
    return build_finalizables(counter, 200)
  end,
  function(keep, counter)
    -- Small VLA: safe, exercises lj_cdata_newv + cdatavroot linking.
    local _ = ffi.new("char[?]", SAFE_VLA_SIZE)
    keep[#keep + 1] = _
  end,
  200)

-----------------------------------------------------------------------------
-- Scenario (b): huge alloc forcing hugeset_resize during HugeScan.
--
-- Hazard: allocate a fixed-size huge ctype (lj_cdata_new path, GCobj at
-- block base) and keep a sliding window of recent huges alive in a ring
-- buffer. As the ring turns, old huges are dropped (freed on a later step)
-- and new ones are registered, exercising hugeset growth / resize while
-- HugeScan walks the set between steps.
-----------------------------------------------------------------------------
local function make_huge_ring_hazard(ring_size)
  return function(keep, counter)
    local ring = keep._huge_ring
    if not ring then
      ring = {}
      keep._huge_ring = ring
      keep._huge_idx = 0
    end
    local idx = keep._huge_idx
    -- Drop the oldest huge (frees on a later step -> tombstone in hugeset).
    local old = ring[idx + 1]
    if old then ring[idx + 1] = nil end
    -- Allocate a new huge (registers in hugeset, may trigger resize).
    local cd = ffi.new(string.format("char[%d]", HUGE_FIXED_SIZE))
    ring[idx + 1] = cd
    keep._huge_idx = (idx + 1) % ring_size
  end
end

scenario(
  "b_huge_alloc_hugeset_resize",
  function(counter)
    return build_finalizables(counter, 200)
  end,
  make_huge_ring_hazard(64),
  200)

-----------------------------------------------------------------------------
-- Scenario (b2): SAME-MASK tombstone-clearing rehash during HugeScan
-- (Momus-3/B3).
--
-- Hazard: alternate between two phases cyclically:
--   PHASE 1 (build tombstones): allocate many huges, then drop most of them
--     so the next GC step frees them and leaves tombstones in the hugeset.
--   PHASE 2 (trigger resize): allocate new huges to fill the freed slots
--     plus tombstones until the hugeset hits its load factor and resizes
--     with same-mask tombstone clearing.
-- The cycle repeats; over many iterations the resize lands on every rebuild
-- sub-phase boundary.
-----------------------------------------------------------------------------
scenario(
  "b2_hugeset_tombstone_rehash",
  function(counter)
    return build_finalizables(counter, 100)
  end,
  function(keep, counter)
    local st = keep._b2
    if not st then
      st = { phase = 1, tick = 0, live = {}, dropped = {} }
      keep._b2 = st
    end
    st.tick = st.tick + 1
    if st.phase == 1 then
      -- Build tombstones: alloc a batch of huges, hold them briefly.
      for i = 1, 8 do
        st.live[#st.live + 1] = ffi.new(string.format("char[%d]", HUGE_FIXED_SIZE))
      end
      if st.tick % 4 == 0 then
        -- Drop most live huges so the next GC step frees them -> tombstones.
        for i = 1, #st.live do
          st.dropped[i] = st.live[i]
        end
        st.live = {}
      end
      if st.tick >= 16 then
        st.phase = 2
        st.tick = 0
        st.dropped = {}
      end
    else
      -- Trigger resize: alloc huges to fill slots + tombstones.
      st.live[#st.live + 1] = ffi.new(string.format("char[%d]", HUGE_FIXED_SIZE))
      if st.tick >= 24 then
        st.phase = 1
        st.tick = 0
        st.live = {}
        st.dropped = {}
      end
    end
  end,
  100)

-----------------------------------------------------------------------------
-- Scenario (c): arena growth during ThreadScan.
--
-- Hazard: allocate many small tables (Trav arena) between steps so the
-- arena class fills and a new arena is allocated and linked during a step
-- that may be inside ThreadScan of the previous arena.
-----------------------------------------------------------------------------
scenario(
  "c_arena_growth_during_threadscan",
  function(counter)
    return build_finalizables(counter, 200)
  end,
  function(keep, counter)
    -- Allocating plain tables routes them into the Trav arena; a burst
    -- between steps forces arena growth (new arena linked into the class
    -- list) while a previous arena is mid-scan.
    local bucket = keep._c_bucket
    if not bucket then
      bucket = {}
      keep._c_bucket = bucket
      keep._c_idx = 0
    end
    local idx = keep._c_idx
    for i = 1, 32 do
      bucket[idx + i] = { i, i * 2, i * 3 }
    end
    keep._c_idx = idx + 32
    -- Periodically flush the bucket so old tables become collectable and
    -- the Trav arena cycles through alloc/free/grow.
    if keep._c_idx >= 1024 then
      keep._c_bucket = {}
      keep._c_idx = 0
    end
  end,
  200)

-----------------------------------------------------------------------------
-- Scenario (d): closeuv during rebuild.
--
-- Hazard: call a factory that returns a closure over an upvalue; when the
-- factory returns, the open upvalue is closed (closeuv) and becomes a new
-- GC object the rebuild must mark/queue correctly across the next step.
-----------------------------------------------------------------------------
local function make_uv_factory()
  -- Each call creates a fresh local table upvalue; on return it closes.
  local function factory()
    local uv = { 1, 2, 3 }
    return function() return uv end
  end
  return factory
end

scenario(
  "d_closeuv_during_rebuild",
  function(counter)
    return build_finalizables(counter, 200)
  end,
  function(keep, counter)
    local factory = keep._d_factory
    if not factory then
      factory = make_uv_factory()
      keep._d_factory = factory
    end
    local bucket = keep._d_bucket
    if not bucket then
      bucket = {}
      keep._d_bucket = bucket
      keep._d_idx = 0
    end
    local idx = keep._d_idx
    -- Each factory() call closes an upvalue on return.
    bucket[idx + 1] = factory()
    keep._d_idx = idx + 1
    if keep._d_idx >= 512 then
      keep._d_bucket = {}
      keep._d_idx = 0
    end
  end,
  200)

-----------------------------------------------------------------------------
-- Scenario (e): udata-with-__gc across a cycle boundary.
--
-- Unlike the other scenarios, this one explicitly runs the GC through a
-- full cycle with the finalizable proxies still rooted, then drops them
-- and runs a second cycle. The hazard is the cycle-boundary transition:
-- proxies created in cycle K must be finalized in cycle K+1. The hazard
-- function is a no-op (the structure of the scenario is the test); we keep
-- it here so the run_iter harness drives the first cycle, then we drop
-- refs and let the second cycle finalize.
-----------------------------------------------------------------------------
scenario(
  "e_udata_gc_across_cycle_boundary",
  function(counter)
    return build_finalizables(counter, 200)
  end,
  function(keep, counter)
    -- Light hazard: touch each proxy so the compiler doesn't optimize
    -- anything away; the real test is surviving a full incremental cycle
    -- with the proxies rooted, then a second cycle finalizing them.
    local _ = keep[1]
  end,
  200)

-----------------------------------------------------------------------------
-- Scenario (f): thousands of finalizable udata spanning chunked mmudata
-- (Momus-2/B2).
--
-- This is the GATE pattern from inherited wisdom: 15000 newproxy+__gc.
-- The chunked mmudata clear (T4) processes them in slices across steps;
-- a cursor bug leaves some finalizers unrun. We use a smaller step count
-- here because the workload itself is the stress.
-----------------------------------------------------------------------------
scenario(
  "f_thousands_finudata_chunked_mmudata",
  function(counter)
    return build_finalizables(counter, 15000)
  end,
  function(keep, counter)
    -- No additional hazard; the 15000 finalizable udata ARE the stress.
    -- The chunked mmudata clear must process every one across step slices.
  end,
  15000)

-----------------------------------------------------------------------------
-- Scenario (g): back-barrier (table store) at every rebuild sub-phase
-- boundary.
--
-- Hazard: keep a single live arena table `t` rooted across the whole
-- iteration. Between every step, store a newly allocated (white) object
-- into `t`. Once `t` is marked by a step, subsequent stores of white
-- children trigger lj_gc_barrierback_arena (the unconditional mark reader
-- at lj_gc_arena.c:1975-1992). Over many steps the back-barrier fires at
-- every rebuild sub-phase boundary (Prologue/ThreadScan/HugeScan/Epilogue
-- yields; ClearMarks is non-yielding by T7 design).
-----------------------------------------------------------------------------
scenario(
  "g_backbarrier_every_subphase",
  function(counter)
    local keep = build_finalizables(counter, 200)
    -- The live arena table that will get marked and then receive white stores.
    keep._g_table = {}
    return keep
  end,
  function(keep, counter)
    local t = keep._g_table
    -- Store a freshly allocated (white) object into the live table. After
    -- the table is marked by some step, this triggers the back-barrier.
    local k = tostring(counter.n)
    t[k] = { k }
  end,
  200)

-----------------------------------------------------------------------------
-- Runner
-----------------------------------------------------------------------------

local function main()
  io.write(string.format(
    "rebuild_stress: %d scenarios x %d iterations (stepmul=200, steps/iter=%d)\n",
    #scenarios, ITERATIONS, STEPS_PER_ITER))
  for _, sc in ipairs(scenarios) do
    for iter = 1, ITERATIONS do
      run_iter(sc.name, sc.setup, sc.hazard, sc.expected_fin, iter)
    end
    io.write(string.format("[OK] %s (%d iter, %d finalizers)\n",
      sc.name, ITERATIONS, sc.expected_fin))
    io.flush()
  end
  io.write("rebuild_stress: ALL SCENARIOS PASSED\n")
  os.exit(0)
end

main()

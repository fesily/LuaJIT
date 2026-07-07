-- Huge traversable object survives a bitmap-sweep cycle, then leaks.
--
-- HYPOTHESIS (design doc "LuaJIT 3.0 new Garbage Collector" / Huge Blocks):
-- huge-block metadata (address, size, mark/gray) is meant to live in a
-- separate hash table keyed by address. That hash is UNBUILT in the current
-- implementation -- only the hugenum/hugemem counters exist (lj_obj.h).
--
-- Consequence under LJ_HASGCMARK (arena build):
--   * Huge GCobjs (>= ArenaHugeThreshold = 512 KB) have no cell bitmap; they
--     are reachable for sweep ONLY via the gc.root nextgc chain.
--   * gc_rebuild_rootchain() reconstructs gc.root purely from arena bitmaps
--     (lj_gc.c:866-895), so a huge survivor is DROPPED from the chain after
--     the first bitmap-sweep cycle it lives through.
--   * Marking still works (header LJ_GC_BLACK, lj_gc.c:116-133), so while a
--     reference is held the object is correctly kept alive.
--   * But once the last reference dies, the object is on NO chain and in NO
--     bitmap -> sweep can never find it -> permanent leak.
--
-- A traversable GCobj that can exceed 512 KB is a GCproto with enough
-- bytecode. We allocate one, let it survive a GC cycle (the chain-drop), drop
-- the reference, and collect -- repeatedly. A correctly tracked huge object is
-- reclaimed each round, so the live heap returns to a flat baseline. A
-- chain-dropped object is unreachable by sweep, so the heap grows by one
-- object per round: a MONOTONIC leak. We assert the baseline stays flat.
--
-- A single alloc/drop is NOT a reliable signal here: collectgarbage("count")
-- carries enough unrelated residue (interned strings, the source buffer, etc.)
-- to mask one object. Accumulation across rounds is the robust detector --
-- a real leak adds the same delta every round; noise does not compound.
--
-- On a LUA_USE_ASSERT arena build this also trips the white-box
-- "close_state: memory leak of N bytes" assert at shutdown, which is the
-- ground-truth confirmation that the objects are unreachable by sweep.
--
-- This test is meaningful only on the arena + gcmark build (the bitmap-sweep
-- path). On other builds it self-skips, mirroring test_alloc_below_cursor.lua.
--
-- Run: luajit -joff test/test_huge_traversable_leak.lua

local pass, fail, skip = 0, 0, 0
local function check(name, cond, msg)
  if cond then pass = pass + 1
  else fail = fail + 1; io.write("FAIL: "..name..(msg and (": "..msg) or "").."\n") end
end

-- checkheap availability is our proxy for "this is the arena build that uses
-- the bitmap-sweep path the bug lives in".
local has_checkheap = pcall(collectgarbage, "checkheap")

-- Build source for a function whose proto bytecode is comfortably over the
-- 512 KB huge threshold. Each "s=s+a" is one ADDVV bytecode and adds NO new
-- constant, so we drive raw bytecode volume without tripping the constant-count
-- limit. 300k instructions produce a multi-MB proto, well past the threshold.
local function build_huge_proto_src(n)
  local parts = { "local s,a=0,1" }
  for i = 1, n do parts[#parts + 1] = "s=s+a" end
  parts[#parts + 1] = "return s"
  return table.concat(parts, "\n")
end

local HUGE_THRESHOLD_KB = 512  -- ArenaHugeThreshold = ArenaSize>>1 = 512 KB.
local ROUNDS = 6

if not has_checkheap then
  skip = skip + 1
  io.write("SKIP: not an arena/gcmark build (no checkheap); "
        .. "bitmap-sweep path not exercised\n")
else
  local src = build_huge_proto_src(300000)

  -- Measure one object's resident size and confirm it really took the huge
  -- path (>= 512 KB). If it didn't, this run proves nothing -- flag it.
  collectgarbage("collect")
  local base0 = collectgarbage("count")
  local probe = assert(loadstring(src))
  local objsize = collectgarbage("count") - base0
  io.write(string.format("huge proto resident size: %.0f KB\n", objsize))
  probe = nil
  collectgarbage("collect"); collectgarbage("collect")

  if objsize < HUGE_THRESHOLD_KB then
    skip = skip + 1
    io.write(string.format(
      "INCONCLUSIVE: proto only %.0f KB (< %d KB); not a huge object, "
      .. "chain-drop path not exercised\n", objsize, HUGE_THRESHOLD_KB))
  else
    -- Each round: allocate, survive a cycle (chain-drop point), drop, reclaim.
    -- Record the settled live-heap size after reclamation.
    local settled = {}
    for r = 1, ROUNDS do
      local f = assert(loadstring(src))
      collectgarbage("collect")          -- survive a cycle: gc_rebuild drops it
      assert(f() == 300000)              -- proto stayed live + intact
      f = nil
      collectgarbage("collect"); collectgarbage("collect")  -- now reclaim
      if has_checkheap then
        local bad = collectgarbage("checkheap")
        check("checkheap_round_"..r, bad == 0, "checkheap="..bad)
      end
      settled[r] = collectgarbage("count")
    end

    -- Growth from the 2nd settled point onward (1st absorbs warm-up residue).
    -- A leak adds ~objsize every round; clean tracking holds the baseline flat.
    local growth = settled[ROUNDS] - settled[2]
    local per_round = growth / (ROUNDS - 2)
    io.write(string.format(
      "settled live heap per round: %s KB\n",
      table.concat((function() local t={} for i=1,ROUNDS do
        t[i]=string.format("%.0f", settled[i]) end return t end)(), " ")))
    io.write(string.format(
      "growth across rounds: %+.0f KB (%.0f KB/round, object %.0f KB)\n",
      growth, per_round, objsize))

    -- If each round retains anywhere near a full object, it is leaking. Use a
    -- generous 0.25x object-size gate so noise can't trip it but a real
    -- per-object leak (~1.0x) always does.
    check("no_monotonic_huge_leak", per_round < objsize * 0.25,
          string.format("%.0f KB/round leaked (>= 1/4 of %.0f KB object)",
                        per_round, objsize))
  end
end

io.write(string.format("\nHuge-traversable-leak: %d passed, %d failed, %d skipped\n",
                       pass, fail, skip))
os.exit(fail == 0 and 0 or 1)

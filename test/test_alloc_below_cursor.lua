-- P0-4: Allocation below the incremental sweep cursor.
--
-- Arena-specific hazard with no analog in other VMs: the bitmap sweep walks
-- arenas in address order, while the allocator can hand back a freed cell
-- from a bin that sits BELOW the current sweep cursor (already-swept region).
-- A new object placed there must not be mistaken for garbage by the rest of
-- the sweep, and must carry correct (block=1) bitmap state. This drives an
-- incremental cycle into the sweep phase, frees objects to seed the bins,
-- allocates fresh objects (likely reusing swept-region cells), finishes the
-- sweep, and asserts every fresh object survived -- with checkheap between
-- steps guarding free-list integrity throughout.
--
-- Run: luajit -joff test/test_alloc_below_cursor.lua

local pass, fail = 0, 0
local function check(name, cond, msg)
  if cond then pass = pass + 1
  else fail = fail + 1; io.write("FAIL: "..name..(msg and (": "..msg) or "").."\n") end
end
local has_checkheap = pcall(collectgarbage, "checkheap")
local function healthy(name)
  if not has_checkheap then pass = pass + 1; return end  -- no-op off arena builds
  local bad = collectgarbage("checkheap")
  check(name, bad == 0, "checkheap="..bad)
end

-- 1. Free-into-bin then realloc during sweep.
do
  collectgarbage("collect")
  local pool = {}
  for i = 1, 6000 do pool[i] = { id = i, s = "p"..i } end
  collectgarbage("stop")
  -- Run full mark; land in the sweep phase.
  for s = 1, 400 do collectgarbage("step", 4) end
  healthy("sweep_entered")
  -- Free the even half -> their cells go to bins (below/around the cursor).
  for i = 1, 6000, 2 do pool[i] = nil end
  collectgarbage("step", 2)
  healthy("sweep_after_free")
  -- Allocate fresh objects mid-sweep; these may reuse swept-region cells.
  local fresh = {}
  for i = 1, 6000, 2 do
    fresh[i] = { reborn = true, id = i, extra = { i } }
    pool[i] = fresh[i]
    if i % 200 == 1 then collectgarbage("step", 1) end
  end
  healthy("sweep_after_realloc")
  collectgarbage("restart")
  collectgarbage("collect")
  healthy("sweep_complete")
  -- All fresh objects (allocated below/around the cursor) must survive.
  local survived = 0
  for i = 1, 6000, 2 do
    if fresh[i] and fresh[i].reborn and fresh[i].id == i then survived = survived + 1 end
  end
  check("below_cursor_survived", survived == 3000, "survived="..survived)
  -- And the never-freed odd half must be fully intact.
  local intact = true
  for i = 2, 6000, 2 do
    if not pool[i] or pool[i].id ~= i then intact = false break end
  end
  check("untouched_intact", intact)
end

-- 2. Interleave free + alloc + step finely, so allocations land at many
--    different sweep-cursor positions (not just one window).
do
  collectgarbage("collect")
  local live = {}
  for i = 1, 8000 do live[i] = { i, "x"..i } end
  collectgarbage("stop")
  for s = 1, 300 do collectgarbage("step", 3) end   -- into sweep
  healthy("interleave_sweep")
  local newobjs = {}
  for i = 1, 8000 do
    if i % 3 == 0 then live[i] = nil end             -- free some
    if i % 3 == 1 then                               -- alloc some
      newobjs[i] = { born_in_sweep = true, id = i }
    end
    if i % 64 == 0 then collectgarbage("step", 1) end -- advance cursor a bit
  end
  healthy("interleave_mutated")
  collectgarbage("restart")
  collectgarbage("collect")
  healthy("interleave_complete")
  local ok = true
  for i = 1, 8000 do
    if i % 3 == 1 and (not newobjs[i] or not newobjs[i].born_in_sweep) then ok = false break end
    if i % 3 == 2 and (not live[i] or live[i][1] ~= i) then ok = false break end
  end
  check("interleave_survival", ok)
end

io.write(string.format("\nAlloc-below-cursor tests: %d passed, %d failed\n", pass, fail))
if fail > 0 then os.exit(1) end

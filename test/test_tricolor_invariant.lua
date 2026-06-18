-- P0-2: Strong/weak tri-color invariant under incremental marking.
--
-- Borrows from Lua ltests checkgrayobj and SpiderMonkey's verify-barrier
-- gczeal modes: deliberately create black->white edges by writing freshly
-- allocated (white) objects into already-marked (black) parents while an
-- incremental GC cycle is mid-flight, then both (a) call checkheap each step
-- to assert the allocator stays consistent through barrier traffic, and
-- (b) complete the cycle and verify every written child SURVIVED -- a lost
-- child is exactly the symptom of a missed/incorrect write barrier (the
-- backward barrier failing to re-gray the parent or push to the SSB).
--
-- Run: luajit -joff test/test_tricolor_invariant.lua

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

-- 1. Backward barrier on tables: blacken parents, then store white children.
do
  collectgarbage("collect")
  local roots = {}
  for i = 1, 400 do roots[i] = { idx = i, kids = {} } end
  collectgarbage("collect")        -- age roots so they can blacken next cycle
  collectgarbage("stop")
  -- Start a fresh cycle and propagate so roots[] become black.
  for s = 1, 40 do collectgarbage("step", 4) end
  healthy("tbar_after_mark")
  -- Now write white children into (black) parents across several steps,
  -- forcing the backward barrier each time; checkheap between writes.
  for wave = 1, 30 do
    for i = 1, 400 do
      roots[i].kids[wave] = { tag = "w"..wave.."_"..i, payload = { wave, i } }
      roots[i].latest = { tag = "latest"..wave.."_"..i }
    end
    collectgarbage("step", 1)
    if wave % 5 == 0 then healthy("tbar_wave_"..wave) end
  end
  collectgarbage("restart")
  collectgarbage("collect")
  healthy("tbar_complete")
  -- Every child written into a black parent must have survived.
  local ok = true
  for i = 1, 400 do
    if not roots[i].latest or roots[i].latest.tag ~= "latest30_"..i then ok = false break end
    for wave = 1, 30 do
      local k = roots[i].kids[wave]
      if not k or k.tag ~= "w"..wave.."_"..i then ok = false break end
    end
    if not ok then break end
  end
  check("tbar_children_survived", ok)
end

-- 2. Forward barrier on non-table objects: rewrite a closed upvalue (white
--    value into a non-gray closure) mid-cycle. Exercises lj_gc_barrierf's
--    else-branch -- the same path whose stale state-assert this suite
--    surfaced -- and verifies the replacement value survives.
do
  collectgarbage("collect")
  local cells = {}
  for i = 1, 200 do
    local v = { gen = 0, id = i }
    cells[i] = {
      get = function() return v end,
      set = function(nv) v = nv end,
    }
  end
  collectgarbage("collect")
  collectgarbage("stop")
  for s = 1, 60 do collectgarbage("step", 3) end   -- drive deep into the cycle
  healthy("fbar_mid_cycle")
  for gen = 1, 10 do
    for i = 1, 200 do cells[i].set({ gen = gen, id = i, fresh = {} }) end
    collectgarbage("step", 1)
  end
  collectgarbage("restart")
  collectgarbage("collect")
  healthy("fbar_complete")
  local ok = true
  for i = 1, 200 do
    local v = cells[i].get()
    if not v or v.gen ~= 10 or v.id ~= i or not v.fresh then ok = false break end
  end
  check("fbar_value_survived", ok)
end

-- 3. Re-graying churn: repeatedly mutate the same black parents so the
--    backward barrier fires thousands of times and the SSB flushes into the
--    per-arena gray stacks. Stress the gray-stack growth + flush path.
do
  collectgarbage("collect")
  local big = {}
  for i = 1, 800 do big[i] = {} end
  collectgarbage("collect")
  collectgarbage("stop")
  for s = 1, 50 do collectgarbage("step", 4) end
  for round = 1, 100 do
    for i = 1, 800 do big[i].slot = { round = round, i = i } end
    collectgarbage("step", 1)
  end
  healthy("regray_mid")
  collectgarbage("restart")
  collectgarbage("collect")
  healthy("regray_done")
  local ok = true
  for i = 1, 800 do
    if not big[i].slot or big[i].slot.round ~= 100 then ok = false break end
  end
  check("regray_survived", ok)
end

io.write(string.format("\nTri-color invariant tests: %d passed, %d failed\n", pass, fail))
if fail > 0 then os.exit(1) end

-- P0-3: Arena free-list integrity (bins + ranges + slab refill).
--
-- The arena allocator's intrusive same-size bins, sorted range array and
-- 16-way slab refill have NO analog in other VM GCs, so generic GC tests
-- never touch this surface. This test hammers the alloc/free fast paths and
-- after every burst calls collectgarbage("checkheap"), the read-only
-- consistency checker, which asserts: every binned cell is in range and in
-- the allocated bitmap state, no free-list cycle, binmask agrees with the
-- bins, range entries are in-range Free heads sorted by size, and the
-- accounting invariant bins+ranges <= freecells (over-count == double free).
--
-- Requires a -DLUAJIT_ENABLE_GCARENA build; checkheap returns 0 (== healthy)
-- as a no-op on non-arena builds, so the asserts below still hold trivially.
-- Run: luajit -joff test/test_arena_freelist.lua

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

-- 1. Same-size churn: drive the intrusive bins hard. Repeatedly free and
--    re-allocate same-shape tables so cells cycle through one bin, exercising
--    the bin push/pop fast path and (on refill) the 16-way slab carve.
do
  collectgarbage("collect")
  local pool = {}
  for i = 1, 4000 do pool[i] = { i } end        -- uniform 1-ish cell shape
  healthy("churn_seed")
  for round = 1, 200 do
    for i = 1, 4000 do pool[i] = nil end         -- mass free -> bins
    for i = 1, 4000 do pool[i] = { round + i } end -- realloc -> bin pop / refill
    if round % 20 == 0 then healthy("churn_round_"..round) end
  end
  collectgarbage("collect")
  healthy("churn_done")
end

-- 2. Mixed sizes: spread allocations across all 8 bins plus range-sized
--    blocks, then free in an interleaved order so coalescing and the sorted
--    range array get exercised alongside the bins.
do
  collectgarbage("collect")
  local objs = {}
  local function mk(sz)            -- sz controls cell count via array part
    local t = {}; for j = 1, sz do t[j] = j end; return t
  end
  for i = 1, 6000 do objs[i] = mk((i % 24) + 1) end
  healthy("mixed_seed")
  -- Free every other one (fragments the heap -> bins + ranges populate).
  for i = 1, 6000, 2 do objs[i] = nil end
  collectgarbage("step", 1)
  healthy("mixed_frag")
  -- Refill the holes with different sizes (forces next-fit / range splits).
  for i = 1, 6000, 2 do objs[i] = mk(((i + 7) % 24) + 1) end
  healthy("mixed_refill")
  collectgarbage("collect")
  healthy("mixed_done")
end

-- 3. Scavenge path: accumulate enough freed space that allocslow triggers a
--    full scavenge (bitmap rescan + coalesce + frontier rollback), then make
--    sure the rebuilt free lists are still consistent.
do
  collectgarbage("collect")
  local keep = {}
  for i = 1, 20000 do
    local t = { a = i, b = "s"..i, c = { i } }
    if i % 4 ~= 0 then keep[#keep+1] = t end   -- drop 1/4 to create free runs
  end
  collectgarbage("step", 1)
  healthy("scavenge_mid")
  -- Allocate larger blocks to force best-fit from the (coalesced) ranges.
  local big = {}
  for i = 1, 2000 do
    local t = {}; for j = 1, 30 do t[j] = j end; big[i] = t
  end
  healthy("scavenge_bigfit")
  collectgarbage("collect")
  healthy("scavenge_done")
end

io.write(string.format("\nArena free-list tests: %d passed, %d failed\n", pass, fail))
if fail > 0 then os.exit(1) end

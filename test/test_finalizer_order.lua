-- Finalizer ordering, resurrection, and re-entrancy.
--
-- Pins the observable finalizer contract that the arena (LJ_HASGCMARK)
-- collector must preserve:
--  (a) ARENA: finalizers run in ASCENDING registration order -- first
--      registered dies first (registration FIFO). Design Finalizers §4.2.
--      Classic reverse-LIFO is NOT the arena contract.
--      CLASSIC (no LJ_HASGCMARK): keeps the historical reverse-LIFO walk
--      (last created dies first); asserted separately below.
--  (b) an object resurrected by its finalizer survives the current cycle and
--      is NOT finalized again unless explicitly re-armed,
--  (c) allocating (and triggering GC work) from inside a finalizer is safe.
--
-- TDD NOTE (plan eliminate-mmudata-ring todo 3):
--   This file locks the registration-FIFO contract for the arena path BEFORE
--   the registry-scan + fin_queue drain rewrite (todos 4/5) lands. Under the
--   current arena code the finalizer walk is still reverse-LIFO, so the
--   arena order asserts are expected RED until todos 4+5 land; they turn GREEN
--   once separateudata enqueues fin_queue in registration order and the
--   drain pops FIFO. Classic asserts stay GREEN throughout.
--
-- Run: luajit -joff test/test_finalizer_order.lua

local pass, fail = 0, 0
local function check(name, cond, msg)
  if cond then pass = pass + 1
  else fail = fail + 1; io.write("FAIL: "..name..(msg and (": "..msg) or "").."\n") end
end

-- Arena (LJ_HASGCMARK) builds expose collectgarbage("checkheap"); classic
-- builds do not. This is the runtime discriminator for which order contract
-- applies: arena = registration FIFO, classic = reverse LIFO.
local has_checkheap = pcall(collectgarbage, "checkheap")
local function healthy(name)
  if not has_checkheap then pass = pass + 1; return end  -- no-op off arena builds
  local bad = collectgarbage("checkheap")
  check(name, bad == 0, "checkheap="..bad)
end

-- 1. Finalize order.
--    ARENA: registration FIFO -- for a batch separated in one atomic pass the
--          drain is strictly ascending (first registered dies first).
--    CLASSIC: reverse LIFO -- strictly descending for a small batch.
do
  -- 1a. Small batch: strict contract order.
  local small = {}
  for i = 1, 12 do
    local u = newproxy(true)
    getmetatable(u).__gc = function() small[#small + 1] = i end
  end
  collectgarbage("collect"); collectgarbage("collect")
  check("finalize_small_count", #small == 12, "#small="..#small)
  if has_checkheap then
    -- Arena: ascending registration FIFO (1,2,...,12).
    local asc = true
    for k = 2, #small do if small[k] ~= small[k-1] + 1 then asc = false break end end
    check("finalize_small_strict_fifo", asc and small[1] == 1,
          #small >= 1 and ("seq="..table.concat(small, ",")) or "n/a")
  else
    -- Classic: strict reverse LIFO (12,11,...,1).
    local desc = true
    for k = 2, #small do if small[k] ~= small[k-1] - 1 then desc = false break end end
    check("finalize_small_strict_reverse", desc and small[1] == 12,
          #small >= 1 and ("seq="..table.concat(small, ",")) or "n/a")
  end

  -- 1b. Large batch: every object finalized exactly once, order predominantly
  --     the contract direction, which is the observable contract at scale
  --     (a single FIFO queue drains in registration order regardless of size).
  local order = {}
  for i = 1, 500 do
    local u = newproxy(true)
    getmetatable(u).__gc = function() order[#order + 1] = i end
  end
  collectgarbage("collect"); collectgarbage("collect")
  check("finalize_count", #order == 500, "#order="..#order)
  local seen = {}
  local dup = false
  for _, v in ipairs(order) do if seen[v] then dup = true break end; seen[v] = true end
  check("finalize_each_once", (#order == 500) and not dup, "dup="..tostring(dup))
  if has_checkheap then
    -- Arena: predominantly ascending (FIFO). Allow slack for any re-entrancy
    -- that re-enqueues, but the batch itself drains in registration order.
    local asc = 0
    for k = 2, #order do if order[k] > order[k-1] then asc = asc + 1 end end
    check("finalize_predominantly_fifo", asc >= (#order - 1) * 0.9,
          "ascending="..asc.."/"..(#order - 1))
  else
    -- Classic: predominantly descending (LIFO walk).
    local desc = 0
    for k = 2, #order do if order[k] < order[k-1] then desc = desc + 1 end end
    check("finalize_predominantly_reverse", desc >= (#order - 1) * 0.9,
          "descending="..desc.."/"..(#order - 1))
  end
  healthy("after_order")
end

-- 2. cdata finalizer order (arena FIFO / classic reverse), same contract.
--    ffi.gc registers a finalizer at registration time; the queue must drain
--    in the same order as udata.
do
  local ffi = require("ffi")
  ffi.cdef[[ typedef struct { int id; } FinOrderR; ]]
  local seq = {}
  local function make(i)
    local cd = ffi.new("FinOrderR", { id = i })
    ffi.gc(cd, function() seq[#seq + 1] = i end)
    return cd
  end
  do
    local keep = {}
    for i = 1, 40 do keep[i] = make(i) end
    for i = 1, #keep do keep[i] = nil end
    keep = nil
  end
  collectgarbage("collect"); collectgarbage("collect")
  check("cdata_finalize_count", #seq == 40, "#seq="..#seq)
  if has_checkheap then
    local asc = true
    for k = 2, #seq do if seq[k] ~= seq[k-1] + 1 then asc = false break end end
    check("cdata_finalize_strict_fifo", asc and (seq[1] == 1),
          #seq >= 1 and ("seq="..table.concat(seq, ",")) or "n/a")
  else
    local desc = true
    for k = 2, #seq do if seq[k] ~= seq[k-1] - 1 then desc = false break end end
    check("cdata_finalize_strict_reverse", desc and (seq[1] == 40),
          #seq >= 1 and ("seq="..table.concat(seq, ",")) or "n/a")
  end
  healthy("after_cdata_order")
end

-- 3. Resurrection: a finalizer stashes its object into a global table. The
--    object must survive the cycle it was finalized in, and must NOT be
--    finalized a second time (the finalizer was consumed).
do
  local stash = {}
  local fin_count = 0
  do
    local mt = { __gc = function(self) fin_count = fin_count + 1; stash[#stash + 1] = self end }
    for i = 1, 100 do
      local u = newproxy(true)
      getmetatable(u).__gc = mt.__gc
    end
  end
  collectgarbage("collect")
  collectgarbage("collect")
  check("resurrect_finalized_once", fin_count == 100, "fin_count="..fin_count)
  check("resurrect_stashed", #stash == 100, "#stash="..#stash)
  healthy("after_resurrect_first")
  -- Drop the strong refs; the proxies are NOT re-finalized (already consumed).
  for i = 1, #stash do stash[i] = nil end
  stash = nil
  collectgarbage("collect")
  collectgarbage("collect")
  check("no_double_finalize", fin_count == 100, "fin_count="..fin_count)
  healthy("after_resurrect_drop")
end

-- 4. Re-entrant allocation: a finalizer allocates new objects and forces GC
--    work. Must not corrupt the heap or lose the newly allocated objects.
do
  local survivors = {}
  local fin = 0
  for i = 1, 200 do
    local u = newproxy(true)
    getmetatable(u).__gc = function()
      fin = fin + 1
      local t = {}
      for j = 1, 10 do t[j] = "fin"..fin.."_"..j end
      survivors[fin] = t                  -- keep them alive past this cycle
      if fin % 16 == 0 then collectgarbage("step", 1) end  -- GC from finalizer
    end
  end
  collectgarbage("collect")
  collectgarbage("collect")
  check("reentrant_fin_count", fin == 200, "fin="..fin)
  local intact = 0
  for i = 1, fin do
    if survivors[i] and #survivors[i] == 10 and survivors[i][1] == "fin"..i.."_1" then
      intact = intact + 1
    end
  end
  check("reentrant_alloc_intact", intact == fin, "intact="..intact.."/"..fin)
  healthy("after_reentrant")
end

-- 5. Multi-cycle staggered survivors (arena FIFO stability).
--    Register A,B,C,D,E; keep B..E alive while A dies first; then drop B..E.
--    Rest must finalize in registration order B,C,D,E (not reordered by
--    fin_order_remove). Classic is reverse-LIFO and not asserted here.
if has_checkheap then
  local seq = {}
  local keep = {}
  for i = 1, 5 do
    local u = newproxy(true)
    local id = i
    getmetatable(u).__gc = function() seq[#seq + 1] = id end
    if i >= 2 then keep[#keep + 1] = u end
  end
  collectgarbage("collect"); collectgarbage("collect")
  check("stagger_first_only_A", #seq == 1 and seq[1] == 1,
        #seq >= 1 and ("seq="..table.concat(seq, ",")) or ("#seq="..#seq))
  keep = nil
  collectgarbage("collect"); collectgarbage("collect")
  check("stagger_rest_fifo", #seq == 5
        and seq[2] == 2 and seq[3] == 3 and seq[4] == 4 and seq[5] == 5,
        #seq >= 1 and ("seq="..table.concat(seq, ",")) or ("#seq="..#seq))
  healthy("after_stagger")
end

-- 6. Nested fullgc from inside a finalizer must not free remaining queue.
--    First finalizer forces collectgarbage("collect"); all 50 must still run.
do
  local n = 0
  for i = 1, 50 do
    local u = newproxy(true)
    getmetatable(u).__gc = function()
      n = n + 1
      if n == 1 then collectgarbage("collect") end
    end
  end
  collectgarbage("collect")
  collectgarbage("collect")
  check("nested_fullgc_all_finalized", n == 50, "n="..n)
  healthy("after_nested_fullgc")
end

io.write(string.format("\nFinalizer order tests: %d passed, %d failed\n", pass, fail))
if fail > 0 then os.exit(1) end

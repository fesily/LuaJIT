-- Adversarial GC tests for bitmap sweep + barrier correctness
-- Run with: luajit -joff test/test_gc_adversarial.lua
local ffi = require("ffi")

local pass, fail = 0, 0
local function check(name, cond, msg)
  if cond then
    pass = pass + 1
  else
    fail = fail + 1
    io.write("FAIL: " .. name .. (msg and (": " .. msg) or "") .. "\n")
  end
end

-- ============================================================
-- 1. Barrier: write white values into black tables during mark
-- ============================================================
do
  collectgarbage("stop")
  local roots = {}
  for i = 1, 200 do
    roots[i] = { data = {} }
  end
  -- Start a GC cycle and propagate partially
  collectgarbage("step", 10)
  collectgarbage("step", 10)
  -- Now some tables are black. Write new (white) values into them.
  for i = 1, 200 do
    roots[i].new_child = { val = "barrier_test_" .. i }
    roots[i].new_tab = { [tostring(i)] = true }
  end
  -- Complete the GC cycle
  collectgarbage("restart")
  collectgarbage("collect")
  -- All new_child/new_tab must survive
  local survived = 0
  for i = 1, 200 do
    if roots[i].new_child and roots[i].new_child.val == "barrier_test_" .. i then
      survived = survived + 1
    end
  end
  check("barrier_write_to_black", survived == 200,
        "survived=" .. survived .. "/200")
end

-- ============================================================
-- 2. Barrier: massive backward barrier (table re-graying)
-- ============================================================
do
  collectgarbage("stop")
  local big = {}
  for i = 1, 500 do big[i] = {} end
  collectgarbage("step", 50)
  -- big should be black now. Write 10000 new values rapidly.
  for round = 1, 50 do
    for i = 1, 200 do
      big[i] = { round = round, idx = i }
    end
    collectgarbage("step", 1)
  end
  collectgarbage("restart")
  collectgarbage("collect")
  local ok = true
  for i = 1, 200 do
    if not big[i] or big[i].round ~= 50 then ok = false; break end
  end
  check("massive_backward_barrier", ok)
end

-- ============================================================
-- 3. Incremental sweep safety: allocate during sweep phase
-- ============================================================
do
  collectgarbage("stop")
  local live = {}
  for i = 1, 1000 do live[i] = { id = i } end
  -- Run full mark
  for s = 1, 200 do collectgarbage("step", 10) end
  -- Now should be in sweep phase. Allocate new objects during sweep.
  local new_objs = {}
  for i = 1, 500 do
    new_objs[i] = { created_during_sweep = true, val = i }
    collectgarbage("step", 1)
  end
  collectgarbage("restart")
  collectgarbage("collect")
  local ok = true
  for i = 1, 500 do
    if not new_objs[i] or not new_objs[i].created_during_sweep then
      ok = false; break
    end
  end
  check("alloc_during_sweep", ok)
end

-- ============================================================
-- 4. Finalize + resurrect: userdata __gc resurrects objects
-- ============================================================
do
  local resurrected = {}
  local count = 0
  for i = 1, 100 do
    local u = newproxy(true)
    local payload = { data = "payload_" .. i }
    getmetatable(u).__gc = function(self)
      count = count + 1
      resurrected[count] = payload
    end
  end
  collectgarbage("collect")
  collectgarbage("collect")
  -- Resurrected payloads must be intact
  check("finalize_resurrect_count", count >= 90,
        "count=" .. count)
  local intact = 0
  for i = 1, count do
    if resurrected[i] and type(resurrected[i].data) == "string" then
      intact = intact + 1
    end
  end
  check("finalize_resurrect_integrity", intact == count,
        "intact=" .. intact .. "/" .. count)
end

-- ============================================================
-- 5. Weak table clearing: ephemerons
-- ============================================================
do
  local wk = setmetatable({}, { __mode = "k" })
  local wv = setmetatable({}, { __mode = "v" })
  local wkv = setmetatable({}, { __mode = "kv" })
  local strong_keys = {}
  local strong_vals = {}
  for i = 1, 500 do
    local k1 = { id = i }
    local k2 = { id = i }
    local k3 = { id = i }
    local v = { val = i }
    wk[k1] = v
    wv[i] = v
    wkv[k3] = v
    if i <= 50 then
      strong_keys[i] = k1
      strong_vals[i] = v
    end
  end
  collectgarbage("collect")
  collectgarbage("collect")
  -- wk: only entries with strong keys survive
  local wk_count = 0
  for _ in pairs(wk) do wk_count = wk_count + 1 end
  check("weak_key_clearing", wk_count == 50,
        "wk_count=" .. wk_count)
  -- wv: entries with strong values survive (strong_vals keeps 50 values)
  local wv_alive = 0
  for _, v in pairs(wv) do
    if v then wv_alive = wv_alive + 1 end
  end
  check("weak_val_clearing", wv_alive == 50,
        "wv_alive=" .. wv_alive)
  -- wkv: entries where EITHER key or value is dead get collected
  local wkv_count = 0
  for _ in pairs(wkv) do wkv_count = wkv_count + 1 end
  check("weak_kv_clearing", wkv_count == 0,
        "wkv_count=" .. wkv_count)
end

-- ============================================================
-- 6. Coroutine stack references across GC
-- ============================================================
do
  local results = {}
  local function producer()
    for i = 1, 100 do
      local big = {}
      for j = 1, 20 do big[j] = "co_" .. i .. "_" .. j end
      coroutine.yield(big)
    end
  end
  local co = coroutine.create(producer)
  for i = 1, 100 do
    local ok, val = coroutine.resume(co)
    assert(ok, "coroutine failed")
    results[i] = val
    if i % 10 == 0 then collectgarbage("collect") end
  end
  local intact = 0
  for i = 1, 100 do
    if results[i] and #results[i] == 20 then intact = intact + 1 end
  end
  check("coroutine_gc", intact == 100, "intact=" .. intact)
end

-- ============================================================
-- 7. Upvalue closing + GC interaction
-- ============================================================
do
  local closures = {}
  for i = 1, 200 do
    local x = { val = i }
    local y = "uv_" .. i
    closures[i] = function() return x, y end
  end
  -- Force GC between creation and use
  collectgarbage("collect")
  collectgarbage("collect")
  local ok = true
  for i = 1, 200 do
    local x, y = closures[i]()
    if x.val ~= i or y ~= "uv_" .. i then ok = false; break end
  end
  check("upvalue_closing", ok)
end

-- ============================================================
-- 8. FFI cdata: fixed-size cdata across GC cycles
-- ============================================================
do
  ffi.cdef[[
    typedef struct { int x; int y; } Point;
  ]]
  local points = {}
  for i = 1, 500 do
    points[i] = ffi.new("Point", { x = i, y = i * 2 })
  end
  collectgarbage("collect")
  collectgarbage("collect")
  local ok = true
  for i = 1, 500 do
    if points[i].x ~= i or points[i].y ~= i * 2 then ok = false; break end
  end
  check("ffi_fixed_cdata", ok)
end

-- ============================================================
-- 9. FFI cdata: VLA cdata across GC cycles
-- ============================================================
do
  local arrays = {}
  for i = 1, 200 do
    local arr = ffi.new("uint8_t[?]", 64)
    for j = 0, 63 do arr[j] = (i + j) % 256 end
    arrays[i] = arr
  end
  collectgarbage("collect")
  collectgarbage("collect")
  local ok = true
  for i = 1, 200 do
    for j = 0, 63 do
      if arrays[i][j] ~= (i + j) % 256 then ok = false; break end
    end
    if not ok then break end
  end
  check("ffi_vla_cdata", ok)
end

-- ============================================================
-- 10. FFI cdata: finalized cdata
-- ============================================================
do
  ffi.cdef[[
    typedef struct { int id; } CdataFin;
  ]]
  local fin_count = 0
  for i = 1, 100 do
    local cd = ffi.new("CdataFin", { id = i })
    ffi.gc(cd, function() fin_count = fin_count + 1 end)
  end
  collectgarbage("collect")
  collectgarbage("collect")
  check("ffi_cdata_finalize", fin_count >= 90,
        "fin_count=" .. fin_count)
end

-- ============================================================
-- 11. Incremental GC: barrier + sweep interleaving stress
-- ============================================================
do
  collectgarbage("stop")
  local root = {}
  -- Build a graph
  for i = 1, 100 do
    root[i] = { children = {} }
    for j = 1, 10 do
      root[i].children[j] = { parent = root[i], val = i * 10 + j }
    end
  end
  -- Interleave mutations with GC steps
  for round = 1, 50 do
    -- Mutate: rewire some pointers
    for i = 1, 100 do
      local j = ((i + round) % 100) + 1
      root[i].children[1] = root[j].children[5]
    end
    collectgarbage("step", 5)
  end
  collectgarbage("restart")
  collectgarbage("collect")
  -- Verify no dangling pointers
  local ok = true
  for i = 1, 100 do
    if not root[i] or not root[i].children then ok = false; break end
    for j = 1, 10 do
      local c = root[i].children[j]
      if c and type(c.val) ~= "number" then ok = false; break end
    end
    if not ok then break end
  end
  check("incremental_barrier_sweep", ok)
end

-- ============================================================
-- 12. Mixed FFI + Lua objects incremental stress
-- ============================================================
do
  collectgarbage("stop")
  local objs = {}
  for i = 1, 1000 do
    if i % 3 == 0 then
      objs[i] = ffi.new("uint8_t[?]", 32)
    elseif i % 3 == 1 then
      objs[i] = { val = i }
    else
      objs[i] = "str_" .. i
    end
    if i % 50 == 0 then collectgarbage("step", 2) end
  end
  -- Drop half
  for i = 1, 1000, 2 do objs[i] = nil end
  for s = 1, 100 do collectgarbage("step", 5) end
  collectgarbage("restart")
  collectgarbage("collect")
  local alive = 0
  for i = 2, 1000, 2 do
    if objs[i] then alive = alive + 1 end
  end
  check("mixed_ffi_lua_incremental", alive == 500, "alive=" .. alive)
end

-- ============================================================
-- 13. String resurrection during sweep
-- ============================================================
do
  local strs = {}
  for i = 1, 10000 do
    strs[i] = "unique_" .. i
  end
  -- Drop all
  strs = nil
  collectgarbage("stop")
  collectgarbage("step", 200)
  -- Now in sweep: re-create strings that might match dead interned ones
  local new_strs = {}
  for i = 1, 5000 do
    new_strs[i] = "unique_" .. i
    if i % 100 == 0 then collectgarbage("step", 1) end
  end
  collectgarbage("restart")
  collectgarbage("collect")
  local ok = true
  for i = 1, 5000 do
    if new_strs[i] ~= "unique_" .. i then ok = false; break end
  end
  check("string_resurrection_sweep", ok)
end

-- ============================================================
-- 14. Large table hash resize during GC
-- ============================================================
do
  collectgarbage("stop")
  local t = {}
  for i = 1, 5000 do
    t["key_" .. i] = { val = i }
    if i % 100 == 0 then collectgarbage("step", 2) end
  end
  collectgarbage("restart")
  collectgarbage("collect")
  local ok = true
  for i = 1, 5000 do
    if not t["key_" .. i] or t["key_" .. i].val ~= i then ok = false; break end
  end
  check("table_hash_resize_gc", ok)
end

-- ============================================================
-- 15. Rapid fullgc cycles (memory accounting stress)
-- ============================================================
do
  for cycle = 1, 200 do
    local t = {}
    for i = 1, 500 do t[i] = { tostring(i) } end
    collectgarbage("collect")
  end
  local mem = collectgarbage("count")
  check("rapid_fullgc", mem < 1024, "mem=" .. math.floor(mem) .. "KB")
end

-- ============================================================
io.write(string.format("\nResults: %d passed, %d failed\n", pass, fail))
if fail > 0 then os.exit(1) end

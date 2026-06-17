-- Stress tests for bitmap sweep GC
local N = 500000

-- 1. Rapid alloc/free churn: tables with string keys
do
  local live = {}
  for i = 1, N do
    live[i % 1000 + 1] = { key = "str" .. i, val = i }
    if i % 10000 == 0 then collectgarbage("collect") end
  end
  for i = 1, #live do live[i] = nil end
  collectgarbage("collect")
  collectgarbage("collect")
  io.write("1. alloc/free churn: OK\n")
end

-- 2. Deep nested tables (tests mark propagation depth)
do
  local root = {}
  local cur = root
  for i = 1, 5000 do
    cur.child = { depth = i, name = "node" .. i }
    cur = cur.child
  end
  collectgarbage("collect")
  -- Verify chain intact
  cur = root
  local depth = 0
  while cur.child do cur = cur.child; depth = depth + 1 end
  assert(depth == 5000, "deep chain broken: " .. depth)
  root = nil
  collectgarbage("collect")
  collectgarbage("collect")
  io.write("2. deep nested tables: OK\n")
end

-- 3. Weak tables: keys and values collected properly
do
  local wk = setmetatable({}, { __mode = "k" })
  local wv = setmetatable({}, { __mode = "v" })
  for i = 1, 10000 do
    local k = { id = i }
    local v = { data = "val" .. i }
    wk[k] = i
    wv[i] = v
  end
  collectgarbage("collect")
  collectgarbage("collect")
  -- Most entries should be collected
  local wk_count, wv_count = 0, 0
  for _ in pairs(wk) do wk_count = wk_count + 1 end
  for _ in pairs(wv) do wv_count = wv_count + 1 end
  assert(wk_count == 0, "weak key table not collected: " .. wk_count)
  assert(wv_count == 0, "weak val table not collected: " .. wv_count)
  io.write("3. weak tables: OK\n")
end

-- 4. Finalizers (__gc metamethods)
do
  local finalized = 0
  local mt = { __gc = function() finalized = finalized + 1 end }
  for i = 1, 1000 do
    local u = newproxy(true)
    getmetatable(u).__gc = function() finalized = finalized + 1 end
  end
  collectgarbage("collect")
  collectgarbage("collect")
  assert(finalized >= 900, "too few finalizations: " .. finalized)
  io.write("4. finalizers: OK (finalized=" .. finalized .. ")\n")
end

-- 5. String interning stress (exercises sweepstr bitmap path)
do
  local strs = {}
  for i = 1, 100000 do
    strs[i % 500 + 1] = string.rep("x", i % 50 + 1) .. tostring(i)
  end
  collectgarbage("collect")
  for i = 1, 100000 do
    local s = "lookup" .. (i % 10000)
    strs[i % 500 + 1] = s
  end
  collectgarbage("collect")
  collectgarbage("collect")
  io.write("5. string interning stress: OK\n")
end

-- 6. Incremental GC stress: allocate while GC is running
do
  collectgarbage("stop")
  local tabs = {}
  for i = 1, 50000 do
    tabs[i % 200 + 1] = { "a" .. i, "b" .. i, i }
    if i % 100 == 0 then collectgarbage("step", 1) end
  end
  collectgarbage("restart")
  collectgarbage("collect")
  collectgarbage("collect")
  io.write("6. incremental GC stress: OK\n")
end

-- 7. Upvalue/closure stress
do
  local closures = {}
  for i = 1, 10000 do
    local x = "upval" .. i
    closures[i % 500 + 1] = function() return x end
  end
  collectgarbage("collect")
  -- Verify surviving closures work
  for i = 1, #closures do
    if closures[i] then closures[i]() end
  end
  closures = nil
  collectgarbage("collect")
  collectgarbage("collect")
  io.write("7. upvalue/closure stress: OK\n")
end

-- 8. Mixed alloc during sweep (string resurrection scenario)
do
  for cycle = 1, 100 do
    local t = {}
    for i = 1, 1000 do
      t[i] = { key = "cycle" .. cycle .. "_" .. i }
    end
    collectgarbage("step", 50)
    -- Create new strings that might match dead ones during sweep
    for i = 1, 500 do
      local _ = "cycle" .. cycle .. "_" .. i
    end
    collectgarbage("step", 50)
  end
  collectgarbage("collect")
  io.write("8. alloc during sweep: OK\n")
end

-- 9. Long-running: many full GC cycles
do
  for cycle = 1, 500 do
    local t = {}
    for i = 1, 2000 do
      t[i] = { tostring(i), name = "n" .. i }
    end
    collectgarbage("collect")
  end
  io.write("9. long-running (500 full cycles): OK\n")
end

-- 10. Fragmentation: varying object sizes
do
  local objs = {}
  for i = 1, 50000 do
    local size = i % 20 + 1
    local t = {}
    for j = 1, size do t[j] = j end
    objs[i % 1000 + 1] = t
  end
  collectgarbage("collect")
  collectgarbage("collect")
  local mem_after = collectgarbage("count")
  io.write("10. fragmentation: OK (mem=" .. math.floor(mem_after) .. "KB)\n")
end

io.write("\nAll stress tests passed.\n")

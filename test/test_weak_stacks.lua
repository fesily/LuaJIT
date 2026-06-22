-- Targeted tests for LJ_HASGCMARK weak table stacks.
-- Run with: luajit [-joff] test/test_weak_stacks.lua

local pass, fail = 0, 0
local function check(name, cond, msg)
  if cond then pass = pass + 1
  else fail = fail + 1; io.write("FAIL: "..name..(msg and (": "..msg) or "").."\n") end
end

local has_checkheap = pcall(collectgarbage, "checkheap")
local function healthy(name)
  if has_checkheap then
    local bad = collectgarbage("checkheap")
    check(name, bad == 0, "checkheap="..tostring(bad))
  else
    pass = pass + 1
  end
end

local function count_pairs(t)
  local n = 0
  for _ in pairs(t) do n = n + 1 end
  return n
end

-- Weak-key only: keep exactly the first quarter of keys alive.
do
  local wk = setmetatable({}, { __mode = "k" })
  local keep = {}
  for i = 1, 400 do
    local k = { k = i }
    wk[k] = i
    if i <= 100 then keep[i] = k end
  end
  for i = 1, 20 do collectgarbage("step", 3) end
  collectgarbage("collect"); collectgarbage("collect")
  check("weakkey_survivors", count_pairs(wk) == 100, "n="..count_pairs(wk))
  healthy("weakkey_checkheap")
end

-- Weak-value only: keep exactly the first quarter of values alive.
do
  local wv = setmetatable({}, { __mode = "v" })
  local keep = {}
  for i = 1, 400 do
    local v = { v = i }
    wv[i] = v
    if i <= 100 then keep[i] = v end
  end
  for i = 1, 20 do collectgarbage("step", 3) end
  collectgarbage("collect"); collectgarbage("collect")
  check("weakval_survivors", count_pairs(wv) == 100, "n="..count_pairs(wv))
  healthy("weakval_checkheap")
end

-- All-weak (kv): no key/value has an outside strong reference.
do
  local wa = setmetatable({}, { __mode = "kv" })
  for i = 1, 400 do
    wa[{ k = i }] = { v = i }
  end
  for i = 1, 20 do collectgarbage("step", 3) end
  collectgarbage("collect"); collectgarbage("collect")
  check("weakall_cleared", count_pairs(wa) == 0, "n="..count_pairs(wa))
  healthy("weakall_checkheap")
end

-- Mixed modes in one cycle: all three stacks should be populated, redirected,
-- re-populated by the second atomic traversal, then cleared correctly.
do
  local wk = setmetatable({}, { __mode = "k" })
  local wv = setmetatable({}, { __mode = "v" })
  local wa = setmetatable({}, { __mode = "kv" })
  local keepk, keepv, keepboth = {}, {}, {}
  for i = 1, 300 do
    local k = { k = i }
    local v = { v = i }
    wk[k] = i
    wv[i] = v
    wa[k] = v
    if i <= 60 then keepk[i] = k end
    if i <= 70 then keepv[i] = v end
    if i <= 40 then keepboth[i] = { k, v } end
    if i % 17 == 0 then collectgarbage("step", 2) end
  end
  collectgarbage("collect"); collectgarbage("collect")
  check("mixed_weakkey", count_pairs(wk) == 60, "n="..count_pairs(wk))
  check("mixed_weakval", count_pairs(wv) == 70, "n="..count_pairs(wv))
  check("mixed_weakall", count_pairs(wa) == 60, "n="..count_pairs(wa))
  healthy("mixed_checkheap")
end

-- Mode mutation before full collection: the second atomic traversal must use
-- the latest __mode and place the table on the correct weak stack.
do
  local mt = { __mode = "v" }
  local t = setmetatable({}, mt)
  local keys = {}
  for i = 1, 120 do
    local k = { k = i }
    local v = { v = i }
    t[k] = v
    keys[i] = k
  end
  mt.__mode = "k"
  collectgarbage("collect"); collectgarbage("collect")
  check("mode_mutation_to_weakkey", count_pairs(t) == 120, "n="..count_pairs(t))
  healthy("mode_mutation_checkheap")
end

io.write(string.format("\nWeak stack tests: %d passed, %d failed\n", pass, fail))
if fail > 0 then os.exit(1) end

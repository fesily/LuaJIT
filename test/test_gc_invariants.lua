-- Phase V: targeted invariant tests for areas the adversarial review flagged.
-- Run with: luajit [-joff] test/test_gc_invariants.lua
local ffi = require("ffi")
ffi.cdef[[ typedef struct { int v; } Inv; ]]

local pass, fail = 0, 0
local function check(name, cond, msg)
  if cond then pass = pass + 1
  else fail = fail + 1; io.write("FAIL: "..name..(msg and (": "..msg) or "").."\n") end
end

-- 1. Weak-key table: keys not reachable must be cleared after fullgc.
--    Tests gc_mayclear under bitmap mark (iswhite must read correctly).
do
  local wk = setmetatable({}, { __mode = "k" })
  local strong = {}
  for i = 1, 500 do
    local k = { id = i }
    wk[k] = i
    if i <= 50 then strong[i] = k end
  end
  collectgarbage("collect"); collectgarbage("collect")
  local n = 0; for _ in pairs(wk) do n = n + 1 end
  check("weak_key_bitmap", n == 50, "n="..n)
end

-- 2. Weak-value table: values not reachable must be cleared.
do
  local wv = setmetatable({}, { __mode = "v" })
  local strong = {}
  for i = 1, 500 do
    local v = { id = i }
    wv[i] = v
    if i <= 30 then strong[i] = v end
  end
  collectgarbage("collect"); collectgarbage("collect")
  local n = 0; for _ in pairs(wv) do n = n + 1 end
  check("weak_val_bitmap", n == 30, "n="..n)
end

-- 3. Incremental sweep + alloc below cursor: allocate objects, start
--    incremental GC, free some (creates bin entries), allocate new ones
--    (from bins = below sweep cursor), complete GC. No crash/double-free.
do
  collectgarbage("stop")
  local pool = {}
  for i = 1, 2000 do pool[i] = { id = i } end
  -- Start mark+sweep
  for s = 1, 300 do collectgarbage("step", 5) end
  -- Now in sweep. Free half (goes to bins).
  for i = 1, 2000, 2 do pool[i] = nil end
  collectgarbage("step", 5)
  -- Allocate new objects (may reuse bin cells below cursor).
  for i = 1, 2000, 2 do pool[i] = { new = true, id = i } end
  for s = 1, 300 do collectgarbage("step", 5) end
  collectgarbage("restart"); collectgarbage("collect")
  local ok = true
  for i = 1, 2000 do
    if not pool[i] then ok = false break end
  end
  check("incr_sweep_realloc", ok)
end

-- 4. Forward barrier during sweep: non-table object (closure) gets a new
--    value written. The barrier's else-branch calls makewhite(o) during
--    sweep (curwhite=0 → gray-only). Verify the object survives.
do
  collectgarbage("stop")
  local holder = (function()
    local val = { initial = true }
    return {
      get = function() return val end,
      set = function(v) val = v end,
    }
  end)()
  -- Drive into sweep
  for s = 1, 400 do collectgarbage("step", 5) end
  -- Write a new white value via upvalue (forward barrier on closed upval)
  holder.set({ replaced = true })
  collectgarbage("restart"); collectgarbage("collect")
  check("fwd_barrier_sweep", holder.get().replaced == true)
end

-- 5. cdata in weak table: live cdata must NOT be cleared.
do
  local wv = setmetatable({}, { __mode = "v" })
  local keep = {}
  for i = 1, 100 do
    local cd = ffi.new("Inv", { v = i })
    wv[i] = cd
    if i <= 40 then keep[i] = cd end
  end
  collectgarbage("collect"); collectgarbage("collect")
  local alive = 0
  for i = 1, 100 do if wv[i] then alive = alive + 1 end end
  check("cdata_weak_val", alive == 40, "alive="..alive)
end

-- 6. Multiple fullgc cycles: memory accounting stays stable.
do
  for c = 1, 50 do
    local t = {}
    for i = 1, 1000 do t[i] = { tostring(i) } end
    collectgarbage("collect")
  end
  local mem = collectgarbage("count")
  check("mem_accounting_stable", mem < 2048, "mem="..math.floor(mem).."KB")
end

io.write(string.format("\nInvariant tests: %d passed, %d failed\n", pass, fail))
if fail > 0 then os.exit(1) end

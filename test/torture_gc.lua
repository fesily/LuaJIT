-- Combined GC torture: interleaves every stressor over many cycles to shake
-- out rare mark/sweep/barrier interleavings. Each fullgc self-verifies the
-- arena bitmap against a naive reference walk (asserts in a LUA_USE_ASSERT
-- build). Run: luajit [-joff] test/torture_gc.lua
local ffi = require("ffi")
ffi.cdef[[ typedef struct { int x, y, z; } TPt; ]]

local errors = 0
local function check(name, cond)
  if not cond then errors = errors + 1; io.write("FAIL: "..name.."\n") end
end

local CYCLES = tonumber(arg and arg[1]) or 300
local strong = {}          -- long-lived strong roots that must survive
local fin_seen = 0

for c = 1, CYCLES do
  collectgarbage("stop")

  -- 1. Build a mixed graph: tables, strings, cdata, closures, weak tables.
  local wk = setmetatable({}, { __mode = "k" })
  local wv = setmetatable({}, { __mode = "v" })
  local graph = {}
  for i = 1, 300 do
    local node = { id = c * 1000 + i, tag = "node_"..c.."_"..i,
                   pt = ffi.new("TPt", { x = i, y = -i, z = c }) }
    node.fn = function() return node.id, node.tag end
    graph[i] = node
    wk[node] = true
    wv[i] = node
  end

  -- 2. Drive an incremental cycle while mutating (forces backward barriers
  --    on black tables + allocation during sweep).
  for round = 1, 40 do
    for i = 1, 300 do
      local j = ((i + round) % 300) + 1
      graph[i].link = graph[j]            -- write barrier into possibly-black tab
      graph[i].extra = { round = round }  -- fresh white child
    end
    collectgarbage("step", 3)
  end

  -- 3. A handful of finalizable proxies whose payload must stay intact.
  for i = 1, 20 do
    local u = newproxy(true)
    local payload = "fin_payload_"..c.."_"..i
    getmetatable(u).__gc = function() fin_seen = fin_seen + 1
      check("fin_payload_intact", payload:sub(1,4) == "fin_") end
  end

  collectgarbage("restart")
  collectgarbage("collect")   -- self-verifies the bitmap

  -- 4. Validate survivors of this cycle's strong graph.
  local ok = true
  for i = 1, 300 do
    local n = graph[i]
    if not n or n.id ~= c * 1000 + i then ok = false break end
    local id, tag = n.fn()
    if id ~= n.id or tag ~= n.tag then ok = false break end
    if n.pt.x ~= i or n.pt.z ~= c then ok = false break end
  end
  check("graph_survivors_cycle_"..c, ok)

  -- 5. Keep ~5% of cycles' first node alive long-term to grow the old set.
  if c % 20 == 0 then strong[#strong + 1] = graph[1] end
end

-- Final: long-lived strong roots must all be intact after everything.
collectgarbage("collect")
collectgarbage("collect")
local intact = 0
for _, n in ipairs(strong) do
  if n and n.fn then local id = n.fn(); if id then intact = intact + 1 end end
end
check("longlived_strong_intact", intact == #strong)

io.write(string.format("torture: %d cycles, %d finalizers, %d strong roots, %d errors\n",
  CYCLES, fin_seen, #strong, errors))
if errors > 0 then os.exit(1) end
io.write("OK\n")

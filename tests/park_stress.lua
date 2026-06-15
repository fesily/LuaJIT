-- Park-bracket stress: maximize lj_tab_resize frequency while the concurrent
-- marker is running (cmark set). Large live heap keeps cmark windows open;
-- the hot loop creates many small tables that grow (array + hash resizes),
-- which is exactly the path the user worries about (lj_tab_resize park).
-- usage: luajit park_stress.lua <conc|inc> <gb> <rounds>
local mode, gb, rounds = ...
gb = tonumber(gb) or 1
rounds = tonumber(rounds) or 200
local conc = (mode == "conc")
if conc then assert(collectgarbage("concurrent") ~= -1, "concgc enable failed") end

local KB_PER_GB = 1024 * 1024
local target_kb = gb * KB_PER_GB

-- Big retained graph to keep GC cycles (and cmark windows) long.
local live = {}
local n = 0
while collectgarbage("count") < target_kb * 0.9 do
  n = n + 1
  live[n] = { id = n, a = live[n-1], data = { n, n+1, n+2 } }
end
collectgarbage("collect")
io.write(string.format("[%s %gGB] built %d nodes, live=%.2f GB\n",
  mode, gb, n, collectgarbage("count")/KB_PER_GB))

-- Hot kernel (JIT-compiled): build small tables that GROW -> lj_tab_resize.
-- Each call grows array part 0->1->2->...->K (multiple resizes) and inserts
-- hash keys (rehash -> resize). Returns a checksum to prevent dead-code elim.
local function churn(K)
  local sum = 0
  for r = 1, K do
    local t = {}                 -- small table
    for i = 1, 24 do t[i] = i end   -- array-part growth (several resizes)
    for i = 1, 24 do t["k"..i] = i end  -- hash-part growth (rehash/resize)
    sum = sum + t[1] + t.k1
  end
  return sum
end

local t0 = os.clock()
local acc = 0
for r = 1, rounds do
  acc = acc + churn(20000)        -- 20000 growing tables per round
  -- keep allocating into the live graph so GC keeps cycling
  for i = 1, 2000 do
    n = n + 1
    live[(n % #live) + 1] = { id = n, data = { n } }
  end
end
io.write(string.format("  churn wall=%.3fs  (acc=%d)\n", os.clock()-t0, acc % 1000))
collectgarbage("collect")

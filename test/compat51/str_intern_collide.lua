-- Interning strings whose Lua 5.1 hashes collide must stay cheap: the intern
-- table switches long chains to the keyed secondary hash
-- (LUAJIT_SECURITY_STRHASH) while sid keeps the 5.1 hash (str_order51.lua).
-- Also checks that interning still returns the same object for colliding
-- strings recreated while the incremental GC sweeps the string table.
local base = string.rep("abcdefgh", 16)  -- 128 chars: the 5.1 hash samples
                                         -- every 5th char, from index 127 down.
local A = string.byte("A")

-- Same 5.1 hash for every i: only indexes 0, 1, 3, 4 vary (unsampled).
local function collide(i)
  return string.char(A + i % 26, A + math.floor(i / 26) % 26) .. "x" ..
         string.char(A + math.floor(i / 676) % 26, A + math.floor(i / 17576) % 26) ..
         base:sub(6)
end
-- Distinct 5.1 hashes: indexes 2, 7, 12, 17 vary (all sampled).
local function spread(i)
  local s = base:sub(1, 2) .. string.char(A + i % 26) .. base:sub(4, 7) ..
            string.char(A + math.floor(i / 26) % 26) .. base:sub(9, 12) ..
            string.char(A + math.floor(i / 676) % 26) .. base:sub(14, 17) ..
            string.char(A + math.floor(i / 17576) % 26) .. base:sub(19)
  return s
end
assert(#collide(1) == 128 and #spread(1) == 128)

local N, ROUNDS = 4000, 10
-- Seconds to intern N new strings, ROUNDS times (strings freed in between).
local function intern_time(make)
  local total = 0
  for r = 1, ROUNDS do
    collectgarbage()
    collectgarbage("stop")
    local keep = {}
    local t0 = os.clock()
    for i = 1, N do keep[i] = make(i) end
    total = total + (os.clock() - t0)
    keep = nil
    collectgarbage("restart")
  end
  collectgarbage()
  return total
end

local spread_t = math.huge
for _ = 1, 3 do spread_t = math.min(spread_t, intern_time(spread)) end
local collide_t = intern_time(collide)
-- Without the fallback every colliding insert walks and memcmp()s the whole
-- chain: ~100x slower at this size. With it the two are within noise.
assert(collide_t < 10 * spread_t + 0.02, string.format(
  "colliding strings intern %.3fs vs %.3fs for spread ones", collide_t, spread_t))

-- Identity under the incremental GC: recreate after dropping, with the
-- collector stepping through the string sweep.
collectgarbage("setpause", 100)
collectgarbage("setstepmul", 400)
for round = 1, 30 do
  local anchor = {}
  for i = 1, 1000 do
    anchor[collide(i + round)] = i
    if i % 50 == 0 then collectgarbage("step", 1) end
  end
  for i = 1, 1000 do
    assert(anchor[collide(i + round)] == i, "interned string lost")
    if i % 100 == 0 then collectgarbage("step", 1) end
  end
  anchor = nil
  for _ = 1, 20 do collectgarbage("step", 1) end
end
collectgarbage("setpause", 200)
collectgarbage("setstepmul", 200)
collectgarbage()

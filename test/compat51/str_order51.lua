-- pairs() order of string-keyed tables must match Lua 5.1 (LJ_DS_STR_HASH_PATCH:
-- sid is the 5.1 string hash). Expected output comes from a stock Lua 5.1:
--   lua5.1 str_order51.lua --dump > str_order51.expected
-- Includes keys whose intern chain switched to the secondary hash, so the
-- string ID of a string born in such a chain is checked too.
local M = 4294967291

local function checksum(ks)
  local c = 0
  for i = 1, #ks do
    local k = ks[i]
    for j = 1, #k do c = (c * 33 + k:byte(j)) % M end
    c = (c * 131 + #k) % M
  end
  return string.format("%.0f", c)
end

-- Short keys sharing the low 16 bits of their Lua 5.1 hash: one intern chain
-- with more than LJ_STR_MAXCOLL entries while the string table is <= 64K.
local SHORT51 = {
  "AmMa", "^pMa", "ANNa", "^ONa", "DJBd", "eKBd", "sXBd", "ejCd", "DkCd", "syCd",
  "CsXe", "qvXe", "CTYe", "qUYe", "ejbe", "Dkbe", "sybe", "DJce", "eKce", "sXce",
  "kJCf", "JKCf", "JlDf", "kmDf", "XQGf", "uTGf", "uqHf", "XtHf", "kJbg", "JKbg",
  "Jleg", "kmeg", "XQfg", "uTfg", "uqig", "Xtig", "LRCh", "iSCh", "LqDh", "itDh",
  "Lqci", "itci", "LRdi", "iSdi", "apAj", "oqAj", "HAqj", "iBqj",
}
local SHORT51_SUFFIX = "_prefab_tag"

local function collect()
  local out = {}
  local function dump(name, t)
    local ks = {}
    for k in pairs(t) do ks[#ks + 1] = tostring(k) end
    local head = {}
    for i = 1, math.min(#ks, 6) do head[i] = ks[i]:sub(1, 16) end
    out[#out + 1] = name .. " n=" .. #ks .. " sum=" .. checksum(ks) ..
                    " head=" .. table.concat(head, ",")
  end

  -- Deterministic LCG (math.random differs between VMs).
  local seed = 12345
  local function rnd(n)
    seed = (seed * 1103515245 + 12345) % 2147483648
    return seed % n
  end
  local function word(len)
    local c = {}
    for i = 1, len do c[i] = string.char(97 + rnd(26)) end
    return table.concat(c)
  end

  -- DST shape: constructor vs. field-by-field assignment.
  dump("rpc_ctor", {
    LeftClick = 1, RightClick = 2, ActionButton = 3, AttackButton = 4,
    InspectButton = 5, ResurrectButton = 6, ControllerActionButton = 7,
    UseItemFromInvTile = 8, EquipActionItem = 9, DropItemFromInvTile = 10,
  })
  do
    local t = {}
    t.LeftClick = 1; t.RightClick = 2; t.ActionButton = 3; t.AttackButton = 4
    t.InspectButton = 5; t.ResurrectButton = 6; t.ControllerActionButton = 7
    t.UseItemFromInvTile = 8; t.EquipActionItem = 9; t.DropItemFromInvTile = 10
    dump("rpc_assign", t)
  end
  dump("mixed_ctor", { 10, 20, 30, alpha = 1, beta = 2, gamma = 3, delta = 4,
    epsilon = 5, zeta = 6, eta = 7, theta = 8, iota = 9 })

  -- Random keys, lengths 1..80 (covers the sampled 5.1 hash above 32 chars).
  for _, n in ipairs({ 3, 7, 17, 64, 100, 513, 2000 }) do
    local t = {}
    for i = 1, n do t[word(1 + rnd(80))] = i end
    dump("rand" .. n, t)
  end

  -- Insert, delete, re-insert (free-slot reuse without rehash).
  do
    local t, keys = {}, {}
    for i = 1, 300 do keys[i] = word(3 + rnd(20)); t[keys[i]] = i end
    for i = 1, 300, 3 do t[keys[i]] = nil end
    for i = 1, 50 do t[word(3 + rnd(20))] = i end
    dump("churn", t)
  end

  -- Strings made at runtime by different paths.
  do
    local t = {}
    for i = 1, 200 do
      t["prefab_" .. i] = i
      t[string.format("%s_%03d", "inst", i)] = i
      t[("componentname"):sub(1, 1 + i % 13) .. i] = i
      t[tostring(i * 7) .. "x"] = i
    end
    dump("runtime", t)
  end

  -- Grow through several rehashes, then a next() walk.
  do
    local t = {}
    for i = 1, 1500 do t["k" .. i .. "_" .. word(2)] = i end
    local seq, k = {}, next(t)
    for i = 1, 20 do seq[i] = k; k = next(t, k) end
    out[#out + 1] = "next20 sum=" .. checksum(seq)
    dump("grow1500", t)
  end

  -- Long keys differing only where the 5.1 hash does not sample: large 5.1
  -- collision groups, so their intern chains go secondary.
  do
    local base = string.rep("abcdefgh", 16)
    local keep, t = {}, {}
    for i = 1, 3000 do
      keep[i] = string.char(65 + i % 26, 65 + math.floor(i / 26) % 26,
                            65 + math.floor(i / 676) % 26) .. base
    end
    for i = 1, 3000, 7 do t[keep[i]] = i end
    for i = 1, 4000 do t["w" .. i] = i end
    dump("collide51_long", t)
  end

  -- Short keys in one 5.1 intern chain; then rebuilt from parts after the
  -- chain went secondary, mixed with fresh strings.
  do
    local t, u = {}, {}
    for i = 1, #SHORT51 do t[SHORT51[i] .. SHORT51_SUFFIX] = i end
    dump("collide51_short", t)
    for i = #SHORT51, 1, -2 do
      u[SHORT51[i] .. SHORT51_SUFFIX] = i
      u["x" .. i] = i
    end
    dump("collide51_short_mixed", u)
  end
  return out
end

if arg and arg[1] == "--dump" then
  io.write(table.concat(collect(), "\n"), "\n")
  return
end

local dir = (arg and arg[0] or ""):match("^(.*)[/\\]") or "."
local expected = {}
for l in io.lines(dir .. "/str_order51.expected") do
  expected[#expected + 1] = (l:gsub("\r$", ""))
end

local function check(label)
  local got = collect()
  assert(#got == #expected,
         label .. ": " .. #got .. " cases, expected " .. #expected)
  for i = 1, #got do
    if got[i] ~= expected[i] then
      error(label .. ": pairs() order differs from Lua 5.1\n  got:      " ..
            got[i] .. "\n  expected: " .. expected[i], 0)
    end
  end
end

check("jit on")
if jit then
  jit.off()
  check("jit off")  -- Second pass also re-hits the already interned keys.
  jit.on()
end

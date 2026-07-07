-- P4 adversarial-collision / probe-cap reseed test (LUAJIT_STRTAB_OPENADDR).
-- Forces the probe-cap reseed path by lowering the cap via the debug hook,
-- then verifies: (1) the reseed counter advanced, (2) every interned string
-- remains look-up-able with correct identity and value (correctness holds
-- across reseed+rehash), (3) lookups stay bounded (no pathological miss cost).
--
-- Run with: luajit -joff test/test_str_oa_reseed.lua
-- Requires: LUAJIT_STRTAB_OPENADDR (exposes lj_str_oa_* via ffi.C).

local ffi = require("ffi")
ffi.cdef[[
  uint32_t lj_str_oa_reseed_count(void);
  void lj_str_oa_set_probecap(uint32_t cap);
]]

local ok = pcall(function() return ffi.C.lj_str_oa_reseed_count() end)
if not ok then
  io.write("str_oa_reseed: SKIPPED (LUAJIT_STRTAB_OPENADDR not enabled)\n")
  os.exit(0)
end
local C = ffi.C

local pass, fail = 0, 0
local function check(name, cond, msg)
  if cond then pass = pass + 1
  else fail = fail + 1; io.write("FAIL: " .. name .. (msg and (": " .. msg) or "") .. "\n") end
end

-- 1. Reseed fires under a lowered probe cap.
collectgarbage("stop")
C.lj_str_oa_set_probecap(3)  -- force reseed on any insert past probe dist 3
local before = C.lj_str_oa_reseed_count()
local N = 4000
local strs = {}
for i = 1, N do
  strs[i] = "reseed_" .. tostring(i) .. "_" .. string.rep("x", i % 17)
end
local after = C.lj_str_oa_reseed_count()
C.lj_str_oa_set_probecap(0)  -- restore default
check("reseed_fired", after > before,
      "reseed_count " .. before .. " -> " .. after .. " (cap=3, N=" .. N .. ")")

-- 2. Every interned string still looks up correctly (identity + value).
local mism = 0
for i = 1, N do
  local s = strs[i]
  local again = "reseed_" .. tostring(i) .. "_" .. string.rep("x", i % 17)
  if s ~= again then mism = mism + 1 end  -- intern identity: same content => same ptr
end
check("identity_after_reseed", mism == 0, mism .. " identity mismatches")

-- 3. Misses stay bounded: a non-interned string lookup must not hang or OOM.
local t_miss = os.clock()
for i = 1, N do
  local _ = "missing_" .. tostring(i) .. "_" .. string.rep("z", i % 13)
end
local miss_dt = os.clock() - t_miss
check("misses_bounded", miss_dt < 2.0,
      "miss loop " .. N .. " took " .. string.format("%.3f", miss_dt) .. "s")

-- 4. Lookups remain correct after restoring the default cap and a full collect.
C.lj_str_oa_set_probecap(0)
collectgarbage("collect")
local mism2 = 0
for i = 1, N do
  local again = "reseed_" .. tostring(i) .. "_" .. string.rep("x", i % 17)
  if strs[i] ~= again then mism2 = mism2 + 1 end
end
check("identity_after_collect", mism2 == 0, mism2 .. " identity mismatches post-collect")

-- 5. Default cap (128): a normal workload must NOT trigger any reseed.
local def_before = C.lj_str_oa_reseed_count()
for i = 1, 20000 do
  local _ = "normal_" .. tostring(i) .. "_" .. string.rep("a", i % 23)
end
local def_after = C.lj_str_oa_reseed_count()
check("no_reseed_under_normal_load", def_after == def_before,
      "reseed_count " .. def_before .. " -> " .. def_after .. " (default cap, 20K strings)")

collectgarbage("restart")
io.write(string.format("str_oa_reseed: %d passed, %d failed (reseed fired %d times under cap=3)\n",
                       pass, fail, after - before))
if fail > 0 then os.exit(1) end

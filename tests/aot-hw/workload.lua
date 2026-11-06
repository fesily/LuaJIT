----------------------------------------------------------------------------
-- One untimed workload run, for xctrace and lldb (macOS tools).
--
--   LUA_PATH='./src/?.lua;;' ./src/luajit -joff tests/aot-hw/workload.lua \
--       fp1 interp [multiplier]
--
-- Cases and iteration counts come from cases.lua (1e8 iterations in total,
-- times an optional multiplier). The multiplier exists because a 1 kHz
-- counter sample needs a workload that runs for at least about a second.
-- jit.aot is only required for the "aot" engine, so that an interpreter
-- profile starts at the loop under test instead of at module-load loops.
----------------------------------------------------------------------------

local case, engine, mult = arg[1], arg[2], tonumber(arg[3] or 1)
local CASES = dofile("tests/aot-hw/cases.lua")
local c = assert(CASES[case], "unknown case "..tostring(case))
local body, n, reps = c[1], c[2], c[3]
local total = n * reps * (mult or 1)

local f
if engine == "aot" then
  local aot = require("jit.aot")
  local dir = os.getenv("AOT_HW_DIR") or "/tmp/aot-hw"
  os.execute("mkdir -p '"..dir.."'")
  local mod = "bench_"..case
  local cf, sf = dir.."/"..mod..".c", dir.."/"..mod..".so"
  local fh = assert(io.open(cf, "wb"))
  fh:write(aot.compile(assert(load("return function(n) "..body.." end"))(), mod))
  fh:close()
  local cmd = string.format(
    "%s -O2 -std=c99 -fPIC -ffp-contract=off -Isrc -bundle -undefined dynamic_lookup %s -o %s",
    os.getenv("CC") or "cc", cf, sf)
  if os.execute(cmd) ~= 0 and os.execute(cmd) ~= true then
    cmd = string.format(
      "%s -O2 -std=c99 -fPIC -ffp-contract=off -Isrc -shared %s -o %s",
      os.getenv("CC") or "cc", cf, sf)
    assert(os.execute(cmd) == 0 or os.execute(cmd) == true, "C compile failed")
  end
  f = assert(package.loadlib(sf, "luaopen_"..mod))()
else
  f = assert(load("return function(n) "..body.." end"))()
  if engine == "interp" then jit.off(f, true) end
  if engine ~= "interp" then jit.on() end
end

io.write(string.format("%s/%s %.17g\n", case, engine, f(total)))

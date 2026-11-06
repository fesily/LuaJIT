----------------------------------------------------------------------------
-- Benchmarks for the jit.aot numeric subset (not a pass/fail test).
--
-- Run from the repository root:
--
--   LUA_PATH='./src/?.lua;;' ./src/luajit tests/aot-hw/bench.lua
--
-- Portable: needs the LuaJIT build in ./src and a C compiler ($CC or cc).
--
-- Cases are chosen so that the integer ones stay inside int32 for every
-- iteration (the interpreter and the AOT both switch to a double result on
-- int32 overflow, which would silently turn them into float benchmarks):
--   int1  s = s + 1      s <= 1e6       pure integer loop
--   int2  s = s + i      s <= 8.0e8     pure integer loop
--   fp1   x = x * c + 0.5                pure double chain
--
-- For the profiling tools in this directory use workload.lua instead (one
-- untimed run of a single case and engine).
----------------------------------------------------------------------------

local jit = require("jit")
local aot = require("jit.aot")

local CC = os.getenv("CC") or "cc"
local DIR = os.getenv("AOT_HW_DIR") or "/tmp/aot-hw"

local CASES = dofile("tests/aot-hw/cases.lua")

local function sh(cmd)
  local a = os.execute(cmd)
  return a == 0 or a == true
end

local function shellquote(s) return "'"..s:gsub("'", "'\\''").."'" end

local function mkcfunc(body)
  return assert(load("return function(n) "..body.." end"))()
end

-- Build (or reuse) the AOT module for one case.
local function aotfunc(name)
  local body = CASES[name][1]
  sh("mkdir -p "..shellquote(DIR))
  local mod = "bench_"..name
  local cf, sf = DIR.."/"..mod..".c", DIR.."/"..mod..".so"
  local fh = assert(io.open(cf, "wb"))
  fh:write(aot.compile(mkcfunc(body), mod))
  fh:close()
  local cmd = string.format(
    "%s -O2 -std=c99 -fPIC -ffp-contract=off -Isrc -bundle -undefined dynamic_lookup %s -o %s",
    CC, shellquote(cf), shellquote(sf))
  if not sh(cmd) then
    -- Non-macOS link line.
    cmd = string.format("%s -O2 -std=c99 -fPIC -ffp-contract=off -Isrc -shared %s -o %s",
      CC, shellquote(cf), shellquote(sf))
    if not sh(cmd) then
      io.stderr:write("C compile failed for ", mod, "\n")
      os.exit(1)
    end
  end
  return assert(package.loadlib(sf, "luaopen_"..mod))()
end

local function best(fn, n, reps)
  local m = 1e9
  for _ = 1, 5 do
    local t0 = os.clock()
    local r
    for _ = 1, reps do r = fn(n) end
    local t = os.clock() - t0
    if t < m then m = t end
  end
  return m
end

print(string.format("%-5s %9s %9s %9s %10s  %s",
  "case", "interp", "jit", "aot", "aot/interp", "value"))
local names = {}
for name in pairs(CASES) do names[#names+1] = name end
table.sort(names)
for _, name in ipairs(names) do
  local body, n, reps = CASES[name][1], CASES[name][2], CASES[name][3]
  jit.on()
  local interp = mkcfunc(body); jit.off(interp, true)
  local jitf = mkcfunc(body)
  local aotf = aotfunc(name)
  local ri, rj, ra = interp(n), jitf(n), aotf(n)
  assert(ri == rj and ri == ra, "result mismatch in "..name)
  if name == "int2" and ri ~= 800020000 then
    error("int2 left the int32 range: "..tostring(ri))
  end
  if name == "modi" and not (ri >= 0 and ri <= 6) then
    error("modi left its expected range: "..tostring(ri))
  end
  local ns = 1e9 / (n * reps)
  local ti, tj, ta = best(interp, n, reps)*ns, best(jitf, n, reps)*ns, best(aotf, n, reps)*ns
  print(string.format("%-5s %9.2f %9.2f %9.2f %10.2f  %s",
    name, ti, tj, ta, ta/ti, tostring(ra)))
end
print(string.format("ns per iteration, %d iterations per case, min of 5", 1e8))

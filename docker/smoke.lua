-- Runtime smoke test for one CPU architecture.
--
-- Purpose: prove that the just-built interpreter runs, that the generated
-- machine code (DynASM -> vm_<arch>.dasm) is executable on this CPU, and that
-- value representations (int/num, endianness, pointer size) behave sanely.
-- Plain Lua 5.1 + LuaJIT features only; no external dependencies.

local jit = require "jit"
print(string.format("smoke: %s/%s %s", jit.arch, jit.os, jit.version))

-- Integer arithmetic loop: must not be folded at compile time.
local sum = 0
for i = 1, 2000000 do sum = sum + i end
assert(sum == 2000001000000, "int loop: " .. tostring(sum))

-- FP path (exercises the FPU/softfp ABI of this target).
local f = 0
for i = 1, 100000 do f = f + 0.5 end
assert(f == 50000, "fp loop: " .. tostring(f))

-- Strings, tables, GC.
local t = {}
for i = 1, 1000 do t[i] = tostring(i) .. "x" end
assert(#t == 1000 and t[1000] == "1000x", "table/string path")
collectgarbage "collect"
collectgarbage "collect"

-- Coroutines.
local co = coroutine.wrap(function(a)
  local b = coroutine.yield(a + 1)
  return b * 2
end)
assert(co(1) == 2, "coroutine yield")
assert(co(21) == 42, "coroutine resume")

-- The JIT compiler must be able to emit and run machine code for this CPU.
if jit.status() then
  local traces = 0
  local sink = 0
  jit.attach(function(what) if what == "start" then traces = traces + 1 end end, "trace")
  for i = 1, 1000 do sink = sink + i end
  jit.attach(function() end, "trace") -- detach
  assert(sink == 500500, "hot loop result")
  assert(traces > 0, "no trace recorded: mcode path is broken")
  print(string.format("smoke: jit traces=%d", traces))
end

-- FFI: pointer size, ABI flags and byte order must agree with reality.
local hasffi, ffi = pcall(require, "ffi")
if hasffi then
  local psz = ffi.sizeof("void *")
  assert(psz == 4 or psz == 8, "odd pointer size: " .. tostring(psz))
  assert(ffi.abi("64bit") == (psz == 8), "ffi.abi('64bit') disagrees with sizeof(void*)")
  local u = ffi.new("union { uint32_t i; uint8_t b[4]; }")
  u.i = 0x01020304
  assert((u.b[0] == 0x04) == ffi.abi("le"), "byte order mismatch")
  local a = ffi.new("int64_t[4]", { 1, 2, 3, 4 })
  local s = ffi.new("int64_t", 0)
  for i = 0, 3 do s = s + a[i] end
  assert(s == 10, "ffi int64 sum: " .. tostring(s))
  print(string.format("smoke: ffi ptr=%d le=%s", psz, tostring(ffi.abi("le"))))
else
  print "smoke: ffi disabled in this build"
end

print "smoke: ok"

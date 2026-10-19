-- JIT-enabled cases check values, side effects and memory safety only.
-- Lua 5.1 debug/tail-frame parity is covered by the interpreted compat suite.
local probe = require("tailcallprobe")
local mode = assert(arg[1])
local count = arg[2] ~= "nocount"
local util = require("jit.util")
local function fixed(n) return probe.values(n) end
local function vararg(...) return probe.values(...) end
local function arguments(n) return n, false, nil end
local function expanded(n) return probe.values(arguments(n)) end
local callable = setmetatable({}, {__call = function(_, n) return probe.values(n) end})
local function metacall(n) return callable(n) end
local function objectcall(n) return probe.objects(n) end
local function invoke(fn, n) return probe.invoke(fn, n) end
local function yieldcall(n) return probe.yield_values(n) end
local function failcall() return probe.fail() end
local function fftail(s) return string.byte(s) end
local function luatail(n) return n+1 end
local function pure(n) return luatail(n) end
local function pack(...) return {n=select("#", ...), ...} end
local counts = {0, 1, 3, 4, 259, 300, 515, 600}

local function check(t, n)
  assert(t.n == n, "wrong result count")
  for i = 1, n do
    assert(t[i] == (i%3 ~= 0 and n*1000+i or nil), "wrong result " .. i)
  end
end

local function drive(wrapper)
  local before = probe.count()
  local scratch = {}
  for i = 1, 2400 do
    local n = counts[i%#counts+1]
    local a, b, c, d = wrapper(n)
    assert(a == (n > 0 and n*1000+1 or nil))
    assert(b == (n > 1 and n*1000+2 or nil))
    assert(c == nil)
    assert(d == (n > 3 and n*1000+4 or nil))
    check(pack(wrapper(n)), n)
    wrapper(n)
    local first = wrapper(n)
    assert(first == a)
    scratch[i%32+1] = {i, a}
    if i%64 == 0 then collectgarbage("collect") end
    if i%127 == 0 then jit.flush() end
  end
  assert(probe.count()-before == 9600, "C call was replayed or skipped")
end

local function suite()
  for _, wrapper in ipairs({fixed, vararg, expanded, metacall}) do drive(wrapper) end
  local before = probe.count()
  for i = 1, 400 do
    local n = i%2 == 0 and 300 or 600
    local t = pack(objectcall(n))
    collectgarbage("collect")
    assert(t.n == n)
    for k = 1, n do assert(t[k].index == k, "lost C result object") end
    local function callback(value) return fixed(value) end
    check(pack(invoke(callback, n)), n)
    local co = coroutine.create(function() return yieldcall(n) end)
    local result = pack(coroutine.resume(co))
    assert(result[1] and result.n == n+1)
    assert(coroutine.resume(co))
    assert(coroutine.status(co) == "dead")
    local ok, message = pcall(failcall)
    assert(not ok and message:find("tail-call probe error", 1, true))
    assert(fftail("abc") == 97)
  end
  assert(probe.count()-before == 2000, "reentry/yield/error side effects changed")
end

jit.off()
jit.flush()
if mode == "off" then
  for _, fn in ipairs({fixed, vararg, expanded, arguments, metacall,
      getmetatable(callable).__call, objectcall, invoke, yieldcall, failcall,
      fftail, drive, suite, pack, check}) do jit.off(fn, true) end
end
if mode ~= "interp" then pcall(jit.on) end
if jit.opt then jit.opt.start("hotloop=3", "hotexit=2", "minstitch=0") end
suite()

if mode == "jit" then
  -- Ordinary Lua tail calls must still compile after removing compatibility IR.
  jit.flush()
  local sum = 0
  for i = 1, 10000 do sum = sum+pure(i) end
  assert(sum == 50015000 and util.traceinfo(1), "ordinary Lua tails did not compile")
  -- Reuse the same thread after compiled calls and stress interpreter counters.
  jit.off()
  local rec
  rec = function(n)
    if n > 0 then return rec(n-1) end
    local info = debug.getinfo(2, "S")
    assert(info and (info.what == "tail") == count)
    return 7
  end
  assert(rec(20) == 7)
end
jit.off()
print("ok tailcall " .. mode)

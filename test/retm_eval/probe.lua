local probe = require("retmprobe")
local HOT = 2000
local mode = arg[1] or "counts"
local util = jit and require("jit.util")
if jit then jit.opt.start("hotloop=3", "hotexit=2") end

local function expect(n, a, b, c, d)
  assert(a == (n >= 1 and n * 1000 + 1 or nil), "first result, n=" .. n)
  assert(b == (n >= 2 and n * 1000 + 2 or nil), "second result, n=" .. n)
  assert(c == nil, "third result must be nil, n=" .. n)
  assert(d == (n >= 4 and n * 1000 + 4 or nil), "fourth result, n=" .. n)
end

local function fixed(n) return probe.values(n) end
local function vararg(...) return probe.values(...) end
local function arguments(n) return n, false, nil end
local function expanded(n) return probe.values(arguments(n)) end
local function prefix(n) return "prefix", probe.values(n) end
local function pack(...) return {n = select("#", ...), ...} end

if mode == "counts" or mode == "vararg" or mode == "expanded" then
  local wrapper = mode == "vararg" and vararg or
                  mode == "expanded" and expanded or fixed
  local warm = tonumber(arg[2]) or 3
  local sum = 0
  for i = 1, HOT do
    local n = i <= HOT - 12 and warm or ({0, 1, 3, 4})[(i % 4) + 1]
    local a, b, c, d = wrapper(n)
    expect(n, a, b, c, d)
    sum = sum + (a or 0)
  end
  print("ok " .. mode .. " sum=" .. sum)
elseif mode == "prefix" then
  for i = 1, HOT do
    local n = i <= HOT - 12 and 3 or ({0, 1, 3, 4})[(i % 4) + 1]
    local tag, a, b, c, d = prefix(n)
    assert(tag == "prefix")
    expect(n, a, b, c, d)
  end
  print("ok prefix")
elseif mode == "big" then
  for i = 1, HOT do
    local n = i <= HOT - 4 and 3 or 300
    local result = pack(fixed(n))
    assert(result.n == n, "wrong result count: " .. result.n .. "/" .. n)
    assert(result[1] == n * 1000 + 1)
    assert(result[299] == (n == 300 and 300299 or nil))
    assert(result[300] == nil)
  end
  print("ok big")
elseif mode == "frames" or mode == "flush" or mode == "luatails" then
  local active = false
  local wrapper
  local function capture()
    if mode == "flush" and active and jit then jit.flush() end
    if not active then return end
    local found = false
    local tails = 0
    for level = 1, 30 do
      local info = debug.getinfo(level, "Sf")
      if not info then break end
      if info.what == "tail" then tails = tails + 1 end
      if info.func == wrapper then found = true end
    end
    assert(found, "C tail caller frame was elided")
    assert(tails == (mode == "luatails" and 2 or 0), "wrong virtual tail count")
  end
  if jit then jit.off(capture) end
  wrapper = function() return probe.callback(capture) end
  local function mid() return wrapper() end
  local function top() return mid() end
  local call = mode == "luatails" and top or wrapper
  for i = 1, HOT do
    active = i > HOT - 10
    local result = call()
    assert(result == 7)
  end
  print("ok " .. mode)
elseif mode == "error" then
  local fail = false
  local wrapper
  local function capture()
    if fail then error("callback error") end
  end
  if jit then jit.off(capture) end
  wrapper = function() return probe.callback(capture) end
  local function driver()
    for i = 1, HOT do
      fail = i == HOT
      local result = wrapper()
      assert(result == 7)
    end
  end
  local found = false
  local ok = xpcall(driver, function()
    for level = 1, 30 do
      local info = debug.getinfo(level, "Sf")
      if not info then break end
      if info.func == wrapper then found = true end
    end
  end)
  assert(not ok and found, "error handler lost the C tail caller frame")
  print("ok error")
elseif mode == "yield" then
  local function wrapper(n) return probe.yield_values(n) end
  local co = coroutine.create(function()
    for i = 1, 30 do
      local result = pack(wrapper(3))
      assert(result.n == 2 and result[1] == 17 and result[2] == nil)
    end
  end)
  for i = 1, 30 do
    local result = pack(coroutine.resume(co, 17, nil))
    assert(result.n == 4 and result[1] == true)
    assert(result[2] == 3001 and result[3] == 3002 and result[4] == nil)
  end
  local ok, err = coroutine.resume(co, 17, nil)
  assert(ok and coroutine.status(co) == "dead", tostring(err))
  print("ok yield")
else
  error("unknown mode " .. mode)
end

if util then
  jit.off()
  local stitches = 0
  for i = 1, 1000 do
    local info = util.traceinfo(i)
    if not info then break end
    if info.linktype == "stitch" then stitches = stitches + 1 end
  end
  print("stitch traces=" .. stitches)
end

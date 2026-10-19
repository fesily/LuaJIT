local probe = require("tailcallprobe")
local compat = arg[1] ~= "stock"
local hasjit = pcall(jit.on)
local escaped, owner, expected
local function observe(read)
  assert(probe.visible(owner) == expected, "wrong physical C-tail caller")
  local value = read()
  local padding = {}
  for i = 1, 2000 do padding[i] = {i} end
  collectgarbage("collect")
  assert(read() == value, "captured value changed during C callback")
  return value
end
jit.off(observe)
local function capture(value)
  local x = value
  local function read() return x end
  escaped = read
  return probe.invoke(observe, read)
end
local function capture_vararg(value, ...)
  local x = value
  local function read() return x end
  escaped = read
  return probe.invoke(observe, read, ...)
end
local function early(value)
  local x = value
  local read
  for i = 1, 2 do
    if i == 2 then return probe.invoke(observe, read) end
    read = function() return x end
    escaped = read
  end
end
local callable = setmetatable({}, {__call = probe.visible})
local function meta() return callable() end
local function lua(n) return n+1 end
local function mixed(fn, n) return fn(n) end
local function run()
  for _, off in ipairs(hasjit and {false, true, false} or {true}) do
    expected = off and compat or false
    for _, fn in ipairs({capture, capture_vararg, early}) do
      owner = fn
      if off then jit.off(fn) else jit.on(fn) end
      for i = 1, 20 do
        local value = {i}
        assert(fn(value, 1, nil, 3) == value)
        collectgarbage("collect")
        assert(escaped() == value, "native replacement closed the wrong slot")
      end
    end
    callable.target = meta
    if off then jit.off(meta) else jit.on(meta) end
    for i = 1, 1000 do assert(meta() == expected, "C __call mode selection") end
    if off then jit.off(mixed) else jit.on(mixed) end
    for i = 1, 2400 do
      local choice = i%3
      local fn = choice == 0 and lua or choice == 1 and probe.values or math.abs
      assert(mixed(fn, 1) == (choice == 0 and 2 or choice == 1 and 1001 or 1),
        "mixed Lua/C/FF tail site")
    end
    local ffmeta = setmetatable({}, {__call = type})
    assert(mixed(ffmeta, 1) == "table", "fast-function __call")
    local luameta = setmetatable({}, {__call = function(_, n) return n+1 end})
    assert(mixed(luameta, 1) == 2, "Lua __call")
  end
  jit.off()
  jit.flush()
  expected = compat
  owner = capture
  local value = {}
  assert(capture(value) == value and escaped() == value)
end
jit.off(run)
if hasjit then jit.opt.start("hotloop=3", "hotexit=2", "minstitch=0") end
run()
print("ok tailcall dispatch")

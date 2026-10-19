-- Interpreted calls clear reused counters and preserve virtual tail frames.
-- Run with JIT globally off and with the tested functions explicitly off.
-- Also runs on Lua 5.1 as a reference.
local jit = jit
local HOT = 1000
local cases = {}

local function capture(samples, tag, expected)
  local owner, tails, shape = nil, {}, {}
  local finished = false
  for level = 1, 128 do
    local info = debug.getinfo(level, "Sf")
    if not info then finished = true; break end
    if info.what == "tail" then
      assert(owner, "tail level without a real frame")
      tails[owner] = (tails[owner] or 0) + 1
      assert(info.func == nil, "a virtual tail frame must have no function")
      shape[#shape + 1] = "tail"
    else
      owner = info.func
      shape[#shape + 1] = info.what .. ":" .. tostring(info.linedefined)
    end
  end
  local detail = tag .. ": " .. table.concat(shape, ",")
  assert(finished, "unbounded virtual tail levels: " .. detail)
  for fn, count in pairs(tails) do
    assert(count == (expected[fn] or 0), "unexpected tail count " .. count .. ": " .. detail)
  end
  for fn, count in pairs(expected) do
    assert((tails[fn] or 0) == count, "missing tail count " .. count .. ": " .. detail)
  end
  samples[tag] = true
end

cases.nested = function(samples)
  local leaf
  leaf = function(i)
    if i == HOT then capture(samples, "leaf", {[leaf] = 2}) end
    return i + 1
  end
  local function mid(i) return leaf(i) end
  local function top(i) return mid(i) end
  return function()
    local sum = 0
    for i = 1, HOT do sum = sum + top(i) end
    capture(samples, "after", {})
    return sum
  end
end

cases.reuse = function(samples)
  local function leaf(i) return i + 1 end
  local function tail(i) return leaf(i) end
  local function plain(i)
    if i == HOT then capture(samples, "plain", {}) end
    return i + 1
  end
  return function()
    local sum = 0
    for i = 1, HOT do
      sum = sum + tail(i)
      sum = sum + plain(i) -- Same call slot as tail(i).
    end
    capture(samples, "after", {})
    return sum
  end
end

cases.vararg = function(samples)
  local leaf
  leaf = function(i)
    if i == HOT then capture(samples, "leaf", {[leaf] = 1}) end
    return i + 1
  end
  local function tail(...) return leaf(...) end
  return function()
    local sum = 0
    for i = 1, HOT do sum = sum + tail(i, false, nil) end
    capture(samples, "after", {})
    return sum
  end
end

cases.vararg_callee = function(samples)
  local leaf
  leaf = function(...)
    local i = ...
    if i == HOT then capture(samples, "leaf", {[leaf] = 1}) end
    return i + 1
  end
  local function tail(i) return leaf(i, false, nil) end
  return function()
    local sum = 0
    for i = 1, HOT do sum = sum + tail(i) end
    capture(samples, "after", {})
    return sum
  end
end

cases.metacall = function(samples)
  local leaf
  leaf = function(i)
    if i == HOT then capture(samples, "leaf", {[leaf] = 1}) end
    return i + 1
  end
  local callable = setmetatable({}, {__call = function(_, i) return leaf(i) end})
  return function()
    local sum = 0
    for i = 1, HOT do sum = sum + callable(i) end
    capture(samples, "after", {})
    return sum
  end
end

cases.side = function(samples)
  local even, odd
  even = function(i)
    if i == HOT then capture(samples, "even", {[even] = 1}) end
    return i + 1
  end
  odd = function(i)
    if i == HOT - 1 then capture(samples, "odd", {[odd] = 1}) end
    return i + 1
  end
  local function tail(i)
    if i % 2 == 0 then return even(i) end
    return odd(i)
  end
  return function()
    local sum = 0
    for i = 1, HOT do sum = sum + tail(i) end
    capture(samples, "after", {})
    return sum
  end
end

cases.recursion = function(samples)
  local recurse
  recurse = function(n, i)
    if n == 0 then
      if i == HOT then capture(samples, "leaf", {[recurse] = 8}) end
      return i + 1
    end
    return recurse(n - 1, i)
  end
  return function()
    local sum = 0
    for i = 1, HOT do sum = sum + recurse(8, i) end
    capture(samples, "after", {})
    return sum
  end
end

cases.guard_exit = function(samples)
  local values = {}
  for i = 1, HOT do values[i] = i end
  values[HOT] = tostring(HOT) -- A type guard exits after both tail updates.
  local leaf
  leaf = function(i)
    local result = values[i] + 1
    if i == HOT then capture(samples, "leaf", {[leaf] = 2}) end
    return result
  end
  local function mid(i) return leaf(i) end
  local function top(i) return mid(i) end
  return function()
    local sum = 0
    for i = 1, HOT do sum = sum + top(i) end
    capture(samples, "after", {})
    return sum
  end
end

cases.ff_metacall = function(samples)
  local obj = setmetatable({i = 0}, {__tostring = function(self)
    if self.i == HOT then capture(samples, "metamethod", {}) end
    return "x"
  end})
  return function()
    local sum = 0
    for i = 1, HOT do
      obj.i = i
      sum = sum + #tostring(obj)
    end
    capture(samples, "after", {})
    return sum
  end
end

local function run(name, mode)
  if jit then
    jit.off()
    jit.flush()
    if jit.opt then jit.opt.start("hotloop=3", "hotexit=2") end
  end
  local samples = {}
  if jit then jit.off(cases[name], true) end
  local driver = cases[name](samples)
  -- A fresh coroutine also isolates old counter slots between cases/modes.
  local co = coroutine.create(function()
    local result = driver()
    return result
  end)
  if jit and mode == "off" then pcall(jit.on) end
  local ok, result = coroutine.resume(co)
  if jit then jit.off() end
  assert(ok, name .. "/" .. mode .. ": " .. tostring(result))
  local want = HOT * (HOT + 3) / 2
  if name == "reuse" then want = want * 2 end
  if name == "ff_metacall" then want = HOT end
  assert(result == want, name .. "/" .. mode .. ": result mismatch")
  assert(samples.after, name .. "/" .. mode .. ": no final stack sample")
  local count = 0
  for _ in pairs(samples) do count = count + 1 end
  assert(count == (name == "side" and 3 or 2), name .. ": missing inner sample")
  print("ok " .. name .. "/" .. mode)
end
if jit then jit.off(run); jit.off(capture) end

local names = {"nested", "reuse", "vararg", "vararg_callee", "metacall",
               "side", "recursion", "guard_exit", "ff_metacall"}
for _, name in ipairs(names) do
  if not arg or not arg[1] or arg[1] == name then
    run(name, "interp")
    if jit then run(name, "off") end
  end
end
if jit then pcall(jit.on) end
print("ok tailcall_count")

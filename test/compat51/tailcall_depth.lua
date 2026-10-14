-- Direct level queries must preserve every tail frame beyond one million.
local jit = jit
local util = jit and require("jit.util")
local probe = arg and arg[1] == "native" and require("hookprobe")
local depths = {0, 1, 999999, 1000000, 1000005}

local function inspect(rec, driver, n)
  local function at(level)
    local info = debug.getinfo(level+1, "Sf")
    if probe then
      local what, fn = probe.frame(level+1)
      assert(what == (info and info.what) and fn == (info and info.func),
        "Lua and native frame queries disagree")
    end
    return info
  end
  -- at() adds one physical frame; level 1 is inspect(), level 2 is rec().
  local real = at(2)
  assert(real and real.func == rec, "missing actual leaf frame")
  if n > 0 then
    for _, offset in ipairs({1, math.min(n, 999999), math.min(n, 1000000), n}) do
      local tail = at(offset+2)
      assert(tail and tail.what == "tail" and tail.func == nil,
        "lost tail level " .. offset .. " of " .. n)
      assert(debug.getlocal(offset+2, 1) == nil, "virtual tail exposes a local")
    end
  end
  local parent = at(n+3)
  assert(parent and parent.func == driver, "physical caller shifted by tail depth")
  assert(at(n+4) == nil, "extra logical frame after coroutine root")
  assert(at(2147483646) == nil, "far level must stay absent")
  -- Output truncation must leave direct frame queries intact.
  local traceback = debug.traceback("deep-count", 1)
  local lines = select(2, traceback:gsub("\n", ""))
  assert(lines <= 26, "traceback output grew with logical tail depth")
  if n > 30 then assert(traceback:find("...", 1, true)) end
  if n > 0 then assert(at(n+2).what == "tail") end
  return true
end

local function make_rec(vararg, driverref, n)
  local rec
  if vararg then
    rec = function(depth, ...)
      if depth > 0 then return rec(depth-1, ...) end
      assert(select("#", ...) == 3 and select(3, ...) == 17)
      local result = inspect(rec, driverref[1], n)
      return result
    end
  else
    rec = function(depth)
      if depth > 0 then return rec(depth-1) end
      local result = inspect(rec, driverref[1], n)
      return result
    end
  end
  return rec
end

local function run(n, mode, vararg, mask)
  if jit then jit.off(); jit.flush() end
  local driverref = {}
  local rec = make_rec(vararg, driverref, n)
  local driver = function()
    local value = rec(n, nil, false, 17)
    return value
  end
  driverref[1] = driver
  local compiled, started = false, {}
  local function traceevent(what, tr, fn)
    if what == "start" then started[tr] = fn == rec
    elseif what == "stop" and started[tr] then compiled = true end
  end
  if jit then
    jit.off(driver)
    if jit.opt then jit.opt.start("hotloop=3", "hotexit=2") end
    if mode == "jit" then jit.attach(traceevent, "trace"); jit.on() end
  end
  local co = coroutine.create(driver)
  local ok, result, calls, returns, tails
  if mask then
    -- count() makes a protected C call. The driver still runs in its coroutine.
    local hookok, hookresult, c, r, t = probe.count(function()
      local resumed, value = coroutine.resume(co)
      assert(resumed, value)
      return value
    end, mask, co)
    ok, result, calls, returns, tails = hookok, hookresult, c, r, t
  else
    ok, result = coroutine.resume(co)
  end
  if jit then
    jit.off()
    if mode == "jit" then jit.attach(traceevent) end
  end
  assert(ok and result == true, tostring(n) .. "/" .. mode .. ": " .. tostring(result))
  if mode == "jit" and not vararg and n > 100 then
    assert(compiled and util.traceinfo(1), "target recursion did not compile")
  end
  if mask then
    assert(tails == (mask == 1 and 0 or n), "tail hook count truncated")
    assert(mask == 2 or calls >= n+1, "call hooks missing from recursion")
    assert(mask == 1 or returns > 0, "ordinary return hook missing")
    assert(debug.gethook() == nil)
  end
  print("ok depth " .. n .. "/" .. mode .. "/" .. tostring(vararg) ..
    (mask and ("/mask=" .. mask .. "/tails=" .. tails) or ""))
end

if jit then
  jit.off(inspect, true)
  jit.off(make_rec)
  jit.off(run)
end
for _, n in ipairs(depths) do
  run(n, "interp", false)
end
run(1000005, "interp", true)
if jit and pcall(jit.on) then
  for _, n in ipairs(depths) do run(n, "jit", false) end
  run(1000005, "jit", true)
end
if probe then
  for mask = 1, 3 do run(1000005, "interp", false, mask) end
  run(1000005, "interp", true, 3)
end
print("ok tailcall_depth")

-- Lua 5.1 tail-hook order, physical caller visibility and callback mutations.
local jit = jit
local modes = {}
for i = 1, #(arg or {}) do modes[arg[i]] = true end
local count = not modes.nocount
local cframe = not modes.nocframe

local function check(actual, expected, tag)
  assert(#actual == #expected, tag .. ": " .. table.concat(actual, ","))
  for i = 1, #expected do
    assert(actual[i] == expected[i], tag .. " #" .. i .. ": " ..
      tostring(actual[i]) .. " ~= " .. expected[i])
  end
end

local function grow(n)
  if n == 0 then return 1 end
  return 1 + grow(n-1)
end

local function chain_case(vararg, mask, change)
  local leaf, middle, outer, driver
  if vararg then
    leaf = function(x, ...) return x, select("#", ...), select(3, ...) end
    middle = function(...) return leaf(...) end
    outer = function(...) return middle(...) end
    driver = function()
      local a, n, last = outer(17, 2, nil, 4)
      assert(a == 17 and n == 3 and last == 4)
    end
  else
    leaf = function() return 17 end
    middle = function() return leaf() end
    outer = function() return middle() end
    driver = function()
      local value = outer()
      assert(value == 17)
    end
  end
  local names = {[leaf]="leaf", [middle]="middle", [outer]="outer", [driver]="driver"}
  local events = {}
  local function replacement(event)
    local info = debug.getinfo(2, "f")
    if info.func == leaf and event == "tail return" then
      events[#events+1] = "replacement"
    end
  end
  local function hook(event)
    local info = debug.getinfo(2, "f")
    local name = names[info.func]
    if name == "outer" or name == "middle" or name == "leaf" then
      local parent = debug.getinfo(3, "Sf")
      local parentname = parent and (names[parent.func] or parent.what) or "none"
      events[#events+1] = event .. ":" .. name .. ":" .. parentname
      if change == "grow" then
        assert(grow(180) == 181)
        collectgarbage("collect")
      elseif name == "leaf" and event == "return" then
        if change == "disable" then debug.sethook()
        elseif change == "replace" then debug.sethook(replacement, "r") end
      elseif event == "tail return" and change == "first" then
        debug.sethook()
      elseif event == "call" and name == "leaf" and change == "call-disable" then
        debug.sethook()
      end
    end
  end
  debug.sethook(hook, mask)
  driver()
  debug.sethook()
  local expected = {}
  if mask:find("c", 1, true) then
    expected = {"call:outer:driver", "call:middle:outer", "call:leaf:middle"}
  end
  if mask:find("r", 1, true) and change ~= "call-disable" then
    expected[#expected+1] = "return:leaf:" .. (count and "tail" or "driver")
    if count and change ~= "disable" then
      if change == "replace" then
        expected[#expected+1] = "replacement"
        expected[#expected+1] = "replacement"
      else
        expected[#expected+1] = "tail return:leaf:tail"
        if change ~= "first" then expected[#expected+1] = "tail return:leaf:driver" end
      end
    end
  end
  check(events, expected, tostring(vararg) .. "/" .. mask .. "/" .. tostring(change))
end

local function alias_case()
  local caller, escaped
  local function leaf(read, value) return read(), value end
  caller = function()
    local x = 1
    local function read() return x end
    escaped = read
    return leaf(read)
  end
  local calls, tails = 0, 0
  debug.sethook(function(event)
    local info = debug.getinfo(2, "f")
    if info.func == leaf then
      if event == "call" then
        calls = calls + 1
        assert(debug.getinfo(3, "f").func == caller)
        if jit then jit.off(escaped) end
        local found
        for i = 1, 20 do
          local name = debug.getlocal(3, i)
          if name == "x" then
            assert(debug.setlocal(3, i, 99) == "x")
            assert(escaped() == 99, "caller closes only after the call hook")
            found = true
          end
        end
        assert(found)
        assert(debug.getlocal(2, 2) == "value")
        assert(debug.setlocal(2, 2, 7) == "value")
        assert(grow(180) == 181)
        collectgarbage("collect")
      elseif event == "tail return" then
        tails = tails + 1
      end
    end
  end, "cr")
  local a, b = caller()
  debug.sethook()
  assert(a == 99 and b == 7 and escaped() == 99)
  assert(calls == 1 and tails == (count and 1 or 0))
end

local function ctail_case()
  local wrapper, middle, outer
  wrapper = function(value) return math.abs(value) end
  middle = function(value) return wrapper(value) end
  outer = function(value) return middle(value) end
  local calls, returns, tails = 0, 0, 0
  debug.sethook(function(event)
    local info = debug.getinfo(2, "f")
    if info.func == math.abs and event == "call" then
      calls = calls+1
      assert(debug.getinfo(3, "f").func == wrapper)
    elseif info.func == wrapper then
      if event == "return" then returns = returns+1
      elseif event == "tail return" then tails = tails+1 end
    end
  end, "cr")
  local value = outer(-17)
  debug.sethook()
  assert(value == 17 and calls == 1 and returns == 1)
  assert(tails == (count and 2 or 0), "C tail call must not add a virtual tail")
end

local function multres_case()
  local values = {}
  for i = 1, 299 do values[i] = {i} end
  local leaf = function() return unpack(values, 1, 300) end
  local middle = function() return leaf() end
  local outer = function() return middle() end
  local tails = 0
  local function pack(...) return {n=select("#", ...), ...} end
  debug.sethook(function(event)
    if debug.getinfo(2, "f").func == leaf then
      assert(grow(180) == 181)
      collectgarbage("collect")
      if event == "tail return" then tails = tails+1 end
    end
  end, "cr")
  local result = pack(outer())
  debug.sethook()
  assert(result.n == 300 and result[300] == nil)
  for i = 1, 299 do assert(result[i] == values[i]) end
  assert(tails == (count and 2 or 0))
end

local function error_case()
  local leaf, caller, escaped
  leaf = function(read) return read() end
  caller = function()
    local x = 1
    local function read() return x end
    escaped = read
    return leaf(read)
  end
  local tails = 0
  debug.sethook(function(event)
    local info = debug.getinfo(2, "f")
    if info.func == leaf and event == "call" then
      assert(debug.getinfo(3, "f").func == caller)
      for i = 1, 20 do
        if debug.getlocal(3, i) == "x" then debug.setlocal(3, i, 99) end
      end
      error("tail call hook failed")
    elseif event == "tail return" then
      tails = tails+1
    end
  end, "cr")
  local ok, message = pcall(caller)
  debug.sethook()
  assert(not ok and message:find("tail call hook failed", 1, true))
  assert(escaped() == 99 and tails == 0, "failed call hook must unwind open values")
  chain_case(false, "cr")
end

local function tail_error_case()
  if not count then return end
  local leaf = function() return 17 end
  local middle = function() return leaf() end
  local outer = function() return middle() end
  local tails = 0
  debug.sethook(function(event)
    if event == "tail return" and debug.getinfo(2, "f").func == leaf then
      tails = tails+1
      error("tail return hook failed")
    end
  end, "r")
  local ok, message = pcall(outer)
  debug.sethook()
  assert(not ok and message:find("tail return hook failed", 1, true))
  assert(tails == 1)
  chain_case(false, "cr")
end

local function coroutine_case()
  local leaf = function() return coroutine.yield(17) end
  local middle = function() return leaf() end
  local outer = function() return middle() end
  local co = coroutine.create(outer)
  local tails = 0
  debug.sethook(co, function(event)
    local info = debug.getinfo(2, "f")
    if event == "tail return" and info.func == leaf then tails = tails+1 end
  end, "cr")
  local ok, value = coroutine.resume(co)
  assert(ok and value == 17 and tails == 0)
  ok, value = coroutine.resume(co, 42)
  debug.sethook(co)
  assert(ok and value == 42 and tails == (count and 2 or 0))
  assert(coroutine.status(co) == "dead")
end

local function recursion_case()
  local calls, returns, tails = 0, 0, 0
  local loop
  loop = function(n)
    if n == 0 then return 17 end
    return loop(n-1)
  end
  debug.sethook(function(event)
    if debug.getinfo(2, "f").func == loop then
      if event == "call" then calls = calls+1
      elseif event == "return" then returns = returns+1
      elseif event == "tail return" then tails = tails+1 end
    end
  end, "cr")
  local result = loop(200)
  debug.sethook()
  assert(result == 17 and calls == 201 and returns == 1)
  assert(tails == (count and 200 or 0))
end

local function mixed_case(bits, large)
  local leaf, middle, outer, driver
  local function result(a, ...)
    return a, select("#", ...), select(3, ...)
  end
  if bits % 2 == 1 then leaf = result
  else leaf = function(a, b, c, d) return a, 3, d end end
  if math.floor(bits/2) % 2 == 1 then
    middle = function(...) return leaf(...) end
  else
    middle = function(a, b, c, d) return leaf(a, b, c, d) end
  end
  if math.floor(bits/4) % 2 == 1 then
    outer = function(...) return middle(...) end
  else
    outer = function(a, b, c, d) return middle(a, b, c, d) end
  end
  local args = {17, 2, nil, 4}
  if large then
    for i = 5, 300 do args[i] = {i} end
  end
  local argc = large and 300 or 4
  driver = function()
    local a, n, last = outer(unpack(args, 1, argc))
    local expectedn = (large and bits == 7) and 299 or 3
    assert(a == 17 and n == expectedn and last == 4)
  end
  local parents = {[outer]=driver, [middle]=outer, [leaf]=middle}
  local calls, tails = 0, 0
  debug.sethook(function(event)
    local fn = debug.getinfo(2, "f").func
    if parents[fn] and event == "call" then
      assert(debug.getinfo(3, "f").func == parents[fn], "mixed caller frame")
      calls = calls+1
      assert(grow(180) == 181)
      collectgarbage("collect")
    elseif fn == leaf and event == "tail return" then tails = tails+1 end
  end, "cr")
  driver()
  debug.sethook()
  assert(calls == 3 and tails == (count and 2 or 0))
end

local function metamethod_case()
  local object, caller
  local leaf = function(self, value)
    assert(self == object)
    return value
  end
  object = setmetatable({}, {__call=leaf})
  caller = function() return object(17) end
  local calls, tails = 0, 0
  debug.sethook(function(event)
    if debug.getinfo(2, "f").func == leaf then
      if event == "call" then
        assert(debug.getinfo(3, "f").func == caller)
        assert(debug.getlocal(2, 1) == "self")
        assert(debug.setlocal(2, 2, 42) == "value")
        calls = calls+1
      elseif event == "tail return" then tails = tails+1 end
    end
  end, "cr")
  local value = caller()
  debug.sethook()
  assert(value == 42 and calls == 1 and tails == (count and 1 or 0))
end

local function native_case()
  local probe = require("hookprobe")
  local leaf = function() return 17 end
  local middle = function() return leaf() end
  local outer = function() return middle() end
  local driver = function()
    local result = outer()
    return result
  end
  local names = {[leaf]="leaf", [middle]="middle", [outer]="outer", [driver]="driver"}
  local reentries = 0
  local ok, value, log = probe.run(driver, function(event)
    assert(grow(180) == 181)
    collectgarbage("collect")
    if event == 4 then
      -- A nested run restores its parent's context; active hooks stay suppressed.
      local nested, result, nestedlog = probe.run(function() return 42 end)
      assert(nested and result == 42 and #nestedlog == 0)
      local co = coroutine.create(function()
        local nestedok, answer, colog = probe.run(function() return 43 end)
        assert(nestedok and answer == 43 and #colog == 0)
        return answer
      end)
      local resumed, answer = coroutine.resume(co)
      assert(resumed and answer == 43)
      reentries = reentries+1
    end
  end)
  assert(ok and value == 17, "native hook scratch must not change results")
  local events = {}
  for _, entry in ipairs(log) do
    local name = names[entry.func]
    if name == "outer" or name == "middle" or name == "leaf" then
      if entry.event == 0 then
        events[#events+1] = "call:" .. name .. ":" .. tostring(names[entry.parent])
      elseif entry.event == 1 then
        events[#events+1] = "return:" .. name
      end
    elseif entry.event == 4 then
      assert(entry.what == "tail" and entry.func == nil,
        "native LUA_HOOKTAILRET must describe a virtual frame")
      events[#events+1] = "tail"
    end
  end
  local expected = {"call:outer:driver", "call:middle:outer", "call:leaf:middle",
    "return:leaf"}
  if count then expected[5], expected[6] = "tail", "tail" end
  check(events, expected, "native")
  assert(reentries == (count and 2 or 0))
  local failed, message = probe.run(driver, function(event)
    if event == 0 then error("native hook callback failed") end
  end)
  assert(not failed and message:find("native hook callback failed", 1, true))
  assert(debug.gethook() == nil, "native hook must restore the prior hook")
  if cframe then
    local values = {}
    for i = 1, 299 do values[i] = {i} end
    local tail = function() return unpack(values, 1, 300) end
    local wrapper = function() return tail() end
    local function pack(...) return {n=select("#", ...), ...} end
    local good, result = probe.run(function() return pack(wrapper()) end)
    assert(good and result.n == 300 and result[300] == nil)
    for i = 1, 299 do assert(result[i] == values[i]) end
  end
  if count then
    local failedtail, tailmessage = probe.run(driver, function(event)
      if event == 4 then error("native tail hook failed") end
    end)
    assert(not failedtail and tailmessage:find("native tail hook failed", 1, true))
    assert(debug.gethook() == nil)
  end
end

local function suite(tag)
  for _, vararg in ipairs({false, true}) do
    for _, mask in ipairs({"c", "r", "cr"}) do chain_case(vararg, mask) end
    for _, change in ipairs({"grow", "disable", "replace", "first", "call-disable"}) do
      chain_case(vararg, "cr", change)
    end
  end
  recursion_case()
  tail_error_case()
  for bits = 0, 7 do mixed_case(bits, false) end
  mixed_case(7, true)
  metamethod_case()
  if cframe then
    alias_case()
    ctail_case()
    multres_case()
    error_case()
    coroutine_case()
  end
  if modes.native then native_case() end
  print("ok tailcall_hooks " .. tag)
end

if jit then jit.off(); jit.flush() end
suite("interp")
if jit and pcall(jit.on) then
  -- Start the hook suite with a fresh trace cache.
  jit.flush()
  suite("jit-on")
  jit.off()
end

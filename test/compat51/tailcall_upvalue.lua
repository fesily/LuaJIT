-- A retained C-tail caller keeps its locals aliased until its actual return.
local jit = jit
local stock = arg and arg[1] == "stock"
local escaped, calls = nil, 0

local function update(wrapper, read, value)
  -- Debug writes are outside the immutable-upvalue contract of compiled reads.
  if jit then jit.off(read) end
  calls = calls + 1
  for level = 1, 40 do
    local info = debug.getinfo(level, "f")
    if not info then break end
    if info.func == wrapper then
      for index = 1, 40 do
        local name, initial = debug.getlocal(level, index)
        if not name then break end
        if name == "x" then
          assert(initial == read(), "local and upvalue already differ")
          assert(debug.setlocal(level, index, value) == "x")
          local _, slot = debug.getlocal(level, index)
          assert(slot == value and read() == value, "setlocal must update closure")
          local replacement = type(value) == "number" and value + 1 or {}
          assert(debug.setupvalue(read, 1, replacement) == "x")
          local _, after = debug.getlocal(level, index)
          assert(after == replacement and read() == replacement,
            "setupvalue must update local")
          debug.setlocal(level, index, value)
          assert(read() == value)
          return value, read(), nil
        end
      end
    end
  end
  error("retained caller local not found")
end
-- Inspect physical frames from the interpreter, as in tailcall_count_jit.lua.
if jit then jit.off(update) end

local function pcall_case(expanded, hot)
  local wrapper
  if expanded then
    wrapper = function(...)
      local x = 1
      local function read() return x end
      escaped = read
      return pcall(update, wrapper, read, ...)
    end
  else
    wrapper = function(value)
      local x = 1
      local function read() return x end
      escaped = read
      if hot then
        for i = 1, 300 do x = x + 1 end
      end
      return pcall(update, wrapper, read, value)
    end
  end
  local ok, slot, captured, tail = wrapper(99)
  assert(ok and slot == 99 and captured == 99 and tail == nil)
  collectgarbage("collect")
  assert(escaped() == 99, "C return must close the updated value")
end

local function fixup_case()
  local wrapper
  wrapper = function()
    local x = 1
    local read
    for i = 1, 2 do
      if i == 2 then return pcall(update, wrapper, read, 99) end
      read = function() return x end
      escaped = read
    end
  end
  local ok, a, b = wrapper()
  assert(ok and a == 99 and b == 99,
    "return fixup must keep the C call in the local lifetime: " .. tostring(a))
  assert(escaped() == 99)
end

local function native_case()
  local wrapper, touched
  wrapper = function()
    local x = 1
    local function read() return x end
    escaped = read
    return table.sort({3, 1, 2}, function(a, b)
      if not touched then
        touched = true
        update(wrapper, read, 99)
      end
      return a < b
    end)
  end
  assert(select("#", wrapper()) == 0 and touched)
  assert(escaped() == 99, "native C callback must retain the alias")
end

local function metacall_case()
  local callable = setmetatable({3, 1, 2}, {__call = table.sort})
  local wrapper, touched
  wrapper = function()
    local x = 1
    local function read() return x end
    escaped = read
    return callable(function(a, b)
      if not touched then
        touched = true
        update(wrapper, read, 99)
      end
      return a < b
    end)
  end
  assert(select("#", wrapper()) == 0 and touched and callable[1] == 1)
  assert(escaped() == 99)
end

local function ff_case()
  local wrapper
  wrapper = function()
    local x = 1
    local function read() return x end
    escaped = read
    return tostring(setmetatable({}, {
      __tostring = function()
        update(wrapper, read, 99)
        return "updated"
      end
    }))
  end
  assert(wrapper() == "updated" and escaped() == 99)
end

local function error_case()
  local wrapper
  wrapper = function()
    local x = 1
    local function read() return x end
    escaped = read
    return xpcall(function() error("probe") end, function()
      update(wrapper, read, 99)
      return "handled"
    end)
  end
  local ok, message = wrapper()
  assert(not ok and message == "handled" and escaped() == 99,
    "error handler alias: " .. tostring(ok) .. ", " .. tostring(message) ..
    ", " .. tostring(escaped()))
end

local function unwind_case()
  local wrapper
  wrapper = function()
    local x = 1
    local function read() return x end
    escaped = read
    return table.sort({2, 1}, function()
      update(wrapper, read, 99)
      error("native callback failed")
    end)
  end
  local ok, message = pcall(wrapper)
  assert(not ok and message:find("native callback failed", 1, true))
  collectgarbage("collect")
  assert(escaped() == 99, "error unwind must close the updated value")
end

local function branch_case()
  local wrapper
  local function lua_callee(callback) return callback() end
  wrapper = function(callee)
    local x = 1
    local function read() return x end
    escaped = read
    return callee(function()
      if callee == pcall then
        update(wrapper, read, 99)
      else
        for level = 1, 20 do
          local info = debug.getinfo(level, "f")
          if not info then break end
          assert(info.func ~= wrapper, "Lua branch must replace the caller")
        end
      end
      return read()
    end)
  end
  for i = 1, 4 do
    if i % 2 == 0 then
      assert(wrapper(lua_callee) == 1 and escaped() == 1)
    else
      local ok, value = wrapper(pcall)
      assert(ok and value == 99 and escaped() == 99)
    end
  end
end

local function grow(n)
  if n == 0 then return 0 end
  return 1 + grow(n - 1)
end

local function growth_case()
  local wrapper
  wrapper = function()
    local x = {value = 1}
    local function read() return x end
    escaped = read
    return pcall(function()
      assert(grow(200) == 200)
      collectgarbage("collect")
      local value = {value = 99}
      update(wrapper, read, value)
      collectgarbage("collect")
      return read()
    end)
  end
  local ok, value = wrapper()
  assert(ok and value.value == 99 and escaped() == value)
  collectgarbage("collect")
  assert(escaped() == value, "closing a GC value must preserve identity")
end

local function yield_case()
  local wrapper
  wrapper = function()
    local x = 1
    local function read() return x end
    escaped = read
    return coroutine.yield(read)
  end
  local co = coroutine.create(wrapper)
  local ok, read = coroutine.resume(co)
  assert(ok and type(read) == "function")
  local found
  for level = 1, 20 do
    local info = debug.getinfo(co, level, "f")
    if not info then break end
    if info.func == wrapper then
      for index = 1, 20 do
        local name = debug.getlocal(co, level, index)
        if not name then break end
        if name == "x" then
          assert(debug.setlocal(co, level, index, 99) == "x")
          assert(read() == 99, "yielded caller must retain the alias")
          found = true
          break
        end
      end
    end
  end
  assert(found, "yielded C-tail caller remains visible")
  local resumed, a, b = coroutine.resume(co, 42, nil)
  assert(resumed and a == 42 and b == nil and read() == 99)
  assert(coroutine.status(co) == "dead")
end

local function many_results_case()
  local values = {}
  for i = 1, 300 do values[i] = i end
  local function wrapper()
    local x = 99
    escaped = function() return x end
    return unpack(values, 1, 300)
  end
  assert(select("#", wrapper()) == 300 and escaped() == 99)
  assert(select(300, wrapper()) == 300)
end

local function lua_case()
  local seen = {}
  local function loop(n)
    local x = n
    if n % 1000 == 0 then seen[n] = function() return x end end
    if n == 0 then return 42 end
    return loop(n - 1)
  end
  assert(loop(10000) == 42, "Lua tail recursion must keep constant stack")
  for n, read in pairs(seen) do assert(read() == n, "Lua frame closes before reuse") end
  local values = {}
  for i = 1, 300 do values[i] = i end
  local function leaf(read, ...)
    assert(read() == 99, "Lua CALLMT closes before moving arguments")
    return select("#", ...), select(300, ...)
  end
  local function wrapper(...)
    local x = 99
    local function read() return x end
    escaped = read
    return leaf(read, ...)
  end
  local n, last = wrapper(unpack(values, 1, 300))
  assert(n == 300 and last == 300 and escaped() == 99)
end

local function block_case()
  local read
  do
    local x = 1
    read = function() return x end
  end
  local x = 99
  assert(x == 99 and read() == 1, "ordinary scope UCLO remains immediate")
end

local function dump_case()
  local source = [[
    return function(callback, value)
      local x = 1
      local function read() return x end
      return pcall(callback, read, value)
    end
  ]]
  local wrapper = assert(loadstring(source))()
  local blob = string.dump(wrapper)
  if jit then
    assert(blob:byte(4) == 0x81, "retained C-tail dump version")
    for _, version in ipairs({2, 0x80}) do
      local old = blob:sub(1, 3) .. string.char(version) .. blob:sub(5)
      local rejected, message = loadstring(old)
      assert(rejected == nil and message:find("incompatible bytecode", 1, true),
        "older tail-close layouts must be rejected")
    end
  end
  wrapper = assert(loadstring(blob))
  local function callback(read, value)
    if jit then jit.off(read) end
    for level = 1, 20 do
      local info = debug.getinfo(level, "f")
      if not info then break end
      if info.func == wrapper then
        for index = 1, 20 do
          local name = debug.getlocal(level, index)
          if not name then break end
          if name == "x" then
            debug.setlocal(level, index, value)
            assert(read() == value, "reloaded bytecode must retain the alias")
            return read()
          end
        end
      end
    end
    error("dumped wrapper not found")
  end
  local ok, value = wrapper(callback, 99)
  assert(ok and value == 99)
end

local function suite(tag)
  calls = 0
  lua_case()
  block_case()
  if not stock then
    pcall_case(false, false)
    pcall_case(true, false)
    pcall_case(false, true)
    fixup_case()
    native_case()
    metacall_case()
    ff_case()
    error_case()
    unwind_case()
    branch_case()
    growth_case()
    yield_case()
    many_results_case()
    dump_case()
    assert(calls == 12, "callbacks must execute exactly once per fixture")
  end
  print("ok tailcall_upvalue " .. tag)
end

if jit then
  jit.off()
  jit.flush()
end
suite("interp")
if jit then
  if pcall(jit.on) then
    jit.opt.start("hotloop=3", "hotexit=2", "minstitch=0")
    for i = 1, 12 do suite("jit-" .. i) end
    local util = require("jit.util")
    assert(util.traceinfo(1), "heated suite must compile traces")
  end
  jit.off()
end

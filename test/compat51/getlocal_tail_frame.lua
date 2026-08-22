--[[
  Repro: debug.getlocal on a Lua 5.1 virtual tail-call frame (lua_Debug.i_ci == 0).

  Game dump (donotstarvetogether_master.dmp):
    AV c0000005 in lua51DS!debug_framepc  cmp [GCfunc+0x0A],0  fn=0x903
    scripts/stacktrace.lua:35 getdebuglocals -> debug.getlocal
    after error() with a tail call still on the stack.

  Lua 5.1 lua_getstack() reports lost tails with i_ci=0. lua_getinfo()
  fills a dummy "tail" record. lua_getlocal() must return no locals
  (nil / (*temporary)), not treat i_ci=0 as stack offset 0.

  Run:
    luajit -joff luajit/test/compat51/getlocal_tail_frame.lua
    lua5.1     luajit/test/compat51/getlocal_tail_frame.lua
]]

io.stdout:setvbuf("no")
io.stderr:setvbuf("no")

local MAX_FRAMES = 64
local MAX_LOCALS = 256

local function engine_tag()
  if type(jit) == "table" then
    return "LuaJIT " .. tostring(jit.version) .. (jit.status() and " jit-on" or " jit-off")
  end
  return tostring(_VERSION)
end

-- Same walk as scripts/stacktrace.lua getdebuglocals + getdebugstack.
-- AV here is the dump. pcall cannot catch it.
local function walk_getlocal(start)
  start = start or 0
  local nframes, ntails = 0, 0
  for level = start, start + MAX_FRAMES - 1 do
    local info = debug.getinfo(level, "S")
    if not info then break end
    nframes = nframes + 1
    if info.what == "tail" then ntails = ntails + 1 end
    for index = 1, MAX_LOCALS do
      local name = debug.getlocal(level, index)
      if not name then break end
    end
  end
  return nframes, ntails
end

local passed, failed = 0, 0

local function expect(name, cond, detail)
  if cond then
    passed = passed + 1
    print("PASS  " .. name)
  else
    failed = failed + 1
    print("FAIL  " .. name)
    if detail and detail ~= "" then print("      " .. detail) end
  end
end

-- Live stack: tail-called leaf walks frames, including what=="tail".
local function case_live_getlocal_on_tail()
  local nframes, ntails, tname, tlevel
  local function leaf()
    print("live: entering leaf, walking for tail frame")
    for i = 0, MAX_FRAMES - 1 do
      local ar = debug.getinfo(i, "S")
      if not ar then break end
      if ar.what == "tail" then
        print("live: getlocal on tail level " .. i)
        tlevel = i
        tname = debug.getlocal(i, 1)
        break
      end
    end
    nframes, ntails = walk_getlocal(0)
  end
  local function mid() return leaf() end
  local function outer() return mid() end
  outer()
  expect("live: saw tail frame", tlevel ~= nil, "tlevel=" .. tostring(tlevel))
  expect("live: ntail>=1", ntails >= 1,
    "ntail=" .. tostring(ntails) .. " nframes=" .. tostring(nframes))
  -- PUC 5.1 may yield (*temporary) from base_ci; never a real local name.
  expect("live: getlocal(tail,1) has no real local",
    tname == nil or tostring(tname):sub(1, 1) == "(",
    "name=" .. tostring(tname))
end

-- Dump path: error() under return-tail calls, then DST-style getlocal walk
-- from the xpcall handler. This is stacktrace.lua:35.
local function case_error_handler_getlocal()
  local nframes, ntails, handler_ran
  local function boom()
    error("variable 'b' is not declared")
  end
  local function spawn()
    return boom()
  end
  local function debug_spawn()
    return spawn()
  end
  local function c_spawn()
    debug_spawn()
  end
  local ok, err = xpcall(c_spawn, function(e)
    print("error: handler, walking getlocal")
    handler_ran = true
    nframes, ntails = walk_getlocal(0)
    return e
  end)
  expect("error: xpcall caught", ok == false and handler_ran == true, tostring(err))
  expect("error: ntail>=1", ntails >= 1,
    "ntail=" .. tostring(ntails) .. " nframes=" .. tostring(nframes))
  expect("error: original message kept",
    type(err) == "string" and err:find("not declared", 1, true) ~= nil,
    tostring(err))
end

print("engine: " .. engine_tag())
print("----")
case_live_getlocal_on_tail()
case_error_handler_getlocal()
print("----")
print(string.format("summary: %d passed, %d failed", passed, failed))
if failed > 0 then os.exit(1) end

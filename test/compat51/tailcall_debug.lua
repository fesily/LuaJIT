--[[
  Lua 5.1.5 vs LuaJIT tailcall/debug tests (hard-capped stack walks).
  Run: lua5.1|src/luajit [-joff] test/compat51/tailcall_debug.lua
       test/compat51/run_compare.sh
]]


io.stdout:setvbuf("no")
io.stderr:setvbuf("no")

local MAX_FRAMES = 64

local function engine_tag()
  if type(jit) == "table" then
    local ver = tostring(jit.version or "?")
    ver = ver:gsub("\n", " ")
    local on = type(jit.status) == "function" and not not jit.status()
    return "LuaJIT " .. ver .. " jit=" .. (on and "on" or "off")
  end
  return tostring(_VERSION)
end

local function norm_src(s)
  if s == nil then return nil end
  s = tostring(s):gsub("\\", "/")
  return s:match("([^/]+)$") or s
end

local function capture_here(what)
  what = what or "Snlu"
  local t = {}
  for i = 0, MAX_FRAMES - 1 do
    local ar = debug.getinfo(i, what)
    if not ar then break end
    t[#t + 1] = {
      what = ar.what,
      source = ar.source,
      short_src = norm_src(ar.short_src),
      linedefined = ar.linedefined,
      lastlinedefined = ar.lastlinedefined,
      currentline = ar.currentline,
      name = ar.name,
      namewhat = ar.namewhat,
      nups = ar.nups,
      istailcall = ar.istailcall,
    }
  end
  return t
end

local function shape_of(frames)
  local p = {}
  for i = 1, #frames do p[i] = frames[i].what end
  return table.concat(p, ",")
end

local function count_what(frames, w)
  local n = 0
  for i = 1, #frames do
    if frames[i].what == w then n = n + 1 end
  end
  return n
end

local function first_what(frames, w)
  for i = 1, #frames do
    if frames[i].what == w then return frames[i], i end
  end
  return nil, nil
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

local function case_two_tails()
  local snap
  local function c() snap = capture_here("Snlu") end
  local function b() return c() end
  local function a() return b() end
  a()
  local nt = count_what(snap, "tail")
  expect("two_tails: ntail==2", nt == 2, "ntail=" .. nt .. " shape=" .. shape_of(snap))
  local tail = first_what(snap, "tail")
  expect("two_tails: tail.currentline==-1", tail and tail.currentline == -1)
  expect("two_tails: tail.linedefined==-1", tail and tail.linedefined == -1)
  expect("two_tails: tail.nups==0", tail and tail.nups == 0)
  expect("two_tails: source sentinel",
    tail and (tail.source == "=(tail call)" or tail.source == "(tail call)"))
  expect("two_tails: short_src has tail call",
    tail and tostring(tail.short_src):find("tail call", 1, true) ~= nil)
  expect("two_tails: name empty/nil", tail and (tail.name == "" or tail.name == nil))
  expect("two_tails: short_src==(tail call) [5.1.5]",
    tail and tail.short_src == "(tail call)",
    tail and ("short_src=" .. tostring(tail.short_src)))
end

local function case_three_tails()
  local snap
  local function d() snap = capture_here("S") end
  local function c() return d() end
  local function b() return c() end
  local function a() return b() end
  a()
  local nt = count_what(snap, "tail")
  expect("three_tails: ntail==3", nt == 3, "ntail=" .. nt .. " shape=" .. shape_of(snap))
end

local function case_no_tail()
  local snap
  local function b() snap = capture_here("S"); return 1 end
  local function a() b(); return 2 end
  a()
  expect("no_tail: ntail==0", count_what(snap, "tail") == 0, "shape=" .. shape_of(snap))
end

local function case_getinfo_on_tail()
  local okS, okL, okU, okN, okF, okSrc = false, false, false, false, false, false
  local detail = ""
  local function leaf()
    local tlevel
    for i = 0, MAX_FRAMES - 1 do
      local ar = debug.getinfo(i, "S")
      if not ar then break end
      if ar.what == "tail" then tlevel = i; break end
    end
    if not tlevel then detail = "no tail"; return end
    local arS = debug.getinfo(tlevel, "S")
    local arl = debug.getinfo(tlevel, "l")
    local aru = debug.getinfo(tlevel, "u")
    local arn = debug.getinfo(tlevel, "n")
    local arf = debug.getinfo(tlevel, "f")
    okS = arS and arS.what == "tail"
    okL = arl and arl.currentline == -1
    okU = aru and aru.nups == 0
    okN = arn and (arn.name == "" or arn.name == nil)
    okF = arf and arf.func == nil
    okSrc = arS and (arS.source == "=(tail call)" or arS.source == "(tail call)")
    detail = string.format("S=%s line=%s nups=%s name=%s func=%s",
      tostring(arS and arS.what), tostring(arl and arl.currentline),
      tostring(aru and aru.nups), tostring(arn and arn.name),
      tostring(arf and type(arf.func)))
  end
  local function b() return leaf() end
  local function a() return b() end
  a()
  expect("getinfo_tail: S.what=tail", okS, detail)
  expect("getinfo_tail: l.currentline=-1", okL, detail)
  expect("getinfo_tail: u.nups=0", okU, detail)
  expect("getinfo_tail: n.name empty/nil", okN, detail)
  expect("getinfo_tail: f.func is nil", okF, detail)
  expect("getinfo_tail: source sentinel", okSrc, detail)
end

-- DST stacktrace.lua:35: getdebuglocals calls debug.getlocal on every
-- frame, including Lua 5.1 virtual tail frames (i_ci==0). lua_getlocal
-- must not treat i_ci=0 as stack offset 0 (AV in debug_framepc).
local function case_getlocal_on_tail()
  local tlevel, tname, walked, nframes
  local function leaf()
    for i = 0, MAX_FRAMES - 1 do
      local ar = debug.getinfo(i, "S")
      if not ar then break end
      if ar.what == "tail" then
        tlevel = i
        tname = debug.getlocal(i, 1)
        break
      end
    end
    nframes = 0
    for i = 0, MAX_FRAMES - 1 do
      if not debug.getinfo(i, "S") then break end
      nframes = nframes + 1
      local idx = 1
      while idx < 256 do
        local name = debug.getlocal(i, idx)
        if not name then break end
        idx = idx + 1
      end
    end
    walked = true
  end
  local function b() return leaf() end
  local function a() return b() end
  a()
  expect("getlocal_tail: found tail", tlevel ~= nil, "tlevel=" .. tostring(tlevel))
  expect("getlocal_tail: walk finished", walked == true, "nframes=" .. tostring(nframes))
  expect("getlocal_tail: no real local",
    tname == nil or tostring(tname):sub(1, 1) == "(",
    "name=" .. tostring(tname))
end


local function case_traceback()
  local tb
  local function leaf() tb = debug.traceback("ERR", 1) end
  local function b() return leaf() end
  local function a() return b() end
  a()
  expect("traceback: non-nil", tb ~= nil)
  local has = tb and (tb:find("tail call", 1, true) or tb:find("tail calls", 1, true))
  expect("traceback: mentions tail", has ~= nil)
end

local function case_vararg()
  local snap
  local function leaf(...) snap = capture_here("S"); return ... end
  local function b(...) return leaf(...) end
  local function a(...) return b(...) end
  a(1, 2, 3)
  expect("vararg: ntail>=1", count_what(snap, "tail") >= 1, "shape=" .. shape_of(snap))
end

local function case_deep()
  local N = 15
  local snap
  local function leaf() snap = capture_here("S") end
  local f = leaf
  for _ = 1, N do
    local inner = f
    f = function() return inner() end
  end
  f()
  local nt = count_what(snap, "tail")
  expect("deep: ntail==N", nt == N, "ntail=" .. nt .. " N=" .. N)
end

local function case_coro()
  local snap
  local function leaf() snap = capture_here("S") end
  local function b() return leaf() end
  local function a() return b() end
  local co = coroutine.create(function() a() end)
  local ok, err = coroutine.resume(co)
  expect("coro: resume ok", ok, err and tostring(err))
  expect("coro: ntail>=1", snap and count_what(snap, "tail") >= 1)
end

local function case_order()
  local snap
  local function leaf() snap = capture_here("S") end
  local function b() return leaf() end
  local function a() return b() end
  a()
  local i_lua, i_tail, i_main
  for i = 1, #snap do
    if snap[i].what == "Lua" and not i_lua then i_lua = i end
    if snap[i].what == "tail" and not i_tail then i_tail = i end
    if snap[i].what == "main" then i_main = i end
  end
  expect("order: Lua before tail", i_lua and i_tail and i_lua < i_tail, "shape=" .. shape_of(snap))
  expect("order: tail before main", i_tail and i_main and i_tail < i_main, "shape=" .. shape_of(snap))
end


local function case_name_after_tail()
  local name_tail, name_nontail
  local function capture_leaf_name()
    local ar = debug.getinfo(2, "n")
    return ar and ar.name or nil
  end
  do
    local function leaf() name_tail = capture_leaf_name() end
    local function c() return leaf() end
    c()
  end
  do
    local function leaf() name_nontail = capture_leaf_name() end
    local function c() leaf() end
    c()
  end
  expect("name: tail-entered leaf is nil", name_tail == nil, "name=" .. tostring(name_tail))
  expect("name: non-tail-entered leaf is leaf", name_nontail == "leaf", "name=" .. tostring(name_nontail))
end

local function case_uniform()
  local snap
  local function d() snap = capture_here("Snlu") end
  local function c() return d() end
  local function b() return c() end
  local function a() return b() end
  a()
  local tails = {}
  for i = 1, #snap do
    if snap[i].what == "tail" then tails[#tails + 1] = snap[i] end
  end
  expect("uniform: ntail>=2", #tails >= 2, "n=" .. #tails)
  local same = true
  for i = 2, #tails do
    if tails[i].source ~= tails[1].source
      or tails[i].short_src ~= tails[1].short_src
      or tails[i].currentline ~= tails[1].currentline then
      same = false
    end
  end
  expect("uniform: placeholder fields match", same)
end

local function emit_machine_dump()
  local snap
  local function leaf() snap = capture_here("Snlu") end
  local function c() return leaf() end
  local function b() return c() end
  local function a() return b() end
  a()
  print("---MACHINE_DUMP_BEGIN---")
  print("engine=" .. engine_tag())
  print("shape=" .. shape_of(snap))
  print("ntail=" .. count_what(snap, "tail"))
  print("nlua=" .. count_what(snap, "Lua"))
  for i = 1, #snap do
    local f = snap[i]
    print(string.format(
      "frame %d what=%s short_src=%s source=%s line=%s linedef=%s nups=%s name=%s",
      i - 1, f.what, tostring(f.short_src), tostring(f.source),
      tostring(f.currentline), tostring(f.linedefined),
      tostring(f.nups), tostring(f.name)))
  end
  print("---MACHINE_DUMP_END---")
end

local function run_all()
  print("engine: " .. engine_tag())
  print("----")
  case_two_tails()
  case_three_tails()
  case_no_tail()
  case_getinfo_on_tail()
  case_getlocal_on_tail()
  case_traceback()
  case_vararg()
  case_deep()
  case_coro()
  case_order()
  case_name_after_tail()
  case_uniform()
  print("----")
  print(string.format("summary: %d passed, %d failed", passed, failed))
  if failed > 0 then os.exit(1) end
end

if arg and arg[1] == "--dump" then
  emit_machine_dump()
else
  run_all()
end

local probe = require("retmprobe")
local util = require("jit.util")
local mode = arg[1]
jit.opt.start("hotloop=3", "hotexit=2", "minstitch=0")

local function fixed(n, typed) return probe.values(n, typed) end
local function vararg(...) return probe.values(...) end
local function arguments(n) return n, false, nil end
local function expanded(n) return probe.values(arguments(n)) end
local function pack(...) return {n = select("#", ...), ...} end
local counts = {0, 1, 3, 4, 300, 600}
local stable = {4, 300, 600}
local function expect(n, typed, a, b, c, d)
  assert(a == (n > 0 and (typed and "changed" or n*1000+1) or nil))
  assert(b == (n >= 2 and n*1000+2 or nil))
  assert(c == nil)
  assert(d == (n >= 4 and n*1000+4 or nil))
end

local function drive(wrapper, wanted, warm, changing)
  local before = probe.stats().calls
  local sum = 0
  for i = 1, 1200 do
    local n = i <= 800 and warm or changing[(i%#changing)+1]
    if wanted == 0 then
      wrapper(n)
      sum = sum+1
    elseif wanted == 1 then
      local a = wrapper(n)
      assert(a == (n > 0 and n*1000+1 or nil))
      sum = sum+(a or 0)
    else
      local a, b, c, d = wrapper(n)
      expect(n, false, a, b, c, d)
      sum = sum+(a or 0)
    end
  end
  assert(probe.stats().calls-before == 1200, "C call was replayed")
  return sum
end

local initial = probe.stats()
if mode == "fixed" or mode == "vararg" or mode == "expanded" then
  local wrapper = mode == "vararg" and vararg or mode == "expanded" and expanded or fixed
  for _, wanted in ipairs({0, 1, 4}) do
    for _, warm in ipairs(counts) do
      jit.flush()
      drive(wrapper, wanted, warm, counts)
    end
  end
  for _, wanted in ipairs({0, 1, 4}) do
    jit.flush()
    local before = probe.stats()
    drive(wrapper, wanted, 300, stable)
    local after = probe.stats()
    if initial.cframe ~= 0 then
      assert(after.returns-before.returns > 1000, "count changes left compiled return")
      assert(after.fallbacks == before.fallbacks, "fixed receiver specialized raw count")
      assert(after.roots > 0 and after.linked > 0, "RETM did not link to the caller")
      if mode == "vararg" then assert(after.retfv > 0, "vararg return lacks RETFV") end
    end
  end
  if mode == "vararg" then
    local function args(i)
      if i%3 == 0 then return 4 end
      if i%3 == 1 then return 4, false end
      return 4, false, "extra", nil, i
    end
    local before = probe.stats().returns
    for i = 1, 2400 do
      local a,b,c,d = vararg(args(i))
      expect(4, false, a,b,c,d)
    end
    if initial.cframe ~= 0 then
      assert(probe.stats().returns-before > 2000, "runtime vararg span did not compile")
    end
    local function legacy(n, missing, ...)
      assert(missing == nil and arg[1] == "extra" and arg[2] == nil)
      return probe.values(n)
    end
    for i = 1, 1200 do
      local a,b,c,d = legacy(4, nil, "extra", nil, i)
      expect(4, false, a,b,c,d)
    end
    local function missing(n, unused, ...) return probe.values(n) end
    for i = 1, 1200 do
      local a,b,c,d = missing(4)
      expect(4, false, a,b,c,d)
    end
  end
elseif mode == "multret" then
  -- 259/515 alias the warm count 3 if an entry guard truncates to eight bits.
  local multi_counts = {0, 1, 3, 4, 259, 300, 515, 600}
  local function scan(t, n)
    assert(t.n == n)
    for k = 1, n do
      assert(t[k] == (k%3 ~= 0 and n*1000+k or nil))
    end
  end
  local function warm()
    local t = {n=3, 3001, 3002}
    for i = 1, 100 do scan(t, 3) end
  end
  jit.off(warm)
  warm()
  local function consume(n)
    local t = pack(fixed(n))
    scan(t, n)
  end
  local function driver()
    for i = 1, 1500 do
      local n = i <= 900 and 3 or multi_counts[(i%#multi_counts)+1]
      consume(n)
    end
  end
  -- Let consume acquire a function trace with a dynamic n argument.
  jit.off(driver)
  driver()
  assert(probe.stats().calls-initial.calls == 1500, "MULTRET exit replayed C")
  if initial.cframe ~= 0 then
    assert(probe.stats().fallbacks > initial.fallbacks, "MULTRET count guard did not run")
  end
elseif mode == "interp" then
  -- The wrapper returns to a C frame, so its RETM remains unconsumed on trace.
  local function driver()
    for i = 1, 1500 do
      local n = i <= 900 and 3 or counts[(i%#counts)+1]
      local t = pack(probe.invoke(fixed, n))
      assert(t.n == n, "direct interpreter exit lost the raw count")
      for k = 1, n do
        assert(t[k] == (k%3 ~= 0 and n*1000+k or nil))
      end
    end
  end
  jit.off(driver)
  driver()
  local after = probe.stats()
  assert(after.calls-initial.calls == 3000, "direct exit replayed C")
  if initial.cframe ~= 0 then
    assert(after.entries > initial.entries, "direct interpreter trace was not entered")
    assert(after.interpreted > 0, "no pending RETM trace returned to the interpreter")
    assert(after.fallbacks > initial.fallbacks, "direct exit did not protect raw count")
  end
elseif mode == "guards" then
  local function first(n, typed)
    local a = fixed(n, typed)
    return a
  end
  local function four(n, typed)
    local a,b,c,d = fixed(n, typed)
    expect(n, typed, a,b,c,d)
    return a
  end
  for i = 1, 1800 do
    local typed = i > 800 and i%2 == 0
    local n = counts[(i%#counts)+1]
    local a = first(n, typed)
    assert(a == (n > 0 and (typed and "changed" or n*1000+1) or nil))
  end
  for i = 1, 1800 do
    local n = counts[(i%#counts)+1]
    four(n, i%2 == 0)
  end
  if initial.cframe ~= 0 then
    assert(probe.stats().preexits > initial.preexits, "no pre-return exit")
    assert(probe.stats().sides > 0, "entry side trace did not compile")
    assert(probe.stats().postexits > initial.postexits, "no post-return exit")
  end
  assert(probe.stats().calls-initial.calls == 3600, "guard exit replayed C")
elseif mode == "frames" or mode == "luatails" or mode == "error" or mode == "flush" then
  local wrapper, active, fail = nil, false, false
  local function capture()
    if mode == "flush" and active then jit.flush() end
    if fail then error("callback error") end
    if not active then return end
    local found, tails = false, 0
    for level = 1, 40 do
      local info = debug.getinfo(level, "Sf")
      if not info then break end
      if info.func == wrapper then found = true end
      if info.what == "tail" then tails = tails+1 end
    end
    if initial.cframe ~= 0 then assert(found, "C call lost Lua wrapper frame") end
    if initial.cframe ~= 0 then
      local expected = mode == "luatails" and initial.count ~= 0 and 2 or 0
      assert(tails == expected, "wrong virtual tail count")
    end
  end
  jit.off(capture)
  wrapper = function() return probe.callback(capture) end
  local function mid() return wrapper() end
  local function top() return mid() end
  local call = mode == "luatails" and top or wrapper
  local function driver()
    for i = 1, 1200 do
      active = i > 1100
      fail = mode == "error" and i == 1200
      local a = call()
      assert(a == 7)
    end
  end
  if mode == "error" then
    local found = false
    local ok = xpcall(driver, function()
      for level = 1, 40 do
        local info = debug.getinfo(level, "f")
        if not info then break end
        if info.func == wrapper then found = true end
      end
    end)
    assert(not ok and (initial.cframe == 0 or found))
  else driver() end
elseif mode == "yield" then
  local function wrapper(n) return probe.yield_values(n) end
  local co = coroutine.create(function()
    for i = 1, 100 do
      local t = pack(wrapper(3))
      assert(t.n == 2 and t[1] == 17 and t[2] == nil)
    end
  end)
  for i = 1, 100 do
    local t = pack(coroutine.resume(co, 17, nil))
    assert(t.n == 4 and t[1] and t[2] == 3001 and t[3] == 3002 and t[4] == nil)
  end
  assert(coroutine.resume(co, 17, nil))
  assert(coroutine.status(co) == "dead")
elseif mode == "reentry" then
  local inner = 0
  local function callback()
    inner = inner+1
    local a,b,c,d = vararg(600, false, inner, nil)
    expect(600, false, a,b,c,d)
    if inner%11 == 0 then collectgarbage() end
    if inner%100 == 0 then
      local returns = probe.other_state()
      assert(initial.cframe == 0 or returns > 1000, "other state did not compile")
    end
  end
  jit.off(callback)
  local function wrapper() return probe.callback(callback) end
  for i = 1, 1200 do
    local a = wrapper()
    assert(a == 7)
  end
  assert(inner == 1200)
elseif mode == "coroutine" then
  local function job()
    for i = 1, 30 do
      drive(vararg, 4, counts[(i%#counts)+1], counts)
      coroutine.yield(i)
    end
  end
  local a,b = coroutine.create(job), coroutine.create(job)
  for i = 1, 31 do
    assert(coroutine.resume(a))
    assert(coroutine.resume(b))
  end
  assert(coroutine.status(a) == "dead" and coroutine.status(b) == "dead")
elseif mode == "lifecycle" then
  for round = 1, 15 do
    drive(fixed, 1, 600, stable)
    for n = 1, 1000 do
      if util.traceinfo(n) then jit.flush(n) end
    end
    collectgarbage()
    drive(vararg, 4, 300, counts)
    jit.flush()
  end
elseif mode == "prefix" then
  local function prefix(n) return "prefix", probe.values(n) end
  for i = 1, 1200 do
    local n = counts[(i%#counts)+1]
    local tag,a,b,c,d = prefix(n)
    assert(tag == "prefix")
    expect(n, false, a,b,c,d)
  end
  assert(probe.stats().roots == 0, "ordinary RETM consumer was enabled")
elseif mode == "blacklist" then
  jit.opt.start("maxrecord=4")
  drive(fixed, 1, 600, counts)
  -- Reset hotcounts as well as penalties before testing recovery.
  jit.opt.start("maxrecord=4000", "hotloop=3")
  jit.flush()
  jit.on(drive, true)
  jit.on(fixed, true)
  drive(fixed, 1, 600, stable)
elseif mode == "gc" then
  local function wrapper(n) return probe.objects(n) end
  for i = 1, 1200 do
    local n = i <= 800 and 4 or counts[(i%#counts)+1]
    local a = wrapper(n)
    assert(n == 0 and a == nil or n > 0 and a.index == 1)
    if i%31 == 0 then collectgarbage() end
  end
  assert(probe.stats().calls-initial.calls == 1200, "GC exit replayed C")
elseif mode == "spills" then
  jit.opt.start("hotexit=1", "tryside=16")
  local ffi = require("ffi")
  ffi.cdef[[
    double retm_weighted(double,double,double,double,double,double,double,double,
                         double,double,double,double,double,double,double,double);
  ]]
  local library = ffi.load((package.cpath:match("^[^;]+"):gsub("%?", "retmprobe")))
  local names, terms, extra = {}, {}, {}
  local count, indexsum, extrasum = 0, 0, 0
  for i = 1, 100 do
    names[#names+1] = "a"..i
    if i%3 ~= 0 then
      terms[#terms+1] = "a"..i
      count, indexsum = count+1, indexsum+i
      if #extra < 16 then
        extra[#extra+1] = "a"..i
        extrasum = extrasum+i
      end
    end
  end
  local function caller(weighted)
    local source = "return function(tail,weighted) return function(n) local "..
      table.concat(names, ",").."=tail(n); local result="..table.concat(terms, "+")..
      "; "..table.concat(names, ",").."=tail(n); result=result+"..table.concat(terms, "+")
    if weighted then source = source.."+weighted("..table.concat(extra, ",")..")" end
    source = source.."; for j=1,10 do result=result+j end; return result end end"
    return assert(loadstring(source, "@retm-spills-"..tostring(weighted)))()
  end
  for _, wrapper in ipairs({fixed, vararg}) do
    jit.flush()
    local before = probe.stats().returns
    local function tail(n)
      if n == 300 then return wrapper(n) end
      return wrapper(n, false, "extra", nil, n)
    end
    local first = caller(false)(tail, library.retm_weighted)
    local second = caller(true)(tail, library.retm_weighted)
    local n, weighted = 300, false
    local function capture()
      local result = (weighted and second or first)(n)
      local expected = 2*(n*1000*count+indexsum)+55
      if weighted then expected = expected+n*1000*16+extrasum end
      assert(result == expected, "parent spill or raw count was overwritten")
    end
    jit.off(capture)
    for i = 1, 2400 do
      n = i%2 == 0 and 300 or 600
      -- Train each receiver consecutively before alternating their return PCs.
      weighted = i > 800 and (i <= 1600 or i%2 == 0)
      local result = probe.callback(capture)
      assert(result == 7)
    end
    if initial.cframe ~= 0 then
      local stats = probe.stats()
      assert(stats.returns > before, "wide receiver did not compile")
      assert(stats.sides > 0 and stats.preexits > 0,
             "target frame entry side trace was not exercised: sides="..
             stats.sides.." preexits="..stats.preexits.." roots="..stats.roots)
      if wrapper == vararg then assert(stats.retfv > 0, "wide vararg return lacks RETFV") end
    end
  end
  assert(probe.stats().calls-initial.calls == 14400, "spill exit replayed C")
else error("unknown mode: "..tostring(mode)) end

jit.off()
local final = probe.stats()
local expected_calls = {
  fixed=25200, vararg=30000, expanded=25200, multret=1500, interp=3000,
  guards=3600, frames=1200, luatails=1200, error=1200, flush=1200, yield=100,
  reentry=16800, coroutine=72000, lifecycle=36000, prefix=1200, blacklist=2400,
  gc=1200, spills=14400
}
assert(final.calls-initial.calls == expected_calls[mode], "C side effects were replayed or lost")
if initial.cframe ~= 0 and mode ~= "error" and mode ~= "yield" and
   mode ~= "flush" and mode ~= "prefix" and mode ~= "interp" then
  assert(final.returns > initial.returns, "no compiled RETM completion")
end
print("ok "..mode.." calls="..(final.calls-initial.calls)..
      " entries="..(final.entries-initial.entries)..
      " returns="..(final.returns-initial.returns)..
      " preexits="..(final.preexits-initial.preexits))

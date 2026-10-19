local probe = require("tailcallprobe")
local util = require("jit.util")
local compat = arg[1] ~= "stock"
local hasjit = pcall(jit.on)
local function factory()
  local wrapper
  wrapper = function() return probe.visible(wrapper) end
  return wrapper
end
local function drive(fn, n)
  local visible = 0
  for i = 1, n do if fn() then visible = visible+1 end end
  return visible
end
local function run()
  jit.flush()
  jit.off()
  local wrapper = factory()
  assert(wrapper() == compat, "cold engine-off policy")
  jit.off(wrapper)
  if hasjit then
    jit.on()
    assert(wrapper() == compat, "explicit off with engine on")
    jit.on(wrapper)
    assert(not wrapper(), "native warmup must replace the C-tail caller")
    local seen, flushes = {}, 0
    local function record(tr, fn)
      if fn == wrapper then seen[tr] = true end
    end
    local function trace(event) if event == "flush" then flushes = flushes+1 end end
    jit.attach(record, "record")
    jit.attach(trace, "trace")
    assert(drive(wrapper, 3000) == 0)
    local linked
    for tr in pairs(seen) do
      local info = util.traceinfo(tr)
      if info and info.linktype == "stitch" and info.link ~= 0 then linked = tr end
    end
    assert(linked, "mode test did not warm an outer trace through the wrapper")
    local function cold() return 19 end
    jit.off(cold)
    assert(flushes == 0 and util.traceinfo(linked),
      "disabling a cold prototype must preserve unrelated traces")
    jit.off(wrapper)
    jit.off(wrapper)
    jit.flush(wrapper)
    assert(flushes == 0 and util.traceinfo(linked),
      "function-off/flush must not globally flush inlined or linked callers")
    -- Warm compatibility tests explicitly clear native copies in other callers.
    jit.flush()
    assert(flushes == 1 and not util.traceinfo(linked),
      "explicit flush must clear previously compiled callers")
    assert(drive(wrapper, 3000) == (compat and 3000 or 0),
      "function-off plus explicit flush must enter the interpreter")
    jit.attach(record)
    jit.attach(trace)
    jit.on(wrapper)
    assert(drive(wrapper, 3000) == 0, "on must restore native behavior")
    jit.flush(wrapper)
    assert(not wrapper(), "flush must not change function policy")
    jit.off()
    jit.flush()
    assert(wrapper() == compat, "engine off plus flush")
    jit.on()
  end

  -- Closures share prototype mode; a separately defined callee does not.
  local a, b = factory(), factory()
  jit.off(a)
  assert(a() == compat and b() == compat, "shared prototype mode")
  if hasjit then
    jit.on(b)
    assert(not a() and not b(), "shared prototype reenable")
  end
  local child
  local function parent()
    child = function() return probe.visible(child) end
    return child
  end
  jit.off(parent, true)
  assert(parent()() == compat, "recursive function-off")

  -- Both normal returns and suspended frames retain their actual layout.
  local callback, callback_wrapper, after
  callback_wrapper = function() return probe.invoke(callback, 17) end
  callback = function(n)
    jit.off(callback_wrapper)
    return n
  end
  jit.off(callback)
  assert(callback_wrapper() == 17)
  callback = function(n)
    after = probe.visible(callback_wrapper)
    if hasjit then jit.on(callback_wrapper) end
    return n
  end
  jit.off(callback)
  assert(callback_wrapper() == 17 and after == compat, "callback off/on")
  if hasjit then
    assert(callback_wrapper() == 17 and not after, "callback native return")
    local change, calls = false, 0
    callback = function(n)
      calls = calls+1
      after = probe.visible(callback_wrapper)
      if change then
        jit.off(callback_wrapper)
        jit.flush()
        change = false
      end
      return n
    end
    jit.off(callback)
    assert(drive(callback_wrapper, 3000) == 3000)
    change = true
    assert(callback_wrapper() == 17 and not after,
      "mode change cannot recreate an already replaced caller")
    assert(drive(callback_wrapper, 3000) == 3000 and after == compat,
      "explicit flush inside a stitched C callback must invalidate its continuation")
    assert(calls == 6001, "callback invalidation replayed or skipped calls")
  end

  local yielding
  yielding = function(n) return coroutine.yield(n) end
  jit.off(yielding)
  local co = coroutine.create(yielding)
  local ok, value = coroutine.resume(co, 42)
  assert(ok and value == 42)
  if hasjit then jit.on(yielding) end
  ok, value = coroutine.resume(co, 19)
  assert(ok and value == 19 and coroutine.status(co) == "dead")

  local dumped = assert(loadstring([[
    return function(target) return require("tailcallprobe").visible(target) end
  ]]))()
  jit.off(dumped)
  local loaded = assert(loadstring(string.dump(dumped)))
  assert(dumped(dumped) == compat)
  assert(loaded(loaded) == (not hasjit and compat),
    "PROTO_NOJIT must not be serialized")
  jit.off(loaded)
  assert(loaded(loaded) == compat, "reloaded bytecode must support function-off")
  jit.off()
  jit.flush()
end
jit.off(run)
if hasjit then jit.opt.start("hotloop=3", "hotexit=2", "minstitch=0") end
run()
print("ok tailcall modes")

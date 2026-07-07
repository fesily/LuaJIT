local function set_aggressive_gc()
  collectgarbage("setpause", 0)
  collectgarbage("setstepmul", 200)
end

local function full_collect()
  collectgarbage("collect")
  collectgarbage("collect")
end

local function make_finalizable(counter, on_gc)
  local ud = newproxy(true)
  debug.setmetatable(ud, {
    __gc = function(self)
      counter.n = counter.n + 1
      if on_gc then
        on_gc(self)
      end
    end,
  })
  return ud
end

local function make_batch(counter, count)
  local keep = {}
  for i = 1, count do
    keep[i] = make_finalizable(counter)
  end
  return keep
end

local function scenario_1()
  local counter = { n = 0 }
  local keep = make_batch(counter, 100)

  keep = nil
  full_collect()

  assert(counter.n == 100, string.format("scenario 1: expected 100 finalizers, got %d", counter.n))

  full_collect()
  assert(counter.n == 100, string.format("scenario 1: expected no second finalize, got %d", counter.n))

  io.write(string.format("scenario 1 (normal once): PASS count=%d\n", counter.n))
  return counter.n
end

local function scenario_2()
  local counter = { n = 0 }
  local resurrected = {}

  local keep = make_finalizable(counter, function(self)
    resurrected[1] = self
  end)

  keep = nil
  full_collect()

  assert(counter.n == 1, string.format("scenario 2: expected 1 finalize after resurrection, got %d", counter.n))
  assert(resurrected[1] ~= nil, "scenario 2: resurrected userdata missing after first collect")
  assert(type(resurrected[1]) == "userdata", "scenario 2: resurrected object is not userdata")

  resurrected[1] = nil
  full_collect()

  assert(counter.n == 1, string.format("scenario 2: expected no second finalize, got %d", counter.n))

  io.write(string.format("scenario 2 (resurrect once-then-collectible): PASS count=%d\n", counter.n))
  return counter.n
end

local function scenario_3()
  local counter = { n = 0 }

  do
    local file = assert(io.tmpfile())
    local mt = debug.getmetatable(file)
    local close = assert(mt and mt.__index and mt.__index.close, "scenario 3: tmpfile metatable missing close")

    debug.setmetatable(file, {
      __gc = function(self)
        counter.n = counter.n + 1
        close(self)
      end,
    })
  end

  full_collect()

  assert(counter.n == 1, string.format("scenario 3: expected 1 finalize for tmpfile userdata, got %d", counter.n))

  io.write(string.format("scenario 3 (huge/IO udata): PASS count=%d\n", counter.n))
  return counter.n
end

local function scenario_4()
  local counter = { n = 0 }
  local keep_a = make_batch(counter, 500)

  collectgarbage("step", 50)

  local keep_b = {}
  for i = 1, 500 do
    keep_b[i] = make_finalizable(counter)
    collectgarbage("step", 50)
  end

  keep_a = nil
  keep_b = nil
  full_collect()

  assert(counter.n == 1000, string.format("scenario 4: expected 1000 finalizers, got %d", counter.n))

  io.write(string.format("scenario 4 (alloc-during-sweep): PASS count=%d\n", counter.n))
  return counter.n
end

local function scenario_5()
  local counter_a = { n = 0 }
  local counter_b = { n = 0 }
  local staged = {}

  local function create_b()
    staged[1] = make_finalizable(counter_b)
  end

  do
    local a = make_finalizable(counter_a, create_b)
    a = nil
  end

  full_collect()
  assert(counter_a.n == 1, string.format("scenario 5: expected A finalize once, got %d", counter_a.n))
  assert(staged[1] ~= nil, "scenario 5: B was not created by A finalizer")

  staged[1] = nil
  full_collect()

  assert(counter_b.n == 1, string.format("scenario 5: expected B finalize once, got %d", counter_b.n))

  io.write(string.format("scenario 5 (shutdown re-entrancy): PASS count=%d\n", counter_b.n))
  return counter_b.n
end

set_aggressive_gc()

local s1 = scenario_1()
local s2 = scenario_2()
local s3 = scenario_3()
local s4 = scenario_4()
local s5 = scenario_5()

print(string.format(
  "udata_finalize_assert: all 5 scenarios PASS (counts: s1=%d s2=%d s3=%d s4=%d s5=%d)",
  s1, s2, s3, s4, s5))

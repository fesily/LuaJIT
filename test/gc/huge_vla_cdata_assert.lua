local LUAJIT = "./luajit"

local scenarios = {
  {
    name = "basic_crash_repro",
    code = [[
local ffi = require("ffi")
local cd = ffi.new("char[?]", 700 * 1024)
cd = nil
collectgarbage("collect")
io.write("scenario 1 basic_crash_repro: PASS\n")
]],
  },
  {
    name = "keep_live_across_full_gc",
    code = [[
local ffi = require("ffi")
local cd = ffi.new("char[?]", 700 * 1024)
collectgarbage("collect")
assert(ffi.sizeof(cd) == 700 * 1024,
  string.format("scenario 2 keep_live_across_full_gc: expected sizeof %d, got %d", 700 * 1024, ffi.sizeof(cd)))
io.write("scenario 2 keep_live_across_full_gc: PASS\n")
]],
  },
  {
    name = "finalized_huge_vla",
    code = [[
local ffi = require("ffi")
local counter = { n = 0 }
do
  local cd = ffi.gc(ffi.new("char[?]", 700 * 1024), function()
    counter.n = counter.n + 1
  end)
  cd = nil
end
collectgarbage("collect")
collectgarbage("collect")
assert(counter.n == 1,
  string.format("scenario 3 finalized_huge_vla: expected counter 1, got %d", counter.n))
io.write("scenario 3 finalized_huge_vla: PASS\n")
]],
  },
  {
    name = "hugeset_resize_stress",
    code = [[
local ffi = require("ffi")
local keep = {}
for i = 1, 50 do
  keep[#keep + 1] = ffi.new("char[?]", 600 * 1024)
  keep[#keep + 1] = ffi.new("char[800000]")
end
keep = nil
collectgarbage("collect")
io.write("scenario 4 hugeset_resize_stress: PASS\n")
]],
  },
  {
    name = "over_aligned_vla",
    skip = "LuaJIT FFI cannot express arbitrary over-alignment for VLA cdata from pure Lua; keeping this as a documented skip.",
  },
}

local function write_temp(code)
  local path = assert(os.tmpname())
  local file = assert(io.open(path, "w"))
  assert(file:write(code))
  assert(file:close())
  return path
end

local function remove_temp(path)
  os.remove(path)
end

local function execute(command)
  local rc = os.execute(command)
  if type(rc) == "number" then
    return math.floor(rc / 256)
  end
  if rc == true then
    return 0
  end
  return 1
end

local function run_child(code)
  local path = write_temp(code)
  local rc = execute(string.format('%s -joff "%s"', LUAJIT, path))
  remove_temp(path)
  return rc
end

local failures = 0
local passes = 0

for i = 1, #scenarios do
  local scenario = scenarios[i]
  if scenario.skip then
    io.write(string.format("scenario %d %s: SKIP (%s)\n", i, scenario.name, scenario.skip))
  else
    local rc = run_child(scenario.code)
    if rc == 0 then
      passes = passes + 1
      io.write(string.format("scenario %d %s: PASS (exit %d)\n", i, scenario.name, rc))
    else
      failures = failures + 1
      io.write(string.format("scenario %d %s: FAIL (exit %d)\n", i, scenario.name, rc))
    end
  end
end

if failures == 0 then
  print(string.format("huge_vla_cdata_assert: all %d scenarios PASS", passes))
  os.exit(0)
end

io.stderr:write(string.format("huge_vla_cdata_assert: %d scenario(s) failed\n", failures))
os.exit(1)

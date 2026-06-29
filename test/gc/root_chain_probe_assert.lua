-- T0 baseline harness for the deprecate-arena-gc-root-chain plan.
-- Child-fork per scenario: each scenario runs in its own ./luajit child with
-- LUAJIT_GC_ROOT_CHAIN_PROBE=1 so the arena assert build emits a per-gct
-- root-chain breakdown to stderr at lj_gc_fullgc entry (see gc_root_chain_probe_print
-- in lj_gc_arena.c). The parent captures combined stdout+stderr, parses the
-- [gc_root_chain_probe] line, and reports per-type counts plus PASS/FAIL.
-- A crash in one scenario does not mask the others (child-fork isolation).
--
-- In a release build the probe is compiled out and the [gc_root_chain_probe]
-- line is absent; the harness then reports "(probe compiled out)" and still
-- verifies the scenario runs without crashing.
--
-- Positive scenarios (current root-chain writers): traces, closed upvalues,
--   cdata with finalizer + resurrection, tables, functions/protos, threads,
--   huge objects.
-- Negative controls (must NOT be on the root chain): udata, VLA cdata.

local LUAJIT = "./src/luajit"
local PROBE_ENV = "LUAJIT_GC_ROOT_CHAIN_PROBE=1"

local scenarios = {
  {
    name = "traces",
    jit = true,  -- traces only exist when JIT is active
    code = [[
local function hot(x)
  local s = 0
  for i = 1, x do s = s + i end
  return s
end
for i = 1, 200 do hot(100) end  -- force trace compilation
collectgarbage("collect")
io.write("scenario traces: PASS\n")
]],
  },
  {
    name = "closed_upvalues",
    code = [[
local closed = {}
for i = 1, 100 do
  local v = i * 2
  closed[i] = function() return v end  -- capture v as closed upvalue
end
collectgarbage("collect")
io.write("scenario closed_upvalues: PASS\n")
]],
  },
  {
    name = "cdata_finalizer_resurrect",
    code = [[
local ffi = require("ffi")
local counter = { n = 0 }
local resurrect = {}
do
  local cd = ffi.gc(ffi.new("int32_t[4]", {1,2,3,4}), function(self)
    counter.n = counter.n + 1
    resurrect[1] = self  -- resurrection: keep a reference from finalizer
  end)
  cd = nil
end
collectgarbage("collect")  -- runs finalizer, resurrects cd
io.write("scenario cdata_finalizer_resurrect: PASS\n")
]],
  },
  {
    name = "tables",
    code = [[
local keep = {}
for i = 1, 500 do keep[i] = {a=i, b=i+1, [tostring(i)] = i*2} end
collectgarbage("collect")
io.write("scenario tables: PASS\n")
]],
  },
  {
    name = "functions_protos",
    code = [[
local keep = {}
for i = 1, 200 do
  keep[i] = load(string.format("return function() return %d end", i))
end
collectgarbage("collect")
io.write("scenario functions_protos: PASS\n")
]],
  },
  {
    name = "threads_coroutines",
    code = [[
local keep = {}
for i = 1, 100 do
  keep[i] = coroutine.create(function() coroutine.yield(i) end)
end
collectgarbage("collect")
io.write("scenario threads_coroutines: PASS\n")
]],
  },
  {
    name = "udata_negative",
    code = [[
local keep = {}
for i = 1, 200 do keep[i] = newproxy(true) end  -- udata: NOT on root chain
collectgarbage("collect")
io.write("scenario udata_negative: PASS\n")
]],
  },
  {
    name = "vla_cdata_negative",
    code = [[
local ffi = require("ffi")
local keep = {}
for i = 1, 100 do keep[i] = ffi.new("char[?]", 256) end  -- VLA: NOT on root chain
collectgarbage("collect")
io.write("scenario vla_cdata_negative: PASS\n")
]],
  },
  {
    name = "huge_objects",
    code = [[
local ffi = require("ffi")
local keep = {}
for i = 1, 20 do keep[i] = ffi.new("char[?]", 700 * 1024) end  -- huge VLA blocks
collectgarbage("collect")
io.write("scenario huge_objects: PASS\n")
]],
  },
  {
    name = "baseline_no_alloc",
    code = [[
collectgarbage("collect")
io.write("scenario baseline_no_alloc: PASS\n")
]],
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

-- Parse the first [gc_root_chain_probe] line out of combined child output.
-- Returns a table: { total=, str=, upval=, th=, proto=, func=, trace=,
--   cdata=, tab=, ud=, other= }, or nil if absent (release build).
local function parse_probe(out)
  local line = out:match("%[gc_root_chain_probe%][^\n]*")
  if not line then return nil end
  local t = {}
  local fields = {
    {"total",  "total="},
    {"str",    "str="},
    {"upval",  "upval="},
    {"th",     "th="},
    {"proto",  "proto="},
    {"func",   "func="},
    {"trace",  "trace="},
    {"cdata",  "cdata="},
    {"tab",    "tab="},
    {"ud",     "ud="},
    {"other",  "other="},
  }
  for _, f in ipairs(fields) do
    local v = line:match(f[2] .. "(%d+)")
    t[f[1]] = v and tonumber(v) or 0
  end
  return t
end

local function run_child(scenario)
  local path = write_temp(scenario.code)
  local jit_flag = scenario.jit and "" or "-joff"
  local cmd = string.format("%s %s %s '%s' 2>&1", PROBE_ENV, LUAJIT, jit_flag, path)
  local h = assert(io.popen(cmd, "r"))
  local out = h:read("*a")
  local ok, exitreason = h:close()
  remove_temp(path)
  -- LuaJIT io.popen:close() returns true on success, or (status, "exit"/"signal", code).
  -- Fall back to treating anything-but-true as failure.
  local rc
  if ok == true then
    rc = 0
  elseif type(ok) == "number" then
    rc = (ok >= 256) and math.floor(ok / 256) or ok
  else
    rc = 1
  end
  return rc, out
end

local passes = 0
local failures = 0
local probe_seen_any = false

for i = 1, #scenarios do
  local s = scenarios[i]
  local rc, out = run_child(s)
  local probe = parse_probe(out)
  if probe then probe_seen_any = true end
  if rc == 0 then
    passes = passes + 1
    if probe then
      io.write(string.format(
        "scenario %d %s: PASS (exit %d) | total=%u str=%u upval=%u th=%u proto=%u func=%u trace=%u cdata=%u tab=%u ud=%u other=%u\n",
        i, s.name, rc, probe.total, probe.str, probe.upval, probe.th,
        probe.proto, probe.func, probe.trace, probe.cdata, probe.tab,
        probe.ud, probe.other))
    else
      io.write(string.format(
        "scenario %d %s: PASS (exit %d) | (probe compiled out -- release build)\n",
        i, s.name, rc))
    end
  else
    failures = failures + 1
    io.write(string.format("scenario %d %s: FAIL (exit %d)\n", i, s.name, rc))
    -- Dump child output to aid debugging.
    io.write("---- child output ----\n")
    io.write(out)
    io.write("----------------------\n")
  end
end

io.write(string.format(
  "root_chain_probe_assert: %d PASS, %d FAIL (probe %s)\n",
  passes, failures, probe_seen_any and "active" or "compiled out"))

os.exit(failures == 0 and 0 or 1)

-- memprof_line_assert.lua — v4 line-precise attribution assertions.
--
-- One function allocates on TWO different source lines (a table on one line,
-- a string on the next). With v4 line-precise attribution, the two
-- allocations must resolve to DIFFERENT sites whose labels carry DIFFERENT
-- line numbers — not both the function's firstline. Under v1/v2/v3 both
-- would collapse to the same firstline label.
--
-- Run on a flag-ON binary (LUAJIT_ENABLE_MEMPROF):
--   ./src/luajit -joff test/gc/memprof_line_assert.lua

local memprof = require("memprof")
local parse = require("tools.memprof.parse")
local aggregate = require("tools.memprof.aggregate")

local STREAM = "/tmp/memprof_line_assert.bin"

local failures = 0
local checks = 0
local function check(cond, msg)
  checks = checks + 1
  if not cond then
    failures = failures + 1
    io.write("  FAIL: ", msg, "\n")
  end
end

-- ONE function that allocates on TWO distinct source lines.
local function make_buffers(n)
  local buffers = {}
  for i = 1, n do
    buffers[i] = {}
    buffers[i].tag = ("x"):rep(50)
  end
  return buffers
end

local make_info = debug.getinfo(make_buffers, "S")
local make_source = make_info.source
local make_firstline = make_info.linedefined

memprof.start{mode="event", depth=1, out=STREAM}
local result = make_buffers(200)
result = nil
collectgarbage("collect")
memprof.stop()

local f = io.open(STREAM, "rb")
check(f ~= nil, "stream file written")
if not f then
  io.write(("memprof_line_assert: %d checks, %d failures\n"):format(checks, failures))
  os.exit(failures == 0 and 0 or 1)
end
local data = f:read("*a")
f:close()
os.remove(STREAM)
check(#data > 5, "stream non-empty (" .. #data .. " bytes)")

local parsed = parse.parse(data)
  check(parsed.version == 4 or parsed.version == 5 or parsed.version == 6
        or parsed.version == 7,
        "stream version 4, 5, 6 or 7 (got " .. tostring(parsed.version) .. ")")

local agg = aggregate.aggregate(parsed)

-- Collect all LFUNC sites whose label matches make_buffers' source and
-- carries a line number. With v4, the table allocation (line A) and the
-- string allocation (line B) must appear as DISTINCT sites with DIFFERENT
-- line numbers — neither being the function's firstline.
local make_prefix = make_source .. ":"
local make_sites = {}
for label, st in pairs(agg.sites_leaf) do
  if label:sub(1, #make_prefix) == make_prefix then
    local ln = tonumber(label:sub(#make_prefix + 1))
    if ln then
      make_sites[#make_sites + 1] = { label = label, line = ln, stat = st }
    end
  end
end

check(#make_sites >= 2,
      "at least 2 distinct line-sites for make_buffers (got " ..
      #make_sites .. ")")

-- Sort by line number for stable output.
table.sort(make_sites, function(a, b) return a.line < b.line end)

-- The two sites must carry DIFFERENT line numbers.
local distinct_lines = false
if #make_sites >= 2 then
  distinct_lines = (make_sites[1].line ~= make_sites[#make_sites].line)
end
check(distinct_lines,
      "the line-sites carry DIFFERENT line numbers (got " ..
      (#make_sites >= 2 and
       (make_sites[1].line .. " and " .. make_sites[#make_sites].line) or
       "fewer than 2 sites") .. ")")

-- None of the line-sites should equal the function's firstline (that would
-- indicate v3-style firstline attribution, not v4 line-precise).
for _, s in ipairs(make_sites) do
  check(s.line ~= make_firstline,
        "site line " .. s.line .. " is not the function firstline " ..
        make_firstline)
end

-- Print the resolved sites for human inspection.
for _, s in ipairs(make_sites) do
  io.write(("  site: %s  alloc_objs=%d alloc_space=%d\n"):format(
    s.label, s.stat.alloc_objects, s.stat.alloc_space))
end

io.write(("OK memprof_line_assert: version=%d make_buffers=%s:%d sites=%d lines=%s\n"):format(
  parsed.version, make_source, make_firstline, #make_sites,
  (function()
    local lines = {}
    for _, s in ipairs(make_sites) do lines[#lines+1] = tostring(s.line) end
    return table.concat(lines, ",")
  end)()))

io.write(("memprof_line_assert: %d checks, %d failures\n"):format(checks, failures))
os.exit(failures == 0 and 0 or 1)

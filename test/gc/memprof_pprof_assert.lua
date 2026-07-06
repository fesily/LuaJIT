------------------------------------------------------------------------------
-- memprof_pprof_assert.lua — asserts the pprof exporter emits a byte-valid
-- google/pprof `Profile` protobuf for a KNOWN workload.
--
-- Run on a flag-ON binary (LUAJIT_ENABLE_MEMPROF):
--   ./src/luajit -joff test/gc/memprof_pprof_assert.lua
--
-- The test generates an event stream, aggregates it, exports a pprof Profile,
-- then RE-PARSES the emitted protobuf back in Lua (minimal decoder below) and
-- asserts:
--   (a) string_table[0] == ""
--   (b) sample_type has exactly the 4 ValueTypes in order:
--       alloc_objects/count, alloc_space/bytes, inuse_objects/count,
--       inuse_space/bytes
--   (c) sample count == number of non-empty sites in the aggregate
--   (d) the sum of alloc_space values across samples == aggregate total
--       alloc bytes
--   (e) each Sample has 4 values and >= 1 location_id
--
-- `go` is NOT installed here, so verification is the Lua self-round-trip,
-- not `go tool pprof`. The emitted file is byte-valid uncompressed protobuf
-- that `go tool pprof <file>` would accept.
------------------------------------------------------------------------------

local memprof = require("memprof")
local parse = require("tools.memprof.parse")
local aggregate = require("tools.memprof.aggregate")
local pprof = require("tools.memprof.pprof")

local STREAM = "/tmp/memprof_pprof_assert.bin"
local PPROF = "/tmp/memprof_pprof_assert.pprof"

-- -- minimal protobuf decoder (wire-format reader) -------------------------
-- Just enough to parse a Profile message and its sub-messages. Not general
-- purpose — only handles wire types 0 (varint) and 2 (length-delimited), the
-- two the pprof Profile uses.

local band, bor, lshift, rshift = bit.band, bit.bor, bit.lshift, bit.rshift

local function read_varint(s, pos, e)
  local v = 0
  local shift = 0
  while true do
    if pos > e then error("pb decode: truncated varint at " .. pos) end
    local b = s:byte(pos)
    pos = pos + 1
    v = bor(v, lshift(band(b, 0x7f), shift))
    if band(b, 0x80) == 0 then break end
    shift = shift + 7
  end
  return v, pos
end

-- Parse a length-delimited field body as a list of varints (packed repeated).
local function parse_packed_varints(s, pos, e)
  local out = {}
  while pos <= e do
    local v
    v, pos = read_varint(s, pos, e)
    out[#out + 1] = v
  end
  return out
end

-- Parse a generic message body into a table: fields[fnum] = list of
-- { wire=wt, value=<number for wt0, string for wt2> }. `end_pos` is the
-- exclusive end (one past last byte) of the message body.
local function parse_message(s, pos, end_pos)
  local fields = {}
  while pos < end_pos do
    local tag
    tag, pos = read_varint(s, pos, end_pos)
    local fnum = rshift(tag, 3)
    local wt = band(tag, 7)
    if wt == 0 then
      local v
      v, pos = read_varint(s, pos, end_pos)
      local l = fields[fnum]
      if not l then l = {}; fields[fnum] = l end
      l[#l + 1] = { wire = 0, value = v }
    elseif wt == 2 then
      local len
      len, pos = read_varint(s, pos, end_pos)
      local body = s:sub(pos, pos + len - 1)
      pos = pos + len
      local l = fields[fnum]
      if not l then l = {}; fields[fnum] = l end
      l[#l + 1] = { wire = 2, value = body }
    elseif wt == 1 then
      pos = pos + 8  -- fixed64, skip
    elseif wt == 5 then
      pos = pos + 4  -- fixed32, skip
    else
      error(("pb decode: unsupported wire type %d at field %d"):format(wt, fnum))
    end
  end
  return fields
end

-- Parse a ValueType body: { type=, unit= } (string_table indices).
local function parse_value_type(s)
  local f = parse_message(s, 1, #s + 1)
  local type_idx = f[1] and f[1][1].value or 0
  local unit_idx = f[2] and f[2][1].value or 0
  return { type = type_idx, unit = unit_idx }
end

-- Parse a Sample body: { location_ids={...}, values={...} }.
local function parse_sample(s)
  local f = parse_message(s, 1, #s + 1)
  local location_ids = {}
  if f[1] then
    for i = 1, #f[1] do
      local body = f[1][i].value
      local packed = parse_packed_varints(body, 1, #body)
      for j = 1, #packed do location_ids[#location_ids + 1] = packed[j] end
    end
  end
  local values = {}
  if f[2] then
    for i = 1, #f[2] do
      local body = f[2][i].value
      local packed = parse_packed_varints(body, 1, #body)
      for j = 1, #packed do values[#values + 1] = packed[j] end
    end
  end
  return { location_ids = location_ids, values = values }
end

-- Parse a full Profile protobuf string.
local function decode_profile(s)
  local prof = parse_message(s, 1, #s + 1)
  local sample_types = {}
  if prof[1] then
    for i = 1, #prof[1] do
      sample_types[#sample_types + 1] = parse_value_type(prof[1][i].value)
    end
  end
  local samples = {}
  if prof[2] then
    for i = 1, #prof[2] do
      samples[#samples + 1] = parse_sample(prof[2][i].value)
    end
  end
  local string_table = {}
  if prof[6] then
    for i = 1, #prof[6] do
      string_table[i] = prof[6][i].value  -- 1-based; pprof index = i-1
    end
  end
  local default_sample_type
  if prof[14] then default_sample_type = prof[14][1].value end
  return {
    sample_types = sample_types,
    samples = samples,
    string_table = string_table,
    default_sample_type = default_sample_type,
  }
end

-- -- workload ---------------------------------------------------------------

local function hot(n)
  local t = {}
  for i = 1, n do
    t[i] = { i }
  end
  return t
end

local hot_info = debug.getinfo(hot, "S")
local hot_label = ("%s:%d"):format(hot_info.source, hot_info.linedefined)

local N_BURNED = 2000
local N_RETAINED = 500

memprof.start{mode="event", depth=1, out=STREAM}
local burned = hot(N_BURNED)
local retained = hot(N_RETAINED)
burned = nil
collectgarbage("collect")
collectgarbage("collect")
memprof.stop()

local f = io.open(STREAM, "rb")
assert(f, "stream not written")
local data = f:read("*a")
f:close()
os.remove(STREAM)
assert(#data > 5, "stream too short: " .. #data)

local parsed = parse.parse(data)
local agg = aggregate.aggregate(parsed)
assert(#parsed.events > 0, "no events parsed")

-- -- export pprof -----------------------------------------------------------

local pb_bytes = pprof.build(agg)
assert(#pb_bytes > 0, "pprof build produced empty output")

local fo = io.open(PPROF, "wb")
assert(fo, "cannot open pprof output")
fo:write(pb_bytes)
fo:close()

-- -- re-parse and assert ----------------------------------------------------

local prof = decode_profile(pb_bytes)

-- (a) string_table[0] == ""
assert(prof.string_table[1] == "",
  ("(a) string_table[0] != '': got %q"):format(prof.string_table[1]))

-- (b) sample_type has exactly the 4 ValueTypes in order:
--     alloc_objects/count, alloc_space/bytes, inuse_objects/count,
--     inuse_space/bytes
assert(#prof.sample_types == 4,
  ("(b) sample_type count != 4: got %d"):format(#prof.sample_types))
local expect_types = {
  { "alloc_objects", "count" },
  { "alloc_space",   "bytes" },
  { "inuse_objects", "count" },
  { "inuse_space",   "bytes" },
}
for i = 1, 4 do
  local st = prof.sample_types[i]
  local t_str = prof.string_table[st.type + 1]
  local u_str = prof.string_table[st.unit + 1]
  assert(t_str == expect_types[i][1],
    ("(b) sample_type[%d].type: got %q want %q"):format(
      i, t_str, expect_types[i][1]))
  assert(u_str == expect_types[i][2],
    ("(b) sample_type[%d].unit: got %q want %q"):format(
      i, u_str, expect_types[i][2]))
end

-- (c) sample count == number of non-empty sites in the aggregate.
local n_sites = 0
for _ in pairs(agg.sites) do n_sites = n_sites + 1 end
assert(#prof.samples == n_sites,
  ("(c) sample count != site count: got %d samples, %d sites"):format(
    #prof.samples, n_sites))

-- (d) sum of alloc_space values across samples == aggregate total alloc bytes.
-- value order is [alloc_objects, alloc_space, inuse_objects, inuse_space],
-- so value[2] is alloc_space.
local sum_alloc_space = 0
for i = 1, #prof.samples do
  local v = prof.samples[i].values
  assert(#v == 4,
    ("(e) sample %d has %d values (want 4)"):format(i, #v))
  sum_alloc_space = sum_alloc_space + v[2]
end
assert(sum_alloc_space == agg.totals.alloc_space,
  ("(d) sum alloc_space: got %d want %d"):format(
    sum_alloc_space, agg.totals.alloc_space))

-- (e) each Sample has 4 values and >= 1 location_id.
for i = 1, #prof.samples do
  local smp = prof.samples[i]
  assert(#smp.values == 4,
    ("(e) sample %d: %d values (want 4)"):format(i, #smp.values))
  assert(#smp.location_ids >= 1,
    ("(e) sample %d: %d location_ids (want >= 1)"):format(
      i, #smp.location_ids))
  assert(smp.location_ids[1] ~= 0,
    ("(e) sample %d: leaf location_id is 0 (reserved)"):format(i))
end

io.write(("OK memprof_pprof_assert: events=%d sites=%d samples=%d pprof_bytes=%d alloc_space=%d\n"):format(
  #parsed.events, n_sites, #prof.samples, #pb_bytes, sum_alloc_space))
io.write(("  pprof file: %s  (view with: go tool pprof %s)\n"):format(
  PPROF, PPROF))

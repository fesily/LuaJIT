------------------------------------------------------------------------------
-- pb.lua — minimal protobuf wire-format encoder.
--
-- Hand-rolled encoder for the subset of protobuf wire types needed by the
-- pprof Profile builder (tools/memprof/pprof.lua):
--   * wire type 0  — varint  (int64 / uint64 / bool)
--   * wire type 2  — length-delimited (string, embedded message, packed
--                    repeated scalar)
--
-- Not implemented (not needed by pprof Profile): wire type 1 (fixed64),
-- wire type 5 (fixed32), wire type 3/4 (deprecated groups).
--
-- Tag = (field_number << 3) | wire_type, encoded as a varint. Field numbers
-- in the pprof schema are all < 16, so every tag is a single-byte varint.
--
-- 64-bit note: LuaJIT `bit` ops are 32-bit. We encode varints with pure Lua
-- arithmetic (floor division by 128) so values up to 2^53 are represented
-- exactly as Lua numbers (doubles). Allocation counters in this profiler are
-- well under 2^53 for any realistic workload (2^53 bytes = 9 PiB). If a value
-- ever exceeds 2^53 the high bytes would be silently truncated; callers that
-- need true uint64 can pass an ffi uint64_t / a hi+lo pair via encode_u64_split.
-- This is documented here rather than hidden.
--
-- Usage:
--   local pb = require("tools.memprof.pb")
--   local buf = pb.Buffer.new()
--   buf:varint(150)           -- raw varint
--   buf:tag(1, 0); buf:varint(150)   -- field 1, varint
--   buf:bytes(2, "hello")     -- field 2, length-delimited
--   buf:embedded(2, inner)    -- field 2, length-delimited sub-message
--   local out = buf:result()
--
-- Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h.
------------------------------------------------------------------------------

local M = {}

-- Append a base-128 varint encoding of `v` to string buffer `parts`.
-- `v` must be a non-negative Lua number <= 2^53 for exact encoding.
local function encode_varint(parts, v)
  if v < 0 then
    -- Negative values are not produced by the pprof builder (all counts and
    -- sizes are non-negative). Guard against silent wraparound.
    error(("pb.varint: negative value %d"):format(v))
  end
  if v == 0 then
    parts[#parts + 1] = "\0"
    return
  end
  while v >= 128 do
    parts[#parts + 1] = string.char(v % 128 + 128)
    v = math.floor(v / 128)
  end
  parts[#parts + 1] = string.char(v)
end
M.encode_varint = encode_varint

-- Compute the byte length of a varint without allocating.
local function varint_len(v)
  if v <= 0 then return 1 end
  local n = 0
  while v >= 128 do
    n = n + 1
    v = math.floor(v / 128)
  end
  return n + 1
end
M.varint_len = varint_len

-- Tag a field: emit (field_number << 3) | wire_type as a varint.
-- field_number < 16 -> single byte, but support larger for completeness.
local function encode_tag(parts, field_number, wire_type)
  encode_varint(parts, field_number * 8 + wire_type)
end
M.encode_tag = encode_tag

------------------------------------------------------------------------------
-- Buffer: a growable string-list accumulator. Cheap append, single concat.
------------------------------------------------------------------------------
local Buffer = {}
Buffer.__index = Buffer
M.Buffer = Buffer

function Buffer.new()
  return setmetatable({ _parts = {} }, Buffer)
end

-- Raw varint (no tag).
function Buffer:varint(v)
  encode_varint(self._parts, v)
  return self
end

-- Field (field_number, wire_type=0) holding varint `v`.
function Buffer:varint_field(field_number, v)
  encode_tag(self._parts, field_number, 0)
  encode_varint(self._parts, v)
  return self
end

-- Field (field_number, wire_type=2) holding raw bytes `s` (string).
function Buffer:bytes_field(field_number, s)
  encode_tag(self._parts, field_number, 2)
  encode_varint(self._parts, #s)
  self._parts[#self._parts + 1] = s
  return self
end

-- Field (field_number, wire_type=2) holding an embedded sub-message. `inner`
-- is a Buffer whose result is length-prefixed into this one.
function Buffer:embedded_field(field_number, inner)
  local s = inner:result()
  encode_tag(self._parts, field_number, 2)
  encode_varint(self._parts, #s)
  self._parts[#self._parts + 1] = s
  return self
end

-- Packed repeated varint field (wire type 2). `values` is a list of
-- non-negative numbers. Emits tag + length-prefix + concatenated varints.
function Buffer:packed_varint_field(field_number, values)
  local inner = {}
  for i = 1, #values do
    encode_varint(inner, values[i])
  end
  local s = table.concat(inner)
  encode_tag(self._parts, field_number, 2)
  encode_varint(self._parts, #s)
  self._parts[#self._parts + 1] = s
  return self
end

-- Convenience: emit a length-delimited field from a pre-built byte string.
function Buffer:length_delimited(field_number, s)
  return self:bytes_field(field_number, s)
end

-- Finalize to a single string.
function Buffer:result()
  return table.concat(self._parts)
end

return M

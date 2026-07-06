------------------------------------------------------------------------------
-- pprof.lua — build a google/pprof `Profile` protobuf from aggregated memprof
-- per-site statistics.
--
-- Consumes the table returned by tools/memprof/aggregate.lua (its `sites` map
-- and `totals`) and emits an UNCOMPRESSED serialized `Profile` protobuf message
-- (per google/pprof proto/profile.proto @ 7023385849c0). The output is accepted
-- directly by `go tool pprof` (raw protobuf; gzip is NOT required).
--
-- Schema (field numbers are EXACT — see task spec / profile.proto):
--   Profile:      1 sample_type [ValueType]  2 sample [Sample]
--                 4 location [Location]      5 function [Function]
--                 6 string_table [string]    14 default_sample_type (int64)
--   ValueType:    1 type (int64 strtab idx)  2 unit (int64 strtab idx)
--   Sample:       1 location_id [uint64 packed]  2 value [int64 packed]
--   Location:     1 id (uint64)  4 line [Line]
--   Line:         1 function_id (uint64)  2 line (int64)
--   Function:     1 id (uint64)  2 name (int64 strtab)  3 system_name (int64)
--                 4 filename (int64 strtab)  5 start_line (int64)
--
-- Heap convention: sample_type order is alloc-pair FIRST:
--   [alloc_objects/count, alloc_space/bytes, inuse_objects/count,
--    inuse_space/bytes]
-- so pprof defaults to inuse_space. Each site -> one Function + one Location
-- + one Sample (depth-1 today: location_id[0] is the single leaf frame).
--
-- string_table[0] == "" is mandatory. string_table indices are 0-based.
--
-- Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h.
------------------------------------------------------------------------------

local pb = require("tools.memprof.pb")
local Buffer = pb.Buffer

local M = {}

-- Canonical sample_type / value ordering (alloc pair first).
local SAMPLE_TYPES = {
  { type = "alloc_objects", unit = "count" },
  { type = "alloc_space",   unit = "bytes" },
  { type = "inuse_objects", unit = "count" },
  { type = "inuse_space",   unit = "bytes" },
}

-- Parse a memprof site label into (name, filename, line).
--   @file:line            (LFUNC)  -> name=label, file=file, line=ln
--   TRACE[n]@file:line    (trace)  -> name=label, file=file, line=ln
--   C:0x<hex>             (cfunc)  -> name=label, file=label, line=0
--   INTERNAL / INTERNAL:POD        -> name=label, file=label, line=0
-- `name` is the whole site label (per spec: "Function.name use the site
-- label"); `filename` is the parsed file part when the label carries one,
-- else the whole label; `line` is the parsed line number or 0.
local function parse_site(label)
  local name = label
  -- LFUNC: @<file>:<line>  (file may contain ':' on some platforms; anchor
  -- to the trailing :<digits>$ so we split on the LAST colon.)
  local file, ln = label:match("^@(.-):(%d+)$")
  if file then
    return name, file, tonumber(ln)
  end
  -- TRACE[n]@<file>:<line>
  local tfile, tln = label:match("^TRACE%[%d+%]@(.-):(%d+)$")
  if tfile then
    return name, tfile, tonumber(tln)
  end
  -- C:0x..., INTERNAL, INTERNAL:POD, or anything unparseable.
  return name, label, 0
end
M.parse_site = parse_site

-- String-table interning. Returns a closure that interns a string and yields
-- its 0-based pprof index. Index 0 is reserved for "".
local function new_string_table()
  local list = { "" }       -- 1-based; list[1] == "" -> pprof idx 0
  local idx = { [""] = 0 }  -- string -> 0-based pprof index
  local function intern(s)
    s = tostring(s)
    local i = idx[s]
    if i then return i end
    list[#list + 1] = s
    i = #list - 1  -- 0-based
    idx[s] = i
    return i
  end
  return list, intern
end

-- Encode a ValueType message: 1 type (int64), 2 unit (int64).
local function encode_value_type(type_str, unit_str, intern)
  local b = Buffer.new()
  b:varint_field(1, intern(type_str))
  b:varint_field(2, intern(unit_str))
  return b:result()
end

-- Encode a Function message:
--   1 id (uint64)  2 name (int64 strtab)  3 system_name (int64 strtab)
--   4 filename (int64 strtab)  5 start_line (int64)
local function encode_function(id, name_idx, sysname_idx, filename_idx, start_line)
  local b = Buffer.new()
  b:varint_field(1, id)
  b:varint_field(2, name_idx)
  b:varint_field(3, sysname_idx)
  b:varint_field(4, filename_idx)
  b:varint_field(5, start_line)
  return b:result()
end

-- Encode a Line message: 1 function_id (uint64), 2 line (int64).
local function encode_line(function_id, line)
  local b = Buffer.new()
  b:varint_field(1, function_id)
  b:varint_field(2, line)
  return b:result()
end

-- Encode a Location message: 1 id (uint64), 4 line [Line].
local function encode_location(id, line_msg)
  local b = Buffer.new()
  b:varint_field(1, id)
  b:bytes_field(4, line_msg)
  return b:result()
end

-- Encode a Sample message:
--   1 location_id [uint64 packed]  2 value [int64 packed]
-- location_id is leaf-first; depth-1 today so a single frame.
local function encode_sample(location_id, values)
  local b = Buffer.new()
  b:packed_varint_field(1, { location_id })
  b:packed_varint_field(2, values)
  return b:result()
end

-- Build a complete Profile protobuf from an aggregate result.
-- `agg` is the table returned by aggregate.aggregate().
-- Returns: string (UNCOMPRESSED serialized Profile protobuf bytes)
function M.build(agg)
  local sites = agg.sites
  if not sites then
    error("pprof.build: agg has no `sites` map (expected aggregate output)")
  end

  local string_list, intern = new_string_table()

  -- Stable iteration order: sort site labels ascending so output is
  -- deterministic across runs.
  local labels = {}
  for label in pairs(sites) do labels[#labels + 1] = label end
  table.sort(labels)

  -- First pass: intern the sample_type strings so default_sample_type can
  -- reference "inuse_space" by its index.
  local sample_type_msgs = {}
  for i = 1, #SAMPLE_TYPES do
    local st = SAMPLE_TYPES[i]
    sample_type_msgs[i] = encode_value_type(st.type, st.unit, intern)
  end
  local inuse_space_idx = intern("inuse_space")

  -- Per-site Function / Location / Sample. IDs start at 1 (0 is reserved).
  local function_msgs = {}
  local location_msgs = {}
  local sample_msgs = {}

  local next_id = 1
  for i = 1, #labels do
    local label = labels[i]
    local stat = sites[label]
    -- A "non-empty" site: present in the sites map (aggregate only creates a
    -- site entry when at least one event is attributed to it). Emit a sample
    -- for every such site.
    local name, filename, line = parse_site(label)
    local name_idx = intern(name)
    local sysname_idx = name_idx  -- system_name may equal name
    local filename_idx = intern(filename)

    local fid = next_id
    local lid = next_id  -- one Function + one Location per site, same id space
    next_id = next_id + 1
    -- (IDs are unique across both Function and Location tables; pprof only
    -- requires id != 0 within each table, and using a shared counter keeps
    -- them trivially unique.)

    local fn_msg = encode_function(fid, name_idx, sysname_idx,
                                   filename_idx, line)
    function_msgs[#function_msgs + 1] = fn_msg

    local line_msg = encode_line(fid, line)
    local loc_msg = encode_location(lid, line_msg)
    location_msgs[#location_msgs + 1] = loc_msg

    -- value order MUST match sample_type order:
    --   alloc_objects, alloc_space, inuse_objects, inuse_space
    local values = {
      stat.alloc_objects or 0,
      stat.alloc_space or 0,
      stat.inuse_objects or 0,
      stat.inuse_space or 0,
    }
    -- Clamp negatives to 0 for count fields (a FREE-only site like
    -- INTERNAL:POD can have inuse_objects < 0; pprof value is int64 so
    -- negatives are wire-legal, but they confuse `go tool pprof` top/heap
    -- displays. alloc_* are always >= 0 by construction.)
    if values[3] < 0 then values[3] = 0 end
    if values[4] < 0 then values[4] = 0 end
    local sm_msg = encode_sample(lid, values)
    sample_msgs[#sample_msgs + 1] = sm_msg
  end

  -- Assemble the Profile.
  local prof = Buffer.new()
  -- 1 sample_type (repeated ValueType, wt2)
  for i = 1, #sample_type_msgs do
    prof:bytes_field(1, sample_type_msgs[i])
  end
  -- 2 sample (repeated Sample, wt2)
  for i = 1, #sample_msgs do
    prof:bytes_field(2, sample_msgs[i])
  end
  -- 4 location (repeated Location, wt2)
  for i = 1, #location_msgs do
    prof:bytes_field(4, location_msgs[i])
  end
  -- 5 function (repeated Function, wt2)
  for i = 1, #function_msgs do
    prof:bytes_field(5, function_msgs[i])
  end
  -- 6 string_table (repeated string, wt2); index 0 == "" already first.
  for i = 1, #string_list do
    prof:bytes_field(6, string_list[i])
  end
  -- 14 default_sample_type (int64) — index of "inuse_space".
  prof:varint_field(14, inuse_space_idx)

  return prof:result()
end

-- Convenience: build and write to a file path (or stdout if path == "-" or
-- nil). Returns the byte count written.
function M.export(agg, out_path)
  local data = M.build(agg)
  if out_path and out_path ~= "-" then
    local f, err = io.open(out_path, "wb")
    if not f then error(("pprof.export: cannot open %s: %s"):format(
      out_path, err or "unknown")) end
    f:write(data)
    f:close()
  else
    io.stdout:write(data)
  end
  return #data
end

return M

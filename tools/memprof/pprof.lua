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
-- location_id is leaf-first; for v3 multi-frame stacks it carries the FULL
-- stack (one Location id per frame, leaf..root). For depth=1 streams this
-- collapses to a single location_id (the legacy v1/v2 shape).
local function encode_sample(location_ids, values)
  local b = Buffer.new()
  b:packed_varint_field(1, location_ids)
  b:packed_varint_field(2, values)
  return b:result()
end

-- Build a complete Profile protobuf from an aggregate result.
-- `agg` is the table returned by aggregate.aggregate(). The site key is the
-- full leaf..root stack joined by ";" (v3 multi-frame attribution); each
-- frame is split out and assigned its own Function + Location so `go tool
-- pprof` graph/tree/peek views resolve the full call path. For depth=1
-- streams each site is a single frame and the output is identical to the
-- legacy v1/v2 shape (one Function + one Location per site).
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

  -- Intern Function by (name, filename) and Location by (function_id, line).
  -- IDs start at 1 (0 is reserved). A shared counter keeps Function and
  -- Location ids trivially unique across both tables.
  local function_msgs = {}
  local location_msgs = {}
  local sample_msgs = {}
  local func_id = {}      -- (name|filename) -> function id
  local loc_id = {}       -- (function_id|line) -> location id
  local next_id = 1

  local function intern_function(name, filename)
    local k = name .. "\0" .. filename
    local id = func_id[k]
    if id then return id end
    id = next_id
    next_id = next_id + 1
    func_id[k] = id
    local name_idx = intern(name)
    local filename_idx = intern(filename)
    function_msgs[#function_msgs + 1] =
      encode_function(id, name_idx, name_idx, filename_idx, 0)
    return id
  end

  local function intern_location(fid, line)
    local k = fid .. "\0" .. line
    local id = loc_id[k]
    if id then return id end
    id = next_id
    next_id = next_id + 1
    loc_id[k] = id
    local line_msg = encode_line(fid, line)
    location_msgs[#location_msgs + 1] = encode_location(id, line_msg)
    return id
  end

  for i = 1, #labels do
    local label = labels[i]
    local stat = sites[label]
    -- Split the full-stack key into individual frame labels (leaf..root).
    -- Frame labels are chunkname:line / builtin names / TRACE[n]@... /
    -- INTERNAL / INTERNAL:POD — none contain ";", so the split is unambiguous.
    -- For depth=1 streams there is no ";" and this yields a single frame.
    local frames = {}
    for piece in label:gmatch("[^;]+") do frames[#frames + 1] = piece end
    if #frames == 0 then frames = { label } end

    -- Build the leaf-first location_id list for this sample.
    local location_ids = {}
    for fi = 1, #frames do
      local name, filename, line = parse_site(frames[fi])
      local fid = intern_function(name, filename)
      local lid = intern_location(fid, line)
      location_ids[fi] = lid
    end

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
    sample_msgs[#sample_msgs + 1] = encode_sample(location_ids, values)
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

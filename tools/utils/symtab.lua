-- Parser of LuaJIT's symtab binary stream.
-- The format spec can be found in <src/lj_memprof.h>.
--
-- Major portions taken verbatim or adapted from the LuaVela.
-- Copyright (C) 2015-2019 IPONWEB Ltd.

local bit = require "bit"

local avl = require "utils.avl"

local band = bit.band
local string_format = string.format

local LJS_MAGIC = "ljs"
local LJS_MIN_VERSION = 0x3
local LJS_CURRENT_VERSION = 0x7
local LJS_EPILOGUE_HEADER = 0x80
local LJS_SYMTYPE_MASK = 0x03

local SYMTAB_LFUNC = 0
local SYMTAB_CFUNC = 1
local SYMTAB_TRACE = 2
local SYMTAB_VM = 3

local M = {}

function M.loc(args)
  local loc = {
    addr = args.addr or 0,
    line = args.line or 0,
    traceno = args.traceno or 0,
  }

  return loc
end

-- Parse a single entry in a symtab: lfunc symbol.
function M.parse_sym_lfunc(reader, symtab)
  local sym_addr = reader:read_uleb128()
  local sym_chunk = reader:read_string()
  local sym_line = reader:read_uleb128()

  symtab.lfunc[sym_addr] = symtab.lfunc[sym_addr] or {}

  if sym_chunk:find('\n') and not symtab.alias[sym_chunk] then
    table.insert(symtab.alias, sym_chunk)
    symtab.alias[sym_chunk] = string_format(
      "function_alias_%d", #symtab.alias
    )
  end

  symtab.lfunc[sym_addr] = {
    source = sym_chunk,
    linedefined = sym_line,
  }
end

function M.parse_sym_trace(reader, symtab, version)
  local traceno = reader:read_uleb128()
  local sym_addr = reader:read_uleb128()
  local sym_line = reader:read_uleb128()
  local mcode, mcode_size = 0, 0

  if version >= 5 then
    -- Machine code range: lets a host frame inside JIT code name its trace.
    mcode = reader:read_uleb128()
    mcode_size = reader:read_uleb128()
    symtab.tracelist[#symtab.tracelist + 1] = {
      start = mcode,
      stop = mcode + mcode_size,
      traceno = traceno,
    }
    symtab.tracelist_dirty = true
  end

  symtab.trace[traceno] = {
    start = M.loc({ addr = sym_addr, line = sym_line }),
    mcode = mcode,
    size = mcode_size,
  }

  if version >= 6 then
    -- Intervals: the exit snapshot that covers each range of the machine
    -- code, with the function and line of the bytecode it was taken at.
    local niv = reader:read_uleb128()
    local iv = {}
    for k = 1, niv do
      local off = reader:read_uleb128()
      local addr = reader:read_uleb128()
      local line = reader:read_uleb128()
      iv[k] = { off = off, addr = addr, line = line }
    end
    symtab.trace[traceno].iv = iv
  end
end

-- Trace whose machine code contains addr, or nil. The list is sorted lazily:
-- trace records arrive both in the prologue and as stream events.
-- File name of a module path, used to qualify a symbol: lua51.dll, not the
-- full path.
function M.module_name(path)
  return (path:match("([^/\\]+)$")) or path
end

-- Module that contains addr, or nil. With version 7 data the image size is
-- known, so an address outside every image (JIT machine code, heap) does not
-- get attributed to the module before it; for older profiles the nearest
-- base below the address is used.
function M.module_of(symtab, addr)
  local best
  for _, m in ipairs(symtab.modules or {}) do
    if m.addr <= addr then
      if m.size and m.size > 0 then
        if addr < m.addr + m.size then
          return m
        end
      elseif not best or m.addr > best.addr then
        best = m
      end
    end
  end
  return best
end

-- Interval of the trace that covers addr: the last one whose offset is not
-- greater, the same mapping lj_trace_unwind() uses to pick the exit.
-- Returns the trace number and, when version 6 data is present, the interval.
function M.trace_iv_at(symtab, addr)
  local traceno = M.trace_at(symtab, addr)
  if not traceno then
    return nil
  end
  local tr = symtab.trace[traceno]
  local iv = tr and tr.iv
  if not iv or #iv == 0 then
    return traceno
  end
  local off = addr - tr.mcode
  local lo, hi, found = 1, #iv, nil
  while lo <= hi do
    local mid = math.floor((lo + hi) / 2)
    if iv[mid].off <= off then
      found = iv[mid]
      lo = mid + 1
    else
      hi = mid - 1
    end
  end
  return traceno, found
end

function M.trace_at(symtab, addr)
  local list = symtab.tracelist
  if not list or #list == 0 then return nil end
  if symtab.tracelist_dirty then
    table.sort(list, function(a, b) return a.start < b.start end)
    symtab.tracelist_dirty = false
  end
  local lo, hi, found = 1, #list, nil
  while lo <= hi do
    local mid = math.floor((lo + hi) / 2)
    if list[mid].start <= addr then
      found = list[mid]
      lo = mid + 1
    else
      hi = mid - 1
    end
  end
  if found and addr < found.stop then
    return found.traceno
  end
  return nil
end

-- Parse a single entry in a symtab: VM assembly function start address.
function M.parse_sym_vm(reader, symtab)
  symtab.vm[reader:read_uleb128()] = true
end

-- Parse a single entry in a symtab: .so library
function M.parse_sym_cfunc(reader, symtab, version)
  local addr = reader:read_uleb128()
  local name = reader:read_string()
  -- Size in bytes; the entry that records an image carries the image size,
  -- which tells whether an address belongs to that image (version 7+).
  local size = version >= 7 and reader:read_uleb128() or 0

  symtab.cfunc = avl.insert(symtab.cfunc, addr, {
    name = name,
    size = size,
  })
  -- A module base entry carries the path of the image: keep it, the
  -- resolver needs it to turn load addresses into link-time addresses.
  local lower = name:lower()
  if name:match("[/\\]") and
     (lower:match("%.exe$") or lower:match("%.dll$") or
      lower:match("%.so$") or lower:match("%.dylib$")) then
    symtab.modules[#symtab.modules + 1] =
      { addr = addr, path = name, size = size }
  end
end

local parsers = {
  [SYMTAB_LFUNC] = M.parse_sym_lfunc,
  [SYMTAB_TRACE] = M.parse_sym_trace,
  [SYMTAB_VM] = M.parse_sym_vm,
  [SYMTAB_CFUNC] = M.parse_sym_cfunc,
}

function M.parse(reader)
  local symtab = {
    lfunc = {},
    trace = {},
    alias = {},
    cfunc = nil,
    modules = {},
    vm = {},
    tracelist = {},
    tracelist_dirty = false,
  }
  local magic = reader:read_octets(3)
  local version = reader:read_octets(1)

  -- Dummy-consume reserved bytes.
  local _ = reader:read_octets(3)

  if magic ~= LJS_MAGIC then
    error("Bad LuaJIT symbol table format prologue: " .. tostring(magic))
  end

  local version_num = string.byte(version)
  symtab.version = version_num
  if version_num < LJS_MIN_VERSION or version_num > LJS_CURRENT_VERSION then
    error(string_format(
         "LuaJIT symbol table format version mismatch: "..
         "the tool supports %d..%d, but your data is %d",
         LJS_MIN_VERSION,
         LJS_CURRENT_VERSION,
         version_num
    ))

  end

  while not reader:eof() do
    local header = reader:read_octet()
    local is_final = band(header, LJS_EPILOGUE_HEADER) ~= 0

    if is_final then
      break
    end

    local sym_type = band(header, LJS_SYMTYPE_MASK)
    if parsers[sym_type] then
      parsers[sym_type](reader, symtab, version_num)
    end
  end

  return symtab
end

function M.id(loc)
  return string_format(
    "f%#xl%dt%dg", loc.addr, loc.line, loc.traceno
  )
end

local function demangle_trace(symtab, loc)
  local traceno = loc.traceno

  assert(traceno ~= 0, "Location is a trace")

  local trace_str = string_format("TRACE [%d]", traceno)
  local trace = symtab.trace[traceno]

  if trace then
    assert(trace.start.traceno == 0, "Trace start is not a trace")
    return trace_str.." started at "..M.demangle(symtab, trace.start)
  end
  return trace_str
end

-- Name of the prototype at `addr`, or nil. The prototype table is keyed by
-- the exact address, so no heuristic is involved.
function M.lfunc_name(symtab, addr, line)
  local entry = symtab.lfunc[addr]
  if not entry then
    return nil
  end
  return string_format("%s:%d", symtab.alias[entry.source] or entry.source, line)
end

-- Name of the C function stored at `addr`, or nil.
--
-- The symbol tables carry no sizes, so a nearest-symbol lookup names an
-- internal function after whatever unrelated symbol happens to precede it.
-- `entry` is the start of the function that contains `addr` (the sampler
-- reports the offset within it and 0 when it is unknown): the name is only
-- used when a symbol starts at exactly that address. Anything else stays
-- unnamed, the caller falls back to a raw address.
function M.cfunc_name(symtab, addr, entry)
  if not symtab.cfunc or entry == nil or entry == 0 then
    return nil
  end
  local key, value = avl.floor(symtab.cfunc, entry)
  if key == entry and entry <= addr then
    return value.name, key
  end
  return nil
end

function M.demangle(symtab, loc)
  if loc.traceno ~= 0 and loc.traceno ~= nil then
    return demangle_trace(symtab, loc)
  end

  local addr = loc.addr

  if addr == 0 then
    return "INTERNAL"
  end

  if symtab.lfunc[addr] then
    local source = symtab.lfunc[addr].source
    return string_format("%s:%d", symtab.alias[source] or source, loc.line)
  end

  local key, value = avl.floor(symtab.cfunc, addr)

  if key then
    return string_format("%s:%#x", value.name, key)
  end

  return string_format("CFUNC %#x", addr)
end

return M

-- Parser of LuaJIT's sysprof binary stream.
-- The format spec can be found in <src/lj_sysprof.h>.

local symtab = require "utils.symtab"
local vmdef = require "jit.vmdef"

local string_format = string.format

local LJP_MAGIC = "ljp"
local LJP_MIN_VERSION = 2
local LJP_CURRENT_VERSION = 4

local M = {}

local VMST = {
  INTERP = 0,
  LFUNC  = 1,
  FFUNC  = 2,
  CFUNC  = 3,
  GC     = 4,
  EXIT   = 5,
  RECORD = 6,
  OPT    = 7,
  ASM    = 8,
  TRACE  = 9,
  SYMTAB = 10,
}


local FRAME = {
  LFUNC  = 1,
  CFUNC  = 2,
  FFUNC  = 3,
  BOTTOM = 0x80
}


local STREAM_END = 0x80
local SYMTAB_LFUNC_EVENT = 10
local SYMTAB_CFUNC_EVENT = 11
local SYMTAB_TRACE_EVENT = 12

local function new_event()
  return {
    lua = {
      vmstate = 0,
      callchain = {},
      trace = {
        traceno = nil,
        addr = 0,
        line = 0,
      }
    },
    host = {
      callchain = {}
    }
  }
end

-- A frame that the symbol table does not describe keeps its raw address: a
-- guessed name is worse than none for a profiler report.
local function raw_name(addr)
  return string_format("%#x", addr)
end

local function parse_lfunc(reader, symbols)
  local addr = reader:read_uleb128()
  local line = reader:read_uleb128()
  local name = symtab.lfunc_name(symbols, addr, line) or raw_name(addr)
  return { name = name, addr = addr }
end

local function parse_ffunc(reader, _)
  local ffid = reader:read_uleb128()
  return { name = vmdef.ffnames[ffid] }
end

local function parse_cfunc(reader, symbols, resolver)
  local addr = reader:read_uleb128()
  -- The entry address lets the frame be matched to the host callchain, and
  -- it is the exact start of the function, so the name is never guessed.
  local name = resolver and resolver:resolve(addr, addr)
  if not name then
    name = symtab.cfunc_name(symbols, addr, addr) or raw_name(addr)
  end
  return { name = name, addr = addr }
end

local frame_parsers = {
  [FRAME.LFUNC] = parse_lfunc,
  [FRAME.FFUNC] = parse_ffunc,
  [FRAME.CFUNC] = parse_cfunc
}

local function parse_lua_callchain(reader, event, symbols, resolver)
  while true do
    local frame_header = reader:read_octet()
    if frame_header == FRAME.BOTTOM then
      break
    end
    local frame = frame_parsers[frame_header](reader, symbols, resolver)
    frame.type = frame_header
    -- Frames are stored in the order they are streamed: innermost first.
    event.lua.callchain[#event.lua.callchain + 1] = frame
  end
end

--~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~--

local function parse_host_callchain(reader, event, symbols, version, resolver)
  local addr = reader:read_uleb128()

  while addr ~= 0 do
    local entry = 0
    if version >= 3 then
      -- Offset within the containing function, plus one; zero if unknown.
      local off = reader:read_uleb128()
      if off ~= 0 then
        entry = addr - (off - 1)
      end
    end
    -- `entry` is the start of the function that contains the frame: the name
    -- is accepted only when a symbol starts there (see symtab.cfunc_name).
    local name = resolver and resolver:resolve(addr, entry)
    if not name then
      -- JIT compiled code: the machine code range names the trace.
      local traceno = symtab.trace_at(symbols, addr)
      if traceno then
        name = string_format("TRACE [%d]", traceno)
      else
        name = symtab.cfunc_name(symbols, addr, entry) or raw_name(addr)
      end
    end
    event.host.callchain[#event.host.callchain + 1] = {
      name = name,
      addr = addr,
      entry = entry,
    }
    addr = reader:read_uleb128()
  end
end

--~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~--

local function parse_trace_callchain(reader, event, symbols, version, resolver)
  local loc = {}
  loc.traceno = reader:read_uleb128()
  loc.addr = reader:read_uleb128()
  loc.line = reader:read_uleb128()

  local name_lua = symtab.demangle(symbols, {
    addr = loc.addr,
    traceno = loc.traceno,
  })
  event.lua.trace = loc
  event.lua.trace.name = name_lua
  if version >= 4 then
    -- Native callers of the trace, innermost first.
    parse_host_callchain(reader, event, symbols, version, resolver)
  end
end

--~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~--

local function parse_host_only(reader, event, symbols, version, resolver)
  parse_host_callchain(reader, event, symbols, version, resolver)
end

local function parse_lua_host(reader, event, symbols, version, resolver)
  parse_lua_callchain(reader, event, symbols, resolver)
  parse_host_callchain(reader, event, symbols, version, resolver)
end

local function parse_trace(reader, event, symbols, version, resolver)
  parse_trace_callchain(reader, event, symbols, version, resolver)
  -- parse_lua_callchain(reader, event)
end

local function parse_symtab(reader, symbols, vmstate, version)
  if vmstate == SYMTAB_LFUNC_EVENT then
    symtab.parse_sym_lfunc(reader, symbols)
  elseif vmstate == SYMTAB_CFUNC_EVENT then
    symtab.parse_sym_cfunc(reader, symbols)
  elseif vmstate == SYMTAB_TRACE_EVENT then
    -- The trace record layout follows the symbol table version.
    symtab.parse_sym_trace(reader, symbols, symbols.version)
  else
    error("Unknown symtab event")
  end
end

local event_parsers = {
  [VMST.INTERP] = parse_host_only,
  [VMST.LFUNC]  = parse_lua_host,
  [VMST.FFUNC]  = parse_lua_host,
  [VMST.CFUNC]  = parse_lua_host,
  [VMST.GC]     = parse_host_only,
  [VMST.EXIT]   = parse_host_only,
  [VMST.RECORD] = parse_host_only,
  [VMST.OPT]    = parse_host_only,
  [VMST.ASM]    = parse_host_only,
  [VMST.TRACE]  = parse_trace,
}


local function is_vm_frame(symbols, entry)
  return entry ~= 0 and symbols.vm[entry] == true
end

local function merge(event, symbols)
  local host = event.host.callchain  -- innermost frame first
  local lua = event.lua.callchain    -- innermost frame first
  local chain = {}
  local hi = 1

  for i = 1, #lua do
    local fr = lua[i]
    local spliced = false

    if fr.type == FRAME.CFUNC and fr.addr ~= nil and fr.addr ~= 0 then
      -- A C function of the Lua stack is located in the host callchain by
      -- its entry address, which every host frame carries (the 'func-off'
      -- field of the stream). No symbol names are involved, so the merge
      -- also works for profiles collected from stripped binaries.
      local j = hi
      while j <= #host and host[j].entry ~= fr.addr do
        j = j + 1
      end
      if j <= #host then
        -- The host frames above this C function are its callees.
        for k = hi, j do chain[#chain + 1] = host[k].name end
        hi = j + 1
        while hi <= #host and is_vm_frame(symbols, host[hi].entry) do
          hi = hi + 1
        end
        spliced = true
      end
    elseif fr.type == FRAME.FFUNC then
      -- A fast function has no C entry point: its implementation frames are
      -- the host frames above the VM handler that invoked it.
      while hi <= #host and not is_vm_frame(symbols, host[hi].entry) do
        chain[#chain + 1] = host[hi].name
        hi = hi + 1
      end
      while hi <= #host and is_vm_frame(symbols, host[hi].entry) do
        hi = hi + 1
      end
      chain[#chain + 1] = fr.name
      spliced = true
    end

    if not spliced then
      chain[#chain + 1] = fr.name
    end
  end
  -- A trace sample carries no Lua chain: the innermost Lua-side frame is the
  -- trace itself, and the host frames are the code it called (deeper) and its
  -- callers (shallower). Replace the frame that belongs to the trace machine
  -- code range with the trace name; if the sampler already abandoned it,
  -- insert the name in front of the host frames.
  local trace = event.lua.trace
  if trace and trace.name and trace.traceno and trace.traceno ~= 0 then
    local t = symbols.trace[trace.traceno]
    local mcode = t and t.mcode or 0
    local msize = t and t.size or 0
    local inserted = false

    while hi <= #host do
      local fr = host[hi]
      if not inserted and mcode ~= 0 and fr.addr >= mcode and
         fr.addr < mcode + msize then
        chain[#chain + 1] = trace.name
        inserted = true
      elseif not is_vm_frame(symbols, fr.entry) then
        chain[#chain + 1] = fr.name
      end
      hi = hi + 1
    end
    if not inserted then
      table.insert(chain, 1, trace.name)
    end
  end

  while hi <= #host do
    if not is_vm_frame(symbols, host[hi].entry) then
      chain[#chain + 1] = host[hi].name
    end
    hi = hi + 1
  end

  -- Frames were read innermost first, the tools print outermost first.
  local rev = {}
  for k = #chain, 1, -1 do rev[#rev + 1] = chain[k] end
  return table.concat(rev, ';') .. ';'
end

local function parse_event(reader, events, symbols, version, resolver)
  local event = new_event()

  local vmstate = reader:read_octet()
  if vmstate == STREAM_END then
    -- TODO: samples & overruns
    return false
  elseif SYMTAB_LFUNC_EVENT <= vmstate and vmstate <= SYMTAB_TRACE_EVENT then
    parse_symtab(reader, symbols, vmstate, version)
    return true
  end

  assert(0 <= vmstate and vmstate <= 9, "Vmstate "..vmstate.." is not valid")
  event.lua.vmstate = vmstate

  event_parsers[vmstate](reader, event, symbols, version, resolver)
  local callchain = merge(event, symbols)
  events[callchain] = (events[callchain] or 0) + 1
  return true
end

function M.parse(reader, symbols, resolver)
  local events = {}

  local magic = reader:read_octets(3)
  local version = string.byte(reader:read_octets(1))
  -- Dummy-consume reserved bytes.
  local _ = reader:read_octets(3)

  if magic ~= LJP_MAGIC then
    error("Bad sysprof event format prologue: " .. tostring(magic))
  end

  if version < LJP_MIN_VERSION or version > LJP_CURRENT_VERSION then
    error(string_format(
      "Sysprof event format version mismatch:"..
      " the tool supports %d..%d, but your data is %d",
      LJP_MIN_VERSION,
      LJP_CURRENT_VERSION,
      version
    ))
  end

  while parse_event(reader, events, symbols, version, resolver) do
    -- Empty body.
  end

  return events
end

return M

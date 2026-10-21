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

-- The text form of a frame record, used by the default report.
local function frame_text(fr)
  if fr.kind == "lua" or fr.kind == "jit" then
    if not fr.chunk then
      return string_format("%#x", fr.addr)
    end
    return string_format("%s:%d", fr.chunk, fr.line)
  elseif fr.kind == "trace" then
    if fr.chunk then
      return string_format("%s:%d [TRACE %d]", fr.chunk, fr.line, fr.traceno)
    end
    return string_format("TRACE [%d]", fr.traceno)
  elseif fr.kind == "vm" then
    return fr.name
  end
  -- host and cfunc frames: module!function, module+offset, or a raw address.
  if fr.sym then
    return fr.module and string_format("%s!%s", fr.module, fr.sym) or fr.sym
  end
  if fr.module then
    return string_format("%s+%#x", fr.module, fr.off)
  end
  return string_format("%#x", fr.addr)
end

-- A host or C function frame of `addr`.
local function addr_frame(kind, symbols, addr, entry, name)
  local m = symtab.module_of(symbols, addr)
  if not m then
    return { kind = kind, addr = addr, entry = entry }
  end
  local fr = { kind = kind, addr = addr, entry = entry,
               module = symtab.module_name(m.path) }
  if name then
    fr.sym = name
  else
    fr.off = addr - m.addr
  end
  return fr
end

-- A Lua position as a frame: the chunk of the prototype and the line.
local function parse_lfunc_loc(symbols, addr, line)
  local entry = symbols.lfunc[addr]
  if not entry then
    return nil
  end
  return {
    kind = "lua",
    addr = addr,
    chunk = symbols.alias[entry.source] or entry.source,
    line = line,
  }
end

local function parse_lfunc(reader, symbols)
  local addr = reader:read_uleb128()
  local line = reader:read_uleb128()
  return parse_lfunc_loc(symbols, addr, line) or
         { kind = "lua", addr = addr, chunk = string_format("%#x", addr),
           line = line }
end

local function parse_ffunc(reader, _)
  local ffid = reader:read_uleb128()
  return { kind = "vm", name = vmdef.ffnames[ffid] }
end

local function parse_cfunc(reader, symbols, resolver)
  local addr = reader:read_uleb128()
  -- The entry address lets the frame be matched to the host callchain, and
  -- it is the exact start of the function, so the name is never guessed.
  local name
  if resolver then
    local _, resolved = resolver:resolve(addr, addr)
    name = resolved
  end
  if not name then
    name = symtab.cfunc_name(symbols, addr, addr)
  end
  return addr_frame("cfunc", symbols, addr, addr, name)
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
    local name
    if resolver then
      local _, resolved = resolver:resolve(addr, entry)
      name = resolved
    end
    local frame
    if not name then
      -- JIT compiled code: the interval that covers the address names the
      -- function and line it is at; without intervals fall back to the trace.
      local traceno, iv = symtab.trace_iv_at(symbols, addr)
      if iv then
        local proto = symbols.lfunc[iv.addr]
        frame = {
          kind = "jit",
          addr = addr,
          entry = entry,
          chunk = proto and (symbols.alias[proto.source] or proto.source),
          line = iv.line,
        }
      elseif traceno then
        frame = { kind = "trace", traceno = traceno, addr = addr }
      else
        name = symtab.cfunc_name(symbols, addr, entry)
      end
    end
    if not frame then
      -- A named frame, or the module with the offset inside it: a bare
      -- number never appears when the image is known.
      frame = addr_frame("host", symbols, addr, entry, name)
    end
    event.host.callchain[#event.host.callchain + 1] = frame
    addr = reader:read_uleb128()
  end
end

--~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~--

local function parse_trace_callchain(reader, event, symbols, version, resolver)
  local loc = {}
  loc.traceno = reader:read_uleb128()
  loc.addr = reader:read_uleb128()
  loc.line = reader:read_uleb128()

  -- The stream carries the function and line of the sampled position (the
  -- exit snapshot that covers it); without it only the trace number is known.
  event.lua.trace = loc
  local pos = parse_lfunc_loc(symbols, loc.addr, loc.line)
  if pos then
    loc.chunk = pos.chunk
  end
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
    -- The cfunc record carries a size from symbol table version 7 on.
    symtab.parse_sym_cfunc(reader, symbols, symbols.version)
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
        for k = hi, j do chain[#chain + 1] = host[k] end
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
        chain[#chain + 1] = host[hi]
        hi = hi + 1
      end
      while hi <= #host and is_vm_frame(symbols, host[hi].entry) do
        hi = hi + 1
      end
      chain[#chain + 1] = fr
      spliced = true
    end

    if not spliced then
      chain[#chain + 1] = fr
    end
  end
  -- A trace sample carries no Lua chain: the innermost Lua-side frame is the
  -- trace itself, and the host frames are the code it called (deeper) and its
  -- callers (shallower). Replace the frame that belongs to the trace machine
  -- code range with the trace name; if the sampler already abandoned it,
  -- insert the name in front of the host frames.
  local trace = event.lua.trace
  if trace and trace.traceno and trace.traceno ~= 0 then
    local t = symbols.trace[trace.traceno]
    local mcode = t and t.mcode or 0
    local msize = t and t.size or 0
    local trace_frame = {
      kind = "trace",
      traceno = trace.traceno,
      chunk = trace.chunk,
      line = trace.chunk and trace.line or nil,
    }
    local inserted = false

    while hi <= #host do
      local fr = host[hi]
      if not inserted and mcode ~= 0 and fr.addr >= mcode and
         fr.addr < mcode + msize then
        chain[#chain + 1] = trace_frame
        inserted = true
      elseif not is_vm_frame(symbols, fr.entry) then
        chain[#chain + 1] = fr
      end
      hi = hi + 1
    end
    if not inserted then
      table.insert(chain, 1, trace_frame)
    end
  end

  while hi <= #host do
    if not is_vm_frame(symbols, host[hi].entry) then
      chain[#chain + 1] = host[hi]
    end
    hi = hi + 1
  end

  -- Frames were read innermost first, the report shows them outermost first.
  local rev = {}
  for k = #chain, 1, -1 do rev[#rev + 1] = chain[k] end
  return rev
end

-- The chain as one string: also the key the samples are aggregated by.
local function frames_text(frames)
  local text = {}
  for k = 1, #frames do text[k] = frame_text(frames[k]) end
  return table.concat(text, ';') .. ';'
end

local JSON_ESCAPES = {
  ['"'] = '\\"', ['\\'] = '\\\\', ['\n'] = '\\n', ['\r'] = '\\r',
  ['\t'] = '\\t',
}

local function json_string(value)
  return '"' .. tostring(value):gsub('[%z\1-\31"\\]', function(c)
    return JSON_ESCAPES[c] or string_format("\\u%04x", string.byte(c))
  end) .. '"'
end

-- Machine readable form of one frame.
local function json_frame(fr)
  local out = { '{"kind":"' .. fr.kind .. '"' }
  local function str(key, value)
    out[#out + 1] = string_format(',"%s":%s', key, json_string(value))
  end
  local function num(key, value)
    out[#out + 1] = string_format(',"%s":%d', key, value)
  end
  if fr.module then str("module", fr.module) end
  if fr.sym then str("symbol", fr.sym) end
  if fr.name then str("name", fr.name) end
  if fr.chunk then str("chunk", fr.chunk) end
  if fr.line and fr.kind == "lua" then num("line", fr.line) end
  if fr.traceno and fr.traceno ~= 0 then num("traceno", fr.traceno) end
  if fr.kind == "trace" and fr.line then num("line", fr.line) end
  if fr.off then num("offset", fr.off) end
  if fr.addr and fr.addr ~= 0 then str("addr", string_format("%#x", fr.addr)) end
  if fr.entry and fr.entry ~= 0 then str("entry", string_format("%#x", fr.entry)) end
  out[#out + 1] = '}'
  return table.concat(out)
end

-- One aggregated chain as a JSON object: the frames keep their fields, so a
-- consumer does not have to split the text form.
function M.event_json(entry)
  local frames = {}
  for k = 1, #entry.frames do frames[k] = json_frame(entry.frames[k]) end
  return string_format('{"count":%d,"frames":[%s]}', entry.count,
                       table.concat(frames, ","))
end

-- The text form of an aggregated chain, as printed by the default report.
function M.event_text(entry)
  return entry.chain
end

function M.frames_text(frames)
  return frames_text(frames)
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
  local frames = merge(event, symbols)
  local chain = frames_text(frames)
  local entry = events[chain]
  if not entry then
    entry = { chain = chain, frames = frames, count = 0 }
    events[chain] = entry
  end
  entry.count = entry.count + 1
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

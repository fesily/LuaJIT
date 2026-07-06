------------------------------------------------------------------------------
-- Memory profiler event-stream parser.
--
-- Reads the v1 binary event stream produced by memprof.start{mode="event",
-- out=...} and turns it into a list of typed event tables plus a symtab map.
--
-- Wire format (authoritative source: src/lj_memprof.c):
--   * Prologue: 5 bytes  "ljm" 0x01 0x00(reserved)
--   * Event header: 1 byte  (opcode << 4) | (src_kind & 0xf)
--     opcodes: EPILOGUE=0 ALLOC=1 REALLOC=2 FREE=3 PODFREE=4
--              SYMTAB_LFUNC=5 SYMTAB_TRACE=6 SYMTAB_CFUNC=7
--     src_kind: INT=0 LFUNC=1 CFUNC=2 TRACE=3
--   * Epilogue byte: 0x80  (top nibble 8)
--   * ULEB128: little-endian base-128, high bit = continuation
--   * ALLOC:    uleb(addr) uleb(size) byte(gct) byte(cls) byte(gcstate) uleb(src_id)
--   * REALLOC:  uleb(addr) uleb(osize) uleb(nsize) uleb(src_id)
--   * FREE:     uleb(addr) uleb(osize) byte(gct) uleb(src_id)
--   * PODFREE:  uleb(cellcount) uleb(bytes)
--   * SYMTAB_LFUNC:  uleb(proto_ptr) uleb(namelen) <namelen bytes> uleb(firstline)
--   * SYMTAB_TRACE:  uleb(traceno) uleb(proto_ptr) uleb(firstline)
--   * Ordering: prologue, event body, SYMTAB_* records (appended at stop),
--     then 0x80 epilogue. Site names arrive AFTER the events that reference
--     them, so callers must buffer events and resolve src_ids in a second pass.
--
-- Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h.
------------------------------------------------------------------------------

local M = {}

-- LuaJIT bitwise ops live in the `bit` library (Lua 5.1 lexical level has no
-- &/<< operators). Cache the ones we need.
local band, bor, lshift, rshift = bit.band, bit.bor, bit.lshift, bit.rshift

-- Opcode / src-kind constants (mirror src/lj_memprof.c enums).
local OP = {
  EPILOGUE = 0, ALLOC = 1, REALLOC = 2, FREE = 3, PODFREE = 4,
  SYMTAB_LFUNC = 5, SYMTAB_TRACE = 6, SYMTAB_CFUNC = 7,
}
local SRC = { INT = 0, LFUNC = 1, CFUNC = 2, TRACE = 3 }
local EPILOGUE_BYTE = 0x80
local PROLOGUE_LEN = 5
local PROLOGUE_MAGIC = "ljm"
local STREAM_VERSION = 1

local SRC_NAME = { [0] = "INT", "LFUNC", "CFUNC", "TRACE" }

-- ULEB128 cap: 10 bytes covers a 64-bit value with room to spare; anything
-- longer is a corrupt stream.
local ULEB128_MAXBYTES = 10

-- A small custom error so callers can distinguish parse failures.
local function err(msg, ofs)
  return error(("memprof parse: %s (at offset %d)"):format(msg, ofs or -1))
end

-- Read a ULEB128 value from string `s` starting at byte position `pos`
-- (1-based). Returns value, new position. Raises on truncation / overlong
-- encodings / overflow, matching the defensive-reader contract.
local function read_uleb128(s, pos, e)
  local v = 0
  local shift = 0
  local n = 0
  local b, lo
  while true do
    if pos > e then err("truncated ULEB128", pos) end
    b = s:byte(pos)
    pos = pos + 1
    n = n + 1
    if n > ULEB128_MAXBYTES then err("over-long ULEB128", pos) end
    lo = band(b, 0x7f)
    v = v + lshift(lo, shift)
    if band(b, 0x80) == 0 then break end
    shift = shift + 7
  end
  -- The only non-canonical ULEB128 shape the C writer cannot produce is a
  -- trailing zero byte after a non-empty continuation (e.g. 0x01 0x00 for the
  -- value 1). Reject it so a corrupt stream cannot hide data in padding.
  if n > 1 and lo == 0 then err("non-canonical ULEB128 (trailing zero)", pos) end
  return v, pos
end
M.read_uleb128 = read_uleb128

-- Parse the full stream. Returns a table:
--   { version=, events={...}, symtab={lfunc={...}, trace={...}}, cfunc_ids={...} }
-- Each event is a table with .op (string), .src (string), and opcode-specific
-- numeric fields. SYMTAB_* records populate the symtab maps and are NOT
-- returned as events.
function M.parse(data)
  if type(data) ~= "string" then err("expected string input") end
  local e = #data
  if e < PROLOGUE_LEN then err("stream shorter than prologue") end

  -- Prologue.
  if data:sub(1, 3) ~= PROLOGUE_MAGIC then err("bad prologue magic") end
  local version = data:byte(4)
  if version ~= STREAM_VERSION then
    err(("unsupported stream version %d"):format(version))
  end
  -- data:byte(5) is the reserved byte; we read but do not validate it.

  local events = {}
  local symtab = {
    lfunc = {},  -- proto_ptr -> {chunkname=, firstline=}
    trace = {},  -- traceno   -> {proto_ptr=, firstline=}
  }
  local cfunc_ids = {}  -- ordered list of CFUNC src_ids seen (for C:<addr> render)

  local pos = PROLOGUE_LEN + 1
  while pos <= e do
    local hdrOfs = pos
    local hdr = data:byte(pos)
    pos = pos + 1
    if hdr == EPILOGUE_BYTE then
      -- Stream end. Trailing bytes after the epilogue are not expected; the
      -- C emitter writes nothing after 0x80. Ignore them silently rather than
      -- erroring so a partially-flushed tail cannot poison a good run.
      break
    end
    local op = rshift(hdr, 4)
    local sk = band(hdr, 0xf)
    local opname, srcname
    -- Map opcode nibble to name. Unknown opcode -> stop (mirror the C
    -- symtab scanner's `goto done` on unknown opcode). This is the documented
    -- behavior: a clean halt, not an error, so a truncated-but-valid prefix
    -- still parses.
    if op == OP.ALLOC then opname = "ALLOC"
    elseif op == OP.REALLOC then opname = "REALLOC"
    elseif op == OP.FREE then opname = "FREE"
    elseif op == OP.PODFREE then opname = "PODFREE"
    elseif op == OP.SYMTAB_LFUNC then opname = "SYMTAB_LFUNC"
    elseif op == OP.SYMTAB_TRACE then opname = "SYMTAB_TRACE"
    elseif op == OP.SYMTAB_CFUNC then opname = "SYMTAB_CFUNC"
    else
      -- Unknown opcode: stop parsing the event body. The C reader does the
      -- same (memprof_dump_symtab `goto done`). Any bytes we did not consume
      -- are left for the caller; we do not error.
      break
    end
    srcname = SRC_NAME[sk] or ("SRC?"..sk)

    if opname == "ALLOC" then
      local addr, size
      addr, pos = read_uleb128(data, pos, e)
      size, pos = read_uleb128(data, pos, e)
      if pos + 2 > e then err("truncated ALLOC gct/cls", pos) end
      local gct = data:byte(pos); pos = pos + 1
      local cls = data:byte(pos); pos = pos + 1
      if pos > e then err("truncated ALLOC gcstate", pos) end
      local gcstate = data:byte(pos); pos = pos + 1
      local src_id
      src_id, pos = read_uleb128(data, pos, e)
      events[#events+1] = {
        op = opname, src = srcname, ofs = hdrOfs,
        addr = addr, size = size, gct = gct, cls = cls,
        gcstate = gcstate, src_id = src_id,
      }
      if sk == SRC.CFUNC then
        -- Record CFUNC src_ids for `C:0x<addr>` rendering. Dedup is the
        -- aggregator's job; here we just note every distinct pointer.
        if not cfunc_ids[addr] and not cfunc_ids[src_id] then
          -- Use src_id (the function pointer) as the CFUNC identity, per
          -- lj_memprof.c memprof_attribution: src_id = (uintptr_t)fn.
          cfunc_ids[src_id] = true
        end
      end

    elseif opname == "REALLOC" then
      local addr, osize, nsize, src_id
      addr, pos = read_uleb128(data, pos, e)
      osize, pos = read_uleb128(data, pos, e)
      nsize, pos = read_uleb128(data, pos, e)
      src_id, pos = read_uleb128(data, pos, e)
      events[#events+1] = {
        op = opname, src = srcname, ofs = hdrOfs,
        addr = addr, osize = osize, nsize = nsize, src_id = src_id,
      }

    elseif opname == "FREE" then
      local addr, osize, gct, src_id
      addr, pos = read_uleb128(data, pos, e)
      osize, pos = read_uleb128(data, pos, e)
      if pos > e then err("truncated FREE gct", pos) end
      gct = data:byte(pos); pos = pos + 1
      src_id, pos = read_uleb128(data, pos, e)
      events[#events+1] = {
        op = opname, src = srcname, ofs = hdrOfs,
        addr = addr, osize = osize, gct = gct, src_id = src_id,
      }

    elseif opname == "PODFREE" then
      local cellcount, bytes
      cellcount, pos = read_uleb128(data, pos, e)
      bytes, pos = read_uleb128(data, pos, e)
      events[#events+1] = {
        op = opname, src = srcname, ofs = hdrOfs,
        cellcount = cellcount, bytes = bytes,
      }

    elseif opname == "SYMTAB_LFUNC" then
      local proto_ptr, namelen, firstline
      proto_ptr, pos = read_uleb128(data, pos, e)
      namelen, pos = read_uleb128(data, pos, e)
      if pos + namelen - 1 > e then err("truncated SYMTAB_LFUNC chunkname", pos) end
      local chunkname = data:sub(pos, pos + namelen - 1)
      pos = pos + namelen
      firstline, pos = read_uleb128(data, pos, e)
      symtab.lfunc[proto_ptr] = { chunkname = chunkname, firstline = firstline }

    elseif opname == "SYMTAB_TRACE" then
      local traceno, proto_ptr, firstline
      traceno, pos = read_uleb128(data, pos, e)
      proto_ptr, pos = read_uleb128(data, pos, e)
      firstline, pos = read_uleb128(data, pos, e)
      symtab.trace[traceno] = { proto_ptr = proto_ptr, firstline = firstline }

    elseif opname == "SYMTAB_CFUNC" then
      -- v1 does not emit SYMTAB_CFUNC records (CFUNC src_ids render as
      -- C:0x<addr> with no file:line). Parse defensively in case a future
      -- emitter adds one: uleb(fn_ptr) uleb(namelen) <name>.
      local fn_ptr, namelen
      fn_ptr, pos = read_uleb128(data, pos, e)
      namelen, pos = read_uleb128(data, pos, e)
      if pos + namelen - 1 > e then err("truncated SYMTAB_CFUNC name", pos) end
      local name = data:sub(pos, pos + namelen - 1)
      pos = pos + namelen
      symtab.cfunc = symtab.cfunc or {}
      symtab.cfunc[fn_ptr] = { name = name }
    end
  end

  return {
    version = version,
    events = events,
    symtab = symtab,
    cfunc_ids = cfunc_ids,
  }
end

-- Render a site identity for an event's (src, src_id). Returns a string usable
-- as both a display label and a hash key.
--   LFUNC  -> chunkname:firstline   (chunkname keeps its leading '@')
--   TRACE  -> TRACE[n]              (n = traceno)
--   CFUNC  -> C:0x<hex>             (v1 has no CFUNC symtab)
--   INT    -> INTERNAL
-- If a LFUNC src_id has no matching symtab record (e.g. symtab overflow at
-- 4096 protos), fall back to L:0x<hex> so the site is still distinguishable.
function M.site_label(src, src_id, symtab)
  if src == "LFUNC" then
    local info = symtab.lfunc[src_id]
    if info then
      return ("%s:%d"):format(info.chunkname, info.firstline)
    end
    return ("L:0x%x"):format(src_id)
  elseif src == "TRACE" then
    -- src_id is the trace number.
    local info = symtab.trace[src_id]
    if info then
      local pinfo = symtab.lfunc[info.proto_ptr]
      if pinfo then
        return ("TRACE[%d]@%s:%d"):format(src_id, pinfo.chunkname, info.firstline)
      end
    end
    return ("TRACE[%d]"):format(src_id)
  elseif src == "CFUNC" then
    return ("C:0x%x"):format(src_id)
  elseif src == "INT" then
    return "INTERNAL"
  end
  return ("?%s"):format(src)
end

-- Map the ALLOC `cls` byte (arena-class axis, 0xff = raw allocf buffer) to a
-- short human-readable type tag. The C emitter uses the ArenaClass enum or
-- 0xff for raw allocf memory (see lj_memprof_emit_realloc ALLOC branch).
local CLS_NAME = {
  [0] = "nontrav", "trav", "pod", "udata", "cdatav",
  [0xff] = "rawbuf",
}
function M.cls_name(cls)
  return CLS_NAME[cls] or ("cls%d"):format(cls)
end

-- Map a FREE record's gct (~LJ_Txxx tag) to a short type tag. The tag values
-- follow lj_obj.h: ~LJ_TSTR=4 .. ~LJ_TUDATA=12. Anything outside is reported
-- numerically.
local GCT_NAME = {
  [4] = "string", [5] = "upval", [6] = "thread", [7] = "proto",
  [8] = "func", [9] = "trace", [10] = "cdata", [11] = "table",
  [12] = "udata",
}
function M.gct_name(gct)
  return GCT_NAME[gct] or ("gct%d"):format(gct)
end

M.OP = OP
M.SRC = SRC
M.PROLOGUE_LEN = PROLOGUE_LEN
M.STREAM_VERSION = STREAM_VERSION

return M

-- memprof_event_assert.lua: v1 event-stream profiler assertions.
-- start event mode, perform KNOWN allocations (N strings, M tables),
-- stop, parse the emitted binary stream, assert it contains >=N string
-- ALLOCs (cls=NonTrav=0), >=M table ALLOCs (cls=Trav=1), plausible sizes,
-- non-empty source attribution, and FREE records after a forced GC.
--
-- Run: ./src/luajit -joff test/gc/memprof_event_assert.lua

local memprof = require("memprof")
local collectgarbage = collectgarbage

local N_STRINGS = 200
local M_TABLES  = 100

local failures = 0
local checks = 0
local function check(cond, msg)
  checks = checks + 1
  if not cond then
    failures = failures + 1
    io.write("  FAIL: ", msg, "\n")
  end
end

-- -- Binary stream parser (matches src/lj_memprof.c wire format) ----------

local function read_uleb128(data, pos)
  local v = 0
  local s = 0
  while true do
    local b = data:byte(pos)
    pos = pos + 1
    v = v + (b % 128) * (2 ^ s)  -- bit lshift via math (b & 0x7f) << s
    if b < 128 then break end
    s = s + 7
  end
  return v, pos
end

local OP = {
  [0] = "EPILOGUE",
  [1] = "ALLOC",
  [2] = "REALLOC",
  [3] = "FREE",
  [4] = "PODFREE",
  [5] = "SYMTAB_LFUNC",
  [6] = "SYMTAB_TRACE",
  [7] = "SYMTAB_CFUNC",
}

local SRC = { [0]="INT", [1]="LFUNC", [2]="CFUNC", [3]="TRACE" }

local function parse_stream(data)
  local pos = 1
  local magic = data:sub(pos, pos+2)
  pos = pos + 3
  check(magic == "ljm", "prologue magic 'ljm' (got '" .. magic .. "')")
  local version = data:byte(pos); pos = pos + 1
  check(version == 1 or version == 2,
        "stream version 1 or 2 (got " .. tostring(version) .. ")")
  local has_cycle = (version >= 2)
  pos = pos + 1  -- reserved

  local events = {}
  local symtab = { lfunc = {}, trace = {}, cfunc = {} }
  local len = #data
  while pos <= len do
    local hdr = data:byte(pos); pos = pos + 1
    if hdr == 0x80 then
      check(true, "epilogue byte 0x80 present")
      break
    end
    local op = math.floor(hdr / 16)
    local sk = hdr % 16
    local ev = { op = OP[op] or ("OP"..tostring(op)), opn = op,
                 src = SRC[sk] or ("S"..tostring(sk)) }
    if op == 1 then -- ALLOC
      ev.addr, pos = read_uleb128(data, pos)
      ev.size, pos = read_uleb128(data, pos)
      ev.gct = data:byte(pos); pos = pos + 1
      ev.cls = data:byte(pos); pos = pos + 1
      ev.gcstate = data:byte(pos); pos = pos + 1
      ev.src_id, pos = read_uleb128(data, pos)
      if has_cycle then ev.gc_cycle, pos = read_uleb128(data, pos) end
    elseif op == 2 then -- REALLOC
      ev.addr, pos = read_uleb128(data, pos)
      ev.osize, pos = read_uleb128(data, pos)
      ev.nsize, pos = read_uleb128(data, pos)
      ev.src_id, pos = read_uleb128(data, pos)
      if has_cycle then ev.gc_cycle, pos = read_uleb128(data, pos) end
    elseif op == 3 then -- FREE
      ev.addr, pos = read_uleb128(data, pos)
      ev.osize, pos = read_uleb128(data, pos)
      ev.gct = data:byte(pos); pos = pos + 1
      ev.src_id, pos = read_uleb128(data, pos)
      if has_cycle then ev.gc_cycle, pos = read_uleb128(data, pos) end
    elseif op == 4 then -- PODFREE
      ev.cellcount, pos = read_uleb128(data, pos)
      ev.bytes, pos = read_uleb128(data, pos)
    elseif op == 5 then -- SYMTAB_LFUNC
      ev.proto, pos = read_uleb128(data, pos)
      local clen; clen, pos = read_uleb128(data, pos)
      ev.chunkname = data:sub(pos, pos + clen - 1); pos = pos + clen
      ev.firstline, pos = read_uleb128(data, pos)
      symtab.lfunc[ev.proto] = { chunkname = ev.chunkname,
                                 firstline = ev.firstline }
    elseif op == 6 then -- SYMTAB_TRACE
      ev.trace_id, pos = read_uleb128(data, pos)
      ev.proto, pos = read_uleb128(data, pos)
      ev.startline, pos = read_uleb128(data, pos)
      symtab.trace[ev.trace_id] = { proto = ev.proto,
                                    startline = ev.startline }
    elseif op == 7 then -- SYMTAB_CFUNC
      ev.func, pos = read_uleb128(data, pos)
      symtab.cfunc[ev.func] = true
    else
      break -- unknown
    end
    events[#events+1] = ev
  end
  return events, symtab
end

-- -- Test scenario --------------------------------------------------------

local outpath = "/tmp/memprof_event_stream.bin"

local ok, err = pcall(function()
  memprof.start{ mode = "event", depth = 1, out = outpath }
end)
check(ok, "memprof.start succeeded (" .. tostring(err) .. ")")
if not ok then
  io.write("memprof_event_assert: ", checks, " checks, ", failures, " failures\n")
  os.exit(failures == 0 and 0 or 1)
end

-- Keep references alive so they appear in the live set until we drop them.
local strs = {}
local tabs = {}
for i = 1, N_STRINGS do
  strs[i] = "memprof_test_string_" .. tostring(i) .. "_x" .. string.rep("a", i % 17)
end
for i = 1, M_TABLES do
  tabs[i] = { k = i, v = "val" .. tostring(i) }
end

-- Force a GC to generate FREE records for previously-dropped objects.
-- Drop half the refs, then fullgc.
for i = 1, math.floor(N_STRINGS/2) do strs[i] = nil end
for i = 1, math.floor(M_TABLES/2) do tabs[i] = nil end
collectgarbage("collect")
collectgarbage("collect")

memprof.stop()

-- -- Parse and assert -----------------------------------------------------

local f = io.open(outpath, "rb")
check(f ~= nil, "output file opened")
if f then
  local data = f:read("*all")
  f:close()
  check(#data > 5, "stream non-empty (" .. #data .. " bytes)")

  local events, symtab = parse_stream(data)

  local n_str_alloc = 0
  local n_tab_alloc = 0
  local n_free = 0
  local n_lfunc_attrib = 0
  local n_total_alloc = 0
  local n_total_realloc = 0
  for _, ev in ipairs(events) do
    if ev.op == "ALLOC" then
      n_total_alloc = n_total_alloc + 1
      if ev.cls == 0 then n_str_alloc = n_str_alloc + 1 end       -- NonTrav
      if ev.cls == 1 then n_tab_alloc = n_tab_alloc + 1 end       -- Trav
      if ev.src == "LFUNC" and ev.src_id ~= 0 then
        n_lfunc_attrib = n_lfunc_attrib + 1
      end
    elseif ev.op == "REALLOC" then
      n_total_realloc = n_total_realloc + 1
    elseif ev.op == "FREE" then
      n_free = n_free + 1
    end
  end

  check(n_str_alloc >= N_STRINGS,
        "string ALLOCs >= " .. N_STRINGS .. " (got " .. n_str_alloc .. ")")
  check(n_tab_alloc >= M_TABLES,
        "table ALLOCs >= " .. M_TABLES .. " (got " .. n_tab_alloc .. ")")
  check(n_total_alloc > 0, "total ALLOC records > 0 (got " .. n_total_alloc .. ")")
  check(n_lfunc_attrib > 0,
        "LFUNC-attributed ALLOCs > 0 (got " .. n_lfunc_attrib .. ")")
  check(n_free > 0, "FREE records > 0 after fullgc (got " .. n_free .. ")")

  -- Symtab: at least one LFUNC entry with a chunkname.
  local n_symtab_lfunc = 0
  for _, info in pairs(symtab.lfunc) do
    if info.chunkname and #info.chunkname > 0 then
      n_symtab_lfunc = n_symtab_lfunc + 1
    end
  end
  check(n_symtab_lfunc > 0,
        "symtab LFUNC entries with chunkname > 0 (got " .. n_symtab_lfunc .. ")")

  -- Plausible sizes: string ALLOCs should have size > 0 (sizeof(GCstr)+len).
  local str_min_size = nil
  local str_ok = 0
  for _, ev in ipairs(events) do
    if ev.op == "ALLOC" and ev.cls == 0 and ev.size > 0 then
      str_ok = str_ok + 1
      if str_min_size == nil or ev.size < str_min_size then
        str_min_size = ev.size
      end
    end
  end
  check(str_ok >= N_STRINGS,
        "string ALLOCs with size>0 >= " .. N_STRINGS .. " (got " .. str_ok .. ")")
  check(str_min_size == nil or str_min_size >= 24,
        "min string ALLOC size plausible (>=24, got " .. tostring(str_min_size) .. ")")

  io.write(string.format(
    "  events: alloc=%d realloc=%d free=%d | str_alloc=%d tab_alloc=%d lfunc_attr=%d symtab_lfunc=%d\n",
    n_total_alloc, n_total_realloc, n_free,
    n_str_alloc, n_tab_alloc, n_lfunc_attrib, n_symtab_lfunc))
end

-- -- Double-start owner guard --------------------------------------------

local ok2 = pcall(function()
  memprof.start{ mode = "event", out = outpath }
end)
-- After stop, a new start should succeed (profiler is inactive).
check(ok2, "re-start after stop succeeds")
if ok2 then memprof.stop() end

io.write("memprof_event_assert: ", checks, " checks, ", failures, " failures\n")
os.exit(failures == 0 and 0 or 1)

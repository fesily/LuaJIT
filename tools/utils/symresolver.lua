-- Resolver of C function names for the profiler tools.
--
-- The name of a native function can come from three sources, tried in this
-- order:
--
--   1. "debug"    -- debug info, queried through addr2line (DWARF) or a tool
--                    with the same interface: exact, but needs the debug
--                    info to be available at analysis time.
--   2. "map"      -- a linker map file: names for builds that shipped
--                    without debug info.
--   3. "embedded" -- the symbol table stored in the profile itself by the
--                    sampler (see lj_symtab.h): always available, limited to
--                    what the sampler could collect.
--
-- The first source that knows an address wins, so an analysis can be made as
-- precise as the artifacts at hand allow. Sources are matched to absolute
-- addresses; link-time addresses (what addr2line and map files use) are
-- derived from the module load address, which the embedded table records for
-- every loaded image.

local avl = require "utils.avl"

local string_format = string.format
local floor = math.floor

local M = {}

-- Preferred base of PE images when the header cannot be read.
local PE_DEFAULT_IMAGE_BASE = 0x140000000

-- Number of addresses per tool invocation. Keeps the command line well below
-- the platform limits while amortising the process start-up cost.
local DEBUG_BATCH_SIZE = 128

-- Parse a hexadecimal string with 64-bit arithmetic. tonumber(s, 16)
-- overflows above 32 bits on Windows, where long is 32 bits, and map files
-- routinely contain 64-bit addresses; the result is converted to a number
-- only at the end, which is exact for every address (below 2^53).
local hex_number
do
  local ok, ffi = pcall(require, "ffi")
  local U64_ZERO = ok and ffi.new("uint64_t", 0) or nil
  hex_number = function(s)
    local n = U64_ZERO
    for i = 1, #s do
      local d = tonumber(s:sub(i, i), 16)
      if not d then return nil end
      n = n * 16 + ffi.cast("uint64_t", d)
    end
    return tonumber(n)
  end
end

local function basename(path)
  return path:match("([^/\\]+)$") or path
end

-- File name without the extension, lower case: how a PDB is matched to the
-- image it belongs to (lua51.pdb -> lua51.dll).
local function stem(path)
  return (basename(path):gsub("%.[^.]*$", "")):lower()
end

local function is_pe(path)
  local lower = path:lower()
  return lower:match("%.exe$") ~= nil or lower:match("%.dll$") ~= nil
end

-- Read the preferred image base and the image size of a PE file.
-- Returns 0, 0 on any problem.
local function pe_info(path)
  local f = io.open(path, "rb")
  if not f then return 0, 0 end
  local hdr = f:read(0x1000) or ""
  f:close()
  if #hdr < 0x40 then return 0, 0 end

  local function u16(off)
    local a, b = hdr:byte(off + 1, off + 2)
    if not a then return nil end
    return a + b * 0x100
  end
  local function u32(off)
    local a, b, c, d = hdr:byte(off + 1, off + 4)
    if not a then return nil end
    return a + b * 0x100 + c * 0x10000 + d * 0x1000000
  end

  local lfanew = u32(0x3c)
  if not lfanew or lfanew + 0x60 > #hdr then return 0, 0 end
  if u32(lfanew) ~= 0x00004550 then return 0, 0 end  -- "PE\0\0"
  local magic = u16(lfanew + 24)
  local size = u32(lfanew + 24 + 56) or 0  -- OptionalHeader.SizeOfImage
  if magic == 0x20b then  -- PE32+
    local lo = u32(lfanew + 48)
    local hi = u32(lfanew + 52)
    if not lo or not hi then return 0, size end
    return hi * 0x100000000 + lo, size
  elseif magic == 0x10b then  -- PE32
    return u32(lfanew + 52) or 0, size
  end
  return 0, size
end

-- Parse a linker map file into a sorted list of {addr, name} entries.
--
-- Two formats are understood:
--   GNU ld (ELF):
--       .text.foo   0x0000000000401136    0x10 foo.o
--                   0x0000000000401136          foo
--   MSVC (/MAP):
--       0001:00000010       _helper  00401010 f   main.obj
local function load_map(path)
  local entries = {}
  local is_rva = false
  local f = assert(io.open(path, "r"), "cannot open map file: " .. path)
  for line in f:lines() do
    local addr, name = line:match("^%s+0x(%x+)%s+([%w%._$@?]+)%s*$")
    if not addr then
      -- MSVC map: segment:offset, name, RVA.
      local seg, off, nm, rva = line:match("^%s*(%x+):(%x+)%s+(%S+)%s+(%x+)")
      if seg and off and rva then
        addr = rva
        name = nm
        is_rva = true
      end
    end
    if addr then
      entries[#entries + 1] = { hex_number(addr), name }
    end
  end
  f:close()
  table.sort(entries, function(a, b) return a[1] < b[1] end)
  -- The address range of the image: lets map_lookup reject addresses that
  -- belong to another module (a map describes exactly one image).
  return {
    entries = entries,
    is_rva = is_rva,
    min_key = #entries > 0 and entries[1][1] or 0,
    max_key = #entries > 0 and entries[#entries][1] or 0,
  }
end

local function map_floor(entries, addr)
  local lo, hi = 1, #entries
  local found
  while lo <= hi do
    local mid = floor((lo + hi) / 2)
    if entries[mid][1] <= addr then
      found = entries[mid]
      lo = mid + 1
    else
      hi = mid - 1
    end
  end
  return found
end

-- PDB backend: symbol names straight from dbghelp.dll, no subprocess.
--
-- Windows only; the images recorded in the profile are loaded by their load
-- addresses, so addresses from the profile can be passed in unchanged. The
-- PDB is looked up next to the image (or through _NT_SYMBOL_PATH), exactly as
-- DbgHelp normally does.
local function pdb_supported()
  return type(jit) == "table" and jit.os == "Windows" and jit.arch == "x64"
end

local PDB_CDEF = [[
typedef struct {
  uint32_t SizeOfStruct;
  uint32_t TypeIndex;
  uint64_t Reserved[2];
  uint32_t Index;
  uint32_t Size;
  uint64_t ModBase;
  uint32_t Flags;
  uint64_t Value;
  uint64_t Address;
  uint32_t Register;
  uint32_t Scope;
  uint32_t Tag;
  uint32_t NameLen;
  uint32_t MaxNameLen;
  char Name[1];
} LJ_SYMBOL_INFO;

int SymInitializeW(void *hProcess, const uint16_t *UserSearchPath,
                   int fInvadeProcess);
uint32_t SymSetOptions(uint32_t SymOptions);
uint64_t SymLoadModuleExW(void *hProcess, void *hFile, const uint16_t *ImageName,
                          const uint16_t *ModuleName, uint64_t BaseOfDll,
                          uint32_t DllSize, void *Data, uint32_t Flags);
int SymFromAddr(void *hProcess, uint64_t Address, uint64_t *Displacement,
                LJ_SYMBOL_INFO *Symbol);
int SymCleanup(void *hProcess);
int MultiByteToWideChar(uint32_t CodePage, uint32_t dwFlags,
                        const char *lpMultiByteStr, int cbMultiByte,
                        uint16_t *lpWideCharStr, int cchWideChar);
]]

local SYMOPT_UNDNAME = 0x2
local SYMOPT_DEFERRED_LOADS = 0x4
-- Do not block on a symbol-server dialog when a file is missing.
local SYMOPT_NO_PROMPTS = 0x80000
-- Load a symbol file that is not next to its image (see _pdb_load).
local SLMFLAG_VIRTUAL = 0x1
-- Pseudo handle of the current process.
local CURRENT_PROCESS = -1
local CP_UTF8 = 65001

-- opts:
--   embedded -- symbol table parsed from the profile (utils.symtab)
--   map      -- path to a linker map file
--   debug    -- { binary = <path>, tool = "addr2line" } or nil
--   pdb      -- list of symbol files, matched to the images by their name
--   base     -- link-time base of the binary, overrides the detection
function M.new(opts)
  local self = {
    embedded = opts.embedded,
    map = opts.map and load_map(opts.map) or nil,
    debug = opts.debug,
    pdb_files = opts.pdb or {},
    pdbutil = opts.pdbutil,  -- tool that reads a PDB without its image
    base = opts.base,
    modules = {},
    cache = {},
    warnings = {},
    pdb_tables = {},  -- module base -> symbol table read without the image
  }
  if self.embedded and self.embedded.modules then
    for _, m in ipairs(self.embedded.modules) do
      self.modules[#self.modules + 1] = m
    end
  end
  -- Symbol files are matched to the images by their name: lua51.pdb belongs
  -- to lua51.dll. The load address comes from the profile.
  self.pdb_by_stem = {}
  for _, f in ipairs(self.pdb_files) do
    self.pdb_by_stem[stem(f)] = f
  end
  if self.debug and self.debug.binary then
    -- The main binary is always a module, even if the profile has no entry
    -- for it (it may have been collected with an empty symbol table).
    local path = self.debug.binary
    local known = false
    for _, m in ipairs(self.modules) do
      if m.path == path then known = true end
    end
    if not known then
      self.modules[#self.modules + 1] = { addr = 0, path = path }
    end
  end
  return setmetatable(self, { __index = M })
end

-- Module that contains addr, or nil. The image sizes recorded in the profile
-- make this exact; without them the nearest base below the address is used.
function M:module_of(addr)
  local best
  for _, m in ipairs(self.modules) do
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

function M:link_base(path)
  if self.base then return self.base end
  if is_pe(path) then
    local base = pe_info(path)
    if base ~= 0 then return base end
    return PE_DEFAULT_IMAGE_BASE
  end
  return 0
end

-- Address as the debug tool and the map file see it.
function M:link_addr(addr)
  local m = self:module_of(addr)
  if not m then return addr end
  local base = self:link_base(m.path)
  return addr - m.addr + base
end

-- Convert a UTF-8 path to a zero-terminated UTF-16 buffer. Every DbgHelp
-- entry point that takes a file name has a W variant: the A ones go through
-- the ANSI code page and would mangle non-ASCII paths.
local function utf8_to_wide(ffi, k32, s)
  local n = k32.MultiByteToWideChar(CP_UTF8, 0, s, -1, nil, 0)
  if n <= 0 then return nil end
  local buf = ffi.new("uint16_t[?]", n)
  if k32.MultiByteToWideChar(CP_UTF8, 0, s, -1, buf, n) <= 0 then return nil end
  return buf
end

-- Lazily open dbghelp.dll. Returns nil when PDB support is unavailable.
function M:_pdb()
  if self.pdb ~= nil then
    return self.pdb or nil
  end
  self.pdb = false
  if not pdb_supported() then return nil end

  local ok, ffi = pcall(require, "ffi")
  if not ok then return nil end
  local ok_lib, lib = pcall(ffi.load, "dbghelp")
  if not ok_lib then return nil end
  local ok_k32, k32 = pcall(ffi.load, "kernel32")
  if not ok_k32 then return nil end
  pcall(ffi.cdef, PDB_CDEF)  -- Already defined if the tool is called twice.

  local handle = ffi.cast("void *", CURRENT_PROCESS)
  local search = self:_pdb_search_path()
  local wide = search ~= "" and utf8_to_wide(ffi, k32, search) or nil
  if lib.SymInitializeW(handle, wide, 0) == 0 then
    return nil
  end
  lib.SymSetOptions(SYMOPT_UNDNAME + SYMOPT_DEFERRED_LOADS + SYMOPT_NO_PROMPTS)

  self.pdb = {
    ffi = ffi,
    lib = lib,
    k32 = k32,
    handle = handle,
    loaded = {},  -- addresses of the images already handed to DbgHelp
    info = ffi.new("uint8_t[?]",
                   ffi.sizeof("LJ_SYMBOL_INFO") + 1024),
  }
  return self.pdb
end

-- Directories DbgHelp searches for the symbol file of an image: the ones of
-- the given symbol files first, then the ones of the images of the profile.
function M:_pdb_search_path()
  local dirs, seen = {}, {}

  local function add(path)
    local dir = path:match("^(.*)[/\\]")
    if not dir then return end
    local key = dir:lower()
    if not seen[key] then
      seen[key] = true
      dirs[#dirs + 1] = dir
    end
  end

  for _, f in ipairs(self.pdb_files) do add(f) end
  for _, m in ipairs(self.modules) do
    if m.path and m.path ~= "" then add(m.path) end
  end
  return table.concat(dirs, ";")
end

local function file_exists(path)
  local f = io.open(path, "rb")
  if f then f:close() return true end
  return false
end

-- Symbol file of an image, as given with --pdb and matched to the image by
-- its name: lua51.pdb belongs to lua51.dll. A file next to the image is not
-- used: it may belong to another build than the profiled one.
local function symbol_file(self, path)
  return self.pdb_by_stem[stem(path)]
end

-- Read one PDB without its image.
--
-- DbgHelp needs the image: it validates the symbol file against the GUID and
-- age recorded in the image and refuses a symbol file on its own. The LLVM PDB
-- reader does not: `llvm-pdbutil dump -section-headers` gives the RVA of every
-- section and `dump -symbols` gives the procedures with their section offset
-- and code size, which is all the resolver needs. The symbol table and the
-- symbols are also in the PDB itself.
-- Run a tool through the shell. The doubled outer quotes are what cmd.exe
-- needs when the program path itself is quoted (llvm-pdbutil usually lives in
-- "C:\Program Files\..."): with a single pair the shell splits the path.
local function shell_cmd(tool, args)
  return string_format('""%s" %s"', tool, args)
end

local function load_pdb_file(tool, path)
  local function run(args)
    local cmd = shell_cmd(tool, string_format('%s "%s"', args, path)) .. " 2>&1"
    local pipe = io.popen(cmd)
    if not pipe then return nil end
    local text = pipe:read("*a")
    pipe:close()
    return text
  end

  local headers = run("dump -section-headers")
  if not headers then return nil, "cannot run " .. tool end

  local sections, cur = {}, nil
  for line in headers:gmatch("[^\r\n]+") do
    local idx = line:match("^%s*SECTION HEADER #(%d+)")
    if idx then
      cur = { index = tonumber(idx) }
      sections[#sections + 1] = cur
    elseif cur then
      local name = line:match("^%s+(%S+) name%s*$")
      if name then cur.name = name end
      local vsize = line:match("^%s+(%x+) virtual size%s*$")
      if vsize then cur.vsize = tonumber(vsize, 16) end
      local rva = line:match("^%s+(%x+) virtual address%s*$")
      if rva then cur.rva = tonumber(rva, 16) end
    end
  end
  local rva_of = {}
  for _, sec in ipairs(sections) do
    if sec.index and sec.rva then rva_of[sec.index] = sec.rva end
  end
  if #sections == 0 then
    return nil, "unexpected output of " .. tool .. " (no section headers)"
  end

  local text = run("dump -symbols")
  if not text then return nil, "cannot run " .. tool end

  local functions, pending = {}, nil
  local count = 0
  for line in text:gmatch("[^\r\n]+") do
    local name = line:match("| S_[GL]PROC32 %[size = %d+%] `(.+)`%s*$")
    if name then
      -- The record continues on the next line with the address and the size.
      pending = name
    elseif pending then
      -- llvm-pdbutil prints the section index and the offset in decimal
      -- (the section headers above are hexadecimal).
      local sec, off, csize = line:match("addr = (%d+):(%d+), code size = (%d+)")
      if sec then
        local base = rva_of[tonumber(sec, 10)]
        if base then
          local rva = base + tonumber(off, 10)
          -- First name wins: identical code may be folded into one address.
          if functions[rva] == nil then
            functions[rva] = { name = pending, size = tonumber(csize) }
            count = count + 1
          end
        end
        pending = nil
      end
    end
  end
  if count == 0 then
    return nil, "unexpected output of " .. tool .. " (no procedures)"
  end
  return { functions = functions }
end

-- Teach DbgHelp about one image of the profile.
--
-- The image is what ties a symbol file to a build: DbgHelp reads the debug
-- directory (GUID and age) of the image and then looks for the matching PDB,
-- next to the image first and then in the search path built above, so a
-- symbol file may live anywhere. Without the image the symbol file given on
-- the command line is loaded on its own (SLMFLAG_VIRTUAL); DbgHelp accepts
-- the load, but whether it answers lookups afterwards depends on its version.
function M:_pdb_load(module)
  local pdb = self:_pdb()
  if not pdb or not module then return end
  if pdb.loaded[module.addr] then return end
  pdb.loaded[module.addr] = true

  local _, size = pe_info(module.path)
  local file = symbol_file(self, module.path)
  local loaded = false

  local function load(image, symfile, flags)
    local wimage = image and utf8_to_wide(pdb.ffi, pdb.k32, image) or nil
    local wfile = symfile and utf8_to_wide(pdb.ffi, pdb.k32, symfile) or nil
    if image and not wimage then return false end
    if symfile and not wfile then return false end
    return pdb.lib.SymLoadModuleExW(pdb.handle, nil, wimage, wfile,
                                    module.addr, size, nil, flags) ~= 0
  end

  if file_exists(module.path) then
    loaded = load(module.path, nil, 0)
  end
  if not loaded and file then
    -- The addresses come from another process, so the symbol file is bound to
    -- the load address recorded in the profile instead of one DbgHelp picks.
    loaded = load(nil, file, SLMFLAG_VIRTUAL)
  end
  if not loaded and file then
    self.warnings[#self.warnings + 1] = string_format(
      "cannot load symbols for %s from %s", module.path, file)
  end
end

-- Symbol table of an image read from its PDB alone (see load_pdb_file).
function M:_pdb_standalone(module)
  local key = module.addr
  if self.pdb_tables[key] ~= nil then
    return self.pdb_tables[key] or nil
  end
  self.pdb_tables[key] = false

  local file = symbol_file(self, module.path)
  if not file then return nil end
  local tool = self.pdbutil or "llvm-pdbutil"
  local table_, err = load_pdb_file(tool, file)
  if not table_ then
    self.warnings[#self.warnings + 1] = string_format(
      "cannot read %s: %s (use --pdbutil= to point at llvm-pdbutil)", file, err)
    return nil
  end
  self.pdb_tables[key] = table_
  return table_
end

-- Name of the function that contains addr, from the symbols DbgHelp loaded.
--
-- SymFromAddr() reports the nearest symbol, not the containing function: a
-- function symbol carries its extent, a public symbol is only a label. The
-- name is accepted only when both the address and the start of the frame's
-- function (the entry, from the stream) really belong to that symbol.
function M:_pdb_dbghelp(addr, entry, module)
  local pdb = self:_pdb()
  if not pdb or entry == nil or entry == 0 then return nil end

  local ffi = pdb.ffi
  local info = ffi.cast("LJ_SYMBOL_INFO *", pdb.info)
  ffi.fill(pdb.info, ffi.sizeof("LJ_SYMBOL_INFO") + 1024, 0)
  info.SizeOfStruct = ffi.sizeof("LJ_SYMBOL_INFO")
  info.MaxNameLen = 1023
  if pdb.lib.SymFromAddr(pdb.handle, addr, nil, info) == 0 then
    return nil
  end

  local sym_addr = tonumber(info.Address)
  local size = tonumber(info.Size)
  local inside
  if size > 0 then
    inside = entry >= sym_addr and entry < sym_addr + size and
             addr >= sym_addr and addr < sym_addr + size
  else
    inside = sym_addr == entry
  end
  if not inside then
    return nil
  end

  local name = ffi.string(info.Name, info.NameLen)
  if name == "" then return nil end
  local real = name:match("^ILT%+%x+%((.+)%)$")
  if real then name = real end
  -- The address of the symbol itself, not the image base.
  return name, sym_addr
end

-- Name of the function that starts at `entry`, from the PDB read directly.
function M:_pdb_from_file(addr, entry, module)
  local table_ = self:_pdb_standalone(module)
  if not table_ then return nil end
  local rva = entry - module.addr
  local fn = table_.functions[rva]
  if not fn then return nil end
  -- The symbol must cover the address: a symbol file of another build (or a
  -- stale frame start) must not give the name of a neighbouring function.
  if fn.size and fn.size > 0 then
    local off = addr - module.addr - rva
    if off < 0 or off >= fn.size then return nil end
  elseif addr ~= entry then
    return nil
  end
  return fn.name, entry
end

-- Name of the function that contains addr, from the PDB of its image. The
-- image is used when it is available (DbgHelp then validates the symbol file
-- against it); otherwise the symbol file is read on its own.
function M:pdb_lookup(addr, entry)
  if entry == nil or entry == 0 then return nil end
  local module = self:module_of(addr)
  -- The symbol file first: it is read on its own, so the answer does not
  -- depend on where this process has mapped the images.
  local name, base = self:_pdb_from_file(addr, entry, module)
  if name then return name, base end
  self:_pdb_load(module)
  return self:_pdb_dbghelp(addr, entry, module)
end

-- Release the DbgHelp state (optional: the process is usually about to exit).
function M:close()
  if self.pdb then
    self.pdb.lib.SymCleanup(self.pdb.handle)
    self.pdb = false
  end
end

-- Name of the function that contains addr, from the DWARF debug info of the
-- image (via addr2line). The PDB source is separate: see pdb_lookup().
function M:debug_lookup(addr, entry)
  local m = self:module_of(addr)
  local path = m and m.path or self.debug.binary
  local link = self:link_addr(addr)
  -- Suppress the tool's diagnostics: an unknown address is not an error.
  local devnull = package.config:sub(1, 1) == "\\" and "2>nul" or "2>/dev/null"
  -- %q would double the backslashes of a Windows path, which the shell does
  -- not undo; quote by hand instead.
  local cmd = shell_cmd(self.debug.tool or "addr2line",
                        string_format('-f -C -e "%s" 0x%x', path, link)) ..
              " " .. devnull
  local pipe = io.popen(cmd)
  if not pipe then return nil end
  local name = pipe:read("*l")
  pipe:close()
  if not name or name == "" or name == "??" then return nil end
  return name, m and m.addr or 0
end

function M:map_lookup(addr, entry)
  local m = self:module_of(addr)
  -- MSVC maps list RVAs, GNU maps list link-time addresses.
  local key = self.map.is_rva and (addr - (m and m.addr or 0))
                           or self:link_addr(addr)
  -- A map describes one image: an address outside its range belongs to
  -- another module and must fall through to the next source.
  if key < self.map.min_key or key > self.map.max_key then
    return nil
  end
  local hit = map_floor(self.map.entries, key)
  if hit then
    local base = m and m.addr or 0
    local addr_of_sym = self.map.is_rva and (hit[1] + base)
                                        or (hit[1] - self:link_base(m and m.path or "") + base)
    -- A map lists the symbols of one image, not their sizes: accept the name
    -- only when the symbol starts at the function that contains the address
    -- (and the caller knows where that function starts).
    if entry == nil or entry == 0 or addr_of_sym ~= entry then
      return nil
    end
    return hit[2], addr_of_sym
  end
  return nil
end

function M:embedded_lookup(addr, entry)
  if not self.embedded or not self.embedded.cfunc then return nil end
  if entry == nil or entry == 0 then
    return nil
  end
  local key, value = avl.floor(self.embedded.cfunc, entry)
  if key == entry and entry <= addr then
    return value.name, key
  end
  return nil
end

-- Resolve a set of addresses with as few tool invocations as possible.
--
-- The debug source is the expensive one (one process per query otherwise), so
-- callers that can look ahead (the CLI parses the profile once to collect the
-- addresses) should use this instead of letting resolve() spawn a tool per
-- address. Addresses that the tool does not know are recorded as misses, so
-- that the sources below it get their turn.
function M:preload(addrs)
  if not self.debug then return end

  if not self.debug.tool then
    -- The PDB is read in-process and a name is only accepted for the exact
    -- function that contains the address, which is known here only for the
    -- addresses the caller will pass to resolve(). Nothing to preload.
    return
  end

  local devnull = package.config:sub(1, 1) == "\\" and "2>nul" or "2>/dev/null"
  local groups = {}
  for addr in pairs(addrs) do
    local m = self:module_of(addr)
    local path = m and m.path or self.debug.binary
    local group = groups[path]
    if not group then
      group = { module = m, addrs = {} }
      groups[path] = group
    end
    group.addrs[#group.addrs + 1] = addr
  end

  for path, group in pairs(groups) do
    local link_base = self:link_base(path)
    local mod_base = group.module and group.module.addr or 0
    local list = group.addrs
    local i = 1
    while i <= #list do
      local last = math.min(i + DEBUG_BATCH_SIZE - 1, #list)
      local query = {}
      for k = i, last do
        query[#query + 1] = string_format("0x%x", list[k] - mod_base + link_base)
      end
      local cmd = shell_cmd(self.debug.tool or "addr2line",
                            string_format('-f -C -e "%s" %s', path,
                                          table.concat(query, " "))) ..
                  " " .. devnull
      local pipe = io.popen(cmd)
      if pipe then
        for k = i, last do
          local name = pipe:read("*l")
          local loc = pipe:read("*l")  -- file:line, not used
          local addr = list[k]
          if name and name ~= "" and name ~= "??" and loc then
            self.cache[addr] = {
              dump = string_format("%s:%#x", name, mod_base),
              name = name,
              base = mod_base,
            }
          else
            self.cache[addr] = false
          end
        end
        pipe:close()
      end
      i = last + 1
    end
  end
end

-- Returns "name:address", like utils.symtab.demangle(), or nil.
-- `entry` is the start of the function that contains `addr` (0 or nil if
-- unknown). A source that only stores symbol addresses uses it to reject
-- names that belong to a neighbouring function.
-- Returns "name:address", then the plain name and the address of the symbol,
-- so that a caller can qualify the name with the module it belongs to.
--
-- A symbol file resolves a function start (entry), not just an address: the
-- same address may be looked up with a wrong entry first (a stale frame), so
-- results are keyed by entry when it is known and a miss does not hide the
-- answer for the address itself.
function M:resolve(addr, entry)
  local key = (entry and entry ~= 0) and entry or addr
  local cached = self.cache[key]
  if cached == nil and key ~= addr then
    cached = self.cache[addr]
  end
  if cached ~= nil then
    if not cached then return nil end
    return cached.dump, cached.name, cached.base
  end
  local name, base
  if self.debug then
    -- The debug info names the function that contains the address.
    name, base = self:debug_lookup(addr, entry)
  elseif #self.pdb_files > 0 then
    name, base = self:pdb_lookup(addr, entry)
  end
  if not name and self.map then
    name, base = self:map_lookup(addr, entry)
  end
  if not name and self.embedded then
    name, base = self:embedded_lookup(addr, entry)
  end
  if name then
    local result = string_format("%s:%#x", name, base or 0)
    self.cache[key] = { dump = result, name = name, base = base or 0 }
    return result, name, base or 0
  end
  self.cache[key] = false
  return nil
end

return M

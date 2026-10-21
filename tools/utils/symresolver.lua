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

int SymInitialize(void *hProcess, const char *UserSearchPath,
                  int fInvadeProcess);
uint32_t SymSetOptions(uint32_t SymOptions);
uint64_t SymLoadModuleEx(void *hProcess, void *hFile, const char *ImageName,
                         const char *ModuleName, uint64_t BaseOfDll,
                         uint32_t DllSize, void *Data, uint32_t Flags);
int SymFromAddr(void *hProcess, uint64_t Address, uint64_t *Displacement,
                LJ_SYMBOL_INFO *Symbol);
int SymCleanup(void *hProcess);
]]

local SYMOPT_UNDNAME = 0x2
local SYMOPT_DEFERRED_LOADS = 0x4
-- Pseudo handle of the current process.
local CURRENT_PROCESS = -1

-- opts:
--   embedded -- symbol table parsed from the profile (utils.symtab)
--   map      -- path to a linker map file
--   debug    -- { binary = <path>, tool = "addr2line" } or nil
--   base     -- link-time base of the binary, overrides the detection
function M.new(opts)
  local self = {
    embedded = opts.embedded,
    map = opts.map and load_map(opts.map) or nil,
    debug = opts.debug,
    base = opts.base,
    modules = {},
    cache = {},
  }
  if self.embedded and self.embedded.modules then
    for _, m in ipairs(self.embedded.modules) do
      self.modules[#self.modules + 1] = m
    end
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

-- Load address of the module that contains addr, or nil.
function M:module_of(addr)
  local best
  for _, m in ipairs(self.modules) do
    if m.addr <= addr and (not best or m.addr > best.addr) then
      best = m
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
  pcall(ffi.cdef, PDB_CDEF)  -- Already defined if the tool is called twice.

  local handle = ffi.cast("void *", CURRENT_PROCESS)
  if lib.SymInitialize(handle, nil, 0) == 0 then
    return nil
  end
  lib.SymSetOptions(SYMOPT_UNDNAME + SYMOPT_DEFERRED_LOADS)

  self.pdb = {
    ffi = ffi,
    lib = lib,
    handle = handle,
    loaded = {},  -- addresses of the images already handed to DbgHelp
    info = ffi.new("uint8_t[?]",
                   ffi.sizeof("LJ_SYMBOL_INFO") + 1024),
  }
  return self.pdb
end

-- Teach DbgHelp about one image of the profile.
function M:_pdb_load(module)
  local pdb = self:_pdb()
  if not pdb or not module then return end
  if pdb.loaded[module.addr] then return end
  pdb.loaded[module.addr] = true
  local _, size = pe_info(module.path)
  pdb.lib.SymLoadModuleEx(pdb.handle, nil, module.path, nil, module.addr,
                          size, nil, 0)
end

-- Name of the function that contains addr, from the PDB of its image.
function M:pdb_lookup(addr)
  local pdb = self:_pdb()
  if not pdb then return nil end
  local module = self:module_of(addr)
  self:_pdb_load(module)

  local ffi = pdb.ffi
  local info = ffi.cast("LJ_SYMBOL_INFO *", pdb.info)
  ffi.fill(pdb.info, ffi.sizeof("LJ_SYMBOL_INFO") + 1024, 0)
  info.SizeOfStruct = ffi.sizeof("LJ_SYMBOL_INFO")
  info.MaxNameLen = 1023
  if pdb.lib.SymFromAddr(pdb.handle, addr, nil, info) == 0 then
    return nil
  end
  local name = ffi.string(info.Name, info.NameLen)
  if name == "" then return nil end
  local real = name:match("^ILT%+%x+%((.+)%)$")
  if real then name = real end
  return name, module and module.addr or 0
end

function M:debug_lookup(addr)
  -- A tool named explicitly (--addr2line=) means DWARF; otherwise the PDB is
  -- the debug source and there is no point in starting a tool per address.
  if not self.debug.tool then
    if self:_pdb() then
      return self:pdb_lookup(addr)
    end
  end
  local m = self:module_of(addr)
  local path = m and m.path or self.debug.binary
  local link = self:link_addr(addr)
  -- Suppress the tool's diagnostics: an unknown address is not an error.
  local devnull = package.config:sub(1, 1) == "\\" and "2>nul" or "2>/dev/null"
  -- %q would double the backslashes of a Windows path, which the shell does
  -- not undo; quote by hand instead.
  local cmd = string_format('%s -f -C -e "%s" 0x%x %s',
                            self.debug.tool or "addr2line", path, link, devnull)
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

  if not self.debug.tool and self:_pdb() then
    -- In-process: one call per address, nothing to batch.
    for addr in pairs(addrs) do
      local name, base = self:pdb_lookup(addr)
      self.cache[addr] = name and string_format("%s:%#x", name, base or 0)
                          or false
    end
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
      local cmd = string_format('%s -f -C -e "%s" %s %s',
                                self.debug.tool or "addr2line", path,
                                table.concat(query, " "), devnull)
      local pipe = io.popen(cmd)
      if pipe then
        for k = i, last do
          local name = pipe:read("*l")
          local loc = pipe:read("*l")  -- file:line, not used
          local addr = list[k]
          if name and name ~= "" and name ~= "??" and loc then
            self.cache[addr] = string_format("%s:%#x", name, mod_base)
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
function M:resolve(addr, entry)
  local cached = self.cache[addr]
  if cached ~= nil then
    return cached or nil
  end
  local name, base
  if self.debug then
    -- The debug info names the function that contains the address.
    name, base = self:debug_lookup(addr)
  end
  if not name and self.map then
    name, base = self:map_lookup(addr, entry)
  end
  if not name and self.embedded then
    name, base = self:embedded_lookup(addr, entry)
  end
  local result
  if name then
    result = string_format("%s:%#x", name, base or 0)
  end
  self.cache[addr] = result or false
  return result
end

return M

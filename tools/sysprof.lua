local evread = require "utils.evread"
local sysprof = require "sysprof.parse"
local symresolver = require "utils.symresolver"

local stdout, stderr = io.stdout, io.stderr
local match, gmatch = string.match, string.gmatch

-- Program options.
local opt_map = {}

function opt_map.help()
  stdout:write [[
luajit-parse-sysprof - parser of the profile collected
                       with LuaJIT's sysprof.

SYNOPSIS

luajit-parse-sysprof [options] sysprof.bin

Supported options are:

  --help                            Show this help and exit
  --map=<file>                      Resolve C names with a linker map file
  --debug=<binary>                  Resolve C names with the debug info of
                                    <binary> (through addr2line)
  --addr2line=<tool>                Debug info tool to use (default: addr2line)
  --pdb=<file>                       Symbol file of an image (repeatable);
                                    matched to the image by its name
  --pdbutil=<tool>                   Reader for a symbol file without the
                                    image (default: llvm-pdbutil)
]]
  os.exit(0)
end

-- Sources of C function names (see utils/symresolver.lua).
local sym_map, sym_debug, sym_tool
local sym_pdbs = {}
local sym_pdbutil

-- Options that take a value accept both --name=value and --name value.
local function opt_value(v, args)
  if type(v) == "string" then
    return v
  end
  local value = args and args[args.argn]
  if value then
    args.argn = args.argn + 1
  end
  return value
end

function opt_map.map(v, args)
  local path = opt_value(v, args)
  if path == nil or path == "" then opterror("--map needs a file name") end
  sym_map = path
end

function opt_map.debug(v, args)
  local binary = opt_value(v, args)
  if binary == nil or binary == "" then opterror("--debug needs a binary") end
  sym_debug = binary
end

function opt_map.pdb(v, args)
  local path = opt_value(v, args)
  if path == nil or path == "" then opterror("--pdb needs a file name") end
  sym_pdbs[#sym_pdbs + 1] = path
end

function opt_map.pdbutil(v, args)
  local tool = opt_value(v, args)
  if tool == nil or tool == "" then opterror("--pdbutil needs a tool") end
  sym_pdbutil = tool
end

function opt_map.addr2line(v, args)
  local tool = opt_value(v, args)
  if tool == nil or tool == "" then opterror("--addr2line needs a tool") end
  sym_tool = tool
end

-- Print error and exit with error status.
local function opterror(...)
  stderr:write("luajit-parse-sysprof.lua: ERROR: ", ...)
  stderr:write("\n")
  os.exit(1)
end

-- Parse single option.
local function parseopt(opt, args)
  local opt_current = #opt == 1 and "-"..opt or "--"..opt
  local f = opt_map[opt]
  if not f then
    opterror("unrecognized option `", opt_current, "'. Try `--help'.\n")
  end
  f(args)
end

-- Parse arguments.
local function parseargs(args)
  -- Process all option arguments.
  args.argn = 1
  repeat
    local a = args[args.argn]
    if not a then
      break
    end
    local lopt, opt = match(a, "^%-(%-?)(.+)")
    if not opt then
      break
    end
    args.argn = args.argn + 1
    if lopt == "" then
      -- Loop through short options.
      for o in gmatch(opt, ".") do
        parseopt(o, args)
      end
    else
      -- Long option, optionally with a value: --name=value.
      local name, value = match(opt, "^([^=]+)=(.*)$")
      if name and opt_map[name] then
        opt_map[name](value)
      else
        parseopt(opt, args)
      end
    end
  until false

  -- Check for proper number of arguments.
  local nargs = #args - args.argn + 1
  if nargs ~= 1 then
    opt_map.help()
  end

  -- Translate a single input file.
  -- TODO: Handle multiple files?
  return args[args.argn]
end

local function make_resolver(symbols)
  if not sym_map and not sym_debug and #sym_pdbs == 0 then
    return nil  -- Only the table embedded in the profile.
  end
  return symresolver.new({
    embedded = symbols,
    map = sym_map,
    pdb = sym_pdbs,
    pdbutil = sym_pdbutil,
    debug = sym_debug and { binary = sym_debug, tool = sym_tool or "addr2line" },
  })
end

-- First pass: collect the addresses that will need a name, so that the
-- debug source can be queried in batches instead of once per address.
local function collect_addresses(inputfile)
  local addrs, symbols = {}, nil
  local collector = {}
  function collector:resolve(addr)
    addrs[addr] = true
    return nil
  end
  evread(function(reader, syms)
    symbols = syms
    return sysprof.parse(reader, syms, collector)
  end, inputfile)
  return addrs, symbols
end

local function dump(inputfile)
  -- XXX: This function exits with a non-zero exit code and
  -- prints an error message if it encounters any failure during
  -- the process of parsing.
  local resolver
  if sym_debug or sym_map or #sym_pdbs > 0 then
    local addrs, symbols = collect_addresses(inputfile)
    resolver = make_resolver(symbols)
    if sym_debug then
      resolver:preload(addrs)
    end
  end
  local events = evread(function(reader, symbols)
    return sysprof.parse(reader, symbols, resolver)
  end, inputfile)

  if resolver then
    for _, w in ipairs(resolver.warnings or {}) do
      stderr:write("luajit-parse-sysprof: warning: ", w, "\n")
    end
    resolver:close()
  end

  for stack, count in pairs(events) do
    print(stack, count)
  end
  -- XXX: The second argument is required to properly close Lua
  -- universe (i.e. invoke <lua_close> before exiting).
  os.exit(0, true)
end

-- XXX: When this script is used as a preloaded module by an
-- application, it should return one function for correct parsing
-- of command line flags like --leak-only and dumping profile
-- info.
local function dump_wrapped(...)
  return dump(parseargs(...))
end

return dump_wrapped

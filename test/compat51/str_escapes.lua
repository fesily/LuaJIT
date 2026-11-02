-- Lua 5.1 escape lexing, as implemented by PUC lua-5.1.5 llex.c read_string():
-- an unknown escape drops the backslash and keeps the character (default: -->
-- save_and_next), and an out-of-range decimal escape is reported as
-- "escape sequence too large".  LuaJIT stock instead rejects unknown escapes
-- ("invalid escape sequence") and enables '\x'/'\u'/'\z'.
--
-- Pins LUA_COMPAT_LEX_XESC (lj_arch.h, on with LUAJIT_ENABLE_LUA51COMPAT) and
-- LUA_COMPAT_LEX_UNICODE (off with it), plus the LJ_ERR_XESCL wording.

local function catch(src)
  local f, err = loadstring(src)
  return f, err
end

local function check(name, cond, detail)
  if not cond then
    io.write("FAIL: "..name.." -- "..tostring(detail).."\n")
    os.exit(1)
  end
end

-- Unknown escapes are tolerated and keep the character.
local q = "\q"
check("backslash-q keeps the char", q == "q" and #q == 1, #q)
local s = "\z   a"
check("backslash-z keeps the char", s == "z   a" and #s == 5, #s)
local u = "\u{41}"
check("backslash-u keeps the chars", u == "u{41}" and #u == 5, #u)
local x = "\x41"
check("backslash-x keeps the chars", x == "x41" and #x == 3, #x)

-- The escapes LuaJIT calls "hex"/"unicode" must stay literal here.  Guarded
-- so a LUA_COMPAT_LEX_UNICODE=1 build reports the mismatch instead of passing.
local hx = "\x3F"
check("hex escape not decoded", hx == "x3F", hx)

-- Regular escapes are unchanged.
check("decimal escape", #"\0" == 1 and "\0" == string.char(0))
check("max decimal escape", "\255" == string.char(255))
check("quoted escapes", "\\\"" == "\\\"" and "\\'" == "\\'")

-- Out of range decimal escapes use the 5.1 wording.
local f, err = catch('return "\\256"')
check("256 rejected", f == nil, "accepted")
check("256 wording", tostring(err):find("escape sequence too large", 1, true), err)
local f2, err2 = catch('return "\\400"')
check("400 rejected", f2 == nil, "accepted")
check("400 wording", tostring(err2):find("escape sequence too large", 1, true), err2)
local f3, err3 = catch('return "\\999"')
check("999 rejected", f3 == nil, "accepted")
check("999 wording", tostring(err3):find("escape sequence too large", 1, true), err3)

print("str_escapes: ok")

-- PUC 5.1 f_parser chooses the parser from the first luaZ_lookahead. When that
-- first read is EOF it selects luaY_parser; the second luaZ_fill (inside
-- luaX_setinput) belongs to that text parser. A bytecode signature delivered by
-- the retry must therefore not reclassify the chunk as binary.
--
-- Pins LJ_DS_PUC51_LOAD_EOF_RETRY together with the first-lookahead rule.

local calls = 0
local f, err = load(function()
  calls = calls + 1
  if calls == 1 then return nil end
  if calls == 2 then return "\27LJ\2" end
  return nil
end, "=eof_retry")

local function check(name, cond, detail)
  if not cond then
    io.write("FAIL: "..name.." -- "..tostring(detail).."\n")
    os.exit(1)
  end
end

check("reader called twice", calls == 2, calls)
check("retried chunk is parsed as text", f == nil, "accepted")
check("not a binary policy error",
      not tostring(err):find("Disallowed functionality", 1, true), err)

print("load_eof_retry: ok")

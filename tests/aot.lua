----------------------------------------------------------------------------
-- Tests for the jit.aot numeric-subset compiler.
--
-- Run from the repository root:
--
--   LUA_PATH='./src/?.lua;;' CC=cc ./src/luajit -joff tests/aot.lua
--
-- Each case generates C, compiles it with the system C compiler, loads the
-- shared module and compares the result against the interpreter (reference
-- functions have the JIT disabled). Generated files live in a unique
-- temporary directory which is removed on success and on failure.
----------------------------------------------------------------------------

local jit = require("jit")
local jutil = require("jit.util")
local aot = require("jit.aot")

local format, find = string.format, string.find
local unpack = unpack or table.unpack

local CC = os.getenv("CC") or "cc"
local INC = "./src"
local LUAJIT = "./src/luajit"

------------------------------------------------------------------------------
-- Harness.

local TMP = os.tmpname()
os.remove(TMP)
assert(io.open(LUAJIT, "rb"), "run this from the repository root")

local function run(cmd)
  local a = os.execute(cmd)
  return a == 0 or a == true
end

local function shellquote(s)
  return "'"..s:gsub("'", "'\\''").."'"
end

local TMPQ = shellquote(TMP)
assert(run("mkdir -p "..TMPQ), "cannot create temporary directory "..TMP)
local cleaned = false

local function cleanup()
  if not cleaned then
    cleaned = true
    run("rm -rf "..TMPQ)
  end
end

local function fatal(msg)
  cleanup()
  error(msg, 0)
end

local ncase = 0

-- Generate, compile and load one AOT module.
local function build(func)
  ncase = ncase + 1
  local mod = "aot_t"..ncase
  local cfile = TMP.."/"..mod..".c"
  local sofile = TMP.."/"..mod..".so"
  local errfile = TMP.."/"..mod..".err"
  local src = aot.compile(func, mod)
  local fh = assert(io.open(cfile, "wb"))
  assert(fh:write(src))
  assert(fh:close())
  local link
  if jit.os == "OSX" then
    link = "-bundle -undefined dynamic_lookup"
  elseif jit.os == "Linux" then
    link = "-shared"
  else
    fatal("this test runner has no link command for "..tostring(jit.os))
  end
  local cmd = format("%s -O2 -std=c99 -fPIC -ffp-contract=off -I%s %s %s -o %s -lm 2>%s",
		     CC, INC, link, shellquote(cfile), shellquote(sofile),
		     shellquote(errfile))
  if not run(cmd) then
    local ef = io.open(errfile, "rb")
    local log = ef and ef:read("*a") or "<no compiler output>"
    if ef then ef:close() end
    fatal(format("C compilation failed\ncommand: %s\n%s", cmd, log))
  end
  local ldr, lerr = package.loadlib(sofile, "luaopen_"..mod)
  if not ldr then fatal("cannot load "..sofile..": "..tostring(lerr)) end
  local f = ldr()
  if type(f) ~= "function" then
    fatal("module "..mod.." did not return a function")
  end
  return f, sofile
end

-- Reference function: never JIT-compiled.
local function def(func)
  jit.off(func, true)
  return func
end

local function pack(...) return {n = select("#", ...), ...} end
local function A(...) return pack(...) end

-- Compare two Lua values, keeping signed zeros and NaN apart.
local function valcmp(a, b)
  local ta = type(a)
  if ta ~= type(b) then return false, "type "..ta.." vs "..type(b) end
  if ta == "number" then
    if a ~= a or b ~= b then
      if a ~= a and b ~= b then return true end
      return false, "NaN mismatch"
    end
    if a ~= b then
      return false, format("%.17g vs %.17g", a, b)
    end
    if a == 0 and (1/a < 0) ~= (1/b < 0) then
      return false, "signed zero mismatch"
    end
    return true
  end
  if ta == "table" then
    if a == b then return true end
    return false, "different tables"
  end
  if a ~= b then return false, tostring(a).." vs "..tostring(b) end
  return true
end

local function callcmp(label, func, faut, args)
  local ra = pack(func(unpack(args, 1, args.n)))
  local rb = pack(faut(unpack(args, 1, args.n)))
  if ra.n ~= rb.n then
    fatal(format("%s: result count %d vs %d", label, ra.n, rb.n))
  end
  for i = 1, ra.n do
    local ok, why = valcmp(ra[i], rb[i])
    if not ok then
      fatal(format("%s: result %d differs (%s)", label, i, why))
    end
  end
end

-- Differential check of one function over a list of argument lists.
local function check(label, func, calls)
  local faut = build(func)
  for i = 1, #calls do
    callcmp(label.." call "..i, func, faut, calls[i])
  end
  return faut
end

local function expectfail(label, args, func, patterns)
  local faut = build(func)
  local ok, msg = pcall(faut, unpack(args, 1, args.n))
  if ok then fatal(label..": expected an error") end
  msg = tostring(msg)
  if not find(msg, patterns[1], 1, true) then
    fatal(label..": error does not contain "..patterns[1]..": "..msg)
  end
  for i = 2, #patterns do
    if not find(msg, patterns[i], 1, true) then
      fatal(label..": error does not contain "..patterns[i]..": "..msg)
    end
  end
end

local ntest = 0
local function ok(label) ntest = ntest + 1 end
local function expect(cond, label)
  if not cond then fatal(label) end
  ntest = ntest + 1
end

------------------------------------------------------------------------------
-- 1. Basic execution and control flow.

local function t_sum(n) local s = 0 for i = 1, n do s = s + i end return s end
local function t_fact(n) local r = 1 for i = 2, n do r = r * i end return r end
-- The iteration guard cannot change the reference result, but it turns a
-- wrong loop condition in the generated module into a fast result mismatch
-- instead of a hanging test run.
local function t_for(a, b, st)
  local s, n = 0, 0
  for i = a, b, st do
    s = s + i
    n = n + 1
    if n > 100000 then break end
  end
  return s
end
local function t_while(n)
  local s = 0 local i = 0 while i < n do i = i + 1 s = s + i end return s
end
local function t_repeat(n)
  local s = 0 repeat s = s + 1 until s >= n return s
end
local function t_nest(n, m)
  local s = 0 for i = 1, n do for j = 1, m do s = s + i * j end end return s
end
local function t_break(n)
  local s = 0 for i = 1, n do if i > 3 then break end s = s + i end return s
end
local function t_emptyfor(n) local s = 0 for i = 1, n do end return s end
local function t_branch(x, y)
  if x < y then return 1 elseif x > y then return 2 elseif x == y then return 3 end
  return 4
end
local function t_noret() end
local function t_multi(a, b) return a, b, a + b end
local function t_earlyret(n)
  for i = 1, n do if i == 3 then return end end return 42
end

def(t_sum) def(t_fact) def(t_for) def(t_while) def(t_repeat) def(t_nest)
def(t_break) def(t_emptyfor) def(t_branch) def(t_noret) def(t_multi)
def(t_earlyret)

check("sum", t_sum, {A(100), A(0), A(1), A(1000)})
check("fact", t_fact, {A(0), A(1), A(10)})
-- Zero steps are only tested in loops with an explicit break: the VM
-- semantics do not stop on its own and no hook can interrupt C code.
check("for", t_for, {A(1, 10, 1), A(10, 1, -1), A(1, 2, 0.5),
		     A(2.5, 1.5, -0.5), A(1, 0, 1), A(1.5, 2.5, -0.0)})
check("while", t_while, {A(0), A(5), A(100)})
check("repeat", t_repeat, {A(0), A(1), A(7)})
check("nest", t_nest, {A(0, 0), A(3, 4), A(1, 1)})
check("break", t_break, {A(0), A(2), A(10)})
check("emptyfor", t_emptyfor, {A(0), A(3)})
check("branch", t_branch, {A(1, 2), A(2, 1), A(2, 2), A(0.0, -0.0)})
check("noret", t_noret, {A()})
check("multi", t_multi, {A(1, 2), A(2.5, 0.5), A(0, 0)})
check("earlyret", t_earlyret, {A(0), A(3), A(10)})

expect(t_sum(100) == 5050, "sum(100) != 5050 in the interpreter")
ok()
expect(t_fact(10) == 3628800, "fact(10) != 3628800 in the interpreter")

------------------------------------------------------------------------------
-- 2. Types, parameters and truthiness.

local function t_add2(a, b) return a + b end
local function t_retarg(a) return a end
local function t_truth(v) if v then return 1 end return 0 end
local function t_or(v) local x = v or 7 return x end
local function t_and(a, b) return a and b end
local function t_not(v) return not v end
local function t_nils()
  local a, b, c
  return a, b, c
end
local function t_bigparams(p1, p2, p3, p4, p5, p6, p7, p8, p9, p10,
			   p11, p12, p13, p14, p15, p16, p17, p18, p19, p20,
			   p21, p22, p23, p24, p25, p26, p27, p28, p29, p30)
  return p1 + p30
end

def(t_add2) def(t_retarg) def(t_truth) def(t_or) def(t_and) def(t_not)
def(t_nils) def(t_bigparams)

check("add2", t_add2, {A(1, 2), A(2.5, 0.5), A(0, 0)})
check("retarg", t_retarg, {A(7), A(7, 8, 9), A(true), A("x"), A()})
check("truth", t_truth, {A(true), A(false), A(nil), A(0), A(""), A(0.0)})
check("or", t_or, {A(true), A(false), A(nil), A(0), A(1.5)})
check("and", t_and, {A(1, 2), A(nil, 2), A(false, 2), A(0, false)})
check("not", t_not, {A(true), A(false), A(nil), A(0)})
check("nils", t_nils, {A()})
-- GC values are only moved, tested for truth and returned, never used in
-- a numeric operation, so they must work without any type conversion.
do
  local tbl = {}
  local ft = build(t_truth)
  callcmp("truth table", t_truth, ft, A(tbl))
  callcmp("truth function", t_truth, ft, A(print))
  callcmp("truth string", t_truth, ft, A(""))
  local fr = build(t_retarg)
  callcmp("retarg table", t_retarg, fr, A(tbl))
  callcmp("retarg function", t_retarg, fr, A(print))
  local fo = build(t_or)
  callcmp("or table", t_or, fo, A(tbl))
  callcmp("or function", t_or, fo, A(print))
  ok()
end
check("bigparams", t_bigparams,
      {A(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
	 21, 22, 23, 24, 25, 26, 27, 28, 29, 30),
       A(0.5, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
	 21, 22, 23, 24, 25, 26, 27, 28, 29, 1.5)})
do
  local faut = build(t_bigparams)
  expect(jutil.funcinfo(t_bigparams).stackslots > 20,
	 "bigparams framesize is not above LUA_MINSTACK")
  callcmp("bigparams", t_bigparams, faut, A(1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
    11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30))
  ok()
end

-- Missing arguments are nil and fail at the numeric operation.
expectfail("missing arg", A(1), t_add2, {"AOT: ", "ADDVV", "number expected"})
-- No implicit string->number coercion.
expectfail("string arg", A("10", 1), t_add2, {"ADDVV", "number expected"})
expectfail("boolean arg", A(true, 1), t_add2, {"number expected"})
expectfail("nil arg", A(nil, 1), t_add2, {"number expected"})
-- ... and no metamethod calls either.
do
  local called = false
  local mt = {__add = function() called = true return 1 end}
  local t = setmetatable({}, mt)
  expectfail("table arg", A(t, 1), t_add2, {"number expected"})
  expect(not called, "a metamethod was called for a table operand")
end
-- cdata is not a number for the generated code.
do
  local ffi = require("ffi")
  local cd = ffi.new("int", 5)
  expectfail("cdata arg", A(cd, 1), t_add2, {"number expected"})
end

------------------------------------------------------------------------------
-- 2b. Anchored string and cdata constants (KSTR/KCDATA, ISEQS/ISNES).

local function t_str_x() return "x" end
local function t_str_empty() return "" end
local function t_str_nul() return "a\0b" end
local function t_str_esc() return "q\"b\\s\255\128\1z" end
local function t_str_eq(x) return x == "x" end
local function t_str_ne(x) return x ~= "x" end
local function t_str_eqif(x)
  if x == "x" then return "hit", "a\0b" end
  return "miss", ""
end

def(t_str_x) def(t_str_empty) def(t_str_nul) def(t_str_esc)
def(t_str_eq) def(t_str_ne) def(t_str_eqif)

check("kstr", t_str_x, {A()})
check("kstr empty", t_str_empty, {A()})
check("kstr nul", t_str_nul, {A()})
check("kstr escapes", t_str_esc, {A()})
check("iseqs", t_str_eq,
      {A("x"), A("y"), A("a\0b"), A(nil), A(false), A(0), A(0/0), A({})})
check("isnes", t_str_ne,
      {A("x"), A("y"), A("a\0b"), A(nil), A(false), A(0), A(0/0), A({})})
check("iseqs if", t_str_eqif, {A("x"), A("y"), A(nil), A(1)})

-- A long string constant must be copied byte for byte.
do
  local long = string.rep("abcdefghij", 512)		-- 5120 bytes
  local f = assert(load("return function() return "..
			format("%q", long).." end"))()
  jit.off(f, true)
  local fa = build(f)
  local r = fa()
  expect(type(r) == "string" and #r == #long and r == long,
	 "long string constant differs")
  ok()
end

-- cdata constants: loaded, stored and returned without conversion. Complex
-- values are not comparable across dumps, so check the parts instead.
do
  local ffi = require("ffi")
  local function c_i64() return 9223372036854775807LL end
  local function c_u64() return 18446744073709551615ULL end
  local function c_cpx() return 2i end
  def(c_i64) def(c_u64) def(c_cpx)
  local f_i64, f_u64, f_cpx = build(c_i64), build(c_u64), build(c_cpx)
  local v_i64, v_u64, v_cpx = f_i64(), f_u64(), f_cpx()
  expect(ffi.istype("int64_t", v_i64) and v_i64 == 9223372036854775807LL,
	 "int64 cdata constant")
  expect(ffi.istype("uint64_t", v_u64) and v_u64 == 18446744073709551615ULL,
	 "uint64 cdata constant")
  expect(ffi.istype("complex", v_cpx) and v_cpx.re == 0 and v_cpx.im == 2,
	 "complex cdata constant")
  collectgarbage("collect")
  collectgarbage("collect")
  expect(f_i64() == 9223372036854775807LL, "int64 constant after a full GC")
  expect(f_u64() == 18446744073709551615ULL, "uint64 constant after a full GC")
  local again = f_cpx()
  expect(again.re == 0 and again.im == 2, "complex constant after a full GC")
  ok()
end

-- A replaced upvalue with the same prototype shape but different constant
-- types must be reported, never dereferenced.
do
  local function retstr() return "x" end
  local other = function() return 2i end
  def(retstr)
  local fa = build(retstr)
  local a, b = jutil.funcinfo(retstr), jutil.funcinfo(other)
  for _, k in ipairs({"stackslots", "params", "bytecodes", "nconsts",
		      "gcconsts", "upvalues"}) do
    expect(a[k] == b[k], "prototype shapes differ in "..k)
  end
  expect(debug.setupvalue(fa, 1, other) ~= nil, "cannot replace the upvalue")
  local okc, msg = pcall(fa)
  expect(not okc and find(tostring(msg), "prototype mismatch", 1, true),
	 "wrong constant type did not fail cleanly: "..tostring(msg))
  ok()
end

------------------------------------------------------------------------------
-- 3. Arithmetic boundaries.

local function t_sub(a, b) return a - b end
local function t_mul(a, b) return a * b end
local function t_div(a, b) return a / b end
local function t_nvsub(a) return 1 - a end
local function t_nvdiv(a) return 2 / a end
local function t_unm(a) return -a end

def(t_sub) def(t_mul) def(t_div) def(t_nvsub) def(t_nvdiv) def(t_unm)

local MAX = 1073741824 + 1073741823		-- INT32_MAX as an integer.
local MIN = -MAX - 1				-- INT32_MIN as an integer.

check("add", t_add2, {A(MAX, 1), A(MIN, -1), A(1, -1), A(0.5, 0.25),
		      A(MAX, 0), A(MIN, 0), A(0.0, -0.0), A(1, 1.0)})
check("sub", t_sub, {A(MAX, -1), A(MIN, 1), A(1, 3), A(0.5, 2)})
check("mul", t_mul, {A(100000, 100000), A(65536, 65536), A(2, 3),
		     A(-3, 0.5), A(MIN, 1), A(MIN, -1)})
check("div", t_div, {A(1, 0), A(-1, 0), A(0, 0), A(1, 4), A(1.5, 0.5),
		     A(MAX, 2)})
check("nvsub", t_nvsub, {A(2), A(2.5), A(0), A(-0.0)})
check("nvdiv", t_nvdiv, {A(4), A(0), A(-0.0), A(0.5)})
check("unm", t_unm, {A(2), A(-2), A(0), A(-0.0), A(MIN), A(MAX), A(0.5),
		     A(1/0), A(-1/0)})
do
  local nan = 0/0
  callcmp("add nan", t_add2, build(t_add2), A(nan, 1))
  callcmp("mul nan", t_mul, build(t_mul), A(nan, 0))
  ok()
end
-- UNM of an integer zero must produce a float -0.0, not an integer zero.
do
  local f = build(t_unm)
  expect(1/f(0) < 0, "unm(0) did not produce -0.0")
  expect(1/f(-0.0) > 0, "unm(-0.0) did not produce +0.0")
  expect(f(MIN) == 2147483648, "unm(INT32_MIN) != 2^31")
  ok()
end

------------------------------------------------------------------------------
-- 3b. Modulo and power.

local function t_mod(a) return a % 2 end
local function t_pow(a) return a ^ 2 end
local function t_modvn(x) return x % 3 end
local function t_modnv(x) return 3 % x end
local function t_modvv(x, y) return x % y end
local function t_powvv(x, y) return x ^ y end
local function t_modip(x, y) x = x % y return x end
local function t_powip(x, y) x = x ^ y return x end

def(t_mod) def(t_pow)
def(t_modvn) def(t_modnv) def(t_modvv) def(t_powvv)
def(t_modip) def(t_powip)

local mod_ints = {A(5, 3), A(-5, 3), A(5, -3), A(-5, -3), A(0, 3), A(0, -3),
		  A(MAX, 3), A(MIN, 3), A(MIN, -1), A(MAX, MIN), A(1, 0), A(0, 0)}
local mod_reals = {A(5.5, 2), A(5, 2.5), A(-5.5, 2), A(5.5, -2),
		   A(-0.0, 3), A(0.0, 3), A(1, 0.0), A(1, -0.0), A(1, 1/0),
		   A(3, 1/0), A(-3, 1/0), A(1/0, 3), A(0/0, 3), A(1, 0/0)}
check("mod ints", t_modvv, mod_ints)
check("mod reals", t_modvv, mod_reals)
check("modvn", t_modvn, {A(5), A(-5), A(0), A(MAX), A(MIN), A(-0.0), A(5.5)})
check("modnv", t_modnv, {A(5), A(-5), A(2.5), A(0), A(-0.0)})
check("mod alias", t_modip, {A(7, 3), A(-7, 3), A(7.5, 2), A(-7.5, -2)})

local pow_args = {A(2, 3), A(2, -2), A(-2, 3), A(-2, 0.5), A(0, 0), A(0, 1),
		  A(-0.0, 3), A(0.0, -1), A(-0.0, -1), A(2.5, 2), A(2, 0.5),
		  A(1, 1/0), A(1, -1/0), A(0/0, 0), A(1/0, 0), A(10, 3)}
check("pow", t_powvv, pow_args)
check("pow alias", t_powip, {A(2, 3), A(2.5, 2), A(-2, 3)})
check("pow const", t_pow, {A(2), A(3), A(-2), A(1.5)})
check("mod const", t_mod, {A(5), A(-5), A(0), A(-0.0), A(5.5)})

do
  local fvn, fnv, fvv = build(t_modvn), build(t_modnv), build(t_modvv)
  expect(fvv(5, 3) == 2, "5%3")
  expect(fvv(-5, 3) == 1, "-5%3")
  expect(fvv(5, -3) == -1, "5%-3")
  expect(fvv(-5, -3) == -2, "-5%-3")
  expect(fvv(MIN, -1) == 0, "INT32_MIN%-1")
  expect(fvn(-0.0) == 0 and 1/fvn(-0.0) > 0, "(-0)%3 is not +0")
  local z = fvv(1, 0)
  expect(z ~= z, "1%0 is not NaN")
  local z2 = fvv(1, 1/0)
  expect(z2 ~= z2, "1%inf is not NaN")
  local z3 = fvv(1/0, 3)
  expect(z3 ~= z3, "inf%3 is not NaN")
  local nz = fnv(-0.0)
  expect(nz ~= nz, "3%-0 is not NaN")
  ok()
end

do
  local fp = build(t_powvv)
  expect(fp(2, 3) == 8, "2^3")
  expect(fp(2, -2) == 0.25, "2^-2")
  expect(fp(-2, 3) == -8, "(-2)^3")
  local q = fp(-2, 0.5)
  expect(q ~= q, "(-2)^0.5 is not NaN")
  expect(fp(0, 0) == 1, "0^0")
  local mz = fp(-0.0, 3)
  expect(mz == 0 and 1/mz < 0, "(-0)^3 is not -0")
  ok()
end

-- Type errors stay number-only and never call metamethods.
do
  local called = false
  local mt = {__mod = function() called = true return 1 end,
	      __pow = function() called = true return 1 end}
  local t = setmetatable({}, mt)
  expectfail("mod nil", A(nil, 1), t_modvv, {"number expected"})
  expectfail("mod string", A("2", 1), t_modvv, {"number expected"})
  expectfail("mod table", A(t, 1), t_modvv, {"number expected"})
  expectfail("pow boolean", A(true, 1), t_powvv, {"number expected"})
  expectfail("pow table", A(t, 1), t_powvv, {"number expected"})
  expect(not called, "a metamethod was called for mod/pow")
end

------------------------------------------------------------------------------
-- 4. Branch boundaries.

local function t_lt(a, b) if a < b then return 1 end return 0 end
local function t_ge(a, b) if a >= b then return 1 end return 0 end
local function t_le(a, b) if a <= b then return 1 end return 0 end
local function t_gt(a, b) if a > b then return 1 end return 0 end
local function t_eq(a, b) return a == b end
local function t_ne(a, b) return a ~= b end
local function t_eqconst(a)
  if a == 0.5 then return 1 end
  if a ~= 1 then return 2 end
  if a == nil then return 3 end
  if a == true then return 4 end
  return 5
end

def(t_lt) def(t_ge) def(t_le) def(t_gt) def(t_eq) def(t_ne) def(t_eqconst)

local NAN = 0/0
local pairs_to_test = {
  A(1, 2), A(2, 1), A(2, 2), A(1.5, 2.5), A(-0.0, 0.0), A(0, -0.0),
  A(1/0, 1), A(-1/0, 1), A(NAN, 0), A(0, NAN), A(NAN, NAN),
  A(MAX, MAX), A(MIN, MAX), A(1, 1.0),
}
check("lt", t_lt, pairs_to_test)
check("ge", t_ge, pairs_to_test)
check("le", t_le, pairs_to_test)
check("gt", t_gt, pairs_to_test)
check("eq", t_eq, pairs_to_test)
check("ne", t_ne, pairs_to_test)
check("eqconst", t_eqconst, {A(0.5), A(1), A(2), A(nil), A(true), A(false),
			     A(NAN), A(0), A(1.0)})
-- Source level `<` and `>=` are both false for NaN. ISGE/ISGT branch on
-- unordered, so a plain C >= or > translation would get this wrong.
do
  local lt, ge, le, gt = build(t_lt), build(t_ge), build(t_le), build(t_gt)
  expect(lt(NAN, 0) == 0 and lt(0, NAN) == 0, "`<` with NaN is not false")
  expect(ge(NAN, 0) == 0 and ge(0, NAN) == 0, "`>=` with NaN is not false")
  expect(le(NAN, 0) == 0 and le(0, NAN) == 0, "`<=` with NaN is not false")
  expect(gt(NAN, 0) == 0 and gt(0, NAN) == 0, "`>` with NaN is not false")
  ok()
end

------------------------------------------------------------------------------
-- 5. Numeric for loop boundaries.

local function t_forcnt(a, b, st)
  local n = 0
  for i = a, b, st do
    n = n + 1
    if n > 5 then break end		-- Guards zero-step loops.
  end
  return n
end
local function t_forlast(a, b)
  local n = 0 for i = a, b do n = n + 1 if n > 10 then break end end return n
end

def(t_forcnt) def(t_forlast)

local NEGZERO = -1/math.huge		-- -0.0 at run time.
expect(1/NEGZERO == -math.huge, "the -0.0 sample is not negative zero")

check("forcnt", t_forcnt, {
  A(1, 10, 1), A(1, 10, 2), A(10, 1, -1), A(10, 1, -3),
  A(1, 2, 0.5), A(2, 1, -0.5), A(1, 0, 1), A(0, 1, 1),
  A(1.5, 2.5, NEGZERO), A(2.5, 1.5, NEGZERO),
  A(1, 2, NEGZERO), A(1.0, 2.0, 0.0),
  A(0/0, 1, 1), A(1, 0/0, 1), A(1, 2, 0/0),
  A(1, 2.5, 1), A(1.5, 3, 1), A(1, 2, 2.0),
  A(MAX - 1, MAX, 1), A(MIN, MIN + 2, 1), A(MAX, MAX, 1),
  A(-1, 1, 2), A(5, 5, 1),
})
check("forlast", t_forlast, {A(1, 3), A(MAX - 2, MAX), A(MIN, MIN + 2),
			     A(MAX - 1, MAX), A(1, 0)})
-- Mixed representations are normalized like lj_meta_for: (1,2,-0.0) is
-- narrowed to an integer zero step, so it keeps going until the break.
do
  local fc = build(t_forcnt)
  expect(fc(1, 2, NEGZERO) == 6, "(1,2,-0.0) was not normalized to int 0")
  expect(fc(1.5, 2.5, NEGZERO) == 0, "(1.5,2.5,-0.0) must not loop")
  expect(fc(2.5, 1.5, NEGZERO) == 6, "(2.5,1.5,-0.0) must loop")
  local fl = build(t_forlast)
  expect(fl(MAX - 2, MAX) == 3, "int loop upper bound: not 3 iterations")
  ok()
end

------------------------------------------------------------------------------
-- 6. Rejection contract.

local function t_call(a) return a(1) end
local function t_tabnew() local t = {} return t end
local function t_tabget(a) return a.x end
local function t_glob() return math.pi end
local function t_concat(a) return "x"..a end
local function t_len(a) return #a end
local function t_bit(a) return a | 1 end
local function t_vararg(...) return ... end
local function t_child()
  local x = 1
  local function inner() return x end
  return x
end
local function t_upval()
  local a = 1
  local function get() return a end
  return get
end
local function t_typed(a) local x = a.x return x end
local function t_closure(a)
  local t = {}
  local function f() return a end
  t.g = f
  return t.g
end
local function t_iter(t)
  local s = 0
  for k, v in next, t do s = s + v end
  return s
end

local reject = {
  {"call", t_call}, {"table constructor", t_tabnew},
  {"table access", t_tabget}, {"global access", t_glob},
  {"concatenation", t_concat},
  {"length", t_len}, {"bitwise", t_bit}, {"vararg", t_vararg},
  {"child prototype", t_child}, {"upvalue", t_upval},
  {"table access 2", t_typed}, {"nested function", t_closure},
  {"iterator", t_iter},
}
for i = 1, #reject do
  local label, func = reject[i][1], reject[i][2]
  local okc, msg = pcall(aot.compile, func, "rej"..i)
  if okc then fatal(label..": compile() accepted an unsupported function") end
  msg = tostring(msg)
  if not find(msg, "AOT", 1, true) then
    fatal(label..": error does not mention AOT: "..msg)
  end
  if not (find(msg, "rejected:", 1, true) or find(msg, "pc=", 1, true)) then
    fatal(label..": error does not locate the reason: "..msg)
  end
  ntest = ntest + 1
end
-- Compile errors for rejected opcodes carry pc and opcode.
do
  local _, msg2 = pcall(aot.compile, t_call, "rejcall")
  msg2 = tostring(msg2)
  expect(find(msg2, "CALL", 1, true) != nil,
	 "rejected call error lacks the opcode: "..msg2)
end
-- Non-function and bad module names.
do
  local okc, msg = pcall(aot.compile, print, "b1")
  expect(not okc and find(tostring(msg), "AOT", 1, true),
	 "compile() accepted a C function")
  okc, msg = pcall(aot.compile, t_sum, "bad-name")
  expect(not okc and find(tostring(msg), "AOT", 1, true),
	 "compile() accepted a non-identifier module name")
  okc, msg = pcall(aot.compile, t_sum, nil)
  expect(not okc, "compile() accepted a missing module name")
end
-- A function which was executed and JIT compiled still compiles from the
-- normalized dump.
do
  jit.on()
  local function hot(n) local s = 0 for i = 1, n do s = s + i end return s end
  for i = 1, 200 do hot(1000) end
  jit.off()
  expect(hot(1000) == 500500, "hot loop result changed")
  local f = build(hot)
  expect(f(1000) == 500500, "AOT of a JIT-ed function gives a wrong result")
  ok()
end

------------------------------------------------------------------------------
-- 7. Slot handling, GC and upvalue hardening.

local function t_retA(a)
  local x = a + 1
  local y = a + 2
  return x, y
end
local function t_repeatcall(a) return a * 2 + 1 end

def(t_retA) def(t_repeatcall)

check("retA", t_retA, {A(0), A(1.5), A(-1)})
do
  local fa = build(t_retA)
  local fb = build(t_repeatcall)
  for i = 1, 2000 do
    local x, y = fa(i)
    if x ~= i + 1 or y ~= i + 2 then
      fatal("repeated call "..i.." gives a wrong result")
    end
    if fb(i) ~= i * 2 + 1 then fatal("repeated call (2) is wrong") end
  end
  -- Full GC must not collect the anchored prototype or its constants.
  collectgarbage("collect")
  collectgarbage("collect")
  local x, y = fa(5)
  expect(x == 6 and y == 7, "wrong result after a full GC")
  -- Missing arguments are filled with nil, extra arguments are ignored.
  local okc, msg = pcall(fa)
  expect(not okc and find(tostring(msg), "number expected", 1, true),
	 "missing parameter did not fail at the numeric operation")
  ok()
end
-- Constants stay exact after a GC: a KNUM constant is copied from the
-- anchored prototype, not recreated from a C literal.
do
  local function frac(a) return a * 0.5 + 1.25 end
  local function kzero(a) return a * -0.0 end
  def(frac) def(kzero)
  local f = build(frac)
  collectgarbage("collect")
  expect(f(3) == 2.75, "fractional constant result is wrong")
  expect(f(-2.5) == 0 and 1/f(-2.5) > 0, "-1.25+1.25 is not +0.0")
  local k = build(kzero)
  collectgarbage("collect")
  expect(k(1) == 0 and 1/k(1) < 0, "the -0.0 constant lost its sign")
  ok()
end
-- Tampering with the internal upvalue must fail cleanly.
do
  local faut = build(t_add2)
  expect(debug.setupvalue(faut, 1, 42) ~= nil, "cannot replace the upvalue")
  local okc, msg = pcall(faut, 1, 2)
  expect(not okc and find(tostring(msg), "invalid prototype upvalue", 1, true),
	 "non-function upvalue did not fail cleanly: "..tostring(msg))
  local other = function(a) return a end
  jit.off(other, true)
  expect(debug.setupvalue(faut, 1, other) ~= nil, "cannot replace the upvalue")
  okc, msg = pcall(faut, 1, 2)
  expect(not okc and find(tostring(msg), "prototype mismatch", 1, true),
	 "mismatched prototype did not fail cleanly: "..tostring(msg))
  ok()
end

------------------------------------------------------------------------------
-- 8. Real command line smoke test.

local nfactory = 0
local function factory(body)
  nfactory = nfactory + 1
  local path = TMP.."/in"..nfactory..".lua"
  local fh = assert(io.open(path, "wb"))
  assert(fh:write(body))
  assert(fh:close())
  return path
end

-- Run a shell command and return (success, combined output). io.popen's
-- close() does not report the exit status in this LuaJIT build, so the
-- command status comes from os.execute and the output goes to a file.
local nlog = 0
local function runcapture(cmd)
  nlog = nlog + 1
  local log = TMP.."/cmd"..nlog..".log"
  local okc = run(cmd.." >"..shellquote(log).." 2>&1")
  local fh = io.open(log, "rb")
  local out = fh and fh:read("*a") or ""
  if fh then fh:close() end
  return okc, out
end

local function cli(args)
  local cmd = format("LUA_PATH=%s %s -joff %s",
		     shellquote(INC.."/?.lua;;"), LUAJIT, args)
  return runcapture(cmd)
end

local function ccompile(cfile, sofile)
  local link = jit.os == "OSX" and "-bundle -undefined dynamic_lookup" or "-shared"
  local cmd = format("%s -O2 -std=c99 -fPIC -ffp-contract=off -I%s %s %s -o %s -lm",
		     CC, INC, link, shellquote(cfile), shellquote(sofile))
  local okc, out = runcapture(cmd)
  return okc, out, cmd
end

do
  local input = factory("return function(n) local s=0 for i=1,n do s=s+i end return s end")
  local cfile = TMP.."/clisum.c"
  local sofile = TMP.."/clisum.so"
  local okc, out = cli(format("-jaot=clisum,%s,%s", shellquote(input),
			      shellquote(cfile)))
  expect(okc, "-jaot= failed: "..out)
  local compiled, cout, ccmd = ccompile(cfile, sofile)
  expect(compiled, "C compilation of the CLI output failed\n"..ccmd.."\n"..cout)
  local ldr = assert(package.loadlib(sofile, "luaopen_clisum"))
  local f = ldr()
  expect(debug.getinfo(f).what == "C", "the module is not a C function")
  expect(f(100) == 5050, "CLI generated module returns the wrong result")
  local okc2, msg = pcall(f, nil)
  expect(not okc2 and find(tostring(msg), "number expected", 1, true),
	 "type error was not reported")
  expect(f(10) == 55, "the module does not work again after an error")
  ok()

  -- Undefined symbols of a real module: public Lua C API and the runtime
  -- only, never private lj_* internals.
  local p = io.popen("nm -u "..shellquote(sofile).." 2>/dev/null")
  local undef = p and p:read("*a") or ""
  if p then p:close() end
  local bad = {}
  for sym in undef:gmatch("%S+") do
    if sym:find("lj_", 1, true) then bad[#bad+1] = sym end
  end
  expect(#bad == 0, "module references private VM symbols: "..
	 table.concat(bad, " "))
  ntest = ntest + 1

  -- A failing factory must not create or truncate the output file.
  local badinput = factory("return 42")
  local sentinel = TMP.."/sentinel.c"
  local sh = assert(io.open(sentinel, "wb"))
  sh:write("PRESERVE")
  sh:close()
  local okbad, outbad = cli(format("-jaot=clisum,%s,%s",
				   shellquote(badinput), shellquote(sentinel)))
  expect(not okbad, "a factory returning a non-function was accepted")
  expect(find(outbad, "AOT", 1, true), "no AOT error for a bad factory: "..outbad)
  local rf = assert(io.open(sentinel, "rb"))
  local content = rf:read("*a")
  rf:close()
  expect(content == "PRESERVE", "the output file was truncated on failure")
  local missing = TMP.."/notcreated.c"
  local okbad2 = cli(format("-jaot=clisum,%s,%s", shellquote(badinput),
			    shellquote(missing)))
  expect(not okbad2, "a factory returning a non-function was accepted (2)")
  expect(not io.open(missing, "rb"), "the output file was created on failure")

  -- A factory with a syntax error or a runtime error is reported.
  local synin = factory("return function( end")
  local ok3, out3 = cli(format("-jaot=clisum,%s,%s", shellquote(synin),
			       shellquote(missing)))
  expect(not ok3 and find(out3, "AOT", 1, true),
	 "a factory with a syntax error was not rejected: "..out3)
  expect(not io.open(missing, "rb"), "output created for a syntax error")
  local raisein = factory("error('factory boom')")
  local ok4, out4 = cli(format("-jaot=clisum,%s,%s", shellquote(raisein),
			       shellquote(missing)))
  expect(not ok4 and find(out4, "AOT", 1, true),
	 "a factory raising an error was not rejected: "..out4)
  expect(not io.open(missing, "rb"), "output created for a raising factory")
  -- An unwritable output file propagates instead of reporting success.
  local ok5, out5 = cli(format("-jaot=clisum,%s,%s", shellquote(input),
			       shellquote(TMP.."/nodir/out.c")))
  expect(not ok5 and find(out5, "AOT", 1, true),
	 "an unwritable output file was not reported: "..out5)
  -- Wrong argument count is rejected.
  local ok6, out6 = cli(format("-jaot=clisum,%s", shellquote(input)))
  expect(not ok6 and find(out6, "AOT", 1, true),
	 "a missing output argument was accepted: "..out6)
  ok()
end

------------------------------------------------------------------------------

cleanup()
print(format("OK: %d AOT checks passed", ntest))

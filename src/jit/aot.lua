----------------------------------------------------------------------------
-- LuaJIT simple ahead-of-time compiler for a numeric subset.
--
-- Copyright (C) 2005-2026 Mike Pall. All rights reserved.
-- Released under the MIT license. See Copyright Notice in luajit.h
----------------------------------------------------------------------------
--
-- This module compiles a Lua function to C source code, which can be
-- compiled offline and loaded as a normal C module. Fixed bytecode with
-- fixed operands is translated one-to-one, so the generated code has no
-- opcode dispatch loop. All values still use the LuaJIT TValue
-- representation and numeric operations keep their runtime type checks.
--
-- Only a small numeric subset is supported (see the opcode whitelist in
-- this file). Anything else is rejected with an "AOT:" error instead of
-- being emulated approximately.
--
-- The generated module is tied to the source tree, architecture and
-- configuration it was generated for (LJ_GC64/LJ_FR2/LJ_DUALNUM and
-- friends) and must be linked against the same LuaJIT build.
--
-- Example usage:
--
--   local aot = require("jit.aot")
--   local csrc = aot.compile(func, "mymod")
--
--   luajit -jaot=mymod,input.lua,output.c
--
-- The command line input is a trusted Lua factory file which is executed
-- once and must return exactly one function:
--
--   return function(n) local s = 0; for i=1,n do s=s+i end; return s end
--
-- Paths passed to -jaot= must not contain commas (jit.* option arguments
-- are comma separated). The factory's top level code runs at compile time,
-- so it must not come from untrusted input.
--
------------------------------------------------------------------------------

local jutil = require("jit.util")
local vmdef = require("jit.vmdef")
local format, sub, match = string.format, string.sub, string.match
local byte = string.byte

local funcinfo, funcbc = jutil.funcinfo, jutil.funcbc
local bcnames = vmdef.bcnames

------------------------------------------------------------------------------

local function fail(msg)
  error("AOT: "..msg, 0)
end

-- Opcode numbers and names, indexed by the BCDEF order in lj_bc.h.
local OPIDX, OPNAME = {}, {}
for i = 0, 255 do
  local name = match(sub(bcnames, 6*i+1, 6*i+6), "^%a+")
  if not name then break end
  OPIDX[name] = i
  OPNAME[i] = name
end

-- Opcodes which branch through the JMP instruction that follows them.
local COND = {
  ISLT = true, ISGE = true, ISLE = true, ISGT = true,
  ISEQV = true, ISNEV = true, ISEQN = true, ISNEN = true,
  ISEQP = true, ISNEP = true,
  ISTC = true, ISFC = true, IST = true, ISF = true,
}

-- Ordered comparisons: condition expression is "fn(x, y)" or its negation.
-- ISGE/ISGT branch on unordered too, so they cannot use C's >= or >.
local CMP = {
  ISLT = {"aot_lt", false}, ISGE = {"aot_lt", true},
  ISLE = {"aot_le", false}, ISGT = {"aot_le", true},
  ISEQV = {"aot_eqv", false}, ISNEV = {"aot_eqv", true},
  ISEQN = {"aot_eqn", false}, ISNEN = {"aot_eqn", true},
  ISEQP = {"aot_eqp", false}, ISNEP = {"aot_eqp", true},
}

-- Arithmetic opcodes: {helper, operand layout}.
local ARITH = {
  ADDVN = {"aot_add", "VN"}, ADDNV = {"aot_add", "NV"}, ADDVV = {"aot_add", "VV"},
  SUBVN = {"aot_sub", "VN"}, SUBNV = {"aot_sub", "NV"}, SUBVV = {"aot_sub", "VV"},
  MULVN = {"aot_mul", "VN"}, MULNV = {"aot_mul", "NV"}, MULVV = {"aot_mul", "VV"},
  DIVVN = {"aot_div", "VN"}, DIVNV = {"aot_div", "NV"}, DIVVV = {"aot_div", "VV"},
}

-- Reasons for rejecting an opcode. Anything not whitelisted is rejected.
local NOSUPPORT = {
  ISEQS = "string comparison", ISNES = "string comparison",
  MODVN = "modulo operator", MODNV = "modulo operator", MODVV = "modulo operator",
  POW = "power operator", CAT = "string concatenation", LEN = "length operator",
  BNOT = "bitwise operator", BAND = "bitwise operator", BOR = "bitwise operator",
  BXOR = "bitwise operator", BSHL = "bitwise operator", BSHR = "bitwise operator",
  BSAR = "bitwise operator",
  KSTR = "string constant", KCDATA = "cdata constant",
  TNEW = "table constructor", TDUP = "table constructor",
  GGET = "global access", GSET = "global access",
  UGET = "upvalue access", USETV = "upvalue access", USETS = "upvalue access",
  USETN = "upvalue access", USETP = "upvalue access", UCLO = "upvalue close",
  FNEW = "nested function",
  CALL = "function call", CALLM = "function call", CALLMT = "function call",
  CALLT = "function call", RETM = "variable number of results",
  VARG = "vararg function", ISNEXT = "generic for loop",
  ITERC = "generic for loop", ITERN = "generic for loop",
  ITERL = "generic for loop", IITERL = "generic for loop",
  JITERL = "generic for loop",
  TGETV = "table access", TGETS = "table access", TGETB = "table access",
  TGETR = "table access", TSETV = "table access", TSETS = "table access",
  TSETB = "table access", TSETM = "table access", TSETR = "table access",
  ISTYPE = "type assertion", ISNUM = "type assertion",
  FUNCF = "function header", IFUNCF = "patched bytecode",
  JFUNCF = "patched bytecode", FUNCV = "function header",
  IFUNCV = "patched bytecode", JFUNCV = "patched bytecode",
  FUNCC = "C function", FUNCCW = "C function",
  JFORI = "patched bytecode", JFORL = "patched bytecode",
  JLOOP = "patched bytecode",
}

local function reason(name)
  return NOSUPPORT[name] or "not in the supported subset"
end

-- The complete whitelist. Nothing is enabled implicitly.
local SUPPORTED = {
  JMP = true, LOOP = true, ILOOP = true,
  MOV = true, KSHORT = true, KNUM = true, KPRI = true, KNIL = true,
  NOT = true, UNM = true,
  FORI = true, FORL = true, IFORL = true,
  RET = true, RET0 = true, RET1 = true,
}
for name in pairs(COND) do SUPPORTED[name] = true end
for name in pairs(ARITH) do SUPPORTED[name] = true end

------------------------------------------------------------------------------

-- Generated C support code. Self-contained helpers only: public Lua C API
-- plus inline TValue accessors from lj_obj.h, no private lj_* symbols.
local CHELPERS = [==[
#include <stddef.h>
#include <stdint.h>

#include "lua.h"
#include "lauxlib.h"
#include "lj_obj.h"

/* The per-opcode helpers are short sequences: one type check, one number
** operation, one slot store. They are forced into the compiled function,
** because a call per bytecode instruction would dominate a loop. Type
** errors are cold. Every operand combination is spelled out, so the hot
** paths carry no shared conversion code.
*/
#if defined(_MSC_VER)
#define AOT_INLINE	static __forceinline
#define AOT_LIKELY(x)	(x)
#define AOT_UNLIKELY(x)	(x)
#define AOT_COLD
#define AOT_UNUSED
#elif defined(__GNUC__)
#define AOT_INLINE	static __inline__ __attribute__((always_inline, unused))
#define AOT_LIKELY(x)	__builtin_expect(!!(x), 1)
#define AOT_UNLIKELY(x)	__builtin_expect(!!(x), 0)
#define AOT_COLD	__attribute__((cold))
#define AOT_UNUSED	__attribute__((unused))
#else
#define AOT_INLINE	static inline
#define AOT_LIKELY(x)	(x)
#define AOT_UNLIKELY(x)	(x)
#define AOT_COLD
#define AOT_UNUSED
#endif

/* Type error: one cold call site per bytecode, never on the hot path. */
static AOT_COLD AOT_UNUSED int aot_errnum(lua_State *L, int pc, const char *op)
{
  return luaL_error(L, "AOT: pc=%d %s: number expected", pc, op);
}

/* Exact double to int32_t conversion, like lj_vm_num2int_check().
** Rejects NaN, infinities and out-of-range values without undefined
** behavior; accepts -0 like the VM does.
*/
AOT_INLINE int aot_num2int(lua_Number x, int32_t *ip)
{
  int32_t i;
  if (!(x >= -2147483648.0 && x < 2147483648.0)) return 0;
  i = (int32_t)(int64_t)x;
  if ((lua_Number)i != x) return 0;
  *ip = i;
  return 1;
}

/* Sign bit of a number, including -0.0 (the VM tests the sign bit, too). */
AOT_INLINE int aot_stepneg(cTValue *v)
{
  return (int)(v->u64 >> 63);
}

/* Truthiness: only nil and false are false. */
AOT_INLINE int aot_true(cTValue *v)
{
  return tvistruecond(v);
}

/* Two shapes per arithmetic opcode, both with per-operand runtime checks:
**  - both doubles: straight-line, no conversion, one not-taken test;
**  - anything else (ints, mixed, non-numbers, errors): cold, out of line,
**    so its code never enters the loop's instruction window.
*/
#define AOT_ARITH_SLOW(name, OP) \
static AOT_COLD AOT_UNUSED void name(lua_State *L, TValue *dst, cTValue *x, \
				     cTValue *y, int pc, const char *op, \
				     uint32_t xt, uint32_t yt) \
{ \
  if (AOT_UNLIKELY(xt > LJ_TISNUM || yt > LJ_TISNUM)) { \
    aot_errnum(L, pc, op); \
    return; \
  } \
  if (xt == LJ_TISNUM && yt == LJ_TISNUM) { \
    int64_t r = (int64_t)intV(x) OP (int64_t)intV(y); \
    if (r >= -2147483648LL && r <= 2147483647LL) { \
      setintV(dst, (int32_t)r); \
    } else { \
      setnumV(dst, (lua_Number)intV(x) OP (lua_Number)intV(y)); \
    } \
  } else if (xt == LJ_TISNUM) { \
    setnumV(dst, (lua_Number)intV(x) OP numV(y)); \
  } else if (yt == LJ_TISNUM) { \
    setnumV(dst, numV(x) OP (lua_Number)intV(y)); \
  } else { \
    setnumV(dst, numV(x) OP numV(y)); \
  } \
}

/* Fast path for two genuine doubles. A TValue that is not a double (int,
** nil, boolean, GC reference, ...) has a NaN bit pattern when read as a
** double, so the self-comparison identifies it without loading the tag into
** an integer register. NaN doubles are also not self-equal, so they take the
** slow path, which applies the normal tag logic and result.
*/
#define AOT_ARITH_FAST(name, slowdown, OP) \
AOT_INLINE void name(lua_State *L, TValue *dst, cTValue *x, cTValue *y, \
		     int pc, const char *op) \
{ \
  lua_Number a = x->n, b = y->n; \
  if (AOT_LIKELY(a == a && b == b)) { \
    setnumV(dst, a OP b); \
    return; \
  } \
  slowdown(L, dst, x, y, pc, op, itype(x), itype(y)); \
}

AOT_ARITH_SLOW(aot_add_slow, +)
AOT_ARITH_SLOW(aot_sub_slow, -)
AOT_ARITH_SLOW(aot_mul_slow, *)
AOT_ARITH_FAST(aot_add, aot_add_slow, +)
AOT_ARITH_FAST(aot_sub, aot_sub_slow, -)
AOT_ARITH_FAST(aot_mul, aot_mul_slow, *)

static AOT_COLD AOT_UNUSED void aot_div_slow(lua_State *L, TValue *dst,
					     cTValue *x, cTValue *y, int pc,
					     const char *op, uint32_t xt, uint32_t yt)
{
  if (AOT_UNLIKELY(xt > LJ_TISNUM || yt > LJ_TISNUM)) {
    aot_errnum(L, pc, op);
    return;
  }
  if (xt == LJ_TISNUM && yt == LJ_TISNUM) {
    setnumV(dst, (lua_Number)intV(x) / (lua_Number)intV(y));
  } else if (xt == LJ_TISNUM) {
    setnumV(dst, (lua_Number)intV(x) / numV(y));
  } else if (yt == LJ_TISNUM) {
    setnumV(dst, numV(x) / (lua_Number)intV(y));
  } else {
    setnumV(dst, numV(x) / numV(y));
  }
}

/* Division is always a double, like the VM (no int fast path). */
AOT_INLINE void aot_div(lua_State *L, TValue *dst, cTValue *x, cTValue *y,
			int pc, const char *op)
{
  lua_Number a = x->n, b = y->n;
  if (AOT_LIKELY(a == a && b == b)) {
    setnumV(dst, a / b);
    return;
  }
  aot_div_slow(L, dst, x, y, pc, op, itype(x), itype(y));
}

AOT_INLINE void aot_unm(lua_State *L, TValue *dst, cTValue *src,
			int pc, const char *op)
{
  uint32_t t = itype(src);
#if LJ_DUALNUM
  if (t == LJ_TISNUM) {
    int32_t i = intV(src);
    if (i == 0) {
      setnumV(dst, -0.0);		/* Integer zero negates to a float -0.0. */
    } else if (i == -2147483647-1) {
      setnumV(dst, 2147483648.0);	/* INT32_MIN has no int result. */
    } else {
      setintV(dst, -i);
    }
    return;
  }
#endif
  if (AOT_UNLIKELY(t > LJ_TISNUM)) { aot_errnum(L, pc, op); return; }
  setnumV(dst, -numV(src));
}

#define AOT_CMP_SLOW(name, OP) \
static AOT_COLD AOT_UNUSED int name(lua_State *L, cTValue *x, cTValue *y, \
				    int pc, const char *op, uint32_t xt, uint32_t yt) \
{ \
  if (AOT_UNLIKELY(xt > LJ_TISNUM || yt > LJ_TISNUM)) { \
    aot_errnum(L, pc, op); \
    return 0; \
  } \
  if (xt == LJ_TISNUM && yt == LJ_TISNUM) return intV(x) OP intV(y); \
  if (xt == LJ_TISNUM) return (lua_Number)intV(x) OP numV(y); \
  if (yt == LJ_TISNUM) return numV(x) OP (lua_Number)intV(y); \
  return numV(x) OP numV(y); \
}
AOT_CMP_SLOW(aot_lt_slow, <)
AOT_CMP_SLOW(aot_le_slow, <=)

AOT_INLINE int aot_lt(lua_State *L, cTValue *x, cTValue *y, int pc,
		      const char *op)
{
  lua_Number a = x->n, b = y->n;
  if (AOT_LIKELY(a == a && b == b)) return a < b;
  return aot_lt_slow(L, x, y, pc, op, itype(x), itype(y));
}

AOT_INLINE int aot_le(lua_State *L, cTValue *x, cTValue *y, int pc,
		      const char *op)
{
  lua_Number a = x->n, b = y->n;
  if (AOT_LIKELY(a == a && b == b)) return a <= b;
  return aot_le_slow(L, x, y, pc, op, itype(x), itype(y));
}

/* Numeric equality, otherwise raw value equality (no metamethods). */
AOT_INLINE int aot_eqv(cTValue *x, cTValue *y)
{
  uint32_t xt = itype(x), yt = itype(y);
  if (xt <= LJ_TISNUM && yt <= LJ_TISNUM) {
    if (xt < LJ_TISNUM && yt < LJ_TISNUM) return x->n == y->n;
    if (xt == LJ_TISNUM && yt == LJ_TISNUM) return intV(x) == intV(y);
    if (xt == LJ_TISNUM) return (lua_Number)intV(x) == numV(y);
    return numV(x) == (lua_Number)intV(y);
  }
  return x->u64 == y->u64;
}

AOT_INLINE int aot_eqp(cTValue *x, int p)
{
  return p == 0 ? (int)tvisnil(x) : p == 1 ? (int)tvisfalse(x) :
	 (int)tvistrue(x);
}

/* FORI: type-check and normalize index/limit/step like lj_meta_for() does
** for mixed number representations, then check the loop direction.
** Returns 1 if the loop body runs. A+3 always gets the visible variable.
*/
AOT_INLINE int aot_fori(lua_State *L, TValue *o, int pc, const char *op)
{
  uint32_t t0 = itype(o), t1 = itype(o+1), t2 = itype(o+2);
  if (AOT_UNLIKELY(t0 > LJ_TISNUM || t1 > LJ_TISNUM || t2 > LJ_TISNUM))
    return aot_errnum(L, pc, op);
#if LJ_DUALNUM
  {
    int32_t k[3];
    int nint = 0;
    if (t0 == LJ_TISNUM) { k[0] = intV(o); nint++; }
    else if (aot_num2int(numV(o), &k[0])) nint++;
    if (t1 == LJ_TISNUM) { k[1] = intV(o+1); nint++; }
    else if (aot_num2int(numV(o+1), &k[1])) nint++;
    if (t2 == LJ_TISNUM) { k[2] = intV(o+2); nint++; }
    else if (aot_num2int(numV(o+2), &k[2])) nint++;
    if (nint == 3) {			/* Narrow to integers. */
      setintV(o, k[0]);
      setintV(o+1, k[1]);
      setintV(o+2, k[2]);
    } else if (nint != 0) {		/* Widen to numbers. */
      if (t0 == LJ_TISNUM) setnumV(o, (lua_Number)intV(o));
      if (t1 == LJ_TISNUM) setnumV(o+1, (lua_Number)intV(o+1));
      if (t2 == LJ_TISNUM) setnumV(o+2, (lua_Number)intV(o+2));
    }
  }
#endif
  if (itype(o) == LJ_TISNUM) {
    int32_t idx = intV(o), lim = intV(o+1), stp = intV(o+2);
    setintV(o+3, idx);
    return stp >= 0 ? idx <= lim : idx >= lim;
  } else {
    lua_Number idx = numV(o), lim = numV(o+1);
    setnumV(o+3, idx);
    return aot_stepneg(o+2) ? idx >= lim : idx <= lim;
  }
}

/* FORL/IFORL: increment, write back, then decide whether to loop back.
** All slots have the same number representation (the loop body cannot write
** A..A+2), so this is one number check plus one representation check.
** Integer overflow exits the loop without writing back, like the VM.
*/
AOT_INLINE int aot_forl(lua_State *L, TValue *o, int pc, const char *op)
{
  uint32_t t0 = itype(o), t1 = itype(o+1), t2 = itype(o+2);
  int isint = (t0 == LJ_TISNUM);
  if (AOT_UNLIKELY(t0 > LJ_TISNUM || t1 > LJ_TISNUM || t2 > LJ_TISNUM ||
		   (t1 == LJ_TISNUM) != isint ||
		   (t2 == LJ_TISNUM) != isint))
    return aot_errnum(L, pc, op);
  if (isint) {
    int64_t nidx = (int64_t)intV(o) + (int64_t)intV(o+2);
    int32_t lim = intV(o+1), stp = intV(o+2);
    if (AOT_UNLIKELY(nidx < -2147483648LL || nidx > 2147483647LL)) return 0;
    setintV(o, (int32_t)nidx);
    setintV(o+3, (int32_t)nidx);
    return stp >= 0 ? (int32_t)nidx <= lim : (int32_t)nidx >= lim;
  } else {
    lua_Number nidx = numV(o) + numV(o+2), lim = numV(o+1);
    setnumV(o, nidx);
    setnumV(o+3, nidx);
    return aot_stepneg(o+2) ? nidx >= lim : nidx <= lim;
  }
}
]==]

------------------------------------------------------------------------------

-- Decode and validate the bytecode of a dumped prototype copy.
local function decode(func, fi)
  local nbc = fi.bytecodes
  local ins = {}
  for pc = 1, nbc-1 do
    local i = funcbc(func, pc)
    if not i then
      fail(format("pc=%d: cannot read bytecode (bytecodes=%d)", pc, nbc))
    end
    local op = i & 0xff
    local name = OPNAME[op]
    if not name then fail(format("pc=%d: unknown opcode %d", pc, op)) end
    if not SUPPORTED[name] then
      fail(format("pc=%d %s: unsupported opcode (%s)", pc, name, reason(name)))
    end
    local t = {
      name = name,
      a = (i >> 8) & 0xff,
      b = (i >> 24) & 0xff,
      c = (i >> 16) & 0xff,
      d = (i >> 16) & 0xffff,
    }
    t.sd = t.d >= 32768 and t.d - 65536 or t.d	-- BCMlits
    if name == "JMP" or name == "FORI" or name == "FORL" or
       name == "IFORL" then
      t.target = pc + t.d - 0x7fff		-- BCBIAS_J
      if t.target < 1 or t.target >= nbc then
	fail(format("pc=%d %s: jump target %d out of range (bytecodes=%d)",
		    pc, name, t.target, nbc))
      end
    end
    ins[pc] = t
  end
  -- Condition opcodes read the displacement of the JMP that follows them.
  for pc = 1, nbc-1 do
    local t = ins[pc]
    if COND[t.name] then
      local j = ins[pc+1]
      if not j or j.name ~= "JMP" then
	fail(format("pc=%d %s: not followed by JMP", pc, t.name))
      end
      t.jt = j.target
    end
  end
  return ins, nbc
end

-- Successors of an instruction, for reachability checking.
local function successors(t, pc)
  local name = t.name
  if COND[name] then
    return {t.jt, pc+2}
  elseif name == "JMP" then
    return {t.target}
  elseif name == "RET" or name == "RET0" or name == "RET1" then
    return {}
  elseif name == "FORI" or name == "FORL" or name == "IFORL" then
    return {pc+1, t.target}
  else
    return {pc+1}
  end
end

------------------------------------------------------------------------------

-- Emit the C body: one braced block and label per bytecode instruction.
local function gen_body(ins, nbc, fi, usekb)
  local framesize, sizekn = fi.stackslots, fi.nconsts
  local out = {}
  local function w(s) out[#out+1] = s end

  local function slotok(x, pc, what)
    if x < 0 or x >= framesize then
      fail(format("pc=%d %s: slot %d out of range (framesize=%d)",
		  pc, what, x, framesize))
    end
  end

  -- Constant operands: the pool base is cached once per call when there
  -- are numeric constants (the prototype is anchored and cannot change).
  local function K(i)
    if usekb then return format("(kb+%d)", i) end
    return format("proto_knumtv(pt, %d)", i)
  end

  local function knok(x, pc, what)
    if x >= sizekn then
      fail(format("pc=%d %s: constant index %d out of range (nconsts=%d)",
		  pc, what, x, sizekn))
    end
  end

  -- Out-of-range edges are only legal if they are unreachable (checked
  -- by the caller). They end up in the error label past the last ins.
  local function G(p)
    if p >= 1 and p <= nbc-1 then return format("goto bc_%d;", p) end
    return format("goto bc_%d;", nbc)
  end

  for pc = 1, nbc-1 do
    local t = ins[pc]
    local name, a, b, c, d = t.name, t.a, t.b, t.c, t.d
    w(format("bc_%d: { /* %d %s */", pc, pc, name))
    if COND[name] then
      local jt = G(t.jt)
      if name == "ISTC" or name == "ISFC" then
	slotok(a, pc, name)
	slotok(d, pc, name)
	local cond = name == "ISTC" and format("aot_true(base+%d)", d) or
		     format("!aot_true(base+%d)", d)
	w(format("if (%s) { base[%d] = base[%d]; %s }", cond, a, d, jt))
      elseif name == "IST" or name == "ISF" then
	slotok(d, pc, name)
	local cond = name == "IST" and format("aot_true(base+%d)", d) or
		     format("!aot_true(base+%d)", d)
	w(format("if (%s) %s", cond, jt))
      else
	local cmp = CMP[name]
	local expr
	if cmp[1] == "aot_eqn" then
	  slotok(a, pc, name)
	  knok(d, pc, name)
	  expr = format("aot_eqv(base+%d, %s)", a, K(d))
	elseif cmp[1] == "aot_eqp" then
	  slotok(a, pc, name)
	  if d > 2 then
	    fail(format("pc=%d %s: bad primitive %d", pc, name, d))
	  end
	  expr = format("aot_eqp(base+%d, %d)", a, d)
	elseif cmp[1] == "aot_eqv" then
	  slotok(a, pc, name)
	  slotok(c, pc, name)
	  expr = format("aot_eqv(base+%d, base+%d)", a, c)
	else
	  slotok(a, pc, name)
	  slotok(c, pc, name)
	  expr = format("%s(L, base+%d, base+%d, %d, \"%s\")",
			cmp[1], a, c, pc, name)
	end
	if cmp[2] then expr = "!"..expr end
	w(format("if (%s) %s", expr, jt))
      end
      w(G(pc+2))
    elseif name == "JMP" then
      w(G(t.target))
    elseif name == "LOOP" or name == "ILOOP" then
      -- Execution hint for the trace compiler and exit extent. No jump.
      w(G(pc+1))
    elseif name == "MOV" then
      slotok(a, pc, name)
      slotok(d, pc, name)
      w(format("base[%d] = base[%d];", a, d))
      w(G(pc+1))
    elseif name == "KSHORT" then
      slotok(a, pc, name)
      w(format("setintV(base+%d, %d);", a, t.sd))
      w(G(pc+1))
    elseif name == "KNUM" then
      slotok(a, pc, name)
      knok(d, pc, name)
      w(format("base[%d] = *%s;", a, K(d)))
      w(G(pc+1))
    elseif name == "KPRI" then
      slotok(a, pc, name)
      if d == 0 then
	w(format("setnilV(base+%d);", a))
      elseif d == 1 or d == 2 then
	w(format("setboolV(base+%d, %d);", a, d == 2 and 1 or 0))
      else
	fail(format("pc=%d KPRI: bad primitive %d", pc, d))
      end
      w(G(pc+1))
    elseif name == "KNIL" then
      if d < a then fail(format("pc=%d KNIL: bad range %d..%d", pc, a, d)) end
      for i = a, d do
	slotok(i, pc, name)
	w(format("setnilV(base+%d);", i))
      end
      w(G(pc+1))
    elseif name == "NOT" then
      slotok(a, pc, name)
      slotok(d, pc, name)
      w(format("setboolV(base+%d, aot_true(base+%d) ? 0 : 1);", a, d))
      w(G(pc+1))
    elseif name == "UNM" then
      slotok(a, pc, name)
      slotok(d, pc, name)
      w(format("aot_unm(L, base+%d, base+%d, %d, \"UNM\");", a, d, pc))
      w(G(pc+1))
    elseif ARITH[name] then
      local fn, layout = ARITH[name][1], ARITH[name][2]
      slotok(a, pc, name)
      slotok(b, pc, name)
      local x, y
      if layout == "VN" then
	knok(c, pc, name)
	x, y = format("base+%d", b), K(c)
      elseif layout == "NV" then
	knok(c, pc, name)
	x, y = K(c), format("base+%d", b)
      else
	slotok(c, pc, name)
	x, y = format("base+%d", b), format("base+%d", c)
      end
      w(format("%s(L, base+%d, %s, %s, %d, \"%s\");", fn, a, x, y, pc, name))
      w(G(pc+1))
    elseif name == "FORI" then
      slotok(a+3, pc, name)
      w(format("if (aot_fori(L, base+%d, %d, \"FORI\")) %s", a, pc, G(pc+1)))
      w(G(t.target))
    elseif name == "FORL" or name == "IFORL" then
      slotok(a+3, pc, name)
      w(format("if (aot_forl(L, base+%d, %d, \"%s\")) %s",
	       a, pc, name, G(t.target)))
      w(G(pc+1))
    elseif name == "RET" then
      local nres = d - 1
      for i = a, a+nres-1 do slotok(i, pc, name) end
      w(format("L->top = base + %d; return %d;", a+nres, nres))
    elseif name == "RET0" then
      w(format("L->top = base + %d; return 0;", a))
    elseif name == "RET1" then
      slotok(a, pc, name)
      w(format("L->top = base + %d; return 1;", a+1))
    else
      fail(format("pc=%d %s: unsupported opcode (%s)", pc, name, reason(name)))
    end
    w("}")
  end

  w(format("bc_%d: /* past the end of the bytecode */", nbc))
  w(format("  return luaL_error(L, \"AOT: pc=%%d: past end of bytecode\", %d);",
	   nbc))
  return out
end

------------------------------------------------------------------------------

-- Check that all reachable control flow edges stay inside the bytecode.
local function check_reachable(ins, nbc)
  local seen, queue, qh = {[1] = true}, {1}, 1
  while qh <= #queue do
    local pc = queue[qh]
    qh = qh + 1
    local succ = successors(ins[pc], pc)
    for i = 1, #succ do
      local p = succ[i]
      if p < 1 or p > nbc-1 then
	fail(format("pc=%d %s: reachable control flow leaves the bytecode",
		    pc, ins[pc].name))
      end
      if not seen[p] then
	seen[p] = true
	queue[#queue+1] = p
      end
    end
  end
end

------------------------------------------------------------------------------

local function checkfunc(func, modname)
  if type(func) ~= "function" then
    fail("compile() expects a Lua function, got "..type(func))
  end
  if type(modname) ~= "string" or
     not match(modname, "^[A-Za-z_][A-Za-z0-9_]*$") then
    fail("module name must be a plain C identifier")
  end
  local fi = funcinfo(func)
  if not fi.proto then
    fail("not a Lua function (C or fast functions are not supported)")
  end
  if fi.isvararg then fail("rejected: vararg function (isvararg=true)") end
  if fi.upvalues ~= 0 then
    fail(format("rejected: %d upvalues (upvalue access is not supported)",
		fi.upvalues))
  end
  if fi.children then
    fail("rejected: child prototypes (nested functions are not supported)")
  end
  return fi
end

-- Public API: compile a Lua function to C source text.
local function compile(func, modname)
  local fi = checkfunc(func, modname)
  local dump = string.dump(func, "d")	-- Strip debug info, keep numbers exact.
  local fcopy, err = load(dump)
  if not fcopy then
    fail("cannot reload dumped prototype: "..tostring(err))
  end
  local fi2 = funcinfo(fcopy)
  local fields = {"stackslots", "params", "bytecodes", "nconsts",
		  "gcconsts", "upvalues"}
  for i = 1, #fields do
    local k = fields[i]
    if fi[k] ~= fi2[k] then
      fail(format("dumped prototype mismatch in %s (%s vs %s)",
		  k, tostring(fi[k]), tostring(fi2[k])))
    end
  end
  if fi2.isvararg or fi2.upvalues ~= 0 or fi2.children then
    fail("dumped prototype is outside the supported subset")
  end
  if fi2.bytecodes < 2 then
    fail(format("rejected: %d bytecode instructions", fi2.bytecodes))
  end

  local ins, nbc = decode(fcopy, fi2)
  -- Non-numeric constants are rejected even if they are only referenced
  -- from unreachable code: the generated module only copies numbers.
  if fi2.gcconsts ~= 0 then
    fail(format("rejected: %d non-numeric constants (gcconsts=%d)",
		fi2.gcconsts, fi2.gcconsts))
  end
  check_reachable(ins, nbc)
  local usekb = fi2.nconsts > 0
  local body = gen_body(ins, nbc, fi2, usekb)

  local out = {}
  local function w(s) out[#out+1] = s end
  w("/* Generated by LuaJIT jit.aot. Do not edit. */")
  w("/* Values use the LuaJIT TValue representation and keep runtime")
  w("** type checks. Generated for the LuaJIT source tree and build")
  w("** configuration this file was produced with. */")
  w(format("#define AOT_MODNAME \"%s\"", modname))
  w(format("#define AOT_FRAMESIZE %d", fi2.stackslots))
  w(format("#define AOT_NPARAMS %d", fi2.params))
  w(format("#define AOT_SIZEBC %d", nbc))
  w(format("#define AOT_SIZEKN %d", fi2.nconsts))
  w("")
  w(format("static const char aot_bc[%d] = {", #dump))
  local bytes = {}
  for i = 1, #dump do bytes[i] = format("%d", byte(dump, i)) end
  w(table.concat(bytes, ","))
  w("};")
  w(CHELPERS)
  w("")
  w("/* Compiled function. The prototype is anchored as the only upvalue")
  w("** of this C closure, so constants stay alive and exact. */")
  w("static int aot_run(lua_State *L)")
  w("{")
  w("  GCfunc *self = curr_func(L);")
  w("  cTValue *uv = &self->c.upvalue[0];")
  w("  GCproto *pt;")
  w("  TValue *base;")
  if usekb then w("  cTValue *kb;") end
  w("")
  w("  if (!tvisfunc(uv) || !isluafunc(funcV(uv)))")
  w("    return luaL_error(L, \"AOT: invalid prototype upvalue\");")
  w("  pt = funcproto(funcV(uv));")
  w("  if (pt->framesize != AOT_FRAMESIZE ||")
  w("      pt->numparams != AOT_NPARAMS ||")
  w("      pt->sizebc != AOT_SIZEBC ||")
  w("      pt->sizekn != AOT_SIZEKN ||")
  w("      pt->sizekgc != 0 || pt->sizeuv != 0)")
  w("    return luaL_error(L, \"AOT: prototype mismatch\");")
  w("  if (!lua_checkstack(L, AOT_FRAMESIZE))")
  w("    return luaL_error(L, \"AOT: stack overflow\");")
  w("  lua_settop(L, AOT_NPARAMS);")
  w("  lua_settop(L, AOT_FRAMESIZE);")
  w("  base = L->base;	/* Slot pointers are only valid after growing. */")
  if usekb then
    w("  kb = mref(pt->k, TValue);	/* Constant pool is fixed for this call. */")
  end
  w("")
  w("  goto bc_1;")
  for i = 1, #body do w(body[i]) end
  w("}")
  w("")
  w(format("int luaopen_%s(lua_State *L)", modname))
  w("{")
  w(format("  if (luaL_loadbuffer(L, aot_bc, sizeof(aot_bc), \"aot:%s\") != 0)",
	   modname))
  w("    return lua_error(L);	/* Error message is on the stack. */")
  w("  lua_pushcclosure(L, aot_run, 1);	/* Anchor the prototype. */")
  w("  return 1;")
  w("}")
  w("")
  return table.concat(out, "\n")
end

------------------------------------------------------------------------------

-- Command line entry point: luajit -jaot=modname,input,output
local function start(modname, input, output)
  if type(modname) ~= "string" or modname == "" then
    fail("start(): module name missing")
  end
  if type(input) ~= "string" or input == "" then
    fail("start(): input file missing")
  end
  if type(output) ~= "string" or output == "" then
    fail("start(): output file missing")
  end
  if match(modname, "^[A-Za-z_][A-Za-z0-9_]*$") == nil then
    fail("start(): module name must be a plain C identifier")
  end
  if input == output then
    fail("start(): input and output file are the same")
  end
  local chunk, err = loadfile(input)
  if not chunk then
    fail(format("start(): cannot load '%s': %s", input, tostring(err)))
  end
  local ok, func = pcall(chunk)
  if not ok then
    fail(format("start(): '%s' failed: %s", input, tostring(func)))
  end
  if type(func) ~= "function" then
    fail(format("start(): '%s' must return a function, got %s",
		input, type(func)))
  end
  local csrc = compile(func, modname)	-- Everything checked before writing.
  local fh, werr = io.open(output, "wb")
  if not fh then
    fail(format("start(): cannot open '%s': %s", output, tostring(werr)))
  end
  local wok, wmsg = fh:write(csrc)
  if not wok then
    fh:close()
    fail(format("start(): cannot write '%s': %s", output, tostring(wmsg)))
  end
  local cok, cmsg = fh:close()
  if not cok then
    fail(format("start(): cannot close '%s': %s", output, tostring(cmsg)))
  end
end

return {
  compile = compile,
  start = start,
}

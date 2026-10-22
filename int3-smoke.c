/*
** Acceptance harness for the one-shot interpreter interrupt (BC_INT3).
**
** Embeds Lua through the public C API only and reports the actual number of
** callbacks, JIT state, trace counts and Lua results for every scenario.
** Returns 0 only if every check passes.
**
** Build (MinGW, from the repository root):
**   gcc -I src int3-smoke.c src/libluajit-5.1.dll.a -o src/int3-smoke.exe
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "luajit.h"

static int failures;

#define CHECK(cond) \
  do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    failures++; \
  } } while (0)

#define CHECKF(cond, ...) \
  do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
    fprintf(stderr, __VA_ARGS__); \
    fprintf(stderr, "\n"); failures++; \
  } } while (0)

/* -- Interrupt callback -------------------------------------------------- */

typedef struct Int3Rec {
  int calls;
  const void *proto;
  unsigned int bcpos;
  lua_State *L;
  int depth;
} Int3Rec;

static void int3_cb(void *data, lua_State *L, const void *proto,
		    unsigned int bcpos)
{
  Int3Rec *r = (Int3Rec *)data;
  r->calls++;
  r->proto = proto;
  r->bcpos = bcpos;
  r->L = L;
  /* The callback is a read-only observer: only inspect, never change. */
  r->depth = lua_gettop(L) >= 0 ? lua_gettop(L) : -1;
  if (proto == NULL || L == NULL) failures++;
}

/* -- Small helpers ------------------------------------------------------- */

static void run(lua_State *L, const char *code)
{
  if (luaL_loadbuffer(L, code, strlen(code), "=smoke") ||
      lua_pcall(L, 0, 0, 0)) {
    fprintf(stderr, "FAIL lua error: %s\n", lua_tostring(L, -1));
    failures++;
    lua_pop(L, 1);
  }
}

/* Run code and leave its single return value as a number in *out. */
static int run_number(lua_State *L, const char *code, double *out)
{
  int ok = 0;
  if (luaL_loadbuffer(L, code, strlen(code), "=smoke") ||
      lua_pcall(L, 0, 1, 0)) {
    fprintf(stderr, "FAIL lua error: %s\n", lua_tostring(L, -1));
    failures++;
    lua_pop(L, 1);
    return 0;
  }
  *out = lua_tonumber(L, -1);
  ok = lua_isnumber(L, -1);
  lua_pop(L, 1);
  return ok;
}

static int run_bool(lua_State *L, const char *code)
{
  int res = 0;
  if (luaL_loadbuffer(L, code, strlen(code), "=smoke") ||
      lua_pcall(L, 0, 1, 0)) {
    fprintf(stderr, "FAIL lua error: %s\n", lua_tostring(L, -1));
    failures++;
    lua_pop(L, 1);
    return 0;
  }
  res = lua_toboolean(L, -1);
  lua_pop(L, 1);
  return res;
}

#if !defined(SMOKE_NO_JIT)
/* Run code, return 1 if it failed with an error. */
static int run_fails(lua_State *L, const char *code)
{
  int failed;
  if (luaL_loadbuffer(L, code, strlen(code), "=smoke") ||
      lua_pcall(L, 0, 0, 0)) {
    lua_pop(L, 1);
    return 1;
  }
  failed = 0;
  return failed;
}
#endif  /* !SMOKE_NO_JIT */

/* Bytecode index of the first instruction with the given opcode name. */
static int find_bcpos_n(lua_State *L, const char *funcpath, const char *opname,
			int nth)
{
  char code[1024];
  snprintf(code, sizeof(code),
    "local f = %s\n"
    "local u = require('jit.util')\n"
    "local v = require('jit.vmdef')\n"
    "local n = 0\n"
    "local alt = { JFORL='FORL', IFORL='FORL', JITERL='ITERL', IITERL='ITERL',\n"
    "              JLOOP='LOOP', ILOOP='LOOP', JFUNCF='FUNCF', IFUNCF='FUNCF',\n"
    "              JFUNCV='FUNCV', IFUNCV='FUNCV' }\n"
    "for i = 0, u.funcinfo(f).bytecodes-1 do\n"
    "  local ins = u.funcbc(f, i) %% 4294967296\n"
    "  local name = v.bcnames:sub((ins%%256)*6+1, (ins%%256)*6+6):gsub(' ','') \n"
    "  if name == '%s' or alt[name] == '%s' then\n"
    "    if n == %d then return i end\n"
    "    n = n + 1\n"
    "  end\n"
    "end\n"
    "return -1\n", funcpath, opname, opname, nth);
  if (luaL_loadbuffer(L, code, strlen(code), "=find") ||
      lua_pcall(L, 0, 1, 0)) {
    fprintf(stderr, "FAIL find_bcpos(%s): %s\n", opname,
	    lua_tostring(L, -1));
    failures++;
    lua_pop(L, 1);
    return -1;
  }
  {
    int pos = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);
    return pos;
  }
}

/* Original (logical) instruction of a global function at a BCPos. */
static long funcbc(lua_State *L, const char *funcpath, int bcpos)
{
  char code[256];
  long ins = -1;
  snprintf(code, sizeof(code),
	   "local f = %s\n"
	   "return require('jit.util').funcbc(f, %d)\n", funcpath, bcpos);
  if (luaL_loadbuffer(L, code, strlen(code), "=funcbc") ||
      lua_pcall(L, 0, 1, 0)) {
    fprintf(stderr, "FAIL funcbc: %s\n", lua_tostring(L, -1));
    failures++;
    lua_pop(L, 1);
    return -1;
  }
  ins = (long)lua_tointeger(L, -1);
  lua_pop(L, 1);
  return ins;
}

static int int3_op(void)
{
  static const char code[] =
    "local v = require('jit.vmdef')\n"
    "for i = 0, 255 do\n"
    "  if v.bcnames:sub(i*6+1, i*6+6):gsub(' ','') == 'INT3' then return i end\n"
    "end\n"
    "return -1\n";
  lua_State *L = luaL_newstate();
  int op = -1;
  if (L == NULL) return -1;
  luaL_openlibs(L);
  if (!luaL_loadbuffer(L, code, sizeof(code) - 1, "=int3op") &&
      !lua_pcall(L, 0, 1, 0))
    op = (int)lua_tointeger(L, -1);
  lua_close(L);
  return op;
}

static int op_int3 = -1;

#if !defined(SMOKE_NO_JIT)
static int trace_count(lua_State *L)
{
  int n = 0;
  char code[512];
  int i;
  for (i = 1; i < 32; i++) {
    snprintf(code, sizeof(code),
	     "return require('jit.util').traceinfo(%d) ~= nil\n", i);
    if (run_bool(L, code)) n++;
  }
  return n;
}
#endif  /* !SMOKE_NO_JIT */

static int opcode_of(lua_State *L, const char *name)
{
  char code[512];
  snprintf(code, sizeof(code),
    "local v = require('jit.vmdef')\n"
    "for i = 0, 255 do\n"
    "  if v.bcnames:sub(i*6+1, i*6+6):gsub(' ','') == '%s' then return i end\n"
    "end\n"
    "return -1\n", name);
  if (luaL_loadbuffer(L, code, strlen(code), "=opcode") ||
      lua_pcall(L, 0, 1, 0)) {
    fprintf(stderr, "FAIL opcode_of(%s): %s\n", name, lua_tostring(L, -1));
    failures++;
    lua_pop(L, 1);
    return -1;
  }
  {
    int op = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);
    return op;
  }
}

#if !defined(SMOKE_NO_JIT)
static void attach_trace_counters(lua_State *L)
{
  run(L,
    "TR = { start = 0, stop = 0, abort = 0, flush = 0, errcode = -1 }\n"
    "jit.attach(function(what, tr, func, pc, msg, info)\n"
    "  if TR[what] then TR[what] = TR[what] + 1 end\n"
    "  if what == 'abort' then TR.errcode = msg end\n"
    "end, 'trace')\n");
}

/* Attach the C function at idx to a VM event. VM event handlers must be real
** Lua closures: a light C function is not a valid vmevent handler value, so
** wrap the probe in a Lua function. The event name must be a vmevent category
** (e.g. "trace"); a sub-event name is matched on the argument.
*/
static void attach_probe(lua_State *L, int idx, const char *category,
			 const char *subevent)
{
  char code[256];
  lua_pushvalue(L, idx);
  lua_setglobal(L, "_VMEVENT_PROBE");
  if (subevent != NULL)
    snprintf(code, sizeof(code),
	     "jit.attach(function(w) if w == '%s' then _VMEVENT_PROBE() end "
	     "end, '%s')", subevent, category);
  else
    snprintf(code, sizeof(code),
	     "jit.attach(function() _VMEVENT_PROBE() end, '%s')", category);
  run(L, code);
}
#endif  /* !SMOKE_NO_JIT */

/* Instruction count of the prototype of a global function path. */
static int func_nbc(lua_State *L, const char *funcpath)
{
  char code[256];
  int n = -1;
  snprintf(code, sizeof(code),
	   "return require('jit.util').funcinfo(%s).bytecodes\n", funcpath);
  if (luaL_loadbuffer(L, code, strlen(code), "=nbc") ||
      lua_pcall(L, 0, 1, 0)) {
    fprintf(stderr, "FAIL funcinfo(%s): %s\n", funcpath,
	    lua_tostring(L, -1));
    failures++;
    lua_pop(L, 1);
    return -1;
  }
  n = (int)lua_tointeger(L, -1);
  lua_pop(L, 1);
  return n;
}

#if !defined(SMOKE_NO_JIT)
static int counter(lua_State *L, const char *name)
{
  char code[128];
  snprintf(code, sizeof(code), "return TR and TR.%s or -1", name);
  if (luaL_loadbuffer(L, code, strlen(code), "=cnt") ||
      lua_pcall(L, 0, 1, 0)) {
    lua_pop(L, 1);
    failures++;
    return -1;
  }
  {
    int v = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);
    return v;
  }
}


#endif  /* !SMOKE_NO_JIT */

/* Find, arm and report the bytecode index of the nth matching opcode. */
static int arm_slot_n(lua_State *L, const char *tbl, const char *fname,
		      const char *opname, int nth, int *bcposp)
{
  char path[128];
  int bcpos, res;
  snprintf(path, sizeof(path), "%s.%s", tbl, fname);
  bcpos = find_bcpos_n(L, path, opname, nth);
  if (bcpos < 0) {
    fprintf(stderr, "FAIL no %s #%d in %s\n", opname, nth, path);
    failures++;
    return -1;
  }
  lua_getglobal(L, tbl);
  lua_getfield(L, -1, fname);
  res = luaJIT_int3_arm(L, -1, (unsigned int)bcpos);
  lua_pop(L, 2);
  if (res != LUAJIT_INT3_OK) {
    fprintf(stderr, "FAIL arm %s at %s#%d (%d): %d\n", path, opname, nth,
	    bcpos, res);
    failures++;
    return -1;
  }
  if (bcposp) *bcposp = bcpos;
  return 0;
}

static int arm_slot(lua_State *L, const char *tbl, const char *fname,
		    const char *opname, int *bcposp)
{
  return arm_slot_n(L, tbl, fname, opname, 0, bcposp);
}

/* -- [1] Live JIT, selective trace flush and one-shot delivery ----------- */
#if !defined(SMOKE_NO_JIT)
static void test_live_jit(void)
{
  lua_State *L = luaL_newstate();
  Int3Rec rec;
  int bcpos = -1, traces0, traces1;
  double v;
  memset(&rec, 0, sizeof(rec));
  luaL_openlibs(L);
  attach_trace_counters(L);
  run(L,
    "G = {}\n"
    "local function f(a) return a * 3 + 1 end\n"
    "G.f = f\n"
    "function G.callf(n)\n"
    "  local s = 0\n"
    "  for i = 1, n do s = s + f(i) end\n"
    "  return s\n"
    "end\n"
    "function G.other(n)\n"
    "  local s = 0\n"
    "  for i = 1, n do s = s + i * 2 end\n"
    "  return s\n"
    "end\n"
    "function G.other2(n)\n"
    "  local s = 0\n"
    "  for i = 1, n do s = s + i * 4 end\n"
    "  return s\n"
    "end\n");
  /* G.callf inlines f, G.other and G.other2 get their own traces. */
  CHECK(run_number(L, "return G.callf(1000)", &v) && v == 1502500);
  CHECK(run_number(L, "return G.callf(1000)", &v) && v == 1502500);
  CHECK(run_number(L, "return G.other(1000)", &v) && v == 1001000);
  CHECK(run_number(L, "return G.other(1000)", &v) && v == 1001000);
  printf("[1] warm: traces=%d start=%d jit=%d\n", trace_count(L),
	 counter(L, "start"), run_bool(L, "return jit.status()"));
  CHECK(trace_count(L) >= 2);
  CHECK(counter(L, "start") >= 2);
  CHECK(run_bool(L, "return jit.status()"));

  /* Starting a session must not touch the JIT or flush anything. */
  traces0 = trace_count(L);
  CHECK(luaJIT_int3_start(L, int3_cb, &rec) == LUAJIT_INT3_OK);
  CHECK(run_bool(L, "return jit.status()"));
  printf("[1] started: traces=%d jit=%d\n", trace_count(L),
	 run_bool(L, "return jit.status()"));
  CHECK(trace_count(L) == traces0);

  /* Arming f must flush only the traces that inlined f. The unrelated trace
  ** of G.other survives: running it again starts no recording.
  */
  CHECK(arm_slot(L, "G", "f", "MULVN", &bcpos) == 0);
  run(L, "TR.start = 0; TR.abort = 0");
  CHECK(run_number(L, "return G.other(1000)", &v) && v == 1001000);
  traces1 = trace_count(L);
  printf("[1] selective: traces=%d->%d start=%d abort=%d\n", traces0, traces1,
	 counter(L, "start"), counter(L, "abort"));
  CHECK(counter(L, "start") == 0);

  /* The (formerly inlined) f now runs interpreted: the trap fires once, the
  ** caller loop re-records and is refused because f is not compilable.
  */
  rec.calls = 0;
  run(L, "TR.start = 0; TR.abort = 0");
  CHECK(run_number(L, "return G.callf(1000)", &v) && v == 1502500);
  printf("[1] hit: calls=%d bcpos=%u want=%d value=%g start=%d abort=%d\n",
	 rec.calls, rec.bcpos, bcpos, v, counter(L, "start"),
	 counter(L, "abort"));
  CHECK(rec.calls == 1);
  CHECK((int)rec.bcpos == bcpos);
  CHECK(rec.proto != NULL);
  CHECK(run_number(L, "return G.callf(10)", &v) && v == 175);
  CHECK(rec.calls == 1);            /* One-shot: no rearm, no repeat. */

  /* Re-arming proves f is still executed interpreted. */
  CHECK(arm_slot(L, "G", "f", "ADDVN", &bcpos) == 0);
  rec.calls = 0;
  CHECK(run_number(L, "return G.callf(1000)", &v) && v == 1502500);
  CHECK(rec.calls == 1);

  /* A pending request survives JIT activity elsewhere and jit.flush(). */
  CHECK(arm_slot(L, "G", "f", "MULVN", &bcpos) == 0);
  run(L, "TR.start = 0");
  CHECK(run_number(L, "return G.other2(1000)", &v) && v == 2002000);
  CHECK(run_number(L, "return G.other2(1000)", &v) && v == 2002000);
  printf("[1] unrelated recording: start=%d\n", counter(L, "start"));
  CHECK(counter(L, "start") > 0);
  run(L, "jit.flush()");
  rec.calls = 0;
  CHECK(run_number(L, "return G.callf(100)", &v) && v == 15250);
  printf("[1] request after jit.flush: calls=%d value=%g\n", rec.calls, v);
  CHECK(rec.calls == 1);
  CHECK(run_bool(L, "return jit.status()"));

  /* The armed prototype cannot be re-enabled, unrelated prototypes can. */
  CHECK(arm_slot(L, "G", "f", "MULVN", &bcpos) == 0);
  lua_getglobal(L, "G");
  lua_getfield(L, -1, "f");
  CHECK(luaJIT_setmode(L, -1, LUAJIT_MODE_FUNC|LUAJIT_MODE_ON) == 0);
  CHECK(luaJIT_setmode(L, -1, LUAJIT_MODE_ALLFUNC|LUAJIT_MODE_ON) == 0);
  CHECK(luaJIT_setmode(L, -1, LUAJIT_MODE_ALLSUBFUNC|LUAJIT_MODE_ON) == 0);
  lua_pop(L, 2);
  lua_getglobal(L, "G");
  lua_getfield(L, -1, "other");
  CHECK(luaJIT_setmode(L, -1, LUAJIT_MODE_FUNC|LUAJIT_MODE_ON) == 1);
  lua_pop(L, 2);
  CHECK(luaJIT_setmode(L, 0, LUAJIT_MODE_ENGINE|LUAJIT_MODE_ON) == 1);
  CHECK(run_bool(L, "return jit.status()"));  /* Never disabled. */

  CHECK(luaJIT_int3_stop(L) == LUAJIT_INT3_OK);
  CHECK(run_bool(L, "return jit.status()"));
  /* After stop the prototype is compilable again, without aborts. */
  rec.calls = 0;
  run(L, "TR.start = 0; TR.abort = 0");
  CHECK(run_number(L, "return G.callf(1000)", &v) && v == 1502500);
  CHECK(run_number(L, "return G.callf(1000)", &v) && v == 1502500);
  printf("[1] stopped: traces=%d start=%d abort=%d calls=%d jit=%d\n",
	 trace_count(L), counter(L, "start"), counter(L, "abort"), rec.calls,
	 run_bool(L, "return jit.status()"));
  CHECK(counter(L, "abort") == 0);  /* f is compilable again. */
  CHECK(rec.calls == 0);
  lua_close(L);
}

/* A pending request also protects prototypes that contain the armed one. */
static void test_nested_proto_gate(void)
{
  lua_State *L = luaL_newstate();
  Int3Rec rec;
  int bcpos = -1;
  double v;
  memset(&rec, 0, sizeof(rec));
  luaL_openlibs(L);
  run(L,
    "G = {}\n"
    "function G.outer()\n"
    "  local function inner(a) return a * 5 end\n"
    "  return inner\n"
    "end\n"
    "function G.plain(n) local s = 0 for i = 1, n do s = s + i end return s end\n"
    "G.inner = G.outer()\n"
    "G.inner(2)\n");
  CHECK(luaJIT_int3_start(L, int3_cb, &rec) == LUAJIT_INT3_OK);
  CHECK(arm_slot(L, "G", "inner", "MULVN", &bcpos) == 0);
  lua_getglobal(L, "G");
  lua_getfield(L, -1, "inner");
  CHECK(luaJIT_setmode(L, -1, LUAJIT_MODE_FUNC|LUAJIT_MODE_ON) == 0);
  lua_pop(L, 1);
  lua_getfield(L, -1, "outer");
  CHECK(luaJIT_setmode(L, -1, LUAJIT_MODE_ALLFUNC|LUAJIT_MODE_ON) == 0);
  CHECK(luaJIT_setmode(L, -1, LUAJIT_MODE_ALLSUBFUNC|LUAJIT_MODE_ON) == 0);
  lua_pop(L, 1);
  lua_getfield(L, -1, "plain");
  CHECK(luaJIT_setmode(L, -1, LUAJIT_MODE_ALLFUNC|LUAJIT_MODE_ON) == 1);
  lua_pop(L, 3);
  rec.calls = 0;
  CHECK(run_number(L, "return G.inner(3)", &v) && v == 15);
  printf("[1b] nested gate: calls=%d value=%g\n", rec.calls, v);
  CHECK(rec.calls == 1);
  CHECK(luaJIT_int3_stop(L) == LUAJIT_INT3_OK);
  lua_close(L);
}

/* A pending request keeps its prototype out of the JIT: the caller keeps
** re-recording and is refused, so the trapped path stays interpreted.
*/
static void test_armed_protection(void)
{
  lua_State *L = luaL_newstate();
  Int3Rec rec;
  int bcpos = -1;
  double v;
  memset(&rec, 0, sizeof(rec));
  luaL_openlibs(L);
  attach_trace_counters(L);
  run(L,
    "G = {}\n"
    "local function f2(x)\n"
    "  if x < 0 then return x * 7 end\n"
    "  return x * 3\n"
    "end\n"
    "G.f2 = f2\n"
    "function G.hot(n)\n"
    "  local s = 0\n"
    "  for i = 1, n do s = s + f2(i) end\n"
    "  return s\n"
    "end\n");
  /* Baseline: G.hot inlines f2. */
  CHECK(run_number(L, "return G.hot(1000)", &v) && v == 1501500);
  CHECK(run_number(L, "return G.hot(1000)", &v) && v == 1501500);
  CHECK(counter(L, "start") >= 1);
  CHECK(luaJIT_int3_start(L, int3_cb, &rec) == LUAJIT_INT3_OK);
  /* Trap the rarely taken branch so the request stays pending. */
  CHECK(arm_slot(L, "G", "f2", "MULVN", &bcpos) == 0);
  run(L, "TR.start = 0; TR.abort = 0");
  CHECK(run_number(L, "return G.hot(1000)", &v) && v == 1501500);
  CHECK(run_number(L, "return G.hot(1000)", &v) && v == 1501500);
  CHECK(run_number(L, "return G.hot(1000)", &v) && v == 1501500);
  printf("[1c] armed: start=%d abort=%d last-abort=%d calls=%d\n",
	 counter(L, "start"), counter(L, "abort"),
	 counter(L, "errcode"), rec.calls);
  CHECK(counter(L, "abort") > 0);   /* Recording refused: f2 is not JIT-able. */
  CHECK(rec.calls == 0);            /* The trapped path was not taken. */
  /* Taking the trapped path delivers exactly once. */
  CHECK(run_number(L, "return G.f2(-2)", &v) && v == -14);
  CHECK(rec.calls == 1);
  CHECK(luaJIT_int3_stop(L) == LUAJIT_INT3_OK);
  /* A fresh caller may inline f2 again: the prototype is compilable. */
  run(L, "function G.hot2(n) local s = 0 for i = 1, n do s = s + G.f2(i) end return s end");
  run(L, "TR.start = 0; TR.abort = 0");
  CHECK(run_number(L, "return G.hot2(1000)", &v) && v == 1501500);
  CHECK(run_number(L, "return G.hot2(1000)", &v) && v == 1501500);
  printf("[1c] stopped: start=%d abort=%d calls=%d\n",
	 counter(L, "start"), counter(L, "abort"), rec.calls);
  CHECK(counter(L, "start") > 0);   /* A fresh trace was recorded. */
  CHECK(counter(L, "abort") == 0);  /* No CJITOFF: f2 is JIT-able again. */
  CHECK(rec.calls == 1);
  lua_close(L);
}

#endif  /* !SMOKE_NO_JIT */

/* -- [2] Session started with the engine already off --------------------- */

static void test_engine_off(void)
{
  lua_State *L = luaL_newstate();
  Int3Rec rec;
  int bcpos = -1;
  double v;
  memset(&rec, 0, sizeof(rec));
  luaL_openlibs(L);
  run(L, "G = {} function G.sum(n) local s=0 for i=1,n do s=s+i end return s end");
  run(L, "jit.off()");
  CHECK(!run_bool(L, "return jit.status()"));
  CHECK(luaJIT_int3_start(L, int3_cb, &rec) == LUAJIT_INT3_OK);
  CHECK(arm_slot(L, "G", "sum", "ADDVV", &bcpos) == 0);
  CHECK(run_number(L, "return G.sum(100)", &v) && v == 5050);
  CHECK(rec.calls == 1);
  CHECK(luaJIT_int3_stop(L) == LUAJIT_INT3_OK);
  CHECK(!run_bool(L, "return jit.status()"));  /* Still off: nothing to do. */
  CHECK(run_number(L, "return G.sum(100)", &v) && v == 5050);
  CHECK(rec.calls == 1);
  {
    int jit = run_bool(L, "return jit.status()");
    printf("[2] engine-off session: calls=%d jit=%d\n", rec.calls, jit);
    CHECK(jit == 0);                 /* jit.off() was never undone. */
  }
  lua_close(L);
}

/* -- [3] Bytecode variants ---------------------------------------------- */

typedef struct Variant {
  const char *fname;
  const char *opname;
  int nth;			/* Nth occurrence of that opcode. */
  const char *call;
  double want;
} Variant;

static void test_variants(void)
{
  lua_State *L = luaL_newstate();
  Int3Rec rec;
  size_t i;
  static const Variant vars[] = {
    { "fixf", "FUNCF", 0, "return G.fixf(1,2,3)", 123 },
    { "vsum", "FUNCV", 0, "return G.vsum(1,2,3,4)", 10 },
    { "usecallm", "CALLM", 0, "return G.usecallm(1,2,3)", 123 },
    { "usecallm2", "RETM", 0, "return G.usecallm2(42)", 42 },
    { "mktab", "TSETM", 0, "return G.mktab(7,8,9)", 3 },
    { "loop", "FORL", 0, "return G.loop(100)", 5050 },
    { "jloop", "JMP", 1, "return G.jloop(3)", 3 },
    { "cfunc", "CALL", 0, "return G.cfunc(3.7)", 3 }
  };
  memset(&rec, 0, sizeof(rec));
  luaL_openlibs(L);
  run(L,
    "G = {}\n"
    "function G.fixf(a, b, c) return a*100 + b*10 + c end\n"
    "function G.vsum(...)\n"
    "  local s = 0\n"
    "  local n = select('#', ...)\n"
    "  for i = 1, n do s = s + select(i, ...) end\n"
    "  return s\n"
    "end\n"
    "function G.usecallm(...) local r = G.fixf(...); return r end\n"
    "function G.usecallm2(...) return ... end\n"
    "function G.mktab(...) local t = {...}; return #t end\n"
    "function G.loop(n) local s = 0 for i = 1, n do s = s + i end return s end\n"
    "function G.cond(x) if x > 0 then return 1 else return 2 end end\n"
    "function G.jloop(n) local i = 0 while i < n do i = i + 1 end return i end\n"
    "function G.cfunc(x) local r = math.floor(x); return r end\n");
  for (i = 0; i < sizeof(vars)/sizeof(vars[0]); i++) {
    double base = -1;
    CHECKF(run_number(L, vars[i].call, &base) && base == vars[i].want,
	   "%s baseline", vars[i].fname);
  }
  CHECK(luaJIT_int3_start(L, int3_cb, &rec) == LUAJIT_INT3_OK);
  for (i = 0; i < sizeof(vars)/sizeof(vars[0]); i++) {
    double v = -1;
    int before = rec.calls, bcpos = -1;
    if (arm_slot_n(L, "G", vars[i].fname, vars[i].opname, vars[i].nth,
		   &bcpos) != 0) continue;
    CHECK(run_number(L, vars[i].call, &v));
    printf("[3] %-9s %-5s bcpos=%2d calls=%d value=%g\n", vars[i].fname,
	   vars[i].opname, bcpos, rec.calls - before, v);
    CHECKF(v == vars[i].want, "%s/%s value %g", vars[i].fname,
	   vars[i].opname, v);
    CHECKF(rec.calls - before == 1, "%s/%s calls %d", vars[i].fname,
	   vars[i].opname, rec.calls - before);
  }
  CHECK(luaJIT_int3_stop(L) == LUAJIT_INT3_OK);
  lua_close(L);
}

/* -- [4] ISNEXT/ITERN despecialization and debug hooks ------------------ */

static int hooks;
static void line_count_hook(lua_State *L, lua_Debug *ar)
{
  (void)L; (void)ar;
  hooks++;
}

static int call_events;
static void count_call_hook(lua_State *L, lua_Debug *ar)
{
  (void)L;
  if (ar->event == LUA_HOOKCALL) call_events++;
}

static void test_isnext_and_hooks(void)
{
  lua_State *L = luaL_newstate();
  Int3Rec rec;
  int bcpos = -1, iterc;
  long ins;
  double v;
  memset(&rec, 0, sizeof(rec));
  luaL_openlibs(L);
  run(L,
    "G = {}\n"
    "local realnext = next\n"
    "G.realnext = realnext\n"
    "local function wrap(t, k)\n"
    "  local nk = realnext(t, k)\n"
    "  if nk ~= nil then return nk, t[nk] end\n"
    "end\n"
    "G.wrap = wrap\n"
    "function G.genfor(t)\n"
    "  local s = 0\n"
    "  for k, v in next, t do s = s + v end\n"
    "  return s\n"
    "end\n"
    "function G.hooksum(n)\n"
    "  local s = 0\n"
    "  for i = 1, n do s = s + i * 2 end\n"
    "  return s\n"
    "end\n"
    "HOOKS = 0\n"
    "function G.fixf(a, b, c) return a*100 + b*10 + c end\n");
  iterc = opcode_of(L, "ITERC");
  CHECK(iterc >= 0);
  CHECK(luaJIT_int3_start(L, int3_cb, &rec) == LUAJIT_INT3_OK);
  /* The generic-for target slot is trapped; ISNEXT must despecialize ITERN
  ** to ITERC without dropping the trap or restoring a stale instruction. */
  CHECK(arm_slot(L, "G", "genfor", "ITERN", &bcpos) == 0);
  rec.calls = 0;
  CHECK(run_number(L,
	"local n = next\n"
	"next = G.wrap\n"
	"local r = G.genfor({a=1,b=2,c=3})\n"
	"next = n\n"
	"return r\n", &v) && v == 6);
  ins = funcbc(L, "G.genfor", bcpos);
  printf("[4] isnext: calls=%d value=%g shadow-op=%ld want=%d\n",
	 rec.calls, v, ins >= 0 ? ins % 256 : -1, iterc);
  CHECK(rec.calls == 1);
  CHECK(ins >= 0 && (int)(ins % 256) == iterc);

  /* Debug hooks and the interrupt must each deliver once, in order. */
  CHECK(arm_slot(L, "G", "hooksum", "ADDVV", &bcpos) == 0);
  rec.calls = 0;
  hooks = 0;
  lua_sethook(L, line_count_hook, LUA_MASKLINE|LUA_MASKCALL|LUA_MASKRET, 7);
  CHECK(run_number(L, "return G.hooksum(100)", &v) && v == 10100);
  lua_sethook(L, NULL, 0, 0);
  printf("[4] hooks: calls=%d hooks=%d value=%g\n", rec.calls, hooks, v);
  CHECK(rec.calls == 1);
  CHECK(hooks > 0);
  CHECK(run_number(L, "return G.hooksum(100)", &v) && v == 10100);
  CHECK(rec.calls == 1);

  /* A trapped function header with a call hook: the hook must observe the
  ** call exactly as often as without the trap (no duplicate delivery from
  ** the re-dispatch after the interrupt).
  */
  {
    int base_events;
    call_events = 0;
    lua_sethook(L, count_call_hook, LUA_MASKCALL, 0);
    CHECK(run_number(L, "return G.fixf(4,5,6)", &v) && v == 456);
    lua_sethook(L, NULL, 0, 0);
    base_events = call_events;
    CHECK(arm_slot_n(L, "G", "fixf", "FUNCF", 0, &bcpos) == 0);
    rec.calls = 0;
    call_events = 0;
    lua_sethook(L, count_call_hook, LUA_MASKCALL, 0);
    CHECK(run_number(L, "return G.fixf(4,5,6)", &v) && v == 456);
    lua_sethook(L, NULL, 0, 0);
    printf("[4] header+call hook: calls=%d events=%d base=%d value=%g\n",
	   rec.calls, call_events, base_events, v);
    CHECK(rec.calls == 1);
    CHECK(call_events == base_events);
  }
  CHECK(luaJIT_int3_stop(L) == LUAJIT_INT3_OK);
  lua_close(L);
}

/* -- [5] GC root, cancel, stop and VM teardown --------------------------- */

static void test_gc_and_teardown(void)
{
  lua_State *L = luaL_newstate();
  Int3Rec rec;
  int bcpos = -1;
  memset(&rec, 0, sizeof(rec));
  luaL_openlibs(L);
  run(L, "G = {} function G.sum(n) local s=0 for i=1,n do s=s+i end return s end");
  CHECK(luaJIT_int3_start(L, int3_cb, &rec) == LUAJIT_INT3_OK);
  CHECK(arm_slot(L, "G", "sum", "ADDVV", &bcpos) == 0);
  /* Drop the only Lua reference: the GC root must keep the proto alive. */
  run(L, "G.sum = nil; collectgarbage('collect'); collectgarbage('collect')");
  CHECK(luaJIT_int3_cancel(L) == LUAJIT_INT3_OK);
  CHECK(luaJIT_int3_cancel(L) == LUAJIT_INT3_OK);  /* No request: still OK. */
  CHECK(rec.calls == 0);
#if defined(SMOKE_NO_JIT)
  CHECK(!run_bool(L, "return jit.status()"));
#else
  CHECK(run_bool(L, "return jit.status()"));       /* The JIT was never off. */
#endif
  CHECK(luaJIT_int3_stop(L) == LUAJIT_INT3_OK);
  CHECK(luaJIT_int3_cancel(L) == LUAJIT_INT3_OK);
  printf("[5] gc/cancel/stop: calls=%d\n", rec.calls);
  lua_close(L);

  /* Teardown with a pending request must not call back or crash. */
  L = luaL_newstate();
  memset(&rec, 0, sizeof(rec));
  luaL_openlibs(L);
  run(L, "G = {} function G.sum(n) local s=0 for i=1,n do s=s+i end return s end");
  CHECK(luaJIT_int3_start(L, int3_cb, &rec) == LUAJIT_INT3_OK);
  CHECK(arm_slot(L, "G", "sum", "ADDVV", &bcpos) == 0);
  lua_close(L);
  CHECK(rec.calls == 0);
  printf("[5] close with pending: calls=%d\n", rec.calls);
}

/* -- [6] Bytecode dump and reload --------------------------------------- */

static int load_ok(lua_State *L, const char *buf, size_t len)
{
  if (luaL_loadbuffer(L, buf, len, "=dump")) {
    lua_pop(L, 1);
    return 0;
  }
  return 1;
}

static void test_dump(void)
{
  lua_State *L = luaL_newstate();
  Int3Rec rec;
  int bcpos = -1;
  double base = -1, v = -1;
  char *dump = NULL, *bad = NULL;
  size_t len = 0;
  int patched = 0;
  long target = 0;
  memset(&rec, 0, sizeof(rec));
  luaL_openlibs(L);
  run(L,
    "G = {}\n"
    "function G.sum(n)\n"
    "  local s = 0\n"
    "  for i = 1, n do s = s + i * 3 end\n"
    "  return s\n"
    "end\n");
  CHECK(run_number(L, "return G.sum(1000)", &base));
  CHECK(luaJIT_int3_start(L, int3_cb, &rec) == LUAJIT_INT3_OK);
  CHECK(arm_slot(L, "G", "sum", "MULVN", &bcpos) == 0);
  target = funcbc(L, "G.sum", bcpos);
  CHECK(target >= 0);
  /* Dump while the request is pending. */
  if (luaL_loadstring(L, "return string.dump(G.sum)") ||
      lua_pcall(L, 0, 1, 0)) {
    fprintf(stderr, "FAIL string.dump: %s\n", lua_tostring(L, -1));
    failures++;
    lua_pop(L, 1);
    lua_close(L);
    return;
  }
  {
    const char *s = lua_tolstring(L, -1, &len);
    dump = (char *)malloc(len);
    memcpy(dump, s, len);
    lua_pop(L, 1);
  }
  /* The dump must not contain BC_INT3: reloaded code behaves identically. */
  CHECK(load_ok(L, dump, len));
  lua_setglobal(L, "G2");
  CHECK(run_number(L, "return G2(1000)", &v));
  printf("[6] dump: len=%u base=%g reload=%g\n", (unsigned)len, base, v);
  CHECK(v == base);
  {
    long ins2 = funcbc(L, "G2", bcpos);
    CHECK(ins2 == target);
  }
  /* Same-version dump with a persistent BC_INT3 must be rejected. Pick a
  ** bytecode instruction whose 4-byte encoding is unique in the dump, so the
  ** patched offset is unambiguous. */
  {
    int nbc = func_nbc(L, "G.sum");
    int patch_off = -1;
    long patch_ins = 0;
    int bi;
    CHECK(nbc > 0);
    for (bi = 0; bi < nbc && patch_off < 0; bi++) {
      long ins = funcbc(L, "G.sum", bi);
      int j, hits = 0;
      if (ins < 0) continue;
      for (j = 0; j + 4 <= (int)len; j++)
	if (memcmp(dump + j, &ins, 4) == 0) hits++;
      if (hits == 1) {
	for (j = 0; j + 4 <= (int)len; j++)
	  if (memcmp(dump + j, &ins, 4) == 0) patch_off = j;
	patch_ins = ins;
      }
    }
    printf("[6] patch target: bcpos-ins=%ld off=%d int3=%d\n", patch_ins,
	   patch_off, op_int3);
    CHECK(patch_off >= 0);
    if (patch_off >= 0) {
      bad = (char *)malloc(len);
      memcpy(bad, dump, len);
      bad[patch_off] = (char)(unsigned char)op_int3;
      patched = 1;
    }
  }
  printf("[6] patched-bad-dump=%d\n", patched);
  if (patched) {
    int ok = load_ok(L, bad, len);
    if (ok) {
      fprintf(stderr, "FAIL: dump with BC_INT3 was accepted\n");
      failures++;
      lua_pop(L, 1);
    }
  }
  /* The running request is still valid after dumping. */
  rec.calls = 0;
  CHECK(run_number(L, "return G.sum(100)", &v));
  printf("[6] request after dump: calls=%d value=%g\n", rec.calls, v);
  CHECK(rec.calls == 1);
  CHECK(luaJIT_int3_stop(L) == LUAJIT_INT3_OK);
  free(dump);
  free(bad);
  lua_close(L);
}

/* -- [7] Profiler interaction ------------------------------------------- */

#if !defined(SMOKE_NO_JIT) && !defined(SMOKE_NO_PROFILE)
static int prof_samples;
static void prof_cb(void *data, lua_State *L, int samples, int vmstate)
{
  (void)data; (void)L; (void)vmstate;
  prof_samples += samples;
}

#define BUSY_LOOP "local x = 0 for i = 1, 20000000 do x = x + i end return x"

static void test_profiler(void)
{
  lua_State *L = luaL_newstate();
  lua_State *L2;
  Int3Rec rec;
  double v;
  memset(&rec, 0, sizeof(rec));
  luaL_openlibs(L);
  run(L, "G = {} function G.sum(n) local s=0 for i=1,n do s=s+i end return s end");
  run(L, "jit.off()");
  CHECK(luaJIT_int3_start(L, int3_cb, &rec) == LUAJIT_INT3_OK);
  /* A profiler must not be able to start while INT3 is active. */
  prof_samples = 0;
  CHECK(run_fails(L,
	"return require('jit.profile').start('i1', function() end)"));
  luaJIT_profile_start(L, "i1", prof_cb, NULL);
  run(L, BUSY_LOOP);
  printf("[7] profile during int3: samples=%d\n", prof_samples);
  CHECK(prof_samples == 0);
  luaJIT_profile_stop(L);
  CHECK(luaJIT_int3_stop(L) == LUAJIT_INT3_OK);

  /* A profiler runs normally after the session ended. */
  prof_samples = 0;
  luaJIT_profile_start(L, "i1", prof_cb, NULL);
  run(L, BUSY_LOOP);
  luaJIT_profile_stop(L);
  printf("[7] profile after stop: samples=%d\n", prof_samples);
  CHECK(prof_samples > 0);
  /* The trace-flushing profiler modes work again, too. */
  prof_samples = 0;
  luaJIT_profile_start(L, "il2f2", prof_cb, NULL);
  run(L, BUSY_LOOP);
  luaJIT_profile_stop(L);
  printf("[7] profile l/f modes after stop: samples=%d jit=%d\n",
	 prof_samples, run_bool(L, "return jit.status()"));
  CHECK(prof_samples > 0);

  /* INT3 must not start while a profiler owns the process, and it must not
  ** flush anything in that case. */
  {
    int traces;
    run(L, "jit.on(); for i = 1, 2 do G.sum(100) end");
    traces = trace_count(L);
    prof_samples = 0;
    luaJIT_profile_start(L, "i1", prof_cb, NULL);
    CHECK(luaJIT_int3_start(L, int3_cb, &rec) == LUAJIT_INT3_BUSY);
    printf("[7] int3 vs profiler: traces=%d->%d\n", traces, trace_count(L));
    CHECK(trace_count(L) == traces);
    luaJIT_profile_stop(L);
  }
  CHECK(luaJIT_int3_start(L, int3_cb, &rec) == LUAJIT_INT3_OK);

  /* Another VM cannot steal the session, and cannot start a profiler. */
  L2 = luaL_newstate();
  luaL_openlibs(L2);
  run(L2, "jit.off()");
  CHECK(luaJIT_int3_start(L2, int3_cb, &rec) == LUAJIT_INT3_BUSY);
  prof_samples = 0;
  luaJIT_profile_start(L2, "i1", prof_cb, NULL);
  run(L2, BUSY_LOOP);
  printf("[7] other-vm profile blocked: samples=%d\n", prof_samples);
  CHECK(prof_samples == 0);
  luaJIT_profile_stop(L2);
  CHECK(luaJIT_int3_stop(L) == LUAJIT_INT3_OK);
  /* Now the other VM may profile normally. */
  prof_samples = 0;
  luaJIT_profile_start(L2, "i1", prof_cb, NULL);
  run(L2, BUSY_LOOP);
  luaJIT_profile_stop(L2);
  printf("[7] other-vm profile after stop: samples=%d\n", prof_samples);
  CHECK(prof_samples > 0);
  CHECK(run_number(L, "return G.sum(10)", &v) && v == 55);
  lua_close(L2);
  lua_close(L);
}

#endif  /* !SMOKE_NO_JIT && !SMOKE_NO_PROFILE */

#if !defined(SMOKE_NO_JIT) && !defined(SMOKE_NO_PROFILE)
/* -- [8] Unsafe contexts ------------------------------------------------- */

static int hook_start_res;
static Int3Rec probe_rec;
static void start_in_hook(lua_State *L, lua_Debug *ar)
{
  (void)ar;
  hook_start_res = luaJIT_int3_start(L, int3_cb, &probe_rec);
}

static int flush_start_res, flush_on_res, flush_arm_res, flush_eng_on_res;
static int flush_probe(lua_State *L)
{
  flush_start_res = luaJIT_int3_start(L, int3_cb, &probe_rec);
  flush_eng_on_res = luaJIT_setmode(L, 0, LUAJIT_MODE_ENGINE|LUAJIT_MODE_ON);
  lua_getglobal(L, "_PENDING_F");
  flush_on_res = luaJIT_setmode(L, -1, LUAJIT_MODE_FUNC|LUAJIT_MODE_ON);
  lua_pop(L, 1);
  flush_arm_res = luaJIT_int3_arm(L, 0, 0);
  return 0;
}

static int record_start_res;
static int record_probe(lua_State *L)
{
  /* Only the interrupt API is probed here: calling luaJIT_setmode from inside
  ** a recording VM event would abort that recording by design.
  */
  record_start_res = luaJIT_int3_start(L, int3_cb, &probe_rec);
  return 0;
}

static void test_unsafe(void)
{
  lua_State *L = luaL_newstate();
  Int3Rec rec;
  int bcpos = -1;
  double v;
  memset(&rec, 0, sizeof(rec));
  memset(&probe_rec, 0, sizeof(probe_rec));
  luaL_openlibs(L);
  run(L, "G = {} function G.sum(n) local s=0 for i=1,n do s=s+i end return s end");
  CHECK(luaJIT_int3_start(L, int3_cb, &rec) == LUAJIT_INT3_OK);
  /* Inside a Lua hook the interrupt API must refuse to change state. */
  hook_start_res = -1;
  lua_sethook(L, start_in_hook, LUA_MASKCOUNT, 1);
  run(L, "local x = 0 for i = 1, 2000 do x = x + i end return x");
  lua_sethook(L, NULL, 0, 0);
  printf("[8] start in hook: %d (want %d)\n", hook_start_res,
	 LUAJIT_INT3_BUSY);
  CHECK(hook_start_res == LUAJIT_INT3_BUSY);
  /* During a flush VM event every interrupt state change is rejected, while
  ** enabling the engine itself is allowed (it was never disabled).
  */
  CHECK(arm_slot(L, "G", "sum", "ADDVV", &bcpos) == 0);
  lua_getglobal(L, "G");
  lua_getfield(L, -1, "sum");
  lua_setglobal(L, "_PENDING_F");
  lua_pop(L, 1);
  lua_pushcfunction(L, flush_probe);
  attach_probe(L, lua_gettop(L), "trace", "flush");
  lua_pop(L, 1);
  flush_start_res = flush_on_res = flush_arm_res = flush_eng_on_res = -1;
  run(L, "jit.flush()");
  printf("[8] flush vmevent: start=%d on=%d arm=%d eng_on=%d\n",
	 flush_start_res, flush_on_res, flush_arm_res, flush_eng_on_res);
  CHECK(flush_start_res == LUAJIT_INT3_BUSY);
  CHECK(flush_on_res == 0);           /* The armed prototype stays NOJIT. */
  CHECK(flush_arm_res == LUAJIT_INT3_BUSY);
  CHECK(flush_eng_on_res == 1);       /* The engine is not ours to gate. */
  /* The session is still fully usable: the request survived the flush. */
  rec.calls = 0;
  CHECK(run_number(L, "return G.sum(100)", &v) && v == 5050);
  CHECK(rec.calls == 1);
  CHECK(luaJIT_int3_stop(L) == LUAJIT_INT3_OK);
  lua_close(L);

  /* Starting a session neither flushes traces nor emits a flush event. */
  L = luaL_newstate();
  memset(&rec, 0, sizeof(rec));
  memset(&probe_rec, 0, sizeof(probe_rec));
  luaL_openlibs(L);
  run(L, "G = {} function G.sum(n) local s=0 for i=1,n do s=s+i end return s end");
  run(L, "for i = 1, 2 do G.sum(1000) end");
  lua_pushcfunction(L, flush_probe);
  attach_probe(L, lua_gettop(L), "trace", "flush");
  lua_pop(L, 1);
  flush_start_res = flush_on_res = flush_arm_res = flush_eng_on_res = -1;
  {
    int traces = trace_count(L);
    CHECK(luaJIT_int3_start(L, int3_cb, &rec) == LUAJIT_INT3_OK);
    printf("[8] start: traces=%d->%d probe=%d jit=%d\n", traces,
	   trace_count(L), flush_start_res, run_bool(L, "return jit.status()"));
    CHECK(trace_count(L) == traces);
    CHECK(flush_start_res == -1);      /* No flush event on start. */
    CHECK(run_bool(L, "return jit.status()"));
  }
  /* Refusals inside a recording VM event: no interrupt state change while
  ** the recorder is active.
  */
  record_start_res = -1;
  lua_pushcfunction(L, record_probe);
  attach_probe(L, lua_gettop(L), "trace", "start");
  lua_pop(L, 1);
  CHECK(luaJIT_int3_stop(L) == LUAJIT_INT3_OK);
  run(L, "jit.flush(); for i = 1, 2 do G.sum(500) end");
  printf("[8] record probe: start=%d traces=%d\n", record_start_res,
	 trace_count(L));
  CHECK(record_start_res == LUAJIT_INT3_BUSY);
  CHECK(trace_count(L) > 0);
  lua_close(L);
}

#endif  /* !SMOKE_NO_JIT && !SMOKE_NO_PROFILE */

/* -- [9] Conditional builds ---------------------------------------------- */

#if defined(SMOKE_NO_PROFILE)
/* Compiled with LUAJIT_DISABLE_PROFILE: the API must report UNAVAILABLE. */
static void test_no_profile_api(void)
{
  lua_State *L = luaL_newstate();
  Int3Rec rec;
  double v;
  memset(&rec, 0, sizeof(rec));
  luaL_openlibs(L);
  CHECK(luaJIT_int3_start(L, int3_cb, &rec) == LUAJIT_INT3_UNAVAILABLE);
  CHECK(luaJIT_int3_arm(L, 0, 0) == LUAJIT_INT3_UNAVAILABLE);
  CHECK(luaJIT_int3_cancel(L) == LUAJIT_INT3_UNAVAILABLE);
  CHECK(luaJIT_int3_stop(L) == LUAJIT_INT3_UNAVAILABLE);
  CHECK(rec.calls == 0);
  /* The VM is otherwise unaffected. */
  run(L, "G = {} function G.sum(n) local s=0 for i=1,n do s=s+i end return s end");
  CHECK(run_number(L, "return G.sum(1000)", &v) && v == 500500);
  CHECK(run_number(L, "return G.sum(1000)", &v) && v == 500500);
  printf("[9] no-profile: calls=%d jit=%d traces=%d\n", rec.calls,
	 run_bool(L, "return jit.status()"), trace_count(L));
#if !defined(SMOKE_NO_JIT)
  CHECK(run_bool(L, "return jit.status()"));
  CHECK(trace_count(L) > 0);
#endif
  lua_close(L);
}
#endif

int main(int argc, char **argv)
{
#if !defined(SMOKE_NO_PROFILE)
  int only = argc > 1 ? atoi(argv[1]) : 0;
#else
  (void)argc; (void)argv;
#endif
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);
#if defined(SMOKE_NO_PROFILE) && defined(SMOKE_NO_JIT)
  /* Both compiled out: no jit.vmdef opcode names, only the API stub. */
  test_no_profile_api();
#elif defined(SMOKE_NO_PROFILE)
  op_int3 = int3_op();
  printf("BC_INT3 opcode = %d\n", op_int3);
  test_no_profile_api();
#else
  op_int3 = int3_op();
  printf("BC_INT3 opcode = %d\n", op_int3);
  if (op_int3 < 0) {
    fprintf(stderr, "FAIL: BC_INT3 opcode not found\n");
    return 1;
  }
#if defined(SMOKE_NO_JIT)
  /* JIT compiled out: only interpreter-side scenarios apply. */
  if (!only || only == 2) test_engine_off();
  if (!only || only == 3) test_variants();
  if (!only || only == 4) test_isnext_and_hooks();
  if (!only || only == 5) test_gc_and_teardown();
  if (!only || only == 6) test_dump();
#else
  if (!only || only == 1) {
    test_live_jit();
    test_nested_proto_gate();
    test_armed_protection();
  }
  if (!only || only == 2) test_engine_off();
  if (!only || only == 3) test_variants();
  if (!only || only == 4) test_isnext_and_hooks();
  if (!only || only == 5) test_gc_and_teardown();
  if (!only || only == 6) test_dump();
  if (!only || only == 7) test_profiler();
  if (!only || only == 8) test_unsafe();
#endif
#endif
  printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
  return failures ? 1 : 0;
}

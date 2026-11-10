/*
** Acceptance harness for the interpreter breakpoints (BC_INT3).
**
** Embeds Lua through the public C API only and reports the actual number of
** callbacks, JIT state, trace counts and Lua results for every scenario.
** Returns 0 only if every check passes.
**
** Build (MinGW, from the repository root):
**   gcc -I src int3-smoke.c src/libluajit-5.1.dll.a -o src/int3-smoke.exe
*/

#include <errno.h>
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

/* Bounded delivery counter: a defect that re-dispatches the trap instead of
** the shadowed instruction delivers callbacks in a loop, so the harness must
** fail instead of hanging. Every legitimate run stays far below the cap.
*/
#define BP_DELIVERY_MAX		100000
static long bp_deliveries;

static void bp_guard(lua_State *L)
{
  (void)L;
  if (++bp_deliveries <= BP_DELIVERY_MAX) return;
  /* A defect that re-dispatches the trap instead of the shadowed instruction
  ** delivers callbacks in a loop. Report and stop: an unbounded loop would
  ** hang the harness instead of failing it.
  */
  fprintf(stderr, "FAIL: more than %d callbacks -- trap re-dispatched?\n",
	  BP_DELIVERY_MAX);
  exit(1);
}

typedef struct Int3Rec {
  int calls;
  const void *func;		/* Running closure of the last delivery. */
  unsigned int bcpos;
  lua_State *L;
  int depth;
} Int3Rec;

static void int3_cb(void *data, lua_State *L, const void *func,
		    unsigned int bcpos)
{
  Int3Rec *r = (Int3Rec *)data;
  bp_guard(L);
  r->calls++;
  r->func = func;
  r->bcpos = bcpos;
  r->L = L;
  /* The callback is a read-only observer: only inspect, never change. */
  r->depth = lua_gettop(L) >= 0 ? lua_gettop(L) : -1;
  if (func == NULL || L == NULL) failures++;
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
  res = luaJIT_int3_arm_bc(L, -1, (unsigned int)bcpos);
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

/* luaJIT_setmode(ON) for a global function: 0 means "refused", e.g. while an
** interrupt request excludes the prototype.
*/
static int set_jit_on(lua_State *L, const char *tbl, const char *fname)
{
  int res;
  lua_getglobal(L, tbl);
  lua_getfield(L, -1, fname);
  res = luaJIT_setmode(L, -1, LUAJIT_MODE_FUNC|LUAJIT_MODE_ON);
  lua_pop(L, 2);
  return res;
}

/* Arm a slot by index and return the status code. */
static int arm_pos(lua_State *L, const char *tbl, const char *fname,
		   int bcpos)
{
  int res;
  lua_getglobal(L, tbl);
  lua_getfield(L, -1, fname);
  res = luaJIT_int3_arm_bc(L, -1, (unsigned int)bcpos);
  lua_pop(L, 2);
  return res;
}

/* Arm the nth slot with this opcode and return the status code. */
static int arm_code(lua_State *L, const char *tbl, const char *fname,
		    const char *opname, int nth)
{
  char path[128];
  int bcpos;
  snprintf(path, sizeof(path), "%s.%s", tbl, fname);
  bcpos = find_bcpos_n(L, path, opname, nth);
  if (bcpos < 0) return -100;
  lua_getglobal(L, tbl);
  lua_getfield(L, -1, fname);
  {
    int res = luaJIT_int3_arm_bc(L, -1, (unsigned int)bcpos);
    lua_pop(L, 2);
    return res;
  }
}

/* -- [1] Live JIT, selective trace flush and call-site delivery --------- */
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

  /* The (formerly inlined) f now runs interpreted: the breakpoint fires on
  ** every execution of the slot, the caller loop re-records and is refused
  ** because f is not compilable.
  */
  rec.calls = 0;
  run(L, "TR.start = 0; TR.abort = 0");
  CHECK(run_number(L, "return G.callf(1000)", &v) && v == 1502500);
  printf("[1] hits: calls=%d bcpos=%u want=%d value=%g start=%d abort=%d\n",
	 rec.calls, rec.bcpos, bcpos, v, counter(L, "start"),
	 counter(L, "abort"));
  CHECK(rec.calls == 1000);		/* One delivery per execution. */
  CHECK((int)rec.bcpos == bcpos);
  CHECK(rec.func != NULL);
  CHECK(run_number(L, "return G.callf(10)", &v) && v == 175);
  CHECK(rec.calls == 1010);		/* The breakpoint stays armed. */

  /* A second breakpoint of the same prototype proves f is still executed
  ** interpreted. Cancel first: the previous one is still armed.
  */
  CHECK(luaJIT_int3_cancel(L) == LUAJIT_INT3_OK);
  CHECK(arm_slot(L, "G", "f", "ADDVN", &bcpos) == 0);
  rec.calls = 0;
  CHECK(run_number(L, "return G.callf(1000)", &v) && v == 1502500);
  CHECK(rec.calls == 1000);

  /* An armed breakpoint survives JIT activity elsewhere and jit.flush(). */
  CHECK(luaJIT_int3_cancel(L) == LUAJIT_INT3_OK);
  CHECK(arm_slot(L, "G", "f", "MULVN", &bcpos) == 0);
  run(L, "TR.start = 0");
  CHECK(run_number(L, "return G.other2(1000)", &v) && v == 2002000);
  CHECK(run_number(L, "return G.other2(1000)", &v) && v == 2002000);
  printf("[1] unrelated recording: start=%d\n", counter(L, "start"));
  CHECK(counter(L, "start") > 0);
  run(L, "jit.flush()");
  rec.calls = 0;
  CHECK(run_number(L, "return G.callf(100)", &v) && v == 15250);
  printf("[1] breakpoint after jit.flush: calls=%d value=%g\n", rec.calls, v);
  CHECK(rec.calls == 100);
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
  lua_pop(L, 2);
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
  CHECK(rec.calls == 100);		/* One delivery per iteration. */
  CHECK(luaJIT_int3_stop(L) == LUAJIT_INT3_OK);
  CHECK(!run_bool(L, "return jit.status()"));  /* Still off: nothing to do. */
  CHECK(run_number(L, "return G.sum(100)", &v) && v == 5050);
  CHECK(rec.calls == 100);		/* Disarmed by stop. */
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
  int hits;			/* Executions of that slot per call. */
} Variant;

static void test_variants(void)
{
  lua_State *L = luaL_newstate();
  Int3Rec rec;
  size_t i;
  static const Variant vars[] = {
    { "fixf", "MULVN", 0, "return G.fixf(1,2,3)", 123, 1 },
    { "vsum", "ADDVV", 0, "return G.vsum(1,2,3,4)", 10, 4 },
    { "usecallm", "CALLM", 0, "return G.usecallm(1,2,3)", 123, 1 },
    { "usecallm2", "RETM", 0, "return G.usecallm2(42)", 42, 1 },
    { "mktab", "TSETM", 0, "return G.mktab(7,8,9)", 3, 1 },
    { "loop", "ADDVV", 0, "return G.loop(100)", 5050, 100 },
    /* JMP#1 is the loop back edge: one execution per iteration. */
    { "jloop", "JMP", 1, "return G.jloop(3)", 3, 3 },
    { "cfunc", "CALL", 0, "return G.cfunc(3.7)", 3, 1 }
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
    CHECKF(rec.calls - before == vars[i].hits, "%s/%s calls %d want %d",
	   vars[i].fname, vars[i].opname, rec.calls - before, vars[i].hits);
    /* Isolate the variants: every armed slot is permanent. */
    CHECK(luaJIT_int3_cancel(L) == LUAJIT_INT3_OK);
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

static int line_events;
static void count_line_hook(lua_State *L, lua_Debug *ar)
{
  (void)L;
  if (ar->event == LUA_HOOKLINE) line_events++;
}

static int ret_events;
static void count_ret_hook(lua_State *L, lua_Debug *ar)
{
  (void)L;
  if (ar->event == LUA_HOOKRET) ret_events++;
}

static void test_isnext_and_hooks(void)
{
  lua_State *L = luaL_newstate();
  Int3Rec rec;
  int bcpos = -1, iterc, itern;
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
    "function G.fixf(a, b, c) return a*100 + b*10 + c end\n"
    "function G.caller(a, b, c) local r = G.fixf(a, b, c) return r end\n");
  iterc = opcode_of(L, "ITERC");
  CHECK(iterc >= 0);
  CHECK(luaJIT_int3_start(L, int3_cb, &rec) == LUAJIT_INT3_OK);
  /* The generic-for target slot is rewritten by ISNEXT/ITERN, so it cannot
  ** be armed at all. */
  CHECK(arm_code(L, "G", "genfor", "ITERN", 0) == LUAJIT_INT3_UNARMABLE);
  CHECK(arm_code(L, "G", "genfor", "ISNEXT", 0) == LUAJIT_INT3_UNARMABLE);
  itern = find_bcpos_n(L, "G.genfor", "ITERN", 0);
  CHECK(itern >= 0);
  /* Arm the loop body instead: the despecialization must leave it alone. */
  CHECK(arm_slot(L, "G", "genfor", "ADDVV", &bcpos) == 0);
  rec.calls = 0;
  CHECK(run_number(L,
	"local n = next\n"
	"next = G.wrap\n"
	"local r = G.genfor({a=1,b=2,c=3})\n"
	"next = n\n"
	"return r\n", &v) && v == 6);
  ins = funcbc(L, "G.genfor", itern);
  printf("[4] isnext: calls=%d value=%g shadow-op=%ld want=%d\n",
	 rec.calls, v, ins >= 0 ? ins % 256 : -1, iterc);
  CHECK(rec.calls == 3);		/* One delivery per iteration. */
  CHECK(ins >= 0 && (int)(ins % 256) == iterc);
  CHECK(luaJIT_int3_cancel(L) == LUAJIT_INT3_OK);

  /* Debug hooks and the interrupt must each deliver once, in order. */
  CHECK(arm_slot(L, "G", "hooksum", "ADDVV", &bcpos) == 0);
  rec.calls = 0;
  hooks = 0;
  lua_sethook(L, line_count_hook, LUA_MASKLINE|LUA_MASKCALL|LUA_MASKRET, 7);
  CHECK(run_number(L, "return G.hooksum(100)", &v) && v == 10100);
  lua_sethook(L, NULL, 0, 0);
  printf("[4] hooks: calls=%d hooks=%d value=%g\n", rec.calls, hooks, v);
  CHECK(rec.calls == 100);		/* One delivery per iteration. */
  CHECK(hooks > 0);
  rec.calls = 0;
  CHECK(run_number(L, "return G.hooksum(100)", &v) && v == 10100);
  CHECK(rec.calls == 100);		/* Exact: no duplicate delivery. */
  CHECK(luaJIT_int3_cancel(L) == LUAJIT_INT3_OK);

  /* A trapped call site with a call hook: the hook must observe the call
  ** exactly as often as without the trap. Function headers themselves are
  ** not armable (the JIT rewrites them).
  */
  CHECK(arm_code(L, "G", "fixf", "FUNCF", 0) == LUAJIT_INT3_UNARMABLE);
  {
    int base_events;
    call_events = 0;
    lua_sethook(L, count_call_hook, LUA_MASKCALL, 0);
    CHECK(run_number(L, "return G.caller(4,5,6)", &v) && v == 456);
    lua_sethook(L, NULL, 0, 0);
    base_events = call_events;
    CHECK(arm_slot(L, "G", "caller", "CALL", &bcpos) == 0);
    rec.calls = 0;
    call_events = 0;
    lua_sethook(L, count_call_hook, LUA_MASKCALL, 0);
    CHECK(run_number(L, "return G.caller(4,5,6)", &v) && v == 456);
    lua_sethook(L, NULL, 0, 0);
    printf("[4] call+call hook: calls=%d events=%d base=%d value=%g\n",
	   rec.calls, call_events, base_events, v);
    CHECK(rec.calls == 1);
    CHECK(call_events == base_events);
  }
  /* A trapped CALLM with a call hook: lj_dispatch_topslot() has to take the
  ** shadowed instruction, because lj_dispatch_call() derives the number of
  ** extra arguments of a vararg callee from L->top (call_init()).
  */
  run(L, "function G.va(...) return select('#', ...) end\n"
	 "function G.vc(...) local r = G.va(...) return r end\n"
	 "function G.vb(...) return G.va(...) end");
  CHECK(arm_code(L, "G", "vc", "CALLM", 0) == LUAJIT_INT3_OK);
  rec.calls = 0;
  call_events = 0;
  lua_sethook(L, count_call_hook, LUA_MASKCALL, 0);
  CHECK(run_number(L, "return G.vc(1,2,3)", &v) && v == 3);
  lua_sethook(L, NULL, 0, 0);
  printf("[4] CALLM+call hook: hits=%d events=%d value=%g depth=%d\n",
	 rec.calls, call_events, v, rec.depth);
  CHECK(rec.calls == 1);
  CHECK(v == 3);	/* The vararg count survived the trapped call site. */
  /* The callback sees exactly the slots this call site consumes:
  ** CALLM A=0 C=2 with the frame's initial nres=1 gives
  ** A+C+nres-1+1+LJ_FR2 = 4+FR2 used slots, i.e. lua_gettop() == 4+FR2.
  ** A topslot() that fell back to pt->framesize would report 2 slots.
  */
  {
    int fr2 = run_bool(L,
	"local b = string.dump(function() end):byte(5)\n"
	"return b ~= nil and math.floor(b / 8) % 2 == 1");
    CHECK(rec.depth == 4 + fr2);
  }
  CHECK(luaJIT_int3_cancel(L) == LUAJIT_INT3_OK);
  CHECK(run_number(L, "return G.vc(1,2,3,4,5)", &v) && v == 5);
  CHECK(arm_code(L, "G", "vb", "CALLMT", 0) == LUAJIT_INT3_OK);
  rec.calls = 0;
  CHECK(run_number(L, "return G.vb(7,8)", &v) && v == 2);
  CHECK(rec.calls == 1);
  CHECK(luaJIT_int3_cancel(L) == LUAJIT_INT3_OK);

  /* A breakpoint must not change the line-event stream. The delivery
  ** publishes the current instruction's PC in the frame; if that PC is left
  ** behind, lj_dispatch_ins() sees oldpc == pc for the instruction it
  ** re-dispatches and takes the "line may have changed" path, i.e. it fires
  ** an extra line event for a same-line instruction.
  */
  run(L, "function G.sameline(x)\n"
	 "  local a = x + 1 local b = a + 1 return b\n"
	 "end");
  {
    int base, armed;
    line_events = 0;
    lua_sethook(L, count_line_hook, LUA_MASKLINE, 0);
    CHECK(run_number(L, "return G.sameline(5)", &v) && v == 7);
    lua_sethook(L, NULL, 0, 0);
    base = line_events;
    CHECK(arm_slot_n(L, "G", "sameline", "ADDVN", 1, &bcpos) == 0);
    rec.calls = 0;
    line_events = 0;
    lua_sethook(L, count_line_hook, LUA_MASKLINE, 0);
    CHECK(run_number(L, "return G.sameline(5)", &v) && v == 7);
    lua_sethook(L, NULL, 0, 0);
    armed = line_events;
    printf("[4] line events: baseline=%d armed=%d hits=%d value=%g\n", base,
	   armed, rec.calls, v);
    CHECK(rec.calls == 1);
    CHECK(armed == base);	/* Same line: no extra event for the trap. */
    CHECK(luaJIT_int3_cancel(L) == LUAJIT_INT3_OK);
  }

  /* The debug name back-scan reads the bytecode of earlier slots too, so it
  ** must see the shadowed definition instruction of an armed slot.
  */
  run(L, "function G.target() return debug.getinfo(1, 'n').name end\n"
	 "function gtarget() return debug.getinfo(1, 'n').name end\n"
	 "function G.callname() local r = G.target() return r end\n"
	 "function G.callglobal() local r = gtarget() return r end");
  CHECK(run_bool(L, "return G.callname() == 'target'"));
  CHECK(run_bool(L, "return G.callglobal() == 'gtarget'"));
  CHECK(arm_slot(L, "G", "callname", "TGETS", &bcpos) == 0);
  rec.calls = 0;
  CHECK(run_bool(L, "return G.callname() == 'target'"));
  CHECK(rec.calls == 1);
  CHECK(luaJIT_int3_cancel(L) == LUAJIT_INT3_OK);
  CHECK(arm_slot(L, "G", "callglobal", "GGET", &bcpos) == 0);
  rec.calls = 0;
  CHECK(run_bool(L, "return G.callglobal() == 'gtarget'"));
  CHECK(rec.calls == 1);
  CHECK(luaJIT_int3_cancel(L) == LUAJIT_INT3_OK);

  CHECK(luaJIT_int3_stop(L) == LUAJIT_INT3_OK);
  lua_close(L);
}

/* -- [5] GC root, cancel, stop and VM teardown --------------------------- */

static void test_gc_and_teardown(void)
{
  lua_State *L = luaL_newstate();
  Int3Rec rec;
  int bcpos = -1;
  double v, alive;
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

  /* A finalizer that runs during lua_close() must not trap: the session is
  ** released before the finalizer loop, so its slots are already restored.
  */
  L = luaL_newstate();
  memset(&rec, 0, sizeof(rec));
  luaL_openlibs(L);
  run(L, "G = {}\n"
	 "function G.hit() G.n = (G.n or 0) + 1 return G.n end\n"
	 "local u = newproxy(true)\n"
	 "getmetatable(u).__gc = function() G.hit() end\n"
	 "G.u = u");
  CHECK(luaJIT_int3_start(L, int3_cb, &rec) == LUAJIT_INT3_OK);
  CHECK(arm_slot(L, "G", "hit", "ADDVN", &bcpos) == 0);
  rec.calls = 0;
  lua_close(L);
  printf("[5] close with finalizer: calls=%d\n", rec.calls);
  CHECK(rec.calls == 0);	/* Disarmed before the finalizer ran. */

  /* The request keeps the prototype alive: its memory must be released only
  ** after the request is gone.
  */
  L = luaL_newstate();
  memset(&rec, 0, sizeof(rec));
  luaL_openlibs(L);
  run(L, "G = {} function G.probe() return 1 end");
  CHECK(luaJIT_int3_start(L, int3_cb, &rec) == LUAJIT_INT3_OK);
  lua_getglobal(L, "G");
  lua_getfield(L, -1, "probe");
  CHECK(luaJIT_int3_arm_bc(L, -1, 1) == LUAJIT_INT3_OK);
  lua_pop(L, 2);
  /* Drop the last Lua reference and collect: only the request holds it. */
  run(L, "G.probe = nil; collectgarbage('collect'); collectgarbage('collect')");
  CHECK(run_number(L, "return collectgarbage('count')", &v) && v > 0);
  alive = v;
  CHECK(luaJIT_int3_cancel(L) == LUAJIT_INT3_OK);
  run(L, "collectgarbage('collect'); collectgarbage('collect')");
  CHECK(run_number(L, "return collectgarbage('count')", &v) && v > 0);
  printf("[5] proto held by the request: %.2f KB, after cancel %.2f KB\n",
	 alive, v);
  CHECK(v < alive);			/* The prototype was released. */
  CHECK(luaJIT_int3_stop(L) == LUAJIT_INT3_OK);

  /* A pending request survives a full GC and still fires. */
  memset(&rec, 0, sizeof(rec));
  run(L, "G = {} function G.sum(n) local s=0 for i=1,n do s=s+i end return s end");
  CHECK(luaJIT_int3_start(L, int3_cb, &rec) == LUAJIT_INT3_OK);
  CHECK(arm_slot(L, "G", "sum", "ADDVV", &bcpos) == 0);
  run(L, "collectgarbage('collect'); collectgarbage('collect')");
  CHECK(run_number(L, "return G.sum(10)", &v) && v == 55);
  printf("[5] armed after GC: calls=%d\n", rec.calls);
  CHECK(rec.calls == 10);		/* One delivery per iteration. */
  CHECK(luaJIT_int3_stop(L) == LUAJIT_INT3_OK);
  lua_close(L);
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
  /* The running breakpoint is still valid after dumping. */
  rec.calls = 0;
  CHECK(run_number(L, "return G.sum(100)", &v));
  printf("[6] breakpoint after dump: calls=%d value=%g\n", rec.calls, v);
  CHECK(rec.calls == 100);		/* One delivery per iteration. */
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

/* Deferred arming: a hook arms once, at one selected dispatch, and counts
** every event so that a delivery/re-arm loop fails the test instead of
** hanging it (10000 events is far above any legitimate run below).
*/
#define HOOK_ARM_EVENTS_MAX	10000

typedef struct HookArm {
  int res;			/* arm() result, -1 while not armed. */
  int bcpos;			/* Slot to arm. */
  int line;			/* Instruction hook: arm at this line. */
  const char *func;		/* Global function to arm. */
  int by_call;			/* Select the callee, not the slot. */
  int events;			/* Hook events seen so far. */
} HookArm;

static HookArm hook_arm;

static void hook_arm_reset(const char *func, int bcpos)
{
  hook_arm.res = -1;
  hook_arm.bcpos = bcpos;
  hook_arm.line = 0;
  hook_arm.func = func;
  hook_arm.by_call = 0;
  hook_arm.events = 0;
}

/* Arm G.<func> at hook_arm.bcpos, with the function at funcidx. */
static void hook_arm_do(lua_State *L, int funcidx)
{
  hook_arm.res = luaJIT_int3_arm_bc(L, funcidx, (unsigned int)hook_arm.bcpos);
}

/* Counted hook: arm G.func once, either at the call of that function
** (LUA_MASKCALL) or at the dispatch of the selected instruction.
*/
static void hook_arm_dispatch(lua_State *L, lua_Debug *ar)
{
  lua_Debug info = *ar;
  if (hook_arm.res != -1) return;  /* Arm once: re-arming would hang. */
  if (++hook_arm.events > HOOK_ARM_EVENTS_MAX) return;  /* Bounded failure. */
  if (!lua_getinfo(L, hook_arm.by_call ? "f" : "fl", &info)) return;
  if (!hook_arm.by_call && hook_arm.line > 0 &&
      info.currentline != hook_arm.line) {
    lua_pop(L, 1);  /* The running function pushed by "f". */
    return;
  }
  lua_getglobal(L, "G");
  lua_getfield(L, -1, hook_arm.func);
  if (lua_rawequal(L, -3, -1))  /* Stack: running function, G, target. */
    hook_arm_do(L, -1);
  lua_pop(L, 3);
}

static int flush_start_res, flush_on_res, flush_arm_res, flush_eng_on_res;
static int flush_stop_res, flush_disarm_res;
static const char *flush_disarm_func;	/* Disarm this slot in the event, if set. */
static int flush_disarm_bcpos;
static int flush_probe(lua_State *L)
{
  flush_start_res = luaJIT_int3_start(L, int3_cb, &probe_rec);
  flush_eng_on_res = luaJIT_setmode(L, 0, LUAJIT_MODE_ENGINE|LUAJIT_MODE_ON);
  lua_getglobal(L, "_PENDING_F");
  flush_on_res = luaJIT_setmode(L, -1, LUAJIT_MODE_FUNC|LUAJIT_MODE_ON);
  lua_pop(L, 1);
  flush_arm_res = luaJIT_int3_arm_bc(L, 0, 0);
  /* Disarming needs no safe point, so a flush event accepts it. The test
  ** disarms a slot of its own, so the breakpoint it counts stays armed.
  */
  if (flush_disarm_func != NULL) {
    lua_getglobal(L, "G");
    lua_getfield(L, -1, flush_disarm_func);
    flush_disarm_res = luaJIT_int3_cancel_bc(L, -1,
					     (unsigned int)flush_disarm_bcpos);
    lua_pop(L, 2);
  }
  flush_stop_res = luaJIT_int3_stop(L);
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

/* A hook that removes itself, arms a breakpoint and raises: the dispatch it
** would have returned to never runs the commit at its exit, so the accepted
** request must install the forced C dispatch itself.
*/
static int selfarm_res = -1;
static const char *selfarm_func;
static int selfarm_bcpos;

static void selfarm_hook(lua_State *L, lua_Debug *ar)
{
  (void)ar;
  lua_sethook(L, NULL, 0, 0);
  lua_getglobal(L, "G");
  lua_getfield(L, -1, selfarm_func);
  selfarm_res = luaJIT_int3_arm_bc(L, -1, (unsigned int)selfarm_bcpos);
  lua_pop(L, 2);
  lua_pushliteral(L, "hook error");
  (void)lua_error(L);
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
  /* A session already owns the process, so a second start is refused even
  ** from the hook (see [23] for a start that a hook is allowed to make). */
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
  flush_stop_res = flush_disarm_res = -1;
  flush_disarm_func = NULL;
  run(L, "jit.flush()");
  printf("[8] flush vmevent: start=%d on=%d arm=%d eng_on=%d disarm=%d "
	 "stop=%d\n", flush_start_res, flush_on_res, flush_arm_res,
	 flush_eng_on_res, flush_disarm_res, flush_stop_res);
  CHECK(flush_start_res == LUAJIT_INT3_BUSY);
  CHECK(flush_on_res == 0);           /* The armed prototype stays NOJIT. */
  CHECK(flush_arm_res == LUAJIT_INT3_BUSY);
  CHECK(flush_eng_on_res == 1);       /* The engine is not ours to gate. */
  CHECK(flush_stop_res == LUAJIT_INT3_BUSY);
  /* The session is still fully usable: the breakpoint survived the flush. */
  rec.calls = 0;
  CHECK(run_number(L, "return G.sum(100)", &v) && v == 5050);
  CHECK(rec.calls == 100);
  CHECK(luaJIT_int3_stop(L) == LUAJIT_INT3_OK);
  lua_close(L);

  /* Arming inside a hook is immediate. The hook runs inside the instruction
  ** dispatch, whose continuation re-reads the opcode of the instruction it is
  ** about to execute: that read must take the shadowed instruction from the
  ** breakpoint, so the instruction the hook belongs to still executes exactly
  ** once and the breakpoint fires from the next dispatch of the slot on. Arm
  ** the slot the hook is dispatching (MULVN, the first body instruction of
  ** G.hookarm): this execution must not deliver, every later one must.
  */
  L = luaL_newstate();
  memset(&rec, 0, sizeof(rec));
  memset(&probe_rec, 0, sizeof(probe_rec));
  luaL_openlibs(L);
  run(L, "G = {}\nfunction G.hookarm(a)\n  return a * 3 + 1\nend");
  CHECK(luaJIT_int3_start(L, int3_cb, &rec) == LUAJIT_INT3_OK);
  bcpos = find_bcpos_n(L, "G.hookarm", "MULVN", 0);
  CHECK(bcpos == 1);	/* The first body instruction. */
  hook_arm_reset("hookarm", bcpos);
  hook_arm.line = 3;		/* The body line: the dispatch of that slot. */
  lua_sethook(L, hook_arm_dispatch, LUA_MASKLINE|LUA_MASKCOUNT, 1);
  CHECK(run_number(L, "local x = 0 for i = 1, 200 do x = x + i end "
		      "return G.hookarm(10)", &v) && v == 31);
  lua_sethook(L, NULL, 0, 0);
  printf("[8] arm in hook: res=%d hits=%d then ", hook_arm.res, rec.calls);
  CHECK(hook_arm.res == LUAJIT_INT3_OK);
  CHECK(rec.calls == 0);	/* The hooking run re-dispatched the ins. */
  CHECK(hook_arm.events <= HOOK_ARM_EVENTS_MAX);
  rec.calls = 0;
  CHECK(run_number(L, "return G.hookarm(10)", &v) && v == 31);
  printf("%d\n", rec.calls);
  CHECK(rec.calls == 1);	/* The next execution delivers. */
  rec.calls = 0;
  CHECK(run_number(L, "return G.hookarm(10)", &v) && v == 31);
  CHECK(rec.calls == 1);	/* Permanent: no disarm, no re-arm. */
  CHECK(luaJIT_int3_cancel(L) == LUAJIT_INT3_OK);
  CHECK(luaJIT_int3_stop(L) == LUAJIT_INT3_OK);
  lua_close(L);

  /* A request accepted by a one-shot hook that then raises must still be
  ** mounted: nothing else rebuilds the dispatch table afterwards here (the
  ** JIT is off and no hook remains), so the pending request has to do it.
  */
  L = luaL_newstate();
  memset(&rec, 0, sizeof(rec));
  luaL_openlibs(L);
  run(L, "G = {} function G.sa(n) local s = 0 "
	 "for i = 1, n do s = s + i end return s end");
  CHECK(luaJIT_int3_start(L, int3_cb, &rec) == LUAJIT_INT3_OK);
  selfarm_bcpos = find_bcpos_n(L, "G.sa", "ADDVV", 0);
  CHECK(selfarm_bcpos >= 0);
  selfarm_res = -1;
  selfarm_func = "sa";
  lua_sethook(L, selfarm_hook, LUA_MASKCOUNT, 1);
  CHECK(run_fails(L, "local s = 0 for i = 1, 500 do s = s + i end"));
  /* The hook removed itself before arming, so nothing rebuilds the dispatch
  ** table from here on: the accepted request has to do it. */
  printf("[8] arm in unhooking hook: res=%d hits=", selfarm_res);
  CHECK(selfarm_res == LUAJIT_INT3_OK);
  rec.calls = 0;
  CHECK(run_number(L, "return G.sa(50)", &v) && v == 1275);
  printf("%d\n", rec.calls);
  CHECK(rec.calls == 50);		/* Mounted and delivered. */
  CHECK(luaJIT_int3_stop(L) == LUAJIT_INT3_OK);
  lua_close(L);

  /* Starting a session neither flushes traces nor emits a flush event. */
  L = luaL_newstate();
  memset(&rec, 0, sizeof(rec));
  memset(&probe_rec, 0, sizeof(probe_rec));
  luaL_openlibs(L);
  run(L, "G = {}\n"
	 "function G.sum(n) local s=0 for i=1,n do s=s+i end return s end\n"
	 "function G.aux(x) local r = x + 1 return r end");
  run(L, "for i = 1, 2 do G.sum(1000) end");
  lua_pushcfunction(L, flush_probe);
  attach_probe(L, lua_gettop(L), "trace", "flush");
  lua_pop(L, 1);
  flush_start_res = flush_on_res = flush_arm_res = flush_eng_on_res = -1;
  flush_stop_res = flush_disarm_res = -1;
  flush_disarm_func = NULL;
  {
    int traces = trace_count(L);
    CHECK(luaJIT_int3_start(L, int3_cb, &rec) == LUAJIT_INT3_OK);
    CHECK(arm_slot(L, "G", "aux", "ADDVN", &flush_disarm_bcpos) == 0);
    flush_disarm_func = "aux";
    printf("[8] start: traces=%d->%d probe=%d jit=%d\n", traces,
	   trace_count(L), flush_start_res, run_bool(L, "return jit.status()"));
    CHECK(trace_count(L) == traces);
    CHECK(flush_start_res == -1);      /* No flush event on start. */
    CHECK(run_bool(L, "return jit.status()"));
  }
  /* Disarming in a flush VM event: accepted, because it needs no safe point.
  ** The slot is restored before the flush returns to the interpreter.
  */
  flush_disarm_res = -1;
  run(L, "jit.flush()");
  printf("[8] disarm in flush: res=%d\n", flush_disarm_res);
  CHECK(flush_disarm_res == LUAJIT_INT3_OK);
  rec.calls = 0;
  CHECK(run_number(L, "return G.aux(1)", &v) && v == 2);
  CHECK(rec.calls == 0);		/* Disarmed: no trap left in the slot. */
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

/* The three dispatch paths an arm from a Lua hook has to survive: the
** instruction trampoline (line/count hook), the call trampoline (call hook),
** and the arm-time flush of JIT-patched bytecode from inside that
** prototype's own dispatch. The trap is planted immediately, so the
** continuation that re-dispatches the interrupted instruction must take the
** shadowed instruction instead of re-reading the slot.
*/
static void test_arm_hook_immediate(void)
{
  lua_State *L;
  Int3Rec rec;
  double v;
  int bcpos;

  /* Current slot: the hook fires for the dispatch of the slot it arms, so
  ** that one execution must still run with its own opcode and the breakpoint
  ** must fire from the next execution on.
  */
  L = luaL_newstate();
  memset(&rec, 0, sizeof(rec));
  luaL_openlibs(L);
  run(L, "G = {}\nfunction G.once(a)\n  return a * 3 + 1\nend");
  CHECK(luaJIT_int3_start(L, int3_cb, &rec) == LUAJIT_INT3_OK);
  bcpos = find_bcpos_n(L, "G.once", "MULVN", 0);
  CHECK(bcpos == 1);	/* The first body instruction: the current slot. */
  hook_arm_reset("once", bcpos);
  hook_arm.line = 3;	/* Only arm at the dispatch of the body line. */
  lua_sethook(L, hook_arm_dispatch, LUA_MASKLINE|LUA_MASKCOUNT, 1);
  CHECK(run_number(L, "return G.once(10)", &v) && v == 31);
  lua_sethook(L, NULL, 0, 0);
  {
    int hits = rec.calls;
    CHECK(hook_arm.res == LUAJIT_INT3_OK);
    CHECK(hits == 0);	/* The current execution ran with its own opcode. */
    CHECK(hook_arm.events <= HOOK_ARM_EVENTS_MAX);
    rec.calls = 0;
    CHECK(run_number(L, "return G.once(10)", &v) && v == 31);
    printf("[8] current slot: res=%d hits=%d then %d events=%d value=%g\n",
	   hook_arm.res, hits, rec.calls, hook_arm.events, v);
    CHECK(rec.calls == 1);	/* The next execution of the slot delivers. */
    CHECK((int)rec.bcpos == bcpos);
    rec.calls = 0;
    CHECK(run_number(L, "return G.once(10)", &v) && v == 31);
    CHECK(rec.calls == 1);	/* Permanent: no disarm after a hit. */
  }
  CHECK(luaJIT_int3_stop(L) == LUAJIT_INT3_OK);
  lua_close(L);

  /* Call hook: lj_dispatch_call's trampoline. The callee's first body
  ** instruction has not been dispatched yet, so arming it from the call hook
  ** fires as soon as execution reaches it: this call delivers.
  */
  L = luaL_newstate();
  memset(&rec, 0, sizeof(rec));
  luaL_openlibs(L);
  run(L, "G = {}\nfunction G.callee(a)\n  return a * 3 + 1\nend");
  CHECK(luaJIT_int3_start(L, int3_cb, &rec) == LUAJIT_INT3_OK);
  bcpos = find_bcpos_n(L, "G.callee", "MULVN", 0);
  CHECK(bcpos == 1);
  hook_arm_reset("callee", bcpos);
  hook_arm.by_call = 1;
  lua_sethook(L, hook_arm_dispatch, LUA_MASKCALL, 0);
  CHECK(run_number(L, "return G.callee(10)", &v) && v == 31);
  lua_sethook(L, NULL, 0, 0);
  {
    int hits = rec.calls;
    CHECK(hook_arm.res == LUAJIT_INT3_OK);
    CHECK(hits == 1);	/* The armed slot was dispatched by this call. */
    CHECK(hook_arm.events <= HOOK_ARM_EVENTS_MAX);
    CHECK((int)rec.bcpos == bcpos);
    rec.calls = 0;
    CHECK(run_number(L, "return G.callee(10)", &v) && v == 31);
    printf("[8] call hook: res=%d hits=%d then %d events=%d value=%g\n",
	   hook_arm.res, hits, rec.calls, hook_arm.events, v);
    CHECK(rec.calls == 1);
    rec.calls = 0;
    CHECK(run_number(L, "return G.callee(10)", &v) && v == 31);
    CHECK(rec.calls == 1);
  }
  CHECK(luaJIT_int3_stop(L) == LUAJIT_INT3_OK);
  lua_close(L);

  /* J-patched loop: the hook arms a slot of the prototype whose JFORI/JFORL
  ** bytecode it is dispatching, so arm's flush rewrites that bytecode while
  ** the dispatcher is mid-way. The flushed loop runs in the interpreter and
  ** the armed slot then delivers once per remaining iteration.
  **
  ** The warm-up needs hotloop=1 to commit the loop trace; the hooked run then
  ** needs the hot counters out of the way again, because hotloop also arms
  ** the hot-call counter and the callee's header would start a recording that
  ** a hook aborts mid-way (arm then correctly refuses while the recorder is
  ** active).
  */
  L = luaL_newstate();
  memset(&rec, 0, sizeof(rec));
  luaL_openlibs(L);
  run(L, "G = {}\n"
	 "function G.jitloop(n)\n"
	 "  local s = 0\n"
	 "  for i = 1, n do\n"
	 "    s = s + i\n"
	 "  end\n"
	 "  return s\n"
	 "end");
  run(L, "jit.opt.start('hotloop=1')");
  CHECK(run_number(L, "return G.jitloop(200)", &v) && v == 20100);
  run(L, "jit.opt.start('hotloop=1000')");
  {
    int forl = find_bcpos_n(L, "G.jitloop", "FORL", 0);
    int op_jforl = opcode_of(L, "JFORL");
    int patched;
    CHECK(forl >= 0 && op_jforl >= 0);
    patched = (int)((unsigned)funcbc(L, "G.jitloop", forl) & 0xffu);
    CHECK(patched == op_jforl);	/* The loop-back slot starts a trace. */
    bcpos = find_bcpos_n(L, "G.jitloop", "ADDVV", 0);
    CHECK(bcpos > 0);
    CHECK(luaJIT_int3_start(L, int3_cb, &rec) == LUAJIT_INT3_OK);
    /* The call hook fires before the callee's first dispatch, i.e. while the
    ** J-patched loop bytecode is still in place: arm() flushes that trace from
    ** inside the dispatcher and plants the trap for the loop body.
    */
    hook_arm_reset("jitloop", bcpos);
    hook_arm.by_call = 1;
    lua_sethook(L, hook_arm_dispatch, LUA_MASKCALL, 0);
    CHECK(run_number(L, "return G.jitloop(200)", &v) && v == 20100);
    lua_sethook(L, NULL, 0, 0);
    {
      int hits = rec.calls;
      CHECK(hook_arm.res == LUAJIT_INT3_OK);  /* The flush was reachable. */
      CHECK(hook_arm.events <= HOOK_ARM_EVENTS_MAX);
      CHECK(hits == 200);	/* The flushed loop reaches the armed slot. */
      CHECK((int)rec.bcpos == bcpos);
      rec.calls = 0;
      CHECK(run_number(L, "return G.jitloop(200)", &v) && v == 20100);
      printf("[8] flush in hook: res=%d hits=%d then %d events=%d "
	     "funcbc=%d/%d value=%g\n", hook_arm.res, hits, rec.calls,
	     hook_arm.events, patched, op_jforl, v);
      CHECK(rec.calls == 200);	/* Permanent: one delivery per iteration. */
    }
  }
  CHECK(luaJIT_int3_stop(L) == LUAJIT_INT3_OK);
  lua_close(L);
}

/* A hook that arms a breakpoint and then throws. lua_pcall's error path
** restores the hookmask from the snapshot taken before the call, and
** lua_sethook(NULL) rebuilds the dispatch table from the restored mask. The
** armed slot must survive both and keep trapping.
*/
static int hook_error_res;
static int hook_error_bcpos;
static int hook_error_events;

static void hook_arm_error(lua_State *L, lua_Debug *ar)
{
  (void)ar;
  if (hook_error_events++ > 0) return;	/* Arm and throw once. */
  lua_getglobal(L, "G");
  lua_getfield(L, -1, "errfunc");
  hook_error_res =
    luaJIT_int3_arm_bc(L, lua_gettop(L), (unsigned int)hook_error_bcpos);
  lua_pop(L, 2);
  lua_pushliteral(L, "arm hook error");
  lua_error(L);  /* Unwind the call through lua_pcall. */
}

static void test_arm_hook_error(void)
{
  static const char code[] = "return G.errfunc(10)";
  lua_State *L = luaL_newstate();
  Int3Rec rec;
  double v;
  int caught = 0, hits, i;
  memset(&rec, 0, sizeof(rec));
  luaL_openlibs(L);
  run(L, "G = {}\nfunction G.errfunc(a)\n  return a * 3 + 1\nend");
  CHECK(luaJIT_int3_start(L, int3_cb, &rec) == LUAJIT_INT3_OK);
  hook_error_bcpos = find_bcpos_n(L, "G.errfunc", "MULVN", 0);
  CHECK(hook_error_bcpos == 1);	/* The first body instruction. */
  hook_error_res = -1;
  hook_error_events = 0;
  lua_sethook(L, hook_arm_error, LUA_MASKCALL, 0);
  if (luaL_loadbuffer(L, code, sizeof(code)-1, "=smoke")) {
    fprintf(stderr, "FAIL lua error: %s\n", lua_tostring(L, -1));
    failures++;
    lua_pop(L, 1);
  } else if (lua_pcall(L, 0, 1, 0)) {
    const char *msg = lua_tostring(L, -1);
    caught = msg != NULL && strstr(msg, "arm hook error") != NULL;
    lua_pop(L, 1);
  } else {
    lua_pop(L, 1);  /* The hook error cannot have been swallowed. */
  }
  lua_sethook(L, NULL, 0, 0);	/* Rebuild dispatch from the restored mask. */
  /* The breakpoint is still armed, so every execution of the slot delivers.
  */
  rec.calls = 0;
  for (i = 0; i < 3; i++)
    CHECK(run_number(L, "return G.errfunc(10)", &v) && v == 31);
  hits = rec.calls;
  printf("[8] arm then hook error: res=%d caught=%d hits=%d\n",
	 hook_error_res, caught, hits);
  CHECK(hook_error_res == LUAJIT_INT3_OK);
  CHECK(caught);
  CHECK(hits == 3);	/* The breakpoint survived the error path. */
  CHECK(luaJIT_int3_cancel(L) == LUAJIT_INT3_OK);
  rec.calls = 0;
  CHECK(run_number(L, "return G.errfunc(10)", &v) && v == 31);
  CHECK(rec.calls == 0);	/* Cancel disarmed the slot. */
  CHECK(luaJIT_int3_stop(L) == LUAJIT_INT3_OK);
  lua_close(L);
}

#endif  /* !SMOKE_NO_JIT && !SMOKE_NO_PROFILE */

/* -- [9] Permanent breakpoints: delivery, disarming, callback actions ---- */

#if !defined(SMOKE_NO_PROFILE)
typedef struct BpRec {
  int calls;			/* Deliveries so far. */
  const void *func;		/* Running closure of the last delivery. */
  unsigned int bcpos;		/* bcpos of the last delivery. */
  int depth;			/* lua_gettop() inside the delivery. */
  /* Actions of the first delivery. */
  int res_arm_bad;		/* arm() with an out-of-range index. */
  int res_arm_self;		/* arm() of the delivering slot. */
  int res_arm_other;		/* arm() of the other slot of arm_ref. */
  int do_arm_other;
  int arm_ref;			/* Registry reference of that function. */
  unsigned int arm_bcpos;
  /* Actions of the Nth delivery. */
  int cancel_at;		/* cancel() on this delivery (0 = never). */
  int stop_at;			/* stop() on this delivery (0 = never). */
  int restart_at;		/* stop() then start() (0 = never). */
  int res_cancel;
  int res_stop;
  int res_restart;
} BpRec;

static void bp_cb(void *data, lua_State *L, const void *func,
		  unsigned int bcpos)
{
  BpRec *r = (BpRec *)data;
  bp_guard(L);
  r->calls++;
  r->func = func;
  r->bcpos = bcpos;
  r->depth = lua_gettop(L) >= 0 ? lua_gettop(L) : -1;
  if (func == NULL || L == NULL) failures++;
  if (r->calls == 1) {
    /* The delivery context accepts the breakpoint operations: an out-of-range
    ** index must fail with INVALID instead of the unsafe-context error.
    */
    r->res_arm_bad = luaJIT_int3_arm_bc(L, 4096, 0);
    if (r->arm_ref != 0) {
      /* One registry read: the function is already rooted, and no allocation,
      ** GC step or Lua code is involved.
      */
      lua_rawgeti(L, LUA_REGISTRYINDEX, r->arm_ref);
      r->res_arm_self = luaJIT_int3_arm_bc(L, -1, bcpos);
      if (r->do_arm_other)
	r->res_arm_other = luaJIT_int3_arm_bc(L, -1, r->arm_bcpos);
      lua_pop(L, 1);
    }
  }
  if (r->cancel_at != 0 && r->calls == r->cancel_at)
    r->res_cancel = luaJIT_int3_cancel(L);
  if (r->stop_at != 0 && r->calls == r->stop_at)
    r->res_stop = luaJIT_int3_stop(L);
  if (r->restart_at != 0 && r->calls == r->restart_at) {
    /* Release the session that is delivering this hit and own a new one: the
    ** delivery must not touch the released state afterwards.
    */
    r->res_stop = luaJIT_int3_stop(L);
    r->res_restart = luaJIT_int3_start(L, bp_cb, r);
  }
}

/* Registry reference of a global function, for the callback to arm. */
static int bp_ref(lua_State *L, const char *tbl, const char *fname)
{
  int ref;
  lua_getglobal(L, tbl);
  lua_getfield(L, -1, fname);
  lua_remove(L, -2);
  ref = luaL_ref(L, LUA_REGISTRYINDEX);
  return ref;
}

static void test_permanent(void)
{
  lua_State *L = luaL_newstate();
  BpRec rec;
  int bcpos = -1;
  double v;
  memset(&rec, 0, sizeof(rec));
  luaL_openlibs(L);
  run(L,
    "G = {}\n"
    "N = 0\n"
    "function G.loop(n)\n"
    "  local s = 0\n"
    "  for i = 1, n do s = s + i end\n"
    "  return s\n"
    "end\n"
    "function G.count() N = N + 1 return 1 end\n"
    "function G.drive(n)\n"
    "  local s = 0\n"
    "  for i = 1, n do s = s + G.count() end\n"
    "  return s\n"
    "end\n"
    "function G.two(n)\n"
    "  local s = 0\n"
    "  for i = 1, n do s = s + i end\n"
    "  return s * 2\n"
    "end\n");

  /* One delivery per execution: 200 iterations, 200 deliveries, and the next
  ** call keeps counting. Re-arming the delivering slot from the callback is a
  ** no-op: it neither adds a slot nor multiplies the deliveries.
  */
  CHECK(luaJIT_int3_start(L, bp_cb, &rec) == LUAJIT_INT3_OK);
  rec.arm_ref = bp_ref(L, "G", "loop");
  CHECK(arm_slot(L, "G", "loop", "ADDVV", &bcpos) == 0);
  CHECK(run_number(L, "return G.loop(200)", &v) && v == 20100);
  printf("[9] permanent: calls=%d bcpos=%u/%d arm_self=%d bad=%d\n",
	 rec.calls, rec.bcpos, bcpos, rec.res_arm_self, rec.res_arm_bad);
  CHECK(rec.calls == 200);
  CHECK((int)rec.bcpos == bcpos);
  CHECK(rec.func != NULL);
  CHECK(rec.depth >= 0);		/* The frame view was readable. */
  CHECK(rec.res_arm_bad == LUAJIT_INT3_INVALID);	/* Context accepted. */
  CHECK(rec.res_arm_self == LUAJIT_INT3_OK);		/* Idempotent. */
  CHECK(run_number(L, "return G.loop(5)", &v) && v == 15);
  CHECK(rec.calls == 205);

  /* cancel() from the callback: no further delivery, the hit that cancelled
  ** still runs its own instruction, and the loop completes. The session
  ** itself stays open, so the next arm works.
  */
  CHECK(luaJIT_int3_cancel(L) == LUAJIT_INT3_OK);
  memset(&rec, 0, sizeof(rec));
  rec.cancel_at = 3;
  CHECK(arm_slot(L, "G", "loop", "ADDVV", &bcpos) == 0);
  CHECK(run_number(L, "return G.loop(100)", &v) && v == 5050);
  printf("[9] cancel in cb: calls=%d cancel=%d value=%g\n", rec.calls,
	 rec.res_cancel, v);
  CHECK(rec.calls == 3);
  CHECK(rec.res_cancel == LUAJIT_INT3_OK);
  CHECK(v == 5050);		/* The disarmed hit still ran. */
  rec.calls = 0;
  CHECK(run_number(L, "return G.loop(100)", &v) && v == 5050);
  CHECK(rec.calls == 0);	/* Disarmed. */
  CHECK(luaJIT_int3_arm_bc(L, 1, 0) == LUAJIT_INT3_INVALID);  /* Session open. */

  /* stop() from the callback: the Nth hit still runs its instruction, so
  ** every side effect of the trapped call happens, and the session is
  ** released afterwards.
  */
  memset(&rec, 0, sizeof(rec));
  rec.stop_at = 5;
  CHECK(arm_slot(L, "G", "drive", "CALL", &bcpos) == 0);
  CHECK(run_number(L, "return G.drive(50)", &v) && v == 50);
  printf("[9] stop in cb: calls=%d stop=%d value=%g\n", rec.calls,
	 rec.res_stop, v);
  CHECK(rec.calls == 5);
  CHECK(rec.res_stop == LUAJIT_INT3_OK);
  CHECK(v == 50);		/* The loop completed. */
  CHECK(run_number(L, "return N", &v) && v == 50);  /* Every side effect. */
  CHECK(luaJIT_int3_arm_bc(L, 1, 0) == LUAJIT_INT3_DISABLED);  /* Released. */

  /* stop() and a start() attempt in the same delivery: the hit that did it
  ** still runs its instruction, the released session is never touched again,
  ** and a delivery cannot create the next one (a host call can).
  */
  memset(&rec, 0, sizeof(rec));
  rec.restart_at = 3;
  CHECK(luaJIT_int3_start(L, bp_cb, &rec) == LUAJIT_INT3_OK);
  CHECK(arm_slot(L, "G", "drive", "CALL", &bcpos) == 0);
  CHECK(run_number(L, "return G.drive(50)", &v) && v == 50);
  printf("[9] restart in cb: calls=%d stop=%d start=%d value=%g\n",
	 rec.calls, rec.res_stop, rec.res_restart, v);
  CHECK(rec.calls == 3);
  CHECK(rec.res_stop == LUAJIT_INT3_OK);
  CHECK(rec.res_restart == LUAJIT_INT3_BUSY);
  CHECK(v == 50);		/* The loop completed: nothing is trapped. */
  CHECK(luaJIT_int3_arm_bc(L, 1, 0) == LUAJIT_INT3_DISABLED);
  CHECK(luaJIT_int3_start(L, bp_cb, &rec) == LUAJIT_INT3_OK);
  rec.restart_at = 0;		/* The new session delivers plainly. */
  CHECK(arm_slot(L, "G", "drive", "CALL", &bcpos) == 0);
  rec.calls = 0;
  CHECK(run_number(L, "return G.drive(10)", &v) && v == 10);
  CHECK(rec.calls == 10);	/* The new session delivers normally. */
  CHECK(luaJIT_int3_stop(L) == LUAJIT_INT3_OK);

  /* Arming another slot from the callback takes effect at once: the very
  ** next execution of that slot delivers.
  */
  memset(&rec, 0, sizeof(rec));
  CHECK(luaJIT_int3_start(L, bp_cb, &rec) == LUAJIT_INT3_OK);
  {
    int bc_mul = find_bcpos_n(L, "G.two", "MULVN", 0);
    CHECK(bc_mul > 0);
    CHECK(arm_slot(L, "G", "two", "ADDVV", &bcpos) == 0);
    rec.arm_ref = bp_ref(L, "G", "two");
    rec.do_arm_other = 1;
    rec.arm_bcpos = (unsigned int)bc_mul;
    CHECK(run_number(L, "return G.two(4)", &v) && v == 20);
    printf("[9] arm from cb: calls=%d arm=%d bcpos=%u want=%d\n", rec.calls,
	   rec.res_arm_other, rec.bcpos, bc_mul);
    CHECK(rec.calls == 5);	/* Four loop hits plus the armed MULVN. */
    CHECK((int)rec.bcpos == bc_mul);
    CHECK(rec.res_arm_self == LUAJIT_INT3_OK);
    CHECK(rec.res_arm_other == LUAJIT_INT3_OK);
  }
  CHECK(luaJIT_int3_stop(L) == LUAJIT_INT3_OK);

  /* Function headers stay refused in every form: the frame is not set up when
  ** the header runs. Loop and return slots are armable now -- mounting flushes
  ** the traces of the prototype and excludes it from compilation, so no trace
  ** of it can rewrite them; the delivery counts are checked in [20].
  */
  memset(&rec, 0, sizeof(rec));
  CHECK(luaJIT_int3_start(L, int3_cb, &rec) == LUAJIT_INT3_OK);
  CHECK(arm_pos(L, "G", "loop", 0) == LUAJIT_INT3_UNARMABLE);  /* FUNCF */
  CHECK(arm_code(L, "G", "loop", "FORL", 0) == LUAJIT_INT3_OK);
  CHECK(luaJIT_int3_cancel(L) == LUAJIT_INT3_OK);
  run(L, "function G.wloop(n) local i = 0 while i < n do i = i + 1 end "
	 "return i end");
  CHECK(arm_code(L, "G", "wloop", "LOOP", 0) == LUAJIT_INT3_OK);
  CHECK(luaJIT_int3_cancel(L) == LUAJIT_INT3_OK);
  /* Numeric-for loops start with the fused loop-init instruction
  ** (lj_parse.c:2862). The tracer patches it together with the loop-back slot
  ** when that slot starts a trace (FORI->JFORI, lj_trace.c:517), and a
  ** prototype excluded from the JIT gets its loop-back slot lazily rewritten
  ** (FORL->IFORL) instead. Both forms are accepted now: mounting flushes the
  ** trace first and captures whatever the slot holds at that point, so a
  ** JFUNCI/JFORL/I-variant is rewritten back before it is trapped.
  */
  run(L, "function G.fori(n) local s = 0 for i = 1, n do s = s + i end "
	 "return s end\n");
  {
    int fori = find_bcpos_n(L, "G.fori", "FORI", 0);
    CHECK(fori >= 0);
    CHECK(arm_code(L, "G", "fori", "FORI", 0) == LUAJIT_INT3_OK);
    CHECK(luaJIT_int3_cancel(L) == LUAJIT_INT3_OK);
#if !defined(SMOKE_NO_JIT)
    {
      int op_fori = opcode_of(L, "FORI");
      int op_jfori = opcode_of(L, "JFORI");
      CHECK(op_fori >= 0 && op_jfori >= 0);
      CHECK(((unsigned)funcbc(L, "G.fori", fori) & 0xffu) ==
	    (unsigned)op_fori);
      /* Make the loop-back slot hot: its trace rewrites the header, too. */
      run(L, "jit.opt.start('hotloop=1')");
      CHECK(run_number(L, "return G.fori(200)", &v) && v == 20100);
      run(L, "jit.opt.start('hotloop=56')");
      CHECK(((unsigned)funcbc(L, "G.fori", fori) & 0xffu) ==
	    (unsigned)op_jfori);
      /* The patched header is accepted; mounting rewrites it back to FORI. */
      CHECK(arm_code(L, "G", "fori", "JFORI", 0) == LUAJIT_INT3_OK);
      CHECK(luaJIT_int3_cancel(L) == LUAJIT_INT3_OK);
    }
#endif
  }
#if !defined(SMOKE_NO_JIT)
  run(L, "function G.ip(n) local s = 0 for i = 1, n do s = s + i end "
	 "return s end\n"
	 "jit.off(G.ip, true)\n");
  {
    int op_forl = opcode_of(L, "FORL");
    int op_iforl = opcode_of(L, "IFORL");
    int iforl = find_bcpos_n(L, "G.ip", "FORL", 0);
    CHECK(op_forl >= 0 && op_iforl >= 0);
    CHECK(iforl >= 0);
    CHECK(((unsigned)funcbc(L, "G.ip", iforl) & 0xffu) == (unsigned)op_forl);
    /* PROTO_NOJIT: the hot loop-back slot is blacklisted, not traced, and
    ** trace_start() rewrites FORL into IFORL.
    */
    run(L, "jit.opt.start('hotloop=1')");
    CHECK(run_number(L, "return G.ip(200)", &v) && v == 20100);
    run(L, "jit.opt.start('hotloop=56')");
    CHECK(((unsigned)funcbc(L, "G.ip", iforl) & 0xffu) == (unsigned)op_iforl);
    CHECK(arm_code(L, "G", "ip", "IFORL", 0) == LUAJIT_INT3_OK);
    CHECK(luaJIT_int3_cancel(L) == LUAJIT_INT3_OK);
  }
#endif
  /* Return slots are armable as well; the return hook still observes the
  ** shadowed instruction (disp_ins() in lj_dispatch_ins()), see [20].
  */
  run(L, "function G.ret1(i) return i * 2 end\n");
  CHECK(arm_code(L, "G", "ret1", "RET1", 0) == LUAJIT_INT3_OK);
  CHECK(luaJIT_int3_cancel(L) == LUAJIT_INT3_OK);
  run(L, "function G.va(...) local a = ... return a end\n");
  CHECK(arm_pos(L, "G", "va", 0) == LUAJIT_INT3_UNARMABLE);  /* FUNCV */
  /* The jump slot that a comparison or a unary test consumes is never
  ** dispatched on its own: the preceding instruction reads the target from
  ** that slot and then either branches or skips it, so a trap there could
  ** never fire. The dispatched jump (a loop back edge) stays armable.
  */
  run(L, "function G.cond(x) if x then return 1 end return 2 end\n");
  {
    int isf = find_bcpos_n(L, "G.cond", "ISF", 0);
    int jmp0 = find_bcpos_n(L, "G.cond", "JMP", 0);
    CHECK(isf >= 0 && jmp0 == isf + 1);	/* The pair is adjacent. */
    CHECK(arm_pos(L, "G", "cond", jmp0) == LUAJIT_INT3_UNARMABLE);
  }
  run(L, "function G.cmpjmp(a, b) if a < b then return 1 end return 2 end\n");
  {
    int isge = find_bcpos_n(L, "G.cmpjmp", "ISGE", 0);
    int jmp0 = find_bcpos_n(L, "G.cmpjmp", "JMP", 0);
    CHECK(isge >= 0 && jmp0 == isge + 1);
    CHECK(arm_pos(L, "G", "cmpjmp", jmp0) == LUAJIT_INT3_UNARMABLE);
  }
  run(L, "function G.wback(n) local i = 0 while i < n do i = i + 1 end "
	 "return i end\n");
  {
    int jmp1 = find_bcpos_n(L, "G.wback", "JMP", 1);
    CHECK(jmp1 > 0);
    CHECK(arm_pos(L, "G", "wback", jmp1) == LUAJIT_INT3_OK);
    rec.calls = 0;
    CHECK(run_number(L, "return G.wback(3)", &v) && v == 3);
    CHECK(rec.calls == 3);	/* Dispatched once per iteration. */
    CHECK(luaJIT_int3_cancel(L) == LUAJIT_INT3_OK);
  }
  /* RETM is never rewritten by the tracer, so it stays armable. */
  run(L, "function G.va2(...) return ... end\n");
  CHECK(arm_code(L, "G", "va2", "RETM", 0) == LUAJIT_INT3_OK);
  /* A breakpoint inside a called function: one delivery per call. */
  run(L, "function G.f(i) return i * 2 end\n");
  memset(&rec, 0, sizeof(rec));
  CHECK(arm_slot(L, "G", "f", "MULVN", &bcpos) == 0);
  run(L, "local s = 0 for i = 1, 100 do s = s + G.f(i) end\n"
	 "assert(s == 10100, s)");
  printf("[9] MULVN per call: calls=%d bcpos=%u/%d\n", rec.calls, rec.bcpos,
	 bcpos);
  CHECK(rec.calls == 100);
  CHECK(luaJIT_int3_stop(L) == LUAJIT_INT3_OK);
  lua_close(L);
}
#endif  /* !SMOKE_NO_PROFILE */

/* -- [10] Breakpoints in several prototypes at once ---------------------- */

#if !defined(SMOKE_NO_PROFILE)
static void test_multi(void)
{
  lua_State *L = luaL_newstate();
  Int3Rec rec;
  int bc_a = -1, bc_b = -1, bc_c = -1, i, res, narmed;
  double v;
  memset(&rec, 0, sizeof(rec));
  luaL_openlibs(L);
  run(L,
    "G = {}\n"
    "function G.a(n) local s=0 for i=1,n do s=s+i end return s end\n"
    "function G.b(n) local s=0 for i=1,n do s=s+i*2 end return s end\n"
    "function G.c(n) local s=0 for i=1,n do s=s+i*3 end return s end\n");
  CHECK(luaJIT_int3_start(L, int3_cb, &rec) == LUAJIT_INT3_OK);
  /* Three prototypes, three slots, all armed at the same time. */
  CHECK(arm_slot(L, "G", "a", "ADDVV", &bc_a) == 0);
  CHECK(arm_slot(L, "G", "b", "ADDVV", &bc_b) == 0);
  CHECK(arm_slot(L, "G", "c", "ADDVV", &bc_c) == 0);
  /* Arming an already armed slot again is a no-op: no new slot, no duplicate
  ** delivery.
  */
  CHECK(arm_slot(L, "G", "a", "ADDVV", &i) == 0);
  CHECK(i == bc_a);
  rec.calls = 0;
  CHECK(run_number(L, "return G.a(10) + G.b(10) + G.c(10)", &v) && v == 330);
  printf("[10] three protos: calls=%d value=%g\n", rec.calls, v);
  CHECK(rec.calls == 30);		/* Ten iterations per prototype. */
  /* cancel() disarms every breakpoint. */
  CHECK(luaJIT_int3_cancel(L) == LUAJIT_INT3_OK);
  rec.calls = 0;
  CHECK(run_number(L, "return G.a(10) + G.b(10) + G.c(10)", &v) && v == 330);
  CHECK(rec.calls == 0);

  /* The table is bounded: keep arming fresh slots until it is full. */
  run(L, "function G.big(n)\n  local s = 0\n"
	 "  s=s+n s=s+n s=s+n s=s+n s=s+n s=s+n s=s+n s=s+n\n"
	 "  s=s+n s=s+n s=s+n s=s+n s=s+n s=s+n s=s+n s=s+n\n"
	 "  s=s+n s=s+n s=s+n s=s+n s=s+n s=s+n s=s+n s=s+n\n"
	 "  s=s+n s=s+n s=s+n s=s+n s=s+n s=s+n s=s+n s=s+n\n"
	 "  s=s+n s=s+n s=s+n s=s+n s=s+n s=s+n s=s+n s=s+n\n"
	 "  return s\n"
	 "end\n");
  lua_getglobal(L, "G");
  lua_getfield(L, -1, "big");
  narmed = 0;
  for (i = 0; i < 200; i++) {
    res = luaJIT_int3_arm_bc(L, -1, (unsigned int)i);
    if (res == LUAJIT_INT3_FULL) break;
    if (res == LUAJIT_INT3_OK) narmed++;
    else if (res != LUAJIT_INT3_UNARMABLE)	/* Header slot. */
      CHECK(res == LUAJIT_INT3_INVALID);	/* Past the last bytecode. */
  }
  lua_pop(L, 2);
  printf("[10] full table: armed=%d stopped_at=%d res=%d\n", narmed, i, res);
  CHECK(i < 200);			/* Hit the limit, not the end. */
  CHECK(res == LUAJIT_INT3_FULL);
  CHECK(narmed > 0);
  /* A hit does not free capacity: executing an armed slot keeps its entry, so
  ** a slot of another prototype is still refused.
  */
  run(L, "function G.big2(n) return n + 1 end");
  rec.calls = 0;
  CHECK(run_number(L, "return G.big(1)", &v) && v == 40);
  printf("[10] after hits: hits=%d still_full=%d\n", rec.calls,
	 arm_code(L, "G", "big2", "ADDVN", 0));
  CHECK(rec.calls > 0);
  CHECK(arm_code(L, "G", "big2", "ADDVN", 0) == LUAJIT_INT3_FULL);
  /* PROTO_NOJIT ownership is a JIT-only mechanism: without the engine
  ** luaJIT_setmode() refuses every request, so neither check applies.
  */
#if !defined(SMOKE_NO_JIT)
  CHECK(set_jit_on(L, "G", "big") == 0);		/* Excluded. */
#endif
  /* The full table does not leak into a dump or a later session, and every
  ** breakpoint released its JIT exclusion. */
  CHECK(luaJIT_int3_cancel(L) == LUAJIT_INT3_OK);
#if !defined(SMOKE_NO_JIT)
  CHECK(set_jit_on(L, "G", "big") == 1);		/* Every request released. */
#endif
  lua_getglobal(L, "G");
  lua_getfield(L, -1, "big");
  CHECK(luaJIT_int3_arm_bc(L, -1, 1) == LUAJIT_INT3_OK);
  lua_pop(L, 2);
  CHECK(luaJIT_int3_stop(L) == LUAJIT_INT3_OK);
  lua_close(L);
}
#endif  /* !SMOKE_NO_PROFILE */

/* -- [11] Call hook and hot-count paths with a trapped slot -------------- */

#if !defined(SMOKE_NO_PROFILE)
static void test_hook_and_hotcount(void)
{
  lua_State *L = luaL_newstate();
  BpRec rec;
  int bcpos = -1;
  double v;
  memset(&rec, 0, sizeof(rec));
  luaL_openlibs(L);
  run(L, "G = {} function G.f(n) return n * 2 end");
  /* A call hook dispatches the call through lj_dispatch_call. One call must
  ** still deliver exactly one interrupt.
  */
  CHECK(luaJIT_int3_start(L, bp_cb, &rec) == LUAJIT_INT3_OK);
  CHECK(arm_slot(L, "G", "f", "MULVN", &bcpos) == 0);
  call_events = 0;
  lua_sethook(L, count_call_hook, LUA_MASKCALL, 0);
  CHECK(run_number(L, "return G.f(21)", &v) && v == 42);
  lua_sethook(L, NULL, 0, 0);
  printf("[11] call hook: hits=%d events=%d value=%g\n", rec.calls,
	 call_events, v);
  CHECK(rec.calls == 1);		/* One call, one delivery. */
  CHECK(call_events >= 1);		/* The hook itself ran. */
  CHECK(luaJIT_int3_cancel(L) == LUAJIT_INT3_OK);

  /* An armed prototype must not trace at all: the hot counters keep tripping,
  ** but the recorder is never entered and trace_start() never patches the
  ** bytecode, so the trapped body instruction fires exactly once per
  ** iteration however the JIT parameters are set.
  */
#if !defined(SMOKE_NO_JIT)
  attach_trace_counters(L);
#endif
  memset(&rec, 0, sizeof(rec));
  run(L, "function G.loop(n) local s = 0 for i = 1, n do s = s + i end "
	 "return s end");
  /* The loop-back slot is a site now; drop it again so the ADDVV count below
  ** stays exact ([20] checks the loop slot itself).
  */
  CHECK(arm_code(L, "G", "loop", "FORL", 0) == LUAJIT_INT3_OK);
  CHECK(luaJIT_int3_cancel(L) == LUAJIT_INT3_OK);
  CHECK(arm_slot(L, "G", "loop", "ADDVV", &bcpos) == 0);
#if !defined(SMOKE_NO_JIT)
  run(L, "TR.start = 0");
#endif
  CHECK(run_number(L, "return G.loop(5000)", &v) && v == 12502500);
  printf("[11] hot loop: hits=%d value=%g\n", rec.calls, v);
  /* One delivery per iteration, no duplicate delivery. */
  CHECK(rec.calls == 5000);
#if !defined(SMOKE_NO_JIT)
  printf("[11] armed proto trace starts=%d\n", counter(L, "start"));
  CHECK(counter(L, "start") == 0);
#endif
  CHECK(luaJIT_int3_cancel(L) == LUAJIT_INT3_OK);
  /* Nothing is armed any more, so the loop no longer traps. */
  rec.calls = 0;
  CHECK(run_number(L, "return G.loop(100)", &v) && v == 5050);
  CHECK(rec.calls == 0);

  /* RETM with a return hook: the trap and the hook both observe the
  ** multi-return instruction, and the continuation re-reads it from the
  ** breakpoint instead of the bytecode.
  */
  run(L, "function G.varret(...) return ... end\n"
	 "function G.driver(a, b) local x, y = G.varret(a, b) return x + y end");
  CHECK(arm_slot(L, "G", "varret", "RETM", &bcpos) == 0);
  memset(&rec, 0, sizeof(rec));
  ret_events = 0;
  lua_sethook(L, count_ret_hook, LUA_MASKRET, 0);
  CHECK(run_number(L, "return G.driver(1, 2)", &v) && v == 3);
  lua_sethook(L, NULL, 0, 0);
  printf("[11] RETM+ret hook: hits=%d events=%d value=%g\n", rec.calls,
	 ret_events, v);
  CHECK(rec.calls == 1);
  CHECK(ret_events >= 1);
  rec.calls = 0;
  CHECK(run_number(L, "return G.driver(4, 5)", &v) && v == 9);
  CHECK(rec.calls == 1);		/* Permanent, one per call. */
  CHECK(luaJIT_int3_cancel(L) == LUAJIT_INT3_OK);

  /* ITERC with a line hook: the generic-for call instruction is dispatched
  ** through the hook trampoline on every iteration.
  */
  run(L, "local function mynext(t, k) return next(t, k) end\n"
	 "MYNEXT = mynext\n"
	 "function G.iter(n)\n"
	 "  local t = {}\n"
	 "  for i = 1, n do t[i] = i end\n"
	 "  local s = 0\n"
	 "  for k, v in MYNEXT, t do s = s + v end\n"
	 "  return s\n"
	 "end");
  CHECK(arm_code(L, "G", "iter", "ITERC", 0) == LUAJIT_INT3_OK);
  memset(&rec, 0, sizeof(rec));
  hooks = 0;
  lua_sethook(L, line_count_hook, LUA_MASKLINE|LUA_MASKCOUNT, 1);
  CHECK(run_number(L, "return G.iter(5)", &v) && v == 15);
  lua_sethook(L, NULL, 0, 0);
  printf("[11] ITERC+line hook: hits=%d hooks=%d value=%g\n", rec.calls,
	 hooks, v);
  /* One delivery per call of the iterator: five values plus the final nil. */
  CHECK(rec.calls == 6);
  CHECK(hooks > 0);
  CHECK(luaJIT_int3_stop(L) == LUAJIT_INT3_OK);
  lua_close(L);
}
#endif  /* !SMOKE_NO_PROFILE */

/* -- [12] Hot-count parameters: the trap reset must never underflow -------- */

#if !defined(SMOKE_NO_PROFILE) && !defined(SMOKE_NO_JIT)
static void test_hotcount_params(void)
{
  static const char *params[] = { "hotloop=0", "hotloop=1", "hotloop=32768",
				  "hotloop=56" };
  lua_State *L = luaL_newstate();
  BpRec rec;
  size_t i;
  int bcpos = -1;
  double v;
  memset(&rec, 0, sizeof(rec));
  luaL_openlibs(L);
  attach_trace_counters(L);
  run(L, "G = {} function G.loop(n) local s = 0 for i = 1, n do s = s + i end "
	 "return s end");
  CHECK(luaJIT_int3_start(L, bp_cb, &rec) == LUAJIT_INT3_OK);
  for (i = 0; i < sizeof(params)/sizeof(params[0]); i++) {
    char code[64];
    int hits;
    snprintf(code, sizeof(code), "jit.opt.start('%s')", params[i]);
    run(L, code);
    memset(&rec, 0, sizeof(rec));
    CHECK(arm_code(L, "G", "loop", "FORL", 0) == LUAJIT_INT3_OK);
    CHECK(luaJIT_int3_cancel(L) == LUAJIT_INT3_OK);
    CHECK(arm_slot(L, "G", "loop", "ADDVV", &bcpos) == 0);
    snprintf(code, sizeof(code), "TR.start = 0");
    run(L, code);
    snprintf(code, sizeof(code), "return G.loop(200)");
    CHECK(run_number(L, code, &v) && v == 20100);
    hits = rec.calls;
    printf("[12] %-15s hits=%d starts=%d\n", params[i], hits,
	   counter(L, "start"));
    /* One interrupt per iteration, no endless re-dispatch, no trace start. */
    CHECK(hits == 200);
    CHECK(counter(L, "start") == 0);
    CHECK(luaJIT_int3_cancel(L) == LUAJIT_INT3_OK);
  }
  CHECK(luaJIT_int3_stop(L) == LUAJIT_INT3_OK);
  lua_close(L);
}
#endif  /* !SMOKE_NO_PROFILE && !SMOKE_NO_JIT */

/* -- [13] PROTO_NOJIT ownership across several requests ------------------- */

#if !defined(SMOKE_NO_PROFILE) && !defined(SMOKE_NO_JIT)
static void test_nojit_ownership(void)
{
  lua_State *L = luaL_newstate();
  Int3Rec rec;
  int bc1 = -1, bc2 = -1;
  double v;
  memset(&rec, 0, sizeof(rec));
  luaL_openlibs(L);
  attach_trace_counters(L);
  run(L,
    "G = {}\n"
    "function G.f(n) local s = 0 for i = 1, n do s = s + i * 2 end "
    "return s end\n"
    "function G.h(x)\n"
    "  if x > 1000 then return x + 5 end\n"
    "  local s = 0 for i = 1, x do s = s + i * 3 end return s\n"
    "end\n");
  CHECK(luaJIT_int3_start(L, int3_cb, &rec) == LUAJIT_INT3_OK);

  /* Two requests for one prototype: the first owns PROTO_NOJIT. Cancelling
  ** both, owner first, must resume compilation (a trace has to start).
  */
  CHECK(arm_slot(L, "G", "f", "MULVN", &bc1) == 0);
  CHECK(arm_slot(L, "G", "f", "ADDVV", &bc2) == 0);
  CHECK(set_jit_on(L, "G", "f") == 0);		/* Refused while armed. */
  CHECK(luaJIT_int3_cancel(L) == LUAJIT_INT3_OK);
  run(L, "TR.start = 0");
  CHECK(run_number(L, "return G.f(2000)", &v) && v == 4002000);
  printf("[13] owner-first cancel: starts=%d\n", counter(L, "start"));
  CHECK(counter(L, "start") > 0);		/* Compiles again. */

  /* The owner is consumed first (only its branch runs) and must hand the
  ** ownership to the surviving request, which then releases it.
  */
  CHECK(arm_slot(L, "G", "h", "MULVN", &bc1) == 0);	/* Owner. */
  CHECK(arm_slot(L, "G", "h", "ADDVN", &bc2) == 0);	/* Untaken branch. */
  rec.calls = 0;
  CHECK(run_number(L, "return G.h(10)", &v) && v == 165);
  printf("[13] owner consumed: calls=%d\n", rec.calls);
  CHECK(rec.calls == 10);	/* Only the taken branch, once per iteration. */
  CHECK(set_jit_on(L, "G", "h") == 0);		/* Survivor still owns. */
  CHECK(luaJIT_int3_cancel(L) == LUAJIT_INT3_OK);
  run(L, "TR.start = 0");
  CHECK(run_number(L, "return G.h(1000)", &v) && v == 1501500);
  printf("[13] owner consumed, released: starts=%d\n", counter(L, "start"));
  CHECK(counter(L, "start") > 0);		/* Compiles again. */

  /* A pre-existing jit.off() is not ours: cancelling must not re-enable it. */
  lua_getglobal(L, "G");
  lua_getfield(L, -1, "f");
  CHECK(luaJIT_setmode(L, -1, LUAJIT_MODE_FUNC|LUAJIT_MODE_OFF) == 1);
  lua_pop(L, 2);
  CHECK(arm_slot(L, "G", "f", "ADDVV", &bc2) == 0);
  CHECK(luaJIT_int3_cancel(L) == LUAJIT_INT3_OK);
  run(L, "TR.start = 0");
  CHECK(run_number(L, "return G.f(2000)", &v) && v == 4002000);
  printf("[13] jit.off preserved: starts=%d\n", counter(L, "start"));
  CHECK(counter(L, "start") == 0);		/* Still disabled. */

  /* A jit.off() applied *after* the request was mounted is the caller's
  ** property too: cancelling must not re-enable the prototype.
  */
  lua_getglobal(L, "G");
  lua_getfield(L, -1, "h");
  CHECK(luaJIT_setmode(L, -1, LUAJIT_MODE_FUNC|LUAJIT_MODE_ON) == 1);
  lua_pop(L, 2);
  CHECK(arm_slot(L, "G", "h", "ADDVV", &bc2) == 0);
  rec.calls = 0;
  CHECK(run_number(L, "return G.h(4)", &v) && v == 30);   /* Mount it. */
  CHECK(rec.calls == 4);
  lua_getglobal(L, "G");
  lua_getfield(L, -1, "h");
  CHECK(luaJIT_setmode(L, -1, LUAJIT_MODE_FUNC|LUAJIT_MODE_OFF) == 1);
  lua_pop(L, 2);
  CHECK(luaJIT_int3_cancel(L) == LUAJIT_INT3_OK);
  run(L, "TR.start = 0");
  CHECK(run_number(L, "return G.h(1000)", &v) && v == 1501500);
  printf("[13] jit.off during mount: starts=%d\n", counter(L, "start"));
  CHECK(counter(L, "start") == 0);		/* Still disabled. */
  CHECK(luaJIT_int3_stop(L) == LUAJIT_INT3_OK);
  lua_close(L);
}
#endif  /* !SMOKE_NO_PROFILE && !SMOKE_NO_JIT */

/* -- [14] Breakpoints on the instructions with a C slow path ------------ */
#if !defined(SMOKE_NO_PROFILE) && !defined(SMOKE_NO_JIT)

/* Run one slow-path case twice: unarmed (baseline) and armed. The trapped
** instruction must yield the same result and take the same fast or slow path,
** and it must deliver exactly `hits' callbacks.
*/
static void slow_case(lua_State *L, Int3Rec *rec, const char *fname,
		      const char *opname, int preset_bcpos, const char *call,
		      int hits, const char *tag)
{
  double base = -1, v = -1;
  int bcpos = preset_bcpos;
  CHECKF(run_number(L, call, &base), "%s baseline", tag);
  if (bcpos < 0) {
    char path[64];
    snprintf(path, sizeof(path), "G.%s", fname);
    bcpos = find_bcpos_n(L, path, opname, 0);
    CHECKF(bcpos > 0, "%s bcpos", tag);
  }
  CHECKF(arm_pos(L, "G", fname, bcpos) == LUAJIT_INT3_OK, "%s arm", tag);
  rec->calls = 0;
  CHECKF(run_number(L, call, &v), "%s trapped", tag);
  printf("[14] %-5s %-6s bcpos=%2d hits=%d base=%g value=%g\n", tag, opname,
	 bcpos, rec->calls, base, v);
  CHECKF(rec->calls == hits, "%s hits %d want %d", tag, rec->calls, hits);
  CHECKF(v == base, "%s value %g want %g", tag, v, base);
  CHECK(luaJIT_int3_cancel(L) == LUAJIT_INT3_OK);
}

static void test_slowpaths(void)
{
  lua_State *L = luaL_newstate();
  Int3Rec rec;
  double v;
  memset(&rec, 0, sizeof(rec));
  luaL_openlibs(L);
  run(L,
    "G = {}\n"
    "local mt = {}\n"
    "mt.__add = function(a, b) return a.v + b end\n"
    "mt.__lt = function(a, b) return a.v < b.v end\n"
    "mt.__index = function(t, k) return k .. '!' end\n"
    "mt.__newindex = function(t, k, val) rawset(t, k, val) end\n"
    "G.mmadd = setmetatable({v = 7}, mt)\n"
    "G.mmlt = setmetatable({v = 1}, mt)\n"
    "G.mmlt2 = setmetatable({v = 2}, mt)\n"
    "G.hasht = setmetatable({}, mt)\n"
    "G.hasht2 = setmetatable({}, mt)\n"
    "function G.mm(a, b) return a + b end\n"
    "function G.cmp(a, b) if a < b then return 1 end return 0 end\n"
    "function G.ts(t) return t.missing end\n"
    "function G.tss(t, x) t.missing = x return 1 end\n"
    /* FFI: a bit operator and a cdata equality with the C slow path. */
    "local ffi = require('ffi')\n"
    "G.u32 = ffi.new('uint32_t', 6)\n"
    "G.u32b = ffi.new('uint32_t', 6)\n"
    "G.u32c = ffi.new('uint32_t', 9)\n"
    "function G.bit(a) return tonumber(a | 1) end\n"
    "function G.eq(a, b) if a == b then return 1 end return 0 end\n"
    "function G.ne(a, b) if a ~= b then return 1 end return 0 end\n"
    /* Globals: a metatable on _G routes a constant global access through the
    ** GGET/GSET slow path.
    */
    "setmetatable(_G, { __index = function(t, k) return 99 end,\n"
    "                   __newindex = function(t, k, val) rawset(t, k, val) end })\n"
    "function G.gg() return undefined_get end\n"
    "function G.gs(x) undefined_set = x return 1 end\n");
  CHECK(luaJIT_int3_start(L, int3_cb, &rec) == LUAJIT_INT3_OK);

  /* Arithmetic metamethod: lj_meta_arith derives the metamethod from the
  ** opcode, which the slow path reads from the trapped slot.
  */
  slow_case(L, &rec, "mm", "ADDVV", -1, "return G.mm(G.mmadd, 5)", 1, "mm");
  /* Ordered comparison: lj_meta_comp, including the polarity bits. The
  ** parser emits the inverted opcode for a conditional jump.
  */
  slow_case(L, &rec, "cmp", "ISGE", -1, "return G.cmp(G.mmlt, G.mmlt2)", 1,
	    "cmp");
  slow_case(L, &rec, "cmp", "ISGE", -1, "return G.cmp(G.mmlt2, G.mmlt)", 1,
	    "cmp2");
  /* Bit operator on cdata: lj_meta_bitop. */
  slow_case(L, &rec, "bit", "BOR", -1, "return G.bit(G.u32)", 1, "bitop");
  /* Cdata equality: lj_meta_equal_cd gets the whole instruction, so both
  ** polarity variants of the iseqne group are covered.
  */
  slow_case(L, &rec, "eq", "ISNEV", -1, "return G.eq(G.u32, G.u32b)", 1,
	    "cdeq");
  slow_case(L, &rec, "eq", "ISNEV", -1, "return G.eq(G.u32, G.u32c)", 1,
	    "cdeq2");
  slow_case(L, &rec, "ne", "ISEQV", -1, "return G.ne(G.u32, G.u32c)", 1,
	    "cdne");
  /* Table hash miss with __index (the global-style table entry). */
  slow_case(L, &rec, "ts", "TGETS", -1, "return #G.ts(G.hasht)", 1, "tgets");
  /* Table hash miss with __newindex, including the side effect. */
  slow_case(L, &rec, "tss", "TSETS", -1,
	    "local r = G.tss(G.hasht2, 5) return r + G.hasht2.missing", 1,
	    "tsets");
  /* Constant global access with a metatable on _G: GGET/GSET. */
  slow_case(L, &rec, "gg", "GGET", -1, "return G.gg()", 1, "gget");
  slow_case(L, &rec, "gs", "GSET", -1,
	    "local r = G.gs(7) return r + undefined_set", 1, "gset");
  CHECK(luaJIT_int3_cancel(L) == LUAJIT_INT3_OK);

  /* Non-table TGETS/TSETS: the value is a string, so the table check fails
  ** and the slot pointer has to be materialised from BASE. The slots are
  ** resolved first: the finder uses string methods, which the replacement
  ** metatable would shadow.
  */
  run(L, "function G.nts(s) return s.len end\n"
	 "function G.ntss(s, x) s.foo = x return 1 end");
  {
    int bc_t = find_bcpos_n(L, "G.nts", "TGETS", 0);
    int bc_s = find_bcpos_n(L, "G.ntss", "TSETS", 0);
    printf("[14] non-table slots: tgets=%d tsets=%d\n", bc_t, bc_s);
    CHECK(bc_t > 0 && bc_s > 0);
    run(L, "SMT = debug.getmetatable('')\n"
	   "debug.setmetatable('', { __index = function(s, k) return #s end,\n"
	   "                         __newindex = function(s, k, val) SSTORE = val end })");
    slow_case(L, &rec, "nts", "TGETS", bc_t, "return G.nts('abc')", 1,
	      "ntgets");
    slow_case(L, &rec, "ntss", "TSETS", bc_s,
	      "local r = G.ntss('abc', 7) return r + SSTORE", 1, "ntsets");
    run(L, "debug.setmetatable('', SMT)");
  }
  CHECK(run_number(L, "return ('abc'):upper() == 'ABC' and 1 or 0", &v) &&
	v == 1);

  /* A loop that takes the slow path on every iteration: the result and the
  ** side effects must match the unarmed run exactly.
  */
  run(L,
    "function G.mmloop(n)\n"
    "  local s = 0\n"
    "  for i = 1, n do s = s + (G.mmadd + i) end\n"
    "  return s\n"
    "end");
  CHECK(run_number(L, "return G.mmloop(100)", &v) && v == 5750);
  {
    int bcpos = -1;
    CHECK(arm_slot(L, "G", "mmloop", "ADDVV", &bcpos) == 0);
    rec.calls = 0;
    CHECK(run_number(L, "return G.mmloop(100)", &v) && v == 5750);
    printf("[14] loop   ADDVV  bcpos=%2d hits=%d value=%g\n", bcpos,
	   rec.calls, v);
    CHECK(rec.calls == 100);	/* One delivery per iteration. */
  }
  CHECK(luaJIT_int3_stop(L) == LUAJIT_INT3_OK);
  lua_close(L);
}
#endif  /* !SMOKE_NO_PROFILE && !SMOKE_NO_JIT */

/* -- [15] Win64 callee-saved registers across an interrupt -------------- */

#if !defined(SMOKE_NO_PROFILE)

#if defined(_WIN32) && defined(__x86_64__) && defined(__GNUC__)
typedef int (*PcallRef)(lua_State *L, int nargs, int nresults, int errfunc);

/* Call fn(L, nargs, nresults, 0) and return a bitmask of the Win64
** non-volatile registers the call failed to preserve: bit 0 = rbx, 1 = rsi,
** 2 = rdi, 3 = r12, 4 = r13, 5 = r14, 6 = r15.
**
** The interpreter runs on this C stack, so a BC_INT3 handler that writes a
** non-volatile register without saving it leaks the write into the caller's
** frame. Registers that lua_pcall itself saves and restores are covered by
** the abi_sample_*() checkpoints below, which sample them from inside the
** call. lua_pcall's own return value is deliberately not preserved: the
** mask overwrites eax after the call. The arguments arrive in rcx = fn,
** rdx = L, r8 = nargs, r9 = nresults.
*/
__attribute__((naked)) static int pcall_reg_probe(PcallRef fn, lua_State *L,
						  int nargs, int nresults)
{
  __asm__ __volatile__(
    ".intel_syntax noprefix\n\t"
    "push rbp\n\t"
    "push rbx\n\t"
    "push rsi\n\t"
    "push rdi\n\t"
    "push r12\n\t"
    "push r13\n\t"
    "push r14\n\t"
    "push r15\n\t"
    "sub rsp, 40\n\t"		/* 32-byte shadow space, 16-byte aligned. */
    "mov rbx, 0x1122334455667788\n\t"
    "mov rsi, 0x2122334455667788\n\t"
    "mov rdi, 0x3122334455667788\n\t"
    "mov r12, 0x4122334455667788\n\t"
    "mov r13, 0x5122334455667788\n\t"
    "mov r14, 0x6122334455667788\n\t"
    "mov r15, 0x7122334455667788\n\t"
    "mov r11, rcx\n\t"		/* fn */
    "mov rcx, rdx\n\t"		/* L */
    "mov rdx, r8\n\t"		/* nargs */
    "mov r8, r9\n\t"		/* nresults */
    "xor r9d, r9d\n\t"		/* errfunc = 0 */
    "call r11\n\t"
    "xor eax, eax\n\t"
    "mov r10, 0x1122334455667788\n\t"
    "cmp rbx, r10\n\t"
    "setne r10b\n\t"
    "movzx r10d, r10b\n\t"
    "or eax, r10d\n\t"
    "mov r10, 0x2122334455667788\n\t"
    "cmp rsi, r10\n\t"
    "setne r10b\n\t"
    "movzx r10d, r10b\n\t"
    "shl r10d, 1\n\t"
    "or eax, r10d\n\t"
    "mov r10, 0x3122334455667788\n\t"
    "cmp rdi, r10\n\t"
    "setne r10b\n\t"
    "movzx r10d, r10b\n\t"
    "shl r10d, 2\n\t"
    "or eax, r10d\n\t"
    "mov r10, 0x4122334455667788\n\t"
    "cmp r12, r10\n\t"
    "setne r10b\n\t"
    "movzx r10d, r10b\n\t"
    "shl r10d, 3\n\t"
    "or eax, r10d\n\t"
    "mov r10, 0x5122334455667788\n\t"
    "cmp r13, r10\n\t"
    "setne r10b\n\t"
    "movzx r10d, r10b\n\t"
    "shl r10d, 4\n\t"
    "or eax, r10d\n\t"
    "mov r10, 0x6122334455667788\n\t"
    "cmp r14, r10\n\t"
    "setne r10b\n\t"
    "movzx r10d, r10b\n\t"
    "shl r10d, 5\n\t"
    "or eax, r10d\n\t"
    "mov r10, 0x7122334455667788\n\t"
    "cmp r15, r10\n\t"
    "setne r10b\n\t"
    "movzx r10d, r10b\n\t"
    "shl r10d, 6\n\t"
    "or eax, r10d\n\t"
    "add rsp, 40\n\t"
    "pop r15\n\t"
    "pop r14\n\t"
    "pop r13\n\t"
    "pop r12\n\t"
    "pop rdi\n\t"
    "pop rsi\n\t"
    "pop rbx\n\t"
    "pop rbp\n\t"
    "ret\n\t"
    ".att_syntax prefix\n\t");
}

static unsigned long long abi_regs_before[4], abi_regs_after[4];
static int abi_samples;

/* Snapshot r12-r15 at a C call made from Lua. lua_pcall() saves and restores
** r12-r14 itself, so a handler write to them is undone before the probe
** above can see it. These checkpoints run while the interpreter frames are
** still on the stack; the test compares the samples taken before and after
** the trapped instruction. Only r12-r15 are sampled: rbx/rsi/rdi hold
** KBASE/PC/DISPATCH in the Win64 interpreter, so they legitimately change
** between two instruction boundaries. The functions are naked, so their own
** prologues cannot hide the incoming values.
*/
#define ABI_SAMPLE(regs) \
  __asm__ __volatile__( \
    ".intel_syntax noprefix\n\t" \
    "mov qword ptr " regs "[rip], r12\n\t" \
    "mov qword ptr " regs "+8[rip], r13\n\t" \
    "mov qword ptr " regs "+16[rip], r14\n\t" \
    "mov qword ptr " regs "+24[rip], r15\n\t" \
    "inc dword ptr abi_samples[rip]\n\t" \
    "xor eax, eax\n\t" \
    "ret\n\t" \
    ".att_syntax prefix\n\t")

__attribute__((naked)) static int abi_sample_before(lua_State *L)
{
  ABI_SAMPLE("abi_regs_before");
}

__attribute__((naked)) static int abi_sample_after(lua_State *L)
{
  ABI_SAMPLE("abi_regs_after");
}
#undef ABI_SAMPLE
#endif

static void test_abi_probe(void)
{
#if defined(_WIN32) && defined(__x86_64__) && defined(__GNUC__)
  lua_State *L = luaL_newstate();
  BpRec rec;
  int bcpos = -1, i, mask = 0;
  memset(&rec, 0, sizeof(rec));
  luaL_openlibs(L);
  /* Only a GC64 build runs the x64 interpreter; a non-GC64 x64 build uses
  ** the x86 backend, so there is no x64 handler to probe. GC64 sets the
  ** frame-info flag in the bytecode dump header.
  */
  if (!run_bool(L,
	"local b = string.dump(function() end):byte(5)\n"
	"return b ~= nil and math.floor(b / 8) % 2 == 1")) {
    printf("[15] abi probe: skipped\n");
    lua_close(L);
    return;
  }
  lua_pushcfunction(L, abi_sample_before);
  lua_setglobal(L, "sample_before");
  lua_pushcfunction(L, abi_sample_after);
  lua_setglobal(L, "sample_after");
  run(L,
    "G = {}\n"
    "function G.f(i)\n"
    "  sample_before()\n"
    "  local r = i * 2\n"
    "  sample_after()\n"
    "  return r\n"
    "end\n");
  CHECK(luaJIT_int3_start(L, bp_cb, &rec) == LUAJIT_INT3_OK);
  CHECK(arm_slot(L, "G", "f", "MULVN", &bcpos) == 0);
  for (i = 1; i <= 8; i++) {
    int b;
    lua_getglobal(L, "G");
    lua_getfield(L, -1, "f");
    lua_remove(L, -2);
    lua_pushinteger(L, i);
    for (b = 0; b < 4; b++) abi_regs_before[b] = abi_regs_after[b] = 0;
    /* The mask replaces lua_pcall's status, so assert on the result. */
    mask |= pcall_reg_probe(lua_pcall, L, 1, 1);
    for (b = 0; b < 4; b++)		/* Sampled inside the call. */
      if (abi_regs_before[b] != abi_regs_after[b]) mask |= 1 << (b + 3);
    CHECK(lua_isnumber(L, -1) && lua_tonumber(L, -1) == i * 2);
    lua_pop(L, 1);
  }
  printf("[15] abi probe: mask=0x%x hits=%d\n", mask, rec.calls);
  if (mask) {
    static const char *const regs[7] =
      { "rbx", "rsi", "rdi", "r12", "r13", "r14", "r15" };
    int b;
    printf("[15] abi probe: clobbered");
    for (b = 0; b < 7; b++)
      if (mask & (1 << b)) printf(" %s", regs[b]);
    printf("\n");
  }
  CHECK(rec.calls == 8);		/* One delivery per call. */
  CHECK(abi_samples == 16);		/* Both samples ran on every call. */
  CHECK(mask == 0);
  CHECK(luaJIT_int3_cancel(L) == LUAJIT_INT3_OK);
  CHECK(luaJIT_int3_stop(L) == LUAJIT_INT3_OK);
  lua_close(L);
#else
  printf("[15] abi probe: skipped\n");
#endif
}

#endif  /* !SMOKE_NO_PROFILE */

/* -- [18] errno preservation --------------------------------------------- */

/* A delivery runs inside the VM dispatch, so the whole hit path must behave
** like any other opcode: no hook, no GC step and no change to errno.
*/
static int errno_calls;

static void errno_cb(void *data, lua_State *L, const void *func,
		     unsigned int bcpos)
{
  (void)data; (void)L; (void)func; (void)bcpos;
  errno_calls++;
}

static void test_errno(void)
{
  lua_State *L = luaL_newstate();
  int bcpos;
  double v;
  luaL_openlibs(L);
  run(L, "E = {}\nfunction E.a(n) local s = 0 for i = 1, n do s = s + i end return s end\n");
  bcpos = find_bcpos_n(L, "E.a", "ADDVV", 0);
  CHECK(bcpos >= 0);
  CHECK(luaJIT_int3_start(L, errno_cb, NULL) == LUAJIT_INT3_OK);
  lua_getglobal(L, "E");
  lua_getfield(L, -1, "a");
  CHECK(luaJIT_int3_arm_bc(L, -1, (unsigned int)bcpos) == LUAJIT_INT3_OK);
  lua_pop(L, 2);
  errno_calls = 0;
  errno = EDOM;
  CHECK(run_number(L, "return E.a(5)", &v) && v == 15);
  printf("[18] errno: hits=%d errno=%d value=%g\n", errno_calls, errno, v);
  CHECK(errno_calls == 5);
  CHECK(errno == EDOM);		/* The delivery must not clobber errno. */
  CHECK(luaJIT_int3_stop(L) == LUAJIT_INT3_OK);
  lua_close(L);
}

/* -- [19] close during finalizers ---------------------------------------- */

/* lua_close() releases the session before the finalizer run, so no callback
** can reach a finalizer of the VM that is being torn down, and the force
** restore has already happened when the finalizer observes the session.
**
** Observed through the public API only (this harness never links against the
** internal symbols): the finalizer asks arm() about a function it kept in the
** registry, which is LUAJIT_INT3_DISABLED once this VM has no session, and a
** fresh VM afterwards can start one, which proves the process-wide slot was
** released.
*/
static int close_calls;
static int close_arm_res = -1000;	/* arm() result seen by the finalizer. */
static unsigned int close_bcpos;	/* Slot that was armed. */
static int close_ref = LUA_NOREF;	/* Registry ref of that function. */

static void close_cb(void *data, lua_State *L, const void *func,
		     unsigned int bcpos)
{
  (void)data; (void)L; (void)func; (void)bcpos;
  close_calls++;
}

static int close_finalizer(lua_State *L)
{
  lua_rawgeti(L, LUA_REGISTRYINDEX, close_ref);
  close_arm_res = luaJIT_int3_arm_bc(L, -1, close_bcpos);
  lua_pop(L, 1);
  return 0;
}

static void test_close_finalizer(void)
{
  lua_State *L = luaL_newstate();
  int bcpos;
  double v;
  luaL_openlibs(L);
  run(L,
    "C = {}\n"
    "function C.a(n) local s = 0 for i = 1, n do s = s + i end return s end\n"
    /* LuaJIT runs __gc for userdata only (Lua 5.1 semantics). */
    "local u = newproxy(true)\n"
    "getmetatable(u).__gc = function() FIN() end\n"
    "G = u\n");
  lua_pushcfunction(L, close_finalizer);
  lua_setglobal(L, "FIN");
  bcpos = find_bcpos_n(L, "C.a", "ADDVV", 0);
  CHECK(bcpos >= 0);
  CHECK(luaJIT_int3_start(L, close_cb, NULL) == LUAJIT_INT3_OK);
  lua_getglobal(L, "C");
  lua_getfield(L, -1, "a");
  CHECK(luaJIT_int3_arm_bc(L, -1, (unsigned int)bcpos) == LUAJIT_INT3_OK);
  close_bcpos = (unsigned int)bcpos;
  close_ref = luaL_ref(L, LUA_REGISTRYINDEX);	/* Pops "a". */
  lua_pop(L, 1);				/* Pops "C". */
  close_calls = 0;
  CHECK(run_number(L, "return C.a(4)", &v) && v == 10);
  CHECK(close_calls == 4);
  close_calls = 0;
  close_arm_res = -1000;
  lua_close(L);			/* Runs the finalizer of G. */
  printf("[19] close: finalizer-arm=%d close-hits=%d\n", close_arm_res,
	 close_calls);
  CHECK(close_arm_res == LUAJIT_INT3_DISABLED);	/* Session was released. */
  CHECK(close_calls == 0);
  {  /* The process-wide slot is free again. */
    lua_State *L2 = luaL_newstate();
    luaL_openlibs(L2);
    CHECK(luaJIT_int3_start(L2, close_cb, NULL) == LUAJIT_INT3_OK);
    CHECK(luaJIT_int3_stop(L2) == LUAJIT_INT3_OK);
    lua_close(L2);
  }
}

/* -- [20] Loop and return slots are breakpoint sites --------------------- */

#if !defined(SMOKE_NO_PROFILE)
static void test_loop_slots(void)
{
  lua_State *L = luaL_newstate();
  Int3Rec rec;
  int bcpos = -1;
  double v;
  memset(&rec, 0, sizeof(rec));
  luaL_openlibs(L);
  run(L,
    "G = {}\n"
    "function G.loop(n) local s = 0 for i = 1, n do s = s + i end "
    "return s end\n"
    "function G.wloop(n) local i = 0 while i < n do i = i + 1 end "
    "return i end\n"
    "function G.ret1(i) return i * 2 end\n"
    "local function mynext(t, k) return next(t, k) end\n"
    "MYNEXT = mynext\n"
    "function G.iter(n)\n"
    "  local t = {}\n"
    "  for i = 1, n do t[i] = i end\n"
    "  local s = 0\n"
    "  for k, x in MYNEXT, t do s = s + x end\n"
    "  return s\n"
    "end\n");
  CHECK(luaJIT_int3_start(L, int3_cb, &rec) == LUAJIT_INT3_OK);

  /* The numeric-for header runs once per loop entry; the loop-back slot runs
  ** after every body execution, including the one that terminates the loop.
  */
  CHECK(arm_slot(L, "G", "loop", "FORI", &bcpos) == 0);
  rec.calls = 0;
  CHECK(run_number(L, "return G.loop(7)", &v) && v == 28);
  printf("[20] FORI: hits=%d value=%g\n", rec.calls, v);
  CHECK(rec.calls == 1);
  CHECK(luaJIT_int3_cancel(L) == LUAJIT_INT3_OK);
  CHECK(arm_slot(L, "G", "loop", "FORL", &bcpos) == 0);
  rec.calls = 0;
  CHECK(run_number(L, "return G.loop(7)", &v) && v == 28);
  printf("[20] FORL: hits=%d value=%g\n", rec.calls, v);
  CHECK(rec.calls == 7);
  rec.calls = 0;
  CHECK(run_number(L, "return G.loop(3)", &v) && v == 6);
  CHECK(rec.calls == 3);		/* Permanent: keeps counting. */
  CHECK(luaJIT_int3_cancel(L) == LUAJIT_INT3_OK);

  /* A while loop: the LOOP slot runs once per iteration. */
  CHECK(arm_slot(L, "G", "wloop", "LOOP", &bcpos) == 0);
  rec.calls = 0;
  CHECK(run_number(L, "return G.wloop(5)", &v) && v == 5);
  printf("[20] LOOP: hits=%d value=%g\n", rec.calls, v);
  CHECK(rec.calls == 5);
  CHECK((int)rec.bcpos == bcpos);
  CHECK(luaJIT_int3_cancel(L) == LUAJIT_INT3_OK);

  /* A generic for: ITERL and ITERC both run once per iterator call -- four
  ** values plus the final nil call -- while the body runs four times.
  */
  CHECK(arm_slot(L, "G", "iter", "ITERL", &bcpos) == 0);
  rec.calls = 0;
  CHECK(run_number(L, "return G.iter(4)", &v) && v == 10);
  printf("[20] ITERL: hits=%d value=%g\n", rec.calls, v);
  CHECK(rec.calls == 5);
  CHECK(luaJIT_int3_cancel(L) == LUAJIT_INT3_OK);
  CHECK(arm_slot(L, "G", "iter", "ITERC", &bcpos) == 0);
  rec.calls = 0;
  CHECK(run_number(L, "return G.iter(4)", &v) && v == 10);
  printf("[20] ITERC: hits=%d value=%g\n", rec.calls, v);
  CHECK(rec.calls == 5);		/* Four values plus the final nil call. */
  CHECK(luaJIT_int3_cancel(L) == LUAJIT_INT3_OK);

  /* A trapped return slot with a return hook: the hook still observes the
  ** shadowed instruction exactly once per return.
  */
  CHECK(arm_slot(L, "G", "ret1", "RET1", &bcpos) == 0);
  rec.calls = 0;
  ret_events = 0;
  lua_sethook(L, count_ret_hook, LUA_MASKRET, 0);
  CHECK(run_number(L, "return G.ret1(21)", &v) && v == 42);
  lua_sethook(L, NULL, 0, 0);
  printf("[20] RET1+ret hook: hits=%d events=%d value=%g\n", rec.calls,
	 ret_events, v);
  CHECK(rec.calls == 1);
  CHECK(ret_events >= 1);
  rec.calls = 0;
  CHECK(run_number(L, "return G.ret1(3)", &v) && v == 6);
  CHECK(rec.calls == 1);		/* One per return, permanently. */
  CHECK(luaJIT_int3_cancel(L) == LUAJIT_INT3_OK);

#if !defined(SMOKE_NO_JIT)
  /* A trapped loop slot in a hot loop: mounting flushed the trace of the
  ** prototype and excluded it from compilation, so trace_start()'s lazy
  ** I-patch and trace_stop() never touch the slot, and the delivery count is
  ** exactly one per iteration however the JIT is configured.
  */
  attach_trace_counters(L);
  run(L, "function G.hot(n) local s = 0 for i = 1, n do s = s + i end "
	 "return s end");
  run(L, "jit.opt.start('hotloop=1')");
  CHECK(run_number(L, "return G.hot(200)", &v) && v == 20100);
  run(L, "jit.opt.start('hotloop=56')");
  {
    int forl = find_bcpos_n(L, "G.hot", "FORL", 0);
    int patched = forl >= 0 ? (int)((unsigned)funcbc(L, "G.hot", forl) & 0xffu)
			    : -1;
    printf("[20] hot FORL: traced opcode=%d\n", patched);
    CHECK(patched == opcode_of(L, "JFORL"));
    CHECK(arm_pos(L, "G", "hot", forl) == LUAJIT_INT3_OK);
    /* The mounted slot holds the plain loop-back instruction, not the traced
    ** form: mounting flushed the trace first.
    */
    CHECK((int)((unsigned)funcbc(L, "G.hot", forl) & 0xffu) ==
	  opcode_of(L, "FORL"));
    rec.calls = 0;
    run(L, "TR.start = 0; TR.abort = 0");
    CHECK(run_number(L, "return G.hot(5000)", &v) && v == 12502500);
    printf("[20] hot FORL: hits=%d starts=%d abort=%d value=%g\n", rec.calls,
	   counter(L, "start"), counter(L, "abort"), v);
    CHECK(rec.calls == 5000);
    CHECK(counter(L, "start") == 0);
    /* After the cancel the prototype compiles again, so the loop slot is
    ** rewritten by the tracer once more.
    */
    CHECK(luaJIT_int3_cancel(L) == LUAJIT_INT3_OK);
    CHECK(run_number(L, "return G.hot(200)", &v) && v == 20100);
    CHECK(run_number(L, "return G.hot(200)", &v) && v == 20100);
    CHECK(counter(L, "start") > 0);
    CHECK(rec.calls == 5000);
  }
#endif
  CHECK(luaJIT_int3_stop(L) == LUAJIT_INT3_OK);
  lua_close(L);
}
#endif  /* !SMOKE_NO_PROFILE */

/* -- [21] The callback reports the running closure ----------------------- */

#if !defined(SMOKE_NO_PROFILE)
static void test_callback_func(void)
{
  lua_State *L = luaL_newstate();
  Int3Rec rec;
  int bcpos = -1;
  double v;
  const void *p1, *p2;
  memset(&rec, 0, sizeof(rec));
  luaL_openlibs(L);
  /* Two closures of one prototype: only the closure identity tells them
  ** apart, and only the closure carries the upvalue a debugger would read.
  */
  run(L,
    "G = {}\n"
    "local function mk(k) return function(x) return x * k end end\n"
    "G.f1 = mk(2)\n"
    "G.f2 = mk(3)\n");
  CHECK(luaJIT_int3_start(L, int3_cb, &rec) == LUAJIT_INT3_OK);
  CHECK(arm_slot(L, "G", "f1", "MULVV", &bcpos) == 0);
  /* lua_topointer() of a function is its GCfunc *: that is what the callback
  ** must report.
  */
  lua_getglobal(L, "G");
  lua_getfield(L, -1, "f1");
  p1 = lua_topointer(L, -1);
  lua_pop(L, 2);
  lua_getglobal(L, "G");
  lua_getfield(L, -1, "f2");
  p2 = lua_topointer(L, -1);
  lua_pop(L, 2);
  CHECK(p1 != NULL && p2 != NULL && p1 != p2);
  rec.calls = 0;
  CHECK(run_number(L, "return G.f1(5)", &v) && v == 10);
  printf("[21] callback func: calls=%d func=%p want=%p bcpos=%u\n", rec.calls,
	 rec.func, p1, rec.bcpos);
  CHECK(rec.calls == 1);
  CHECK(rec.func == p1);		/* The running closure, not the proto. */
  CHECK((int)rec.bcpos == bcpos);
  CHECK(run_number(L, "return G.f2(5)", &v) && v == 15);
  CHECK(rec.calls == 2);
  CHECK(rec.func == p2);		/* Same prototype, different closure. */
  CHECK(luaJIT_int3_cancel(L) == LUAJIT_INT3_OK);
  CHECK(luaJIT_int3_stop(L) == LUAJIT_INT3_OK);
  lua_close(L);
}
#endif  /* !SMOKE_NO_PROFILE */

/* -- [22] Arming by source line ------------------------------------------ */

#if !defined(SMOKE_NO_PROFILE)
static void test_arm_by_line(void)
{
  lua_State *L = luaL_newstate();
  Int3Rec rec;
  double v;
  int bcpos, isnext;
  memset(&rec, 0, sizeof(rec));
  luaL_openlibs(L);
  /* Lines of this chunk: 1 G={}, 2-5 G.hdr(x), 6 G.multi, 7 G.genfor,
  ** 8 G.strip, 9 G.stripped.
  */
  run(L,
    "G = {}\n"
    "function G.hdr(x)\n"
    "  local a = x + 1\n"
    "  return a * 2\n"
    "end\n"
    "function G.multi(x) local a = x + 1 local b = a + 1 return b end\n"
    "function G.genfor(t) local s = 0 for k, x in next, t do s = s + x end "
    "return s end\n"
    "function G.strip(x) return x + 1 end\n"
    "G.stripped = assert(loadstring(string.dump(G.strip, true)))\n");
  CHECK(luaJIT_int3_start(L, int3_cb, &rec) == LUAJIT_INT3_OK);

  /* Several slots on one line: the first body slot. The function header sits
  ** on the same line (it shares linedefined) but is never a slot, so a scan
  ** that started at slot 0 would arm it and fail with UNARMABLE.
  */
  bcpos = find_bcpos_n(L, "G.multi", "ADDVN", 0);
  CHECK(bcpos == 1);
  lua_getglobal(L, "G");
  lua_getfield(L, -1, "multi");
  CHECK(luaJIT_int3_arm(L, -1, 6) == LUAJIT_INT3_OK);
  lua_pop(L, 2);
  rec.calls = 0;
  CHECK(run_number(L, "return G.multi(5)", &v) && v == 7);
  printf("[22] line 6: hits=%d value=%g bcpos=%d\n", rec.calls, v, bcpos);
  CHECK(rec.calls == 1);
  CHECK((int)rec.bcpos == bcpos);
  CHECK(luaJIT_int3_cancel(L) == LUAJIT_INT3_OK);

  /* A line that only holds the function header has no slot, and neither has a
  ** line without code.
  */
  lua_getglobal(L, "G");
  lua_getfield(L, -1, "hdr");
  CHECK(luaJIT_int3_arm(L, -1, 2) == LUAJIT_INT3_NOTFOUND);	/* Header only. */
  CHECK(luaJIT_int3_arm(L, -1, 99) == LUAJIT_INT3_NOTFOUND);	/* No code. */
  CHECK(luaJIT_int3_arm(L, -1, 3) == LUAJIT_INT3_OK);
  lua_pop(L, 2);
  rec.calls = 0;
  CHECK(run_number(L, "return G.hdr(3)", &v) && v == 8);
  printf("[22] line 3 (body of hdr): hits=%d value=%g\n", rec.calls, v);
  CHECK(rec.calls == 1);
  CHECK(luaJIT_int3_cancel(L) == LUAJIT_INT3_OK);

  /* A single-line generic-for: the prep pair (ISNEXT and the ITERN slot it
  ** points at) cannot carry a trap (refused below), so the earliest slot the
  ** line entry point can arm is the pre-loop KSHORT, once per call. The same
  ** line also proves the scan keeps bytecode order: a later slot is not chosen.
  */
  isnext = find_bcpos_n(L, "G.genfor", "ISNEXT", 0);
  bcpos = find_bcpos_n(L, "G.genfor", "KSHORT", 0);
  CHECK(bcpos > 0 && isnext > bcpos);
  lua_getglobal(L, "G");
  lua_getfield(L, -1, "genfor");
  CHECK(luaJIT_int3_arm_bc(L, -1, isnext) == LUAJIT_INT3_UNARMABLE);
  CHECK(luaJIT_int3_arm(L, -1, 7) == LUAJIT_INT3_OK);
  lua_pop(L, 2);
  rec.calls = 0;
  CHECK(run_number(L, "return G.genfor({a=1,b=2,c=3})", &v) && v == 6);
  printf("[22] line 7 (ISNEXT at %d refused): hits=%d value=%g bcpos=%d\n",
	 isnext, rec.calls, v, (int)rec.bcpos);
  CHECK(rec.calls == 1);
  CHECK((int)rec.bcpos == bcpos);
  CHECK(luaJIT_int3_cancel(L) == LUAJIT_INT3_OK);

  /* A stripped chunk carries no line map: every slot reports line 0, which is
  ** the debug interface's "no line information", not a source line. The line
  ** entry point must find nothing and arm nothing; the slot entry point still
  ** works, because it needs no line info.
  */
  lua_getglobal(L, "G");
  lua_getfield(L, -1, "stripped");
  CHECK(luaJIT_int3_arm(L, -1, 0) == LUAJIT_INT3_NOTFOUND);
  CHECK(luaJIT_int3_arm(L, -1, 1) == LUAJIT_INT3_NOTFOUND);
  lua_pop(L, 2);
  rec.calls = 0;
  CHECK(run_number(L, "return G.stripped(1)", &v) && v == 2);
  printf("[22] stripped line 0/1: hits=%d (nothing armed)\n", rec.calls);
  CHECK(rec.calls == 0);
  lua_getglobal(L, "G");
  lua_getfield(L, -1, "stripped");
  CHECK(luaJIT_int3_arm_bc(L, -1, 1) == LUAJIT_INT3_OK);
  lua_pop(L, 2);
  rec.calls = 0;
  CHECK(run_number(L, "return G.stripped(1)", &v) && v == 2);
  CHECK(rec.calls == 1);
  CHECK((int)rec.bcpos == 1);
  CHECK(luaJIT_int3_cancel(L) == LUAJIT_INT3_OK);

  /* A C function has no bytecode: both entry points refuse it. */
  lua_getglobal(L, "print");
  CHECK(luaJIT_int3_arm(L, -1, 1) == LUAJIT_INT3_INVALID);
  CHECK(luaJIT_int3_arm_bc(L, -1, 1) == LUAJIT_INT3_INVALID);
  lua_pop(L, 1);
  CHECK(luaJIT_int3_stop(L) == LUAJIT_INT3_OK);
  lua_close(L);
}
#endif  /* !SMOKE_NO_PROFILE */

/* -- [24] Disarming one slot ---------------------------------------------- */

#if !defined(SMOKE_NO_PROFILE)
static int hint_bcpos;			/* Slot of G.h the probes disarm. */
static int gcd_res;

/* __gc runs under the GC hook: disarming there needs no safe point. */
static int gc_disarm_cfunc(lua_State *L)
{
  lua_getglobal(L, "G");
  lua_getfield(L, -1, "h");
  gcd_res = luaJIT_int3_cancel_bc(L, -1, (unsigned int)hint_bcpos);
  lua_pop(L, 2);
  return 0;
}

#if !defined(SMOKE_NO_JIT)
static int rec_disarm_res;
/* A trace callback runs inside the recorder; disarm a foreign prototype. */
static int rec_disarm_probe(lua_State *L)
{
  lua_getglobal(L, "G");
  lua_getfield(L, -1, "h");
  rec_disarm_res = luaJIT_int3_cancel_bc(L, -1, (unsigned int)hint_bcpos);
  lua_pop(L, 2);
  return 0;
}
#endif  /* !SMOKE_NO_JIT */

static void test_cancel_bc(void)
{
  lua_State *L = luaL_newstate();
  Int3Rec rec;
  double v;
  int bc_a = -1, bc_b = -1;
  memset(&rec, 0, sizeof(rec));
  luaL_openlibs(L);
  run(L, "G = {}\n"
	 "function G.a(x) local r = x + 1 return r end\n"
	 "function G.b(x) local r = x + 2 return r end");
  CHECK(luaJIT_int3_start(L, int3_cb, &rec) == LUAJIT_INT3_OK);
  CHECK(arm_slot(L, "G", "a", "ADDVN", &bc_a) == 0);
  CHECK(arm_slot(L, "G", "b", "ADDVN", &bc_b) == 0);
  rec.calls = 0;
  CHECK(run_number(L, "return G.a(1) + G.b(1)", &v) && v == 5);
  CHECK(rec.calls == 2);
  /* Disarm one: the other keeps firing, the session stays open. */
  lua_getglobal(L, "G");
  lua_getfield(L, -1, "a");
  CHECK(luaJIT_int3_cancel_bc(L, -1, (unsigned int)bc_a) == LUAJIT_INT3_OK);
  lua_pop(L, 2);
  rec.calls = 0;
  CHECK(run_number(L, "return G.a(1) + G.b(1)", &v) && v == 5);
  printf("[24] one slot disarmed: hits=%d\n", rec.calls);
  CHECK(rec.calls == 1);
  /* Idempotent: disarming it again is a no-op. */
  lua_getglobal(L, "G");
  lua_getfield(L, -1, "a");
  CHECK(luaJIT_int3_cancel_bc(L, -1, (unsigned int)bc_a) == LUAJIT_INT3_OK);
  /* Out of range and non-Lua functions are caller errors. */
  CHECK(luaJIT_int3_cancel_bc(L, -1, 4096) == LUAJIT_INT3_INVALID);
  lua_pop(L, 2);
  lua_getglobal(L, "print");
  CHECK(luaJIT_int3_cancel_bc(L, -1, 0) == LUAJIT_INT3_INVALID);
  lua_pop(L, 1);
  /* The last armed slot of a prototype releases its JIT exclusion. */
  lua_getglobal(L, "G");
  lua_getfield(L, -1, "b");
  CHECK(luaJIT_int3_cancel_bc(L, -1, (unsigned int)bc_b) == LUAJIT_INT3_OK);
  lua_pop(L, 2);
  rec.calls = 0;
  CHECK(run_number(L, "return G.a(1) + G.b(1)", &v) && v == 5);
  CHECK(rec.calls == 0);
  /* A session that is gone reports DISABLED. */
  CHECK(luaJIT_int3_stop(L) == LUAJIT_INT3_OK);
  lua_getglobal(L, "G");
  lua_getfield(L, -1, "b");
  CHECK(luaJIT_int3_cancel_bc(L, -1, (unsigned int)bc_b)
	== LUAJIT_INT3_DISABLED);
  lua_pop(L, 2);
  lua_close(L);

  /* A finalizer runs under the GC hook: disarming needs no safe point, so it
  ** is accepted there and the slot is restored before the finalizer returns.
  */
  L = luaL_newstate();
  memset(&rec, 0, sizeof(rec));
  luaL_openlibs(L);
  run(L, "G = {}\n"
	 "function G.h(x) local a = x + 1 return a end\n"
	 "local u = newproxy(true)\n"
	 "getmetatable(u).__gc = function() _DISARM() end\n"
	 "G.u = u");
  CHECK(luaJIT_int3_start(L, int3_cb, &rec) == LUAJIT_INT3_OK);
  CHECK(arm_slot(L, "G", "h", "ADDVN", &hint_bcpos) == 0);
  lua_pushcfunction(L, gc_disarm_cfunc);
  lua_setglobal(L, "_DISARM");
  gcd_res = -1;
  run(L, "G.u = nil; collectgarbage('collect'); collectgarbage('collect')");
  printf("[24] disarm in __gc: res=%d\n", gcd_res);
  CHECK(gcd_res == LUAJIT_INT3_OK);
  rec.calls = 0;
  CHECK(run_number(L, "return G.h(1)", &v) && v == 2);
  CHECK(rec.calls == 0);
  CHECK(luaJIT_int3_stop(L) == LUAJIT_INT3_OK);
  lua_close(L);

#if !defined(SMOKE_NO_JIT)
  /* A trace callback runs inside the recorder: disarming another prototype
  ** there is accepted and applied when the event returns.
  */
  L = luaL_newstate();
  memset(&rec, 0, sizeof(rec));
  luaL_openlibs(L);
  run(L, "G = {}\n"
	 "function G.h(x) local a = x + 1 return a end\n"
	 "function G.hot(n) local s = 0 for i = 1, n do s = s + i end return s end");
  CHECK(luaJIT_int3_start(L, int3_cb, &rec) == LUAJIT_INT3_OK);
  CHECK(arm_slot(L, "G", "h", "ADDVN", &hint_bcpos) == 0);
  rec_disarm_res = -1;
  lua_pushcfunction(L, rec_disarm_probe);
  attach_probe(L, lua_gettop(L), "trace", "start");
  lua_pop(L, 1);
  run(L, "for i = 1, 2 do G.hot(1000) end");
  printf("[24] disarm in recorder: res=%d traces=%d\n",
	 rec_disarm_res, trace_count(L));
  CHECK(rec_disarm_res == LUAJIT_INT3_OK);
  CHECK(trace_count(L) > 0);
  rec.calls = 0;
  CHECK(run_number(L, "return G.h(1)", &v) && v == 2);
  CHECK(rec.calls == 0);
  CHECK(luaJIT_int3_stop(L) == LUAJIT_INT3_OK);
  lua_close(L);
#endif  /* !SMOKE_NO_JIT */
}
#endif  /* !SMOKE_NO_PROFILE */

/* -- [23] Session control from a Lua debug hook -------------------------- */

#if !defined(SMOKE_NO_PROFILE)
static int hookctl_res, hookctl_arm_res, hookctl_done;
static Int3Rec hookctl_rec;

/* Run the hook body only inside G.h, so the call is deterministic: every
** scenario installs a line hook, and its first events belong to the chunk that
** calls the function.
*/
static int hookctl_enter(lua_State *L, lua_Debug *ar)
{
  lua_Debug info = *ar;
  if (hookctl_done) return 0;
  if (!lua_getinfo(L, "f", &info)) return 0;	/* Pushes the running function. */
  lua_getglobal(L, "G");
  lua_getfield(L, -1, "h");
  if (!lua_rawequal(L, -3, -1)) {
    lua_pop(L, 3);
    return 0;
  }
  lua_pop(L, 2);				/* G, G.h. */
  hookctl_done = 1;
  return 1;
}

static void hookctl_cancel_hook(lua_State *L, lua_Debug *ar)
{
  if (!hookctl_enter(L, ar)) return;
  hookctl_res = luaJIT_int3_cancel(L);
  lua_pop(L, 1);
}

static void hookctl_stop_hook(lua_State *L, lua_Debug *ar)
{
  if (!hookctl_enter(L, ar)) return;
  hookctl_res = luaJIT_int3_stop(L);
  lua_pop(L, 1);
}

static int hookctl_cancel_res;

/* Arm a slot and disarm it again inside the same hook: the request never
** reaches a safe point, so the slot must never be mounted.
*/
static void hookctl_armcancel_hook(lua_State *L, lua_Debug *ar)
{
  if (!hookctl_enter(L, ar)) return;
  hookctl_res = luaJIT_int3_arm_bc(L, -1, 1);
  hookctl_cancel_res = luaJIT_int3_cancel_bc(L, -1, 1);
  lua_pop(L, 1);
}

static void hookctl_start_hook(lua_State *L, lua_Debug *ar)
{
  if (!hookctl_enter(L, ar)) return;
  hookctl_res = luaJIT_int3_start(L, int3_cb, &hookctl_rec);
  if (hookctl_res == LUAJIT_INT3_OK)
    hookctl_arm_res = luaJIT_int3_arm_bc(L, -1, 1);	/* First body slot. */
  lua_pop(L, 1);
}

static void test_hook_control(void)
{
  lua_State *L;
  Int3Rec rec;
  double v;
  int bcpos = -1;

  /* A trap is delivered on the dispatch entry of its slot, i.e. before the
  ** line hook of the instruction it belongs to: cancelling from that hook
  ** cannot suppress the callback that already ran, but it takes effect for
  ** every later dispatch, and for a slot that has not been reached yet.
  */
  L = luaL_newstate();
  memset(&rec, 0, sizeof(rec));
  luaL_openlibs(L);
  run(L, "G = {}\n"
	 "function G.h(x) local a = x + 1 local b = a + 2 return b end");
  CHECK(luaJIT_int3_start(L, int3_cb, &rec) == LUAJIT_INT3_OK);
  CHECK(arm_slot_n(L, "G", "h", "ADDVN", 0, &bcpos) == 0);
  rec.calls = 0;
  hookctl_res = -1;
  hookctl_done = 0;
  lua_sethook(L, hookctl_cancel_hook, LUA_MASKLINE, 0);
  CHECK(run_number(L, "return G.h(1)", &v) && v == 4);
  lua_sethook(L, NULL, 0, 0);
  printf("[23] cancel in hook: res=%d hits=%d value=%g\n",
	 hookctl_res, rec.calls, v);
  CHECK(hookctl_res == LUAJIT_INT3_OK);
  CHECK(rec.calls == 1);	/* Trap delivered before the line hook. */
  /* Cancelled: the session lives on, nothing is armed, no more hits. */
  CHECK(run_number(L, "return G.h(1)", &v) && v == 4);
  CHECK(rec.calls == 1);
  /* A slot the hook reaches before its dispatch is preempted. */
  CHECK(arm_slot_n(L, "G", "h", "ADDVN", 1, &bcpos) == 0);
  hookctl_res = -1;
  hookctl_done = 0;
  lua_sethook(L, hookctl_cancel_hook, LUA_MASKLINE, 0);
  CHECK(run_number(L, "return G.h(1)", &v) && v == 4);
  lua_sethook(L, NULL, 0, 0);
  printf("[23] cancel before the slot: res=%d hits=%d\n",
	 hookctl_res, rec.calls);
  CHECK(hookctl_res == LUAJIT_INT3_OK);
  CHECK(rec.calls == 1);	/* Slot 1 ran untrapped: no new hit. */
  CHECK(luaJIT_int3_stop(L) == LUAJIT_INT3_OK);
  lua_close(L);

  /* Stop from a hook: the session is released inside the hook, the slot that
  ** has not been reached yet is restored before its dispatch, and the API
  ** reports DISABLED afterwards.
  */
  L = luaL_newstate();
  memset(&rec, 0, sizeof(rec));
  luaL_openlibs(L);
  run(L, "G = {}\n"
	 "function G.h(x) local a = x + 1 local b = a + 2 return b end");
  CHECK(luaJIT_int3_start(L, int3_cb, &rec) == LUAJIT_INT3_OK);
  CHECK(arm_slot_n(L, "G", "h", "ADDVN", 1, &bcpos) == 0);
  rec.calls = 0;
  hookctl_res = -1;
  hookctl_done = 0;
  lua_sethook(L, hookctl_stop_hook, LUA_MASKLINE, 0);
  CHECK(run_number(L, "return G.h(1)", &v) && v == 4);
  lua_sethook(L, NULL, 0, 0);
  printf("[23] stop in hook: res=%d hits=%d value=%g\n",
	 hookctl_res, rec.calls, v);
  CHECK(hookctl_res == LUAJIT_INT3_OK);
  CHECK(rec.calls == 0);
  lua_getglobal(L, "G");
  lua_getfield(L, -1, "h");
  CHECK(luaJIT_int3_arm_bc(L, -1, 1) == LUAJIT_INT3_DISABLED);
  lua_pop(L, 2);
  CHECK(luaJIT_int3_start(L, int3_cb, &rec) == LUAJIT_INT3_OK);
  CHECK(luaJIT_int3_stop(L) == LUAJIT_INT3_OK);
  lua_close(L);

  /* A queued request disarmed before the mount never patches the slot. */
  L = luaL_newstate();
  memset(&rec, 0, sizeof(rec));
  luaL_openlibs(L);
  run(L, "G = {} function G.h(x) local a = x + 1 return a end");
  CHECK(luaJIT_int3_start(L, int3_cb, &rec) == LUAJIT_INT3_OK);
  hookctl_res = hookctl_cancel_res = -1;
  hookctl_done = 0;
  lua_sethook(L, hookctl_armcancel_hook, LUA_MASKLINE, 0);
  CHECK(run_number(L, "return G.h(1)", &v) && v == 2);
  lua_sethook(L, NULL, 0, 0);
  printf("[23] arm+disarm in hook: arm=%d disarm=%d hits=%d\n",
	 hookctl_res, hookctl_cancel_res, rec.calls);
  CHECK(hookctl_res == LUAJIT_INT3_OK);
  CHECK(hookctl_cancel_res == LUAJIT_INT3_OK);
  CHECK(rec.calls == 0);
  CHECK(run_number(L, "return G.h(1)", &v) && v == 2);
  CHECK(rec.calls == 0);	/* Never mounted: no trap in the slot. */
  CHECK(luaJIT_int3_stop(L) == LUAJIT_INT3_OK);
  lua_close(L);

  /* Start and arm from a hook without a session: both are accepted, the
  ** request mounts at the next safe point, so the instruction that is being
  ** dispatched right now still runs untrapped and the next call hits.
  */
  L = luaL_newstate();
  memset(&rec, 0, sizeof(rec));
  memset(&hookctl_rec, 0, sizeof(hookctl_rec));
  luaL_openlibs(L);
  run(L, "G = {} function G.h(x) local a = x + 1 return a end");
  hookctl_res = hookctl_arm_res = -1;
  hookctl_done = 0;
  lua_sethook(L, hookctl_start_hook, LUA_MASKLINE, 0);
  CHECK(run_number(L, "return G.h(1)", &v) && v == 2);
  printf("[23] start+arm in hook: start=%d arm=%d hits=%d\n",
	 hookctl_res, hookctl_arm_res, hookctl_rec.calls);
  CHECK(hookctl_res == LUAJIT_INT3_OK);
  CHECK(hookctl_arm_res == LUAJIT_INT3_OK);
  CHECK(hookctl_rec.calls == 0);	/* The current dispatch is untrapped. */
  CHECK(run_number(L, "return G.h(1)", &v) && v == 2);
  lua_sethook(L, NULL, 0, 0);
  CHECK(hookctl_rec.calls == 1);	/* Mounted at the next safe point. */
  CHECK(luaJIT_int3_stop(L) == LUAJIT_INT3_OK);
  lua_close(L);
}
#endif  /* !SMOKE_NO_PROFILE */

/* -- [16] Automatic deferred mounting ------------------------------------ */

#if !defined(SMOKE_NO_PROFILE) && !defined(SMOKE_NO_JIT)

/* Arm a slot while the recorder owns the VM. The request is only accepted
** with OK, it must not be published inside the event (the prototype stays
** compilable), and it is mounted at the next safe point.
*/
static int defer_bcpos = -1;		/* Slot to arm from the record event. */
static const char *defer_func;		/* Global function to arm. */
static int defer_arm_res;		/* arm() result, -1 = probe never ran. */
static int defer_setmode_res;		/* jit.on inside the event, -1 = n/a. */
static int defer_events;		/* Record events seen. */
static const BpRec *defer_rec;		/* Delivery counter of the session. */
static int defer_hits_at_arm;		/* Deliveries before the arm. */

static int defer_record_probe(lua_State *L)
{
  defer_events++;
  if (defer_arm_res == -1 && defer_bcpos >= 0) {
    defer_hits_at_arm = defer_rec ? defer_rec->calls : 0;
    lua_getglobal(L, "G");
    lua_getfield(L, -1, defer_func);
    defer_arm_res = luaJIT_int3_arm_bc(L, -1, (unsigned int)defer_bcpos);
    if (defer_arm_res == LUAJIT_INT3_OK) {
      /* A pending request has not excluded the prototype from the JIT yet. */
      defer_setmode_res =
	luaJIT_setmode(L, -1, LUAJIT_MODE_FUNC|LUAJIT_MODE_ON);
    }
    lua_pop(L, 2);
  }
  return 0;
}

static void defer_attach(lua_State *L)
{
  lua_pushcfunction(L, defer_record_probe);
  lua_setglobal(L, "_DEFER_REC");
  run(L, "jit.attach(function() _DEFER_REC() end, 'trace', 'start')");
}

static void defer_reset(const char *func, int bcpos, BpRec *rec)
{
  defer_func = func;
  defer_bcpos = bcpos;
  defer_arm_res = -1;
  defer_setmode_res = -1;
  defer_events = 0;
  defer_hits_at_arm = 0;
  defer_rec = rec;
}

/* Arm a breakpoint of G.a from a C helper called by G.b: the running frame is
** G.b, the armed prototype is another one. The request must mount before the
** helper returns (a C function is a safe context) and must not disturb G.b.
*/
static int armc_arm_res = -1;
static int armc_bcpos = -1;
static int armc_calls;
static void armc_cb(void *data, lua_State *L, const void *func,
		     unsigned int bcpos)
{
  (void)data; (void)L; (void)func; (void)bcpos;
  armc_calls++;
}
static int armc_helper(lua_State *L)
{
  lua_getglobal(L, "G");
  lua_getfield(L, -1, "a");
  armc_arm_res = luaJIT_int3_arm_bc(L, -1, (unsigned int)armc_bcpos);
  lua_pop(L, 2);
  return 0;
}

static void test_arm_from_c(void)
{
  lua_State *L = luaL_newstate();
  int bcpos = -1;
  double v;
  luaL_openlibs(L);
  run(L,
    "G = {}\n"
    "function G.a(n) local s = 0 for i = 1, n do s = s + i end return s end\n"
    "function G.b(n) local s = 0 for i = 1, n do s = s + i * 2 "
    "call_helper() end return s end\n");
  lua_pushcfunction(L, armc_helper);
  lua_setglobal(L, "call_helper");
  armc_bcpos = find_bcpos_n(L, "G.a", "ADDVV", 0);
  CHECK(armc_bcpos >= 0);
  CHECK(luaJIT_int3_start(L, armc_cb, NULL) == LUAJIT_INT3_OK);
  /* Mount G.a, run it, then drop the mapping but keep the session. */
  bcpos = find_bcpos_n(L, "G.a", "ADDVV", 0);
  CHECK(bcpos >= 0);
  lua_getglobal(L, "G");
  lua_getfield(L, -1, "a");
  CHECK(luaJIT_int3_arm_bc(L, -1, (unsigned int)bcpos) == LUAJIT_INT3_OK);
  lua_pop(L, 2);
  CHECK(run_number(L, "return G.a(4)", &v) && v == 10);
  CHECK(luaJIT_int3_cancel(L) == LUAJIT_INT3_OK);
  /* While G.b runs, a C helper arms G.a again: the trap is planted in G.a,
  ** and G.b keeps executing its own bytecode.
  */
  armc_arm_res = -1;
  armc_calls = 0;
  CHECK(run_number(L, "return G.b(8)", &v) && v == 72);
  printf("[17] arm from C: arm=%d a-hits=%d value=%g\n", armc_arm_res,
	 armc_calls, v);
  CHECK(armc_arm_res == LUAJIT_INT3_OK);
  CHECK(armc_calls == 0);		/* G.a did not run here. */
  CHECK(run_number(L, "return G.a(3)", &v) && v == 6);
  printf("[17] arm from C: a(3)=%g a-hits=%d\n", v, armc_calls);
  CHECK(armc_calls == 3);		/* And its own breakpoint still works. */
  CHECK(luaJIT_int3_stop(L) == LUAJIT_INT3_OK);
  lua_close(L);
}

static void test_deferred_mount(void)
{
  lua_State *L = luaL_newstate();
  BpRec rec;
  int bcpos = -1;
  double v;

  memset(&rec, 0, sizeof(rec));
  luaL_openlibs(L);
  run(L,
    "G = {}\n"
    "function G.df(n) local s=0 for i=1,n do s=s+i end return s end\n"
    "function G.other(n) local s=0 for i=1,n do s=s+n end return s end\n"
    "function G.self(n) local s=0 for i=1,n do s=s+i end return s end\n");
  CHECK(luaJIT_int3_start(L, bp_cb, &rec) == LUAJIT_INT3_OK);
  defer_attach(L);
  /* Warm the target so that it owns traces a mount has to flush. */
  run(L, "jit.opt.start('hotloop=1')");
  CHECK(run_number(L, "return G.df(200)", &v) && v == 20100);
  run(L, "jit.opt.start('hotloop=56')");
  CHECK(trace_count(L) > 0);
  bcpos = find_bcpos_n(L, "G.df", "ADDVV", 0);
  CHECK(bcpos >= 0);
  defer_reset("df", bcpos, &rec);
  /* Record another prototype: its record event arms G.df. */
  run(L, "jit.opt.start('hotloop=1'); for i=1,4 do G.other(300) end; "
	 "jit.opt.start('hotloop=56')");
  printf("[16] record event: arm=%d events=%d setmode=%d hits-at-arm=%d\n",
	 defer_arm_res, defer_events, defer_setmode_res, defer_hits_at_arm);
  CHECK(defer_events >= 1);		/* A real record event armed it. */
  CHECK(defer_arm_res == LUAJIT_INT3_OK);	/* Accepted, not BUSY. */
  CHECK(defer_hits_at_arm == 0);	/* Nothing delivered in the event. */
  CHECK(defer_setmode_res == 1);	/* Still pending, not mounted. */
  CHECK(set_jit_on(L, "G", "df") == 0);	/* Mounted: excluded now. */
  rec.calls = 0;
  CHECK(run_number(L, "return G.df(50)", &v) && v == 1275);
  printf("[16] deferred hits: %d\n", rec.calls);
  CHECK(rec.calls == 50);		/* Permanent and repeatable. */
  rec.calls = 0;
  CHECK(run_number(L, "return G.df(50)", &v) && v == 1275);
  CHECK(rec.calls == 50);
  CHECK(luaJIT_int3_cancel(L) == LUAJIT_INT3_OK);

  /* Arm a slot of the prototype the recorder is recording itself. The commit
  ** refuses to work while the recorder owns the VM, so the request stays
  ** pending, the finished trace is flushed when it is mounted at the next safe
  ** point, and the next activation delivers.
  */
  memset(&rec, 0, sizeof(rec));
  bcpos = find_bcpos_n(L, "G.self", "ADDVV", 0);
  CHECK(bcpos >= 0);
  defer_reset("self", bcpos, &rec);
  rec.calls = 0;
  CHECK(run_number(L, "jit.opt.start('hotloop=1'); return G.self(200)", &v) &&
	v == 20100);
  run(L, "jit.opt.start('hotloop=56')");
  CHECK(defer_arm_res == LUAJIT_INT3_OK);	/* Accepted, not BUSY. */
  rec.calls = 0;
  CHECK(run_number(L, "return G.self(30)", &v) && v == 465);
  printf("[16] self record event: arm=%d hits=%d value=%g\n", defer_arm_res,
	 rec.calls, v);
  CHECK(rec.calls == 30);	/* Mounted, and the trace was flushed. */
  rec.calls = 0;
  CHECK(run_number(L, "return G.self(30)", &v) && v == 465);
  CHECK(rec.calls == 30);
  CHECK(luaJIT_int3_stop(L) == LUAJIT_INT3_OK);
  lua_close(L);
}

#endif  /* !SMOKE_NO_PROFILE && !SMOKE_NO_JIT */

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
  CHECK(luaJIT_int3_arm_bc(L, 0, 0) == LUAJIT_INT3_UNAVAILABLE);
  CHECK(luaJIT_int3_cancel(L) == LUAJIT_INT3_UNAVAILABLE);
  CHECK(luaJIT_int3_stop(L) == LUAJIT_INT3_UNAVAILABLE);
  CHECK(rec.calls == 0);
  /* The VM is otherwise unaffected. */
  run(L, "G = {} function G.sum(n) local s=0 for i=1,n do s=s+i end return s end");
  CHECK(run_number(L, "return G.sum(1000)", &v) && v == 500500);
  CHECK(run_number(L, "return G.sum(1000)", &v) && v == 500500);
  printf("[9] no-profile: calls=%d jit=%d", rec.calls,
	 run_bool(L, "return jit.status()"));
#if !defined(SMOKE_NO_JIT)
  printf(" traces=%d", trace_count(L));
#endif
  printf("\n");
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
  if (!only || only == 9) test_permanent();
  if (!only || only == 10) test_multi();
  if (!only || only == 11) test_hook_and_hotcount();
  if (!only || only == 15) test_abi_probe();
  if (!only || only == 18) test_errno();
  if (!only || only == 19) test_close_finalizer();
  if (!only || only == 20) test_loop_slots();
  if (!only || only == 21) test_callback_func();
  if (!only || only == 22) test_arm_by_line();
  if (!only || only == 23) test_hook_control();
  if (!only || only == 24) test_cancel_bc();
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
  if (!only || only == 8) {
    test_unsafe();
    test_arm_hook_immediate();
    test_arm_hook_error();
  }
  if (!only || only == 9) test_permanent();
  if (!only || only == 10) test_multi();
  if (!only || only == 11) test_hook_and_hotcount();
  if (!only || only == 12) test_hotcount_params();
  if (!only || only == 13) test_nojit_ownership();
  if (!only || only == 14) test_slowpaths();
  if (!only || only == 15) test_abi_probe();
  if (!only || only == 16) test_deferred_mount();
  if (!only || only == 17) test_arm_from_c();
  if (!only || only == 18) test_errno();
  if (!only || only == 19) test_close_finalizer();
  if (!only || only == 20) test_loop_slots();
  if (!only || only == 21) test_callback_func();
  if (!only || only == 22) test_arm_by_line();
  if (!only || only == 23) test_hook_control();
  if (!only || only == 24) test_cancel_bc();
#endif
#endif
  printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
  return failures ? 1 : 0;
}

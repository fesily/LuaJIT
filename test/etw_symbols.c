/*
** Real ETW integration test for the Windows JIT symbol provider (lj_etw.c).
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
**
** Drives a real ETW session (StartTraceW/EnableTraceEx2/OpenTraceW/
** ProcessTrace) against the Microsoft-JScript provider LuaJIT emits, and
** checks the wire protocol the way a consumer (WPA/PerfView) reads it. Trace
** addresses/sizes are compared against the traces actually committed by the
** VM, which are read back from the interpreter state (oracle). The consumer
** thread only looks at the ETW payload.
**
** Exit codes: 0 pass, 1 failure, 77 skip (no privilege for a session).
*/

#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* This executable uses require(); the VM owns its internal assertion handler. */
#undef LUA_USE_ASSERT
#undef LUA_USE_APICHECK
#include "lj_obj.h"
#include "lj_jit.h"
#include "lj_dispatch.h"
#include "lauxlib.h"
#include "lualib.h"

#define ETW_WAIT_MS		5000
#define ETW_SETTLE_MS		400
#define ETW_MAX_TRACES		64
#define ETW_MAX_MSGS		256
#define ETW_HEAD_CHARS		192	/* Payload strings are truncated. */
#define ETW_TAIL_CHARS		48
#define ETW_MAX_LOGS		16	/* Pool: logs outlive the stack frame. */

static int failures;

static void fail(const char *scenario, const char *fmt, ...)
{
  va_list ap;
  fprintf(stderr, "FAIL: %s: ", scenario);
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  fflush(stderr);
  failures++;
}

static void ok(const char *scenario)
{
  printf("ok: %s\n", scenario);
  fflush(stdout);
}

static void skip(const char *fmt, ...)
{
  va_list ap;
  fprintf(stderr, "SKIP: ");
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  fflush(stderr);
  exit(77);
}

static const char *winerr(DWORD e)
{
  static char buf[64];
  sprintf(buf, "error %lu", (unsigned long)e);
  return buf;
}

static int no_privilege(DWORD e)
{
  return e == ERROR_ACCESS_DENIED || e == ERROR_PRIVILEGE_NOT_HELD;
}

static void need_privilege(DWORD e)
{
  if (no_privilege(e))
    skip("creating an ETW session needs an elevated or "
	 "Performance Log Users shell (%s)", winerr(e));
}

/* -- Oracle: the traces the VM actually committed ------------------------- */

typedef struct TraceRef {
  void *mcode;
  UINT64 size;
  UINT32 traceno;
  int is_root;
} TraceRef;

/* -- Event records -------------------------------------------------------- */

typedef struct EventMsg {
  USHORT id;
  ULONG pid;
  void *ctx;
  void *addr;
  UINT64 size;
  UINT64 source_id;
  UINT32 method_id;
  UINT32 line;
  int name_chars;		/* UTF-16 units excluding NUL. */
  int url_chars;
  WCHAR name_head[ETW_HEAD_CHARS];
  WCHAR name_tail[ETW_TAIL_CHARS];
  WCHAR url_head[ETW_HEAD_CHARS];
  WCHAR url_tail[ETW_TAIL_CHARS];
} EventMsg;

typedef struct EventLog {
  CRITICAL_SECTION lock;
  HANDLE changed;		/* Auto-reset, set on every new message. */
  int count;
  int overflow;
  int malformed;
  int seen;			/* Callback invocations. */
  int foreign;			/* Events from another process. */
  EventMsg msgs[ETW_MAX_MSGS];
} EventLog;

/* Event logs are large, so they live in a static pool instead of on the
** stack. The session restricts delivery to this process, so any foreign
** event is a filter failure and fails the scenario.
*/
static EventLog log_pool[ETW_MAX_LOGS];
static int n_logs;

static EventLog *log_new(void)
{
  EventLog *l;
  if (n_logs >= ETW_MAX_LOGS) {
    fprintf(stderr, "FAIL: event log pool exhausted\n");
    exit(1);
  }
  l = &log_pool[n_logs++];
  memset(l, 0, sizeof(*l));
  InitializeCriticalSection(&l->lock);
  l->changed = CreateEventW(NULL, FALSE, FALSE, NULL);
  return l;
}

static void copy_head_tail(WCHAR *head, WCHAR *tail, const WCHAR *src, int n)
{
  int hn = n < ETW_HEAD_CHARS - 1 ? n : ETW_HEAD_CHARS - 1;
  int tn = n < ETW_TAIL_CHARS - 1 ? n : ETW_TAIL_CHARS - 1;
  memcpy(head, src, (size_t)hn * sizeof(WCHAR));
  head[hn] = 0;
  memcpy(tail, src + (n - tn), (size_t)tn * sizeof(WCHAR));
  tail[tn] = 0;
}

/* Payload layout is fixed by the manifest; pointer fields follow the width of
** the emitting process. Mirrors PerfView's JScriptTraceEventParser.
*/
static int parse_event(PEVENT_RECORD rec, EventMsg *m)
{
  const BYTE *p = (const BYTE *)rec->UserData;
  ULONG len = rec->UserDataLength;
  const size_t P = sizeof(void *);
  memset(m, 0, sizeof(*m));
  m->id = rec->EventHeader.EventDescriptor.Id;
  m->pid = rec->EventHeader.ProcessId;
  switch (m->id) {
  case 1: case 2: case 3: case 4:	/* Init/complete: no payload. */
    return len == 0;
  case 7: case 8: case 11: case 12:	/* ScriptContext: one pointer. */
    if (len != P) return 0;
    memcpy(&m->ctx, p, P);
    return 1;
  case 5: case 6: case 9: case 10: {	/* Method events. */
    size_t fixed = 2*P + 8 + 4 + 2 + 2 + 8 + 4 + 4;
    size_t n;
    if (len < fixed + 2) return 0;
    memcpy(&m->ctx, p, P); p += P;
    memcpy(&m->addr, p, P); p += P;
    memcpy(&m->size, p, 8); p += 8;
    memcpy(&m->method_id, p, 4); p += 4;
    p += 4;				/* MethodFlags + MethodAddressRangeID. */
    memcpy(&m->source_id, p, 8); p += 8;
    memcpy(&m->line, p, 4); p += 4;
    p += 4;				/* Column. */
    n = (len - fixed) / 2;
    if (((len - fixed) & 1) || n < 1) return 0;
    if (((const WCHAR *)p)[n - 1] != 0) return 0;	/* NUL-terminated. */
    m->name_chars = (int)n - 1;
    copy_head_tail(m->name_head, m->name_tail, (const WCHAR *)p,
		   m->name_chars);
    return 1;
  }
  case 39: case 40: case 41: case 42: {	/* Source events. */
    size_t fixed = 8 + P + 4;
    size_t n;
    if (len < fixed + 2) return 0;
    memcpy(&m->source_id, p, 8); p += 8;
    memcpy(&m->ctx, p, P); p += P;
    p += 4;				/* SourceFlags. */
    n = (len - fixed) / 2;
    if (((len - fixed) & 1) || n < 1) return 0;
    if (((const WCHAR *)p)[n - 1] != 0) return 0;
    m->url_chars = (int)n - 1;
    copy_head_tail(m->url_head, m->url_tail, (const WCHAR *)p, m->url_chars);
    return 1;
  }
  default:
    return 0;
  }
}

static const GUID provider_guid =
  { 0x57277741, 0x3638, 0x4a4b,
    { 0xbd, 0xba, 0x0a, 0xc6, 0xe4, 0x5d, 0xa5, 0x6c } };

static void WINAPI event_cb(PEVENT_RECORD rec)
{
  EventLog *log = (EventLog *)rec->UserContext;
  EventMsg m;
  EnterCriticalSection(&log->lock);
  log->seen++;
  LeaveCriticalSection(&log->lock);
  if (memcmp(&rec->EventHeader.ProviderId, &provider_guid, sizeof(GUID)) != 0 ||
      rec->EventHeader.ProcessId != GetCurrentProcessId()) {
    EnterCriticalSection(&log->lock);
    log->foreign++;
    LeaveCriticalSection(&log->lock);
    return;
  }
  EnterCriticalSection(&log->lock);
  if (!parse_event(rec, &m)) {
    log->malformed++;
  } else if (log->count < ETW_MAX_MSGS) {
    log->msgs[log->count++] = m;
  } else {
    log->overflow = 1;
  }
  LeaveCriticalSection(&log->lock);
  SetEvent(log->changed);
}

static int log_count(EventLog *log)
{
  int n;
  EnterCriticalSection(&log->lock);
  n = log->count;
  LeaveCriticalSection(&log->lock);
  return n;
}

static int log_bad(EventLog *log)
{
  int n;
  EnterCriticalSection(&log->lock);
  n = log->overflow || log->malformed || log->foreign;
  LeaveCriticalSection(&log->lock);
  return n;
}

static int log_malformed(EventLog *log)
{
  int n;
  EnterCriticalSection(&log->lock);
  n = log->malformed;
  LeaveCriticalSection(&log->lock);
  return n;
}

static int log_foreign(EventLog *log)
{
  int n;
  EnterCriticalSection(&log->lock);
  n = log->foreign;
  LeaveCriticalSection(&log->lock);
  return n;
}

/* Drop everything recorded so far. Used by the stress scenario, which
** deliberately overruns a fixed-size log before its final assertions.
*/
static void log_reset(EventLog *log)
{
  EnterCriticalSection(&log->lock);
  log->count = 0;
  log->overflow = 0;
  log->malformed = 0;
  log->foreign = 0;
  log->seen = 0;
  LeaveCriticalSection(&log->lock);
}

typedef int (*MsgPred)(const EventMsg *m, const void *arg);

/* Wait for the first match at index >= from; returns the index or -1. */
static int log_wait(EventLog *log, int from, MsgPred pred, const void *arg,
		    DWORD ms)
{
  ULONG64 start = GetTickCount64();
  for (;;) {
    int i, n;
    EnterCriticalSection(&log->lock);
    n = log->count;
    for (i = from; i < n; i++) {
      if (pred(&log->msgs[i], arg)) {
	LeaveCriticalSection(&log->lock);
	return i;
      }
    }
    LeaveCriticalSection(&log->lock);
    if (GetTickCount64() - start >= ms) return -1;
    WaitForSingleObject(log->changed, 20);
  }
}

/* Count matches in [from, count) without waiting. */
static int log_count_if(EventLog *log, int from, MsgPred pred, const void *arg)
{
  int i, n, c = 0;
  EnterCriticalSection(&log->lock);
  n = log->count;
  for (i = from; i < n; i++)
    if (pred(&log->msgs[i], arg)) c++;
  LeaveCriticalSection(&log->lock);
  return c;
}

/* Settle window for assertions that something did NOT happen. Expected
** events are never waited for with a fixed sleep.
*/
static void log_settle(void)
{
  Sleep(ETW_SETTLE_MS);
}

static int log_msg(EventLog *log, int idx, EventMsg *out)
{
  int okk = 0;
  if (idx < 0) return 0;
  EnterCriticalSection(&log->lock);
  if (idx < log->count) { *out = log->msgs[idx]; okk = 1; }
  LeaveCriticalSection(&log->lock);
  return okk;
}

static int pred_id(const EventMsg *m, const void *arg)
{
  return m->id == (USHORT)(uintptr_t)arg;
}

static int pred_ctx(const EventMsg *m, const void *arg)
{
  return m->ctx == arg;
}

static int pred_ctx_load(const EventMsg *m, const void *arg)
{
  return m->id == 11 && m->ctx == arg;
}

static int pred_ctx_unload(const EventMsg *m, const void *arg)
{
  return m->id == 12 && m->ctx == arg;
}

static int pred_unload_id(const EventMsg *m, const void *arg)
{
  return m->id == 10 && m->method_id == (UINT32)(uintptr_t)arg;
}

static int pred_load_addr(const EventMsg *m, const void *arg)	/* MethodLoad. */
{
  const TraceRef *t = (const TraceRef *)arg;
  return m->id == 9 && m->addr == t->mcode && m->size == t->size;
}

static int pred_rundown_addr(const EventMsg *m, const void *arg)
{
  const TraceRef *t = (const TraceRef *)arg;			/* MethodDCStart. */
  return m->id == 5 && m->addr == t->mcode && m->size == t->size;
}

static int pred_name_has(const EventMsg *m, const void *arg)
{
  return m->id == 9 && wcsstr(m->name_tail, (const WCHAR *)arg) != NULL;
}

/* SourceLoad with at least this many URL code units: used to pick the
** truncated overlong name out of a stream that still carries earlier events.
*/
static int pred_source_len(const EventMsg *m, const void *arg)
{
  return m->id == 41 && m->url_chars >= (int)(uintptr_t)arg;
}

/* SourceLoad whose URL equals the given name (deterministic stage ordering). */
static int pred_url_is(const EventMsg *m, const void *arg)
{
  return m->id == 41 && wcscmp(m->url_head, (const WCHAR *)arg) == 0;
}

/* Diagnostic: every (id, context) pair seen in [from, count). */
static void log_dump_ctxs(EventLog *log, int from, const char *sc)
{
  int i, n;
  fprintf(stderr, "contexts for %s:\n", sc);
  EnterCriticalSection(&log->lock);
  n = log->count;
  for (i = from; i < n; i++)
    fprintf(stderr, "  id %u ctx %p method %u\n", log->msgs[i].id,
	    log->msgs[i].ctx, log->msgs[i].method_id);
  LeaveCriticalSection(&log->lock);
  fflush(stderr);
}

/* -- Sessions ------------------------------------------------------------ */

typedef struct Session {
  WCHAR name[128];
  TRACEHANDLE handle;
  TRACEHANDLE consumer;
  HANDLE thread;
  EventLog *log;
  volatile LONG live;
  DWORD pt_rc;			/* ProcessTrace return code. */
  int pt_done;
} Session;

/* Every session this process starts is tracked so that failure exits still
** stop it. Only our own session names are ever touched.
*/
#define ETW_MAX_SESSIONS	16
static Session *live_sessions[ETW_MAX_SESSIONS];
static int n_live_sessions;

static void sessions_track(Session *s)
{
  int i;
  for (i = 0; i < ETW_MAX_SESSIONS; i++)
    if (live_sessions[i] == NULL) { live_sessions[i] = s; n_live_sessions++; return; }
  if (i == ETW_MAX_SESSIONS) {
    fprintf(stderr, "FAIL: too many concurrent ETW sessions\n");
    exit(1);
  }
}

static void sessions_untrack(Session *s)
{
  int i;
  for (i = 0; i < ETW_MAX_SESSIONS; i++)
    if (live_sessions[i] == s) {
      live_sessions[i] = NULL;
      n_live_sessions--;
      return;
    }
}

static void session_stop(Session *s, const char **why);

static void cleanup_sessions(void)
{
  int i;
  for (i = 0; i < ETW_MAX_SESSIONS; i++)
    if (live_sessions[i] != NULL) {
      const char *why = NULL;
      session_stop(live_sessions[i], &why);
    }
}

static DWORD WINAPI consumer_proc(LPVOID param)
{
  Session *s = (Session *)param;
  s->pt_rc = ProcessTrace(&s->consumer, 1, NULL, NULL);
  s->pt_done = 1;
  return 0;
}

static int session_enable(Session *s, UINT64 keywords, DWORD *out_err);
static int session_capture(Session *s, UINT64 keywords, DWORD *out_err);

/* Start a real-time session with the given keywords. */
static int session_start(Session *s, const WCHAR *tag, UINT64 keywords,
			 EventLog *log, DWORD *out_err)
{
  const size_t propsz = sizeof(EVENT_TRACE_PROPERTIES) + sizeof(s->name);
  EVENT_TRACE_PROPERTIES *props = (EVENT_TRACE_PROPERTIES *)calloc(1, propsz);
  EVENT_TRACE_LOGFILEW lf;
  ULONG rc;
  memset(s, 0, sizeof(*s));
  s->log = log;
  _snwprintf(s->name, 128, L"LuaJIT-ETW-%lu-%llu-%s",
	     (unsigned long)GetCurrentProcessId(),
	     (unsigned long long)GetTickCount64(), tag);
  props->Wnode.BufferSize = (ULONG)propsz;
  props->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
  props->Wnode.ClientContext = 1;			/* QPC. */
  props->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
  props->BufferSize = 128;
  props->MinimumBuffers = 8;
  props->MaximumBuffers = 32;
  props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
  rc = StartTraceW(&s->handle, s->name, props);
  if (rc != ERROR_SUCCESS) {
    *out_err = rc;
    free(props);
    return 0;
  }
  sessions_track(s);
  memset(&lf, 0, sizeof(lf));
  lf.LoggerName = s->name;
  lf.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME |
			PROCESS_TRACE_MODE_EVENT_RECORD;
  lf.EventRecordCallback = event_cb;
  lf.Context = log;
  s->consumer = OpenTraceW(&lf);
  if (s->consumer == (TRACEHANDLE)INVALID_HANDLE_VALUE) {
    *out_err = GetLastError();
    ControlTraceW(s->handle, NULL, props, EVENT_TRACE_CONTROL_STOP);
    sessions_untrack(s);
    free(props);
    return 0;
  }
  s->thread = CreateThread(NULL, 0, consumer_proc, s, 0, NULL);
  if (!s->thread) {
    *out_err = GetLastError();
    CloseTrace(s->consumer);
    ControlTraceW(s->handle, NULL, props, EVENT_TRACE_CONTROL_STOP);
    sessions_untrack(s);
    free(props);
    return 0;
  }
  InterlockedExchange(&s->live, 1);
  free(props);
  /* A consumer that returns immediately would silently break every later
  ** assertion, so surface it as the session-level failure it is. */
  if (WaitForSingleObject(s->thread, 500) == WAIT_OBJECT_0) {
    *out_err = s->pt_rc ? s->pt_rc : ERROR_INVALID_DATA;
    ControlTraceW(s->handle, NULL, props, EVENT_TRACE_CONTROL_STOP);
    sessions_untrack(s);
    CloseTrace(s->consumer);
    CloseHandle(s->thread);
    s->thread = NULL;
    return 0;
  }
  return session_enable(s, keywords, out_err);
}

static int session_enable(Session *s, UINT64 keywords, DWORD *out_err)
{
  /* Microsoft-JScript is also used by every Chakra host in the machine (e.g.
  ** Edge/WebView2), which floods the provider GUID. Restrict delivery to this
  ** process, so only our own JIT events reach the consumer.
  */
  ULONG pid = GetCurrentProcessId();
  EVENT_FILTER_DESCRIPTOR filter;
  ENABLE_TRACE_PARAMETERS params;
  filter.Ptr = (ULONGLONG)(ULONG_PTR)&pid;
  filter.Size = (ULONG)sizeof(pid);
  filter.Type = EVENT_FILTER_TYPE_PID;
  memset(&params, 0, sizeof(params));
  params.Version = ENABLE_TRACE_PARAMETERS_VERSION_2;
  params.EnableFilterDesc = &filter;
  params.FilterDescCount = 1;
  {
    ULONG rc = EnableTraceEx2(s->handle, &provider_guid,
			      EVENT_CONTROL_CODE_ENABLE_PROVIDER,
			      TRACE_LEVEL_INFORMATION, keywords, 0, 0, &params);
    if (rc != ERROR_SUCCESS) { *out_err = rc; return 0; }
  }
  return 1;
}

static int session_capture(Session *s, UINT64 keywords, DWORD *out_err)
{
  ULONG rc = EnableTraceEx2(s->handle, &provider_guid,
			    EVENT_CONTROL_CODE_CAPTURE_STATE,
			    TRACE_LEVEL_INFORMATION, keywords, 0, 0, NULL);
  if (rc != ERROR_SUCCESS) { *out_err = rc; return 0; }
  return 1;
}

/* Stop and reap the session; reports whether any events were lost. */
static void session_stop(Session *s, const char **why)
{
  static char msg[128];
  const size_t propsz = sizeof(EVENT_TRACE_PROPERTIES) + sizeof(s->name);
  EVENT_TRACE_PROPERTIES *props = (EVENT_TRACE_PROPERTIES *)calloc(1, propsz);
  props->Wnode.BufferSize = (ULONG)propsz;
  props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
  ControlTraceW(s->handle, NULL, props, EVENT_TRACE_CONTROL_STOP);
  InterlockedExchange(&s->live, 0);
  if (props->EventsLost || props->RealTimeBuffersLost || props->LogBuffersLost) {
    sprintf(msg, "session lost %lu events / %lu rt buffers / %lu log buffers",
	    (unsigned long)props->EventsLost,
	    (unsigned long)props->RealTimeBuffersLost,
	    (unsigned long)props->LogBuffersLost);
    *why = msg;
  }
  free(props);
  sessions_untrack(s);
  CloseTrace(s->consumer);
  WaitForSingleObject(s->thread, 10000);
  CloseHandle(s->thread);
  s->thread = NULL;
  s->handle = 0;
  s->consumer = 0;
}

/* -- Lua VM helpers ------------------------------------------------------ */

#define WORK_CHECKSUM	5000050000.0
#define WORK_SRC	"local s=0\nfor i=1,100000 do s=s+i end\nreturn s\n"

/* Read the traces actually committed by the VM (the oracle). */
static int vm_traces(lua_State *L, TraceRef *out, int max)
{
  jit_State *J = L2J(L);
  int n = 0;
  ptrdiff_t i;
  for (i = 1; i < (ptrdiff_t)J->sizetrace && n < max; i++) {
    GCobj *o = gcref(J->trace[i]);
    if (o != NULL) {
      GCtrace *T = gco2trace(o);
      out[n].mcode = (void *)T->mcode;
      out[n].size = (UINT64)T->szmcode;
      out[n].traceno = (UINT32)T->traceno;
      out[n].is_root = T->root == 0;
      n++;
    }
  }
  return n;
}

static int vm_traces_count(lua_State *L)
{
  TraceRef traces[ETW_MAX_TRACES];
  return vm_traces(L, traces, ETW_MAX_TRACES);
}

static int vm_first_root(lua_State *L, TraceRef *out)
{
  TraceRef traces[ETW_MAX_TRACES];
  int ntraces = vm_traces(L, traces, ETW_MAX_TRACES);
  int i;
  for (i = 0; i < ntraces; i++)
    if (traces[i].is_root) { *out = traces[i]; return 1; }
  return 0;
}

static void *vm_ctx(lua_State *L)
{
  return (void *)J2G(L2J(L));
}

static lua_State *vm_new(void)
{
  lua_State *L = luaL_newstate();
  if (!L) {
    fprintf(stderr, "FAIL: luaL_newstate\n");
    exit(1);
  }
  luaL_openlibs(L);
  return L;
}

/* Compile the hot-loop chunk; leaves the function on the stack. */
static void vm_load_work(lua_State *L, const char *chunkname)
{
  int rc = luaL_loadbuffer(L, WORK_SRC, strlen(WORK_SRC), chunkname);
  if (rc != 0) {
    fprintf(stderr, "FAIL: loadbuffer: %s\n", lua_tostring(L, -1));
    exit(1);
  }
}

/* Call the function on top of the stack once; checks the checksum. */
static int vm_call_work(lua_State *L, double *out)
{
  int rc;
  if (lua_type(L, -1) != LUA_TFUNCTION) {
    fprintf(stderr, "FAIL: vm_call_work: top=%d type=%s\n", lua_gettop(L),
	    lua_typename(L, lua_type(L, -1)));
    fflush(stderr);
    exit(1);
  }
  rc = lua_pcall(L, 0, 1, 0);
  if (rc != 0) {
    fprintf(stderr, "FAIL: pcall: %s\n", lua_tostring(L, -1));
    fflush(stderr);
    exit(1);
  }
  *out = lua_tonumber(L, -1);
  lua_pop(L, 1);
  return *out == WORK_CHECKSUM ? 0 : -1;
}

/* Load `chunkname`, run it `reps` times and check every result. */
static int vm_prewarm(lua_State *L, const char *chunkname, int reps,
		      const char *scenario)
{
  double v = 0;
  int i;
  vm_load_work(L, chunkname);
  for (i = 0; i < reps; i++) {
    lua_pushvalue(L, -1);
    if (vm_call_work(L, &v) != 0) {
      fail(scenario, "checksum %.0f != %.0f", v, WORK_CHECKSUM);
      lua_pop(L, 1);
      return 0;
    }
  }
  lua_pop(L, 1);
  return 1;
}

/* jit.flush() with no argument (all traces) or one root trace. */
static void vm_flush(lua_State *L, int traceno)
{
  int rc;
  lua_getglobal(L, "jit");
  lua_getfield(L, -1, "flush");
  if (traceno < 0) rc = lua_pcall(L, 0, 0, 0);
  else { lua_pushinteger(L, traceno); rc = lua_pcall(L, 1, 0, 0); }
  if (rc != 0) {
    fprintf(stderr, "FAIL: jit.flush: %s\n", lua_tostring(L, -1));
    exit(1);
  }
  lua_pop(L, 1);
}

static void vm_collectgarbage(lua_State *L, int n)
{
  int i;
  for (i = 0; i < n; i++) lua_gc(L, LUA_GCCOLLECT, 0);
}

/* -- Shared assertions --------------------------------------------------- */

/* Wait for a full start rundown of the live cache (events 1, 7, 39, 5, 2). */
static int wait_rundown(EventLog *log, int mark, const char *sc,
			TraceRef *traces, int ntraces)
{
  int i, idx;
  idx = log_wait(log, mark, pred_id, (const void *)(uintptr_t)1, ETW_WAIT_MS);
  if (idx < 0) { fail(sc, "no DCStartInit"); return 0; }
  if (log_wait(log, mark, pred_id, (const void *)(uintptr_t)7, ETW_WAIT_MS) < 0)
    fail(sc, "no ScriptContextDCStart");
  for (i = 0; i < ntraces; i++) {
    if (log_wait(log, mark, pred_rundown_addr, &traces[i], ETW_WAIT_MS) < 0)
      fail(sc, "no MethodDCStart for trace %u", traces[i].traceno);
  }
  if (log_count_if(log, mark, pred_id, (const void *)(uintptr_t)39) < 1)
    fail(sc, "no SourceDCStart");
  if (log_wait(log, mark, pred_id, (const void *)(uintptr_t)2, ETW_WAIT_MS) < 0)
    fail(sc, "no DCStartComplete");
  return 1;
}

/* 1. Collector before VM/trace. */
static void scenario_collector_first(void)
{
  const char *sc = "collector-first";
  EventLog *log = log_new();
  Session s;
  DWORD err = 0;
  lua_State *L;
  TraceRef traces[ETW_MAX_TRACES];
  int ntraces, mark, idx, i;
  EventMsg m;
  /* Collector first: the session is enabled before the provider registers, so
  ** the registration itself triggers the rundown/load snapshot. */
  if (!session_start(&s, L"first", 0x3, log, &err)) {
    need_privilege(err);
    fail(sc, "session_start: %s", winerr(err));
    return;
  }
  L = vm_new();
  mark = 0;
  vm_prewarm(L, "@etw-a.lua", 3, sc);
  ntraces = vm_traces(L, traces, ETW_MAX_TRACES);
  if (ntraces < 1) fail(sc, "no trace committed by the VM");
  if (log_wait(log, mark, pred_id, (const void *)(uintptr_t)11,
	       ETW_WAIT_MS) < 0)
    fail(sc, "no ScriptContextLoad");
  for (i = 0; i < ntraces; i++) {
    idx = log_wait(log, mark, pred_load_addr, &traces[i], ETW_WAIT_MS);
    if (idx < 0) {
      fail(sc, "no MethodLoad for trace %u at %p+%llu", traces[i].traceno,
	   traces[i].mcode, (unsigned long long)traces[i].size);
      continue;
    }
    if (!log_msg(log, idx, &m)) continue;
    if (m.line != 2)
      fail(sc, "trace %u: Line=%u, want 2", traces[i].traceno, m.line);
    if (wcsstr(m.name_head, L"LuaJIT::TRACE_") != m.name_head ||
	wcsstr(m.name_tail, L"etw-a.lua:2") == NULL)
      fail(sc, "trace %u: MethodName %ls...%ls", traces[i].traceno,
	   m.name_head, m.name_tail);
    /* The SourceLoad with the same SourceID names the same chunk. */
    {
      int j, n = log_count(log), found = 0;
      EventMsg sm;
      for (j = mark; j < n; j++) {
	if (!log_msg(log, j, &sm)) continue;
	if (sm.id == 41 && sm.source_id == m.source_id) {
	  found = 1;
	  if (wcscmp(sm.url_head, L"etw-a.lua") != 0)
	    fail(sc, "SourceLoad url %ls", sm.url_head);
	}
      }
      if (!found)
	fail(sc, "no SourceLoad for SourceID %llu",
	     (unsigned long long)m.source_id);
    }
  }
  if (log_bad(log)) fail(sc, "malformed/overflowed/foreign event log");
  lua_close(L);
  {
    const char *why = NULL;
    session_stop(&s, &why);
    if (why) fail(sc, "%s", why);
  }
  if (!failures) ok(sc);
}

/* 1b. Provider already registered, symbol cache still empty: the enable
** snapshot must still emit the rundown brackets and the context record, and
** must not invent method records.
*/
static void scenario_empty_cache(void)
{
  const char *sc = "empty-cache";
  EventLog *log = log_new();
  Session s;
  DWORD err = 0;
  lua_State *L = vm_new();		/* Registers the provider, no traces. */
  if (!session_start(&s, L"empty", 0x3, log, &err)) {
    need_privilege(err);
    fail(sc, "session_start: %s", winerr(err));
    lua_close(L);
    return;
  }
  if (log_wait(log, 0, pred_id, (const void *)(uintptr_t)1, ETW_WAIT_MS) < 0)
    fail(sc, "no DCStartInit for an empty cache");
  if (log_wait(log, 0, pred_id, (const void *)(uintptr_t)7, ETW_WAIT_MS) < 0)
    fail(sc, "no ScriptContextDCStart for an empty cache");
  if (log_wait(log, 0, pred_id, (const void *)(uintptr_t)11, ETW_WAIT_MS) < 0)
    fail(sc, "no ScriptContextLoad for an empty cache");
  if (log_wait(log, 0, pred_id, (const void *)(uintptr_t)2, ETW_WAIT_MS) < 0)
    fail(sc, "no DCStartComplete for an empty cache");
  log_settle();
  if (log_count_if(log, 0, pred_id, (const void *)(uintptr_t)5) != 0)
    fail(sc, "method rundown for an empty symbol cache");
  if (log_count_if(log, 0, pred_id, (const void *)(uintptr_t)9) != 0)
    fail(sc, "method load for an empty symbol cache");
  if (log_bad(log)) fail(sc, "malformed/overflowed/foreign event log");
  lua_close(L);
  {
    const char *why = NULL;
    session_stop(&s, &why);
    if (why) fail(sc, "%s", why);
  }
  if (!failures) ok(sc);
}

/* 2. Trace before collector, VM blocked idle. */
static void scenario_trace_first_idle(void)
{
  const char *sc = "trace-first-idle";
  EventLog *log = log_new();
  Session s;
  DWORD err = 0;
  lua_State *L;
  TraceRef traces[ETW_MAX_TRACES];
  int ntraces, mark;
  L = vm_new();
  vm_prewarm(L, "@etw-a.lua", 3, sc);
  ntraces = vm_traces(L, traces, ETW_MAX_TRACES);
  if (ntraces < 1) fail(sc, "no trace committed");
  mark = 0;
  if (!session_start(&s, L"idle", 0x3, log, &err)) {
    need_privilege(err);
    fail(sc, "session_start: %s", winerr(err));
    lua_close(L);
    return;
  }
  /* Nothing runs Lua below: the rundown must come from the cache alone. */
  wait_rundown(log, mark, sc, traces, ntraces);
  if (log_wait(log, mark, pred_id, (const void *)(uintptr_t)9,
	       ETW_WAIT_MS) < 0)
    fail(sc, "no MethodLoad replay for the idle VM");
  if (log_bad(log)) fail(sc, "malformed/overflowed/foreign event log");
  lua_close(L);
  {
    const char *why = NULL;
    session_stop(&s, &why);
    if (why) fail(sc, "%s", why);
  }
  if (!failures) ok(sc);
}

typedef struct Runner {
  HANDLE ready;
  HANDLE stop;
  int checksum_bad;
  lua_State *L;
} Runner;

static DWORD WINAPI runner_proc(LPVOID param)
{
  Runner *r = (Runner *)param;
  double v = 0;
  int first = 1;
  r->L = vm_new();
  vm_load_work(r->L, "@etw-a.lua");
  for (;;) {
    /* lua_pcall consumes the function, so push a fresh copy every round. */
    lua_pushvalue(r->L, -1);
    if (vm_call_work(r->L, &v) != 0) r->checksum_bad = 1;
    if (first) { first = 0; SetEvent(r->ready); }
    if (WaitForSingleObject(r->stop, 0) == WAIT_OBJECT_0) break;
  }
  lua_close(r->L);
  r->L = NULL;
  return 0;
}

/* 3. Trace before collector while the VM keeps executing prewarmed code. */
static void scenario_trace_first_running(void)
{
  const char *sc = "trace-first-running";
  EventLog *log = log_new();
  Session s;
  DWORD err = 0;
  Runner r;
  TraceRef traces[ETW_MAX_TRACES];
  int ntraces, mark;
  HANDLE th;
  memset(&r, 0, sizeof(r));
  r.ready = CreateEventW(NULL, FALSE, FALSE, NULL);
  r.stop = CreateEventW(NULL, FALSE, FALSE, NULL);
  th = CreateThread(NULL, 0, runner_proc, &r, 0, NULL);
  if (WaitForSingleObject(r.ready, 30000) != WAIT_OBJECT_0) {
    fail(sc, "VM thread never became ready");
    return;
  }
  ntraces = vm_traces(r.L, traces, ETW_MAX_TRACES);
  if (ntraces < 1) fail(sc, "no trace committed");
  mark = 0;
  if (!session_start(&s, L"running", 0x3, log, &err)) {
    need_privilege(err);
    fail(sc, "session_start: %s", winerr(err));
    SetEvent(r.stop);
    WaitForSingleObject(th, 10000);
    return;
  }
  wait_rundown(log, mark, sc, traces, ntraces);
  if (r.checksum_bad) fail(sc, "checksum changed while running");
  if (log_bad(log)) fail(sc, "malformed/overflowed/foreign event log");
  SetEvent(r.stop);
  WaitForSingleObject(th, 30000);
  CloseHandle(th);
  {
    const char *why = NULL;
    session_stop(&s, &why);
    if (why) fail(sc, "%s", why);
  }
  if (!failures) ok(sc);
}

/* 4. Independent keyword subscriptions (0x1 / 0x2 / 0x4). */
static void scenario_keyword_matrix(void)
{
  const char *sc = "keyword-matrix";
  lua_State *L = vm_new();
  TraceRef traces[ETW_MAX_TRACES];
  int ntraces, i;
  struct Case {
    const WCHAR *tag;
    UINT64 kw;
    int replay, start, stop;
  } cases[3] = {
    { L"kw1", 0x1, 1, 0, 0 },
    { L"kw2", 0x2, 0, 1, 0 },
    { L"kw4", 0x4, 0, 0, 1 },
  };
  vm_prewarm(L, "@etw-a.lua", 3, sc);
  ntraces = vm_traces(L, traces, ETW_MAX_TRACES);
  if (ntraces < 1) fail(sc, "no trace committed");
  for (i = 0; i < 3; i++) {
    EventLog *log = log_new();
    Session s;
    DWORD err = 0;
    int mark;
    if (!session_start(&s, cases[i].tag, cases[i].kw, log, &err)) {
      need_privilege(err);
      fail(sc, "session_start %ls: %s", cases[i].tag, winerr(err));
      continue;
    }
    mark = 0;
    if (cases[i].start) {
      wait_rundown(log, mark, sc, traces, ntraces);
      log_settle();
      if (log_count_if(log, mark, pred_id, (const void *)(uintptr_t)9) != 0)
	fail(sc, "%ls: MethodLoad despite start-rundown-only keyword",
	     cases[i].tag);
    } else if (cases[i].replay) {
      if (log_wait(log, mark, pred_load_addr, &traces[0], ETW_WAIT_MS) < 0)
	fail(sc, "%ls: no runtime replay", cases[i].tag);
      log_settle();
      if (log_count_if(log, mark, pred_id, (const void *)(uintptr_t)5) != 0)
	fail(sc, "%ls: MethodDCStart despite runtime-only keyword",
	     cases[i].tag);
    } else {
      if (log_wait(log, mark, pred_id, (const void *)(uintptr_t)3,
		   ETW_WAIT_MS) < 0)
	fail(sc, "%ls: no DCStopInit", cases[i].tag);
      if (log_wait(log, mark, pred_id, (const void *)(uintptr_t)40,
		   ETW_WAIT_MS) < 0)
	fail(sc, "%ls: no SourceDCStop", cases[i].tag);
      if (log_wait(log, mark, pred_id, (const void *)(uintptr_t)6,
		   ETW_WAIT_MS) < 0)
	fail(sc, "%ls: no MethodDCStop", cases[i].tag);
      if (log_wait(log, mark, pred_id, (const void *)(uintptr_t)4,
		   ETW_WAIT_MS) < 0)
	fail(sc, "%ls: no DCStopComplete", cases[i].tag);
      log_settle();
      if (log_count_if(log, mark, pred_id, (const void *)(uintptr_t)9) != 0)
	fail(sc, "%ls: MethodLoad despite end-rundown-only keyword",
	     cases[i].tag);
      if (log_count_if(log, mark, pred_id, (const void *)(uintptr_t)5) != 0)
	fail(sc, "%ls: MethodDCStart despite end-rundown-only keyword",
	     cases[i].tag);
    }
    if (log_bad(log))
      fail(sc, "%ls: malformed/overflowed/foreign log", cases[i].tag);
    {
      const char *why = NULL;
      session_stop(&s, &why);
      if (why) fail(sc, "%ls: %s", cases[i].tag, why);
    }
  }
  lua_close(L);
  if (!failures) ok(sc);
}

/* 5. Two sessions (0x1 and 0x2), second joining late; closing one must not
** stop the other from seeing new loads and unloads.
*/
static void scenario_two_sessions(void)
{
  const char *sc = "two-sessions";
  EventLog *log_a = log_new();
  EventLog *log_b = log_new();
  Session sa, sb;
  DWORD err = 0;
  lua_State *L = vm_new();
  TraceRef traces[ETW_MAX_TRACES];
  int ntraces, mark_a, mark_b, idx;
  EventMsg m;
  if (!session_start(&sa, L"A", 0x1, log_a, &err)) {
    need_privilege(err);
    fail(sc, "session_start A: %s", winerr(err));
    return;
  }
  mark_a = 0;
  vm_prewarm(L, "@etw-a.lua", 3, sc);
  ntraces = vm_traces(L, traces, ETW_MAX_TRACES);
  if (ntraces < 1) fail(sc, "no trace committed");
  else if (log_wait(log_a, mark_a, pred_load_addr, &traces[0],
		    ETW_WAIT_MS) < 0)
    fail(sc, "session A: no runtime MethodLoad");
  /* Second session joins late with the start-rundown keyword. */
  if (!session_start(&sb, L"B", 0x2, log_b, &err)) {
    fail(sc, "session_start B: %s", winerr(err));
  } else {
    mark_b = 0;
    if (!wait_rundown(log_b, mark_b, sc, traces, ntraces))
      fail(sc, "session B: incomplete rundown of the existing trace");
    /* Close B; A must still see a new trace and its unload. */
    {
      const char *why = NULL;
      session_stop(&sb, &why);
      if (why) fail(sc, "session B stop: %s", why);
    }
    mark_a = log_count(log_a);
    vm_prewarm(L, "@etw-b.lua", 3, sc);
    {
      TraceRef now[ETW_MAX_TRACES];
      int nnow = vm_traces(L, now, ETW_MAX_TRACES), i, j, found = -1;
      for (i = 0; i < nnow && found < 0; i++) {
	int known = 0;
	for (j = 0; j < ntraces; j++)
	  if (now[i].mcode == traces[j].mcode && now[i].size == traces[j].size)
	    known = 1;
	if (!known) found = i;
      }
      if (found < 0) {
	fail(sc, "no new trace after @etw-b.lua");
      } else if ((idx = log_wait(log_a, mark_a, pred_load_addr, &now[found],
				 ETW_WAIT_MS)) < 0) {
	fail(sc, "session A: no MethodLoad for the new trace");
      } else if (log_msg(log_a, idx, &m)) {
	if (wcsstr(m.name_tail, L"etw-b.lua:2") == NULL)
	  fail(sc, "session A: new trace name %ls...%ls", m.name_head,
	       m.name_tail);
	mark_a = log_count(log_a);
	vm_flush(L, -1);
	vm_collectgarbage(L, 2);
	if (log_wait(log_a, mark_a, pred_unload_id,
		     (const void *)(uintptr_t)m.method_id,
		     ETW_WAIT_MS) < 0)
	  fail(sc, "session A: no MethodUnload for the new trace");
      }
    }
  }
  if (log_bad(log_a) || log_bad(log_b))
    fail(sc, "malformed/overflowed event log");
  lua_close(L);
  {
    const char *why = NULL;
    session_stop(&sa, &why);
    if (why) fail(sc, "%s", why);
  }
  if (!failures) ok(sc);
}

/* 6. Repeated CAPTURE_STATE: stable identities, released records stay gone. */
static void scenario_capture_repeat(void)
{
  const char *sc = "capture-repeat";
  EventLog *log = log_new();
  Session s;
  DWORD err = 0;
  lua_State *L = vm_new();
  TraceRef traces[ETW_MAX_TRACES];
  int ntraces, mark, idx, i;
  EventMsg m0[ETW_MAX_TRACES], m1;
  memset(m0, 0, sizeof(m0));
  vm_prewarm(L, "@etw-a.lua", 3, sc);
  ntraces = vm_traces(L, traces, ETW_MAX_TRACES);
  if (ntraces < 1) fail(sc, "no trace committed");
  if (!session_start(&s, L"cap", 0x3, log, &err)) {
    need_privilege(err);
    fail(sc, "session_start: %s", winerr(err));
    lua_close(L);
    return;
  }
  mark = log_count(log);
  if (!session_capture(&s, 0x3, &err)) fail(sc, "capture 1: %s", winerr(err));
  for (i = 0; i < ntraces; i++) {
    idx = log_wait(log, mark, pred_load_addr, &traces[i], ETW_WAIT_MS);
    if (idx < 0 || !log_msg(log, idx, &m0[i]))
      fail(sc, "capture 1: missing trace %u", traces[i].traceno);
  }
  mark = log_count(log);
  if (!session_capture(&s, 0x3, &err)) fail(sc, "capture 2: %s", winerr(err));
  for (i = 0; i < ntraces; i++) {
    idx = log_wait(log, mark, pred_load_addr, &traces[i], ETW_WAIT_MS);
    if (idx < 0 || !log_msg(log, idx, &m1)) {
      fail(sc, "capture 2: missing trace %u", traces[i].traceno);
      continue;
    }
    if (m0[i].method_id != 0 &&
	(m0[i].method_id != m1.method_id || m0[i].source_id != m1.source_id))
      fail(sc, "trace %u: identity changed across snapshots (%u -> %u)",
	   traces[i].traceno, m0[i].method_id, m1.method_id);
  }
  /* Release one trace, then capture again: it must not come back. */
  mark = log_count(log);
  vm_flush(L, (int)traces[0].traceno);
  vm_collectgarbage(L, 2);
  if (log_wait(log, mark, pred_unload_id,
	       (const void *)(uintptr_t)m0[0].method_id, ETW_WAIT_MS) < 0)
    fail(sc, "released trace was not unloaded");
  mark = log_count(log);
  if (!session_capture(&s, 0x3, &err)) fail(sc, "capture 3: %s", winerr(err));
  log_settle();
  if (log_count_if(log, mark, pred_unload_id,
		   (const void *)(uintptr_t)m0[0].method_id) != 0)
    fail(sc, "unloaded trace resurrected in a later snapshot");
  if (log_bad(log)) fail(sc, "malformed/overflowed/foreign event log");
  lua_close(L);
  {
    const char *why = NULL;
    session_stop(&s, &why);
    if (why) fail(sc, "%s", why);
  }
  if (!failures) ok(sc);
}

/* 7. Full flush + re-prewarm: a new lifetime gets a new identity. */
static void scenario_flush_reprewarm(void)
{
  const char *sc = "flush-reprewarm";
  EventLog *log = log_new();
  Session s;
  DWORD err = 0;
  lua_State *L = vm_new();
  TraceRef traces[ETW_MAX_TRACES];
  int ntraces, mark, idx;
  EventMsg old;
  memset(&old, 0, sizeof(old));
  vm_prewarm(L, "@etw-a.lua", 3, sc);
  ntraces = vm_traces(L, traces, ETW_MAX_TRACES);
  if (ntraces < 1) fail(sc, "no trace committed");
  if (!session_start(&s, L"flush", 0x3, log, &err)) {
    need_privilege(err);
    fail(sc, "session_start: %s", winerr(err));
    lua_close(L);
    return;
  }
  mark = 0;
  idx = log_wait(log, mark, pred_load_addr, &traces[0], ETW_WAIT_MS);
  if (idx < 0 || !log_msg(log, idx, &old)) {
    fail(sc, "no initial MethodLoad");
  } else {
    mark = log_count(log);
    vm_flush(L, -1);
    if (log_wait(log, mark, pred_unload_id,
		 (const void *)(uintptr_t)old.method_id, ETW_WAIT_MS) < 0)
      fail(sc, "old trace not unloaded by jit.flush()");
    mark = log_count(log);
    vm_prewarm(L, "@etw-a.lua", 3, sc);
    {
      TraceRef now[ETW_MAX_TRACES];
      int nnow = vm_traces(L, now, ETW_MAX_TRACES), i, found = 0;
      EventMsg neu;
      memset(&neu, 0, sizeof(neu));
      for (i = 0; i < nnow && !found; i++) {
	idx = log_wait(log, mark, pred_load_addr, &now[i], ETW_WAIT_MS);
	if (idx >= 0 && log_msg(log, idx, &neu)) found = 1;
      }
      if (!found)
	fail(sc, "no MethodLoad after re-prewarm");
      else if (neu.method_id == old.method_id)
	fail(sc, "recycled MethodID %u for a new lifetime", neu.method_id);
    }
  }
  if (log_bad(log)) fail(sc, "malformed/overflowed/foreign event log");
  lua_close(L);
  {
    const char *why = NULL;
    session_stop(&s, &why);
    if (why) fail(sc, "%s", why);
  }
  if (!failures) ok(sc);
}

/* 8. Single trace flush: no unload at unlink, exactly one at GC. */
static void scenario_single_flush_gc(void)
{
  const char *sc = "single-flush-gc";
  EventLog *log = log_new();
  Session s;
  DWORD err = 0;
  lua_State *L = vm_new();
  TraceRef root;
  int mark, idx;
  EventMsg m;
  memset(&m, 0, sizeof(m));
  vm_prewarm(L, "@etw-a.lua", 3, sc);
  if (!vm_first_root(L, &root)) fail(sc, "no root trace committed");
  if (!session_start(&s, L"single", 0x3, log, &err)) {
    need_privilege(err);
    fail(sc, "session_start: %s", winerr(err));
    lua_close(L);
    return;
  }
  mark = 0;
  idx = log_wait(log, mark, pred_load_addr, &root, ETW_WAIT_MS);
  if (idx < 0 || !log_msg(log, idx, &m)) {
    fail(sc, "no initial MethodLoad");
  } else {
    mark = log_count(log);
    vm_flush(L, (int)root.traceno);	/* Unlink only. */
    log_settle();
    if (log_count_if(log, mark, pred_unload_id,
		     (const void *)(uintptr_t)m.method_id) != 0)
      fail(sc, "unload reported before the trace was released");
    vm_collectgarbage(L, 2);
    if (log_wait(log, mark, pred_unload_id,
		 (const void *)(uintptr_t)m.method_id, ETW_WAIT_MS) < 0)
      fail(sc, "no unload after GC");
    mark = log_count(log);
    vm_collectgarbage(L, 2);
    log_settle();
    if (log_count_if(log, mark, pred_unload_id,
		     (const void *)(uintptr_t)m.method_id) != 0)
      fail(sc, "trace unloaded twice");
  }
  if (log_bad(log)) fail(sc, "malformed/overflowed/foreign event log");
  lua_close(L);
  {
    const char *why = NULL;
    session_stop(&s, &why);
    if (why) fail(sc, "%s", why);
  }
  if (!failures) ok(sc);
}

/* 9. Two VMs and a coroutine: one context each, none for coroutines. */
static void scenario_two_vms_coroutine(void)
{
  const char *sc = "two-vms-coroutine";
  EventLog *log = log_new();
  Session s;
  DWORD err = 0;
  lua_State *La = vm_new();
  lua_State *Lb = vm_new();
  void *ctxa, *ctxb;
  TraceRef ta[ETW_MAX_TRACES], tb[ETW_MAX_TRACES];
  int na, nb, mark, idx;
  EventMsg m;
  vm_prewarm(La, "@etw-a.lua", 3, sc);
  vm_prewarm(Lb, "@etw-b.lua", 3, sc);
  na = vm_traces(La, ta, ETW_MAX_TRACES);
  nb = vm_traces(Lb, tb, ETW_MAX_TRACES);
  if (na < 1 || nb < 1) fail(sc, "missing traces (%d/%d)", na, nb);
  ctxa = vm_ctx(La);
  ctxb = vm_ctx(Lb);
  if (ctxa == ctxb) fail(sc, "both VMs share one ScriptContextID");
  mark = 0;
  if (!session_start(&s, L"twovm", 0x3, log, &err)) {
    need_privilege(err);
    fail(sc, "session_start: %s", winerr(err));
    lua_close(Lb); lua_close(La);
    return;
  }
  if (log_wait(log, mark, pred_ctx_load, ctxa, ETW_WAIT_MS) < 0)
    fail(sc, "no rundown for VM A");
  if (log_wait(log, mark, pred_ctx_load, ctxb, ETW_WAIT_MS) < 0)
    fail(sc, "no rundown for VM B");
  /* A coroutine in VM A must not add a context. */
  {
    static const char co[] =
      "local s=0\nfor i=1,100000 do s=s+i end\nreturn s\n";
    lua_getglobal(La, "coroutine");		/* 1: module */
    lua_getfield(La, -1, "create");		/* 2: create */
    luaL_loadbuffer(La, co, strlen(co), "@etw-co.lua");	/* 3: fn */
    if (lua_pcall(La, 1, 1, 0) != 0) {
      fail(sc, "coroutine.create: %s", lua_tostring(La, -1));
    } else {
      lua_getfield(La, -2, "resume");		/* 3: resume (module at -2) */
      lua_pushvalue(La, -2);			/* 4: thread */
      if (lua_pcall(La, 1, 2, 0) != 0)
	fail(sc, "coroutine resume: %s", lua_tostring(La, -1));
      else if (!lua_toboolean(La, -2))
	fail(sc, "coroutine resume returned false");
      else if (lua_tonumber(La, -1) != WORK_CHECKSUM)
	fail(sc, "coroutine checksum %.0f", lua_tonumber(La, -1));
    }
    lua_settop(La, 0);
  }
  log_settle();
  {
    int i, n = log_count(log), ctxs = 0;
    for (i = 0; i < n; i++) {
      if (!log_msg(log, i, &m) || m.id != 11) continue;
      if (m.ctx == ctxa || m.ctx == ctxb) ctxs++;
      else fail(sc, "unexpected third ScriptContextID %p", m.ctx);
    }
    if (ctxs != 2) fail(sc, "expected 2 context loads, saw %d", ctxs);
  }
  /* Close VM B: its context unloads, VM A keeps working. */
  mark = log_count(log);
  lua_close(Lb);
  idx = log_wait(log, mark, pred_ctx_unload, ctxb, ETW_WAIT_MS);
  if (idx < 0) {
    fail(sc, "no ScriptContextUnload for VM B");
    log_dump_ctxs(log, 0, sc);
  }
  mark = log_count(log);
  if (!session_capture(&s, 0x3, &err)) fail(sc, "capture: %s", winerr(err));
  if (log_wait(log, mark, pred_ctx_load, ctxa, ETW_WAIT_MS) < 0)
    fail(sc, "VM A vanished after VM B closed");
  log_settle();
  if (log_count_if(log, mark, pred_ctx_load, ctxb) != 0)
    fail(sc, "closed VM B still reported");
  if (log_bad(log)) fail(sc, "malformed/overflowed/foreign event log");
  lua_close(La);
  {
    const char *why = NULL;
    session_stop(&s, &why);
    if (why) fail(sc, "%s", why);
  }
  if (!failures) ok(sc);
}

/* 10. VM create/close racing enable/capture; the provider must survive. */
typedef struct Churn {
  HANDLE stop;
  HANDLE ready;
  Session *s;
  int errors;
} Churn;

static DWORD WINAPI churn_proc(LPVOID param)
{
  Churn *c = (Churn *)param;
  DWORD err;
  SetEvent(c->ready);
  while (WaitForSingleObject(c->stop, 0) != WAIT_OBJECT_0) {
    if (!session_capture(c->s, 0x3, &err)) c->errors = 1;
    if (!session_enable(c->s, 0x3, &err)) c->errors = 1;
    Sleep(20);
  }
  return 0;
}

static void scenario_concurrent_close(void)
{
  const char *sc = "concurrent-close";
  EventLog *log = log_new();
  Session s;
  Churn c;
  DWORD err = 0;
  HANDLE th;
  int i;
  if (!session_start(&s, L"churn", 0x3, log, &err)) {
    need_privilege(err);
    fail(sc, "session_start: %s", winerr(err));
    return;
  }
  memset(&c, 0, sizeof(c));
  c.s = &s;
  c.stop = CreateEventW(NULL, FALSE, FALSE, NULL);
  c.ready = CreateEventW(NULL, FALSE, FALSE, NULL);
  th = CreateThread(NULL, 0, churn_proc, &c, 0, NULL);
  WaitForSingleObject(c.ready, 10000);
  for (i = 0; i < 40; i++) {
    lua_State *L = vm_new();
    vm_prewarm(L, "@etw-a.lua", 2, sc);
    lua_close(L);
  }
  SetEvent(c.stop);
  WaitForSingleObject(th, 30000);
  CloseHandle(th);
  if (c.errors) fail(sc, "enable/capture failed during the churn");
  /* The churn snapshot loop overruns this fixed-size log on purpose; only
  ** protocol damage is a failure here. */
  if (log_malformed(log) || log_foreign(log))
    fail(sc, "malformed/foreign events during the churn");
  log_reset(log);
  /* A fresh VM still registers. */
  {
    lua_State *L = vm_new();
    int mark = log_count(log);
    vm_prewarm(L, "@etw-a.lua", 2, sc);
    if (log_wait(log, mark, pred_ctx_load, vm_ctx(L), ETW_WAIT_MS) < 0) {
      fail(sc, "no ScriptContextLoad after the churn");
      log_dump_ctxs(log, mark, sc);
    }
    lua_close(L);
  }
  if (log_malformed(log) || log_foreign(log))
    fail(sc, "malformed/foreign events after the churn");
  {
    const char *why = NULL;
    session_stop(&s, &why);
    if (why) fail(sc, "%s", why);
  }
  if (!failures) ok(sc);
}

/* 11. Non-ASCII, overlong and line-info-free chunk names. */
static void scenario_names_and_lines(void)
{
  const char *sc = "names-and-lines";
  EventLog *log = log_new();
  Session s;
  DWORD err = 0;
  lua_State *L = vm_new();
  char longname[4300];
  size_t i;
  if (!session_start(&s, L"names", 0x3, log, &err)) {
    need_privilege(err);
    fail(sc, "session_start: %s", winerr(err));
    lua_close(L);
    return;
  }
  /* Non-ASCII chunk name: "etw-\u00e4\u20ac.lua" (10 code units). */
  {
    int mark = log_count(log), idx;
    EventMsg m;
    vm_prewarm(L, "@etw-\xc3\xa4\xe2\x82\xac.lua", 2, sc);
    idx = log_wait(log, mark, pred_id, (const void *)(uintptr_t)41,
		   ETW_WAIT_MS);
    if (idx < 0 || !log_msg(log, idx, &m)) {
      fail(sc, "no SourceLoad for the non-ASCII name");
    } else if (wcscmp(m.url_head, L"etw-\u00e4\u20ac.lua") != 0 ||
	       m.url_chars != 10) {
      fail(sc, "non-ASCII url %ls (%d units)", m.url_head, m.url_chars);
    }
  }
  /* Method name of the same trace carries the UTF-16 suffix. */
  {
    int mark = log_count(log), idx;
    EventMsg m;
    vm_prewarm(L, "@etw-\xc3\xa4\xe2\x82\xac.lua", 2, sc);
    idx = log_wait(log, mark, pred_id, (const void *)(uintptr_t)9,
		   ETW_WAIT_MS);
    if (idx < 0 || !log_msg(log, idx, &m)) {
      fail(sc, "no MethodLoad for the non-ASCII name");
    } else if (wcsstr(m.name_tail, L"\u00e4\u20ac.lua:2") == NULL) {
      fail(sc, "non-ASCII method name %ls...%ls", m.name_head, m.name_tail);
    }
  }
  /* A bare path (DST's engine passes "scripts/foo.lua" without '@') is kept;
  ** a source blob used as its own chunk name collapses to "(string)".
  */
  {
    static const char bare[] =
      "local s=0\nfor i=1,100000 do s=s+i end\nreturn s\n";
    double v = 0;
    int mark = log_count(log), idx;
    EventMsg m;
    luaL_loadbuffer(L, bare, strlen(bare), "scripts/etw-bare-path.lua");
    if (vm_call_work(L, &v) != 0) fail(sc, "bare-path checksum");
    idx = log_wait(log, mark, pred_url_is, L"scripts/etw-bare-path.lua",
		   ETW_WAIT_MS);
    if (idx < 0) {
      fail(sc, "bare path not kept as a symbol source");
      log_dump_ctxs(log, mark, sc);
    } else if (log_msg(log, idx, &m) &&
	       wcscmp(m.url_head, L"scripts/etw-bare-path.lua") != 0) {
      fail(sc, "bare path url %ls", m.url_head);
    }
  }
  {
    static const char blob[] =
      "local s=0\nfor i=1,100000 do s=s+i end\nreturn s\n";
    double v = 0;
    int mark = log_count(log), idx;
    EventMsg m;
    /* The source itself as the chunk name, as loadstring without a name does. */
    luaL_loadbuffer(L, blob, strlen(blob), blob);
    if (vm_call_work(L, &v) != 0) fail(sc, "blob checksum");
    idx = log_wait(log, mark, pred_url_is, L"(string)", ETW_WAIT_MS);
    if (idx < 0)
      fail(sc, "source blob did not collapse to (string)");
  }
  /* Overlong chunk name: 4094 ASCII units then a surrogate pair. */
  {
    int mark = log_count(log), idx;
    EventMsg m;
    size_t n = 0;
    longname[n++] = '@';
    for (i = 0; i < 4094; i++) longname[n++] = 'x';
    longname[n++] = (char)0xf0; longname[n++] = (char)0x9f;
    longname[n++] = (char)0x98; longname[n++] = (char)0x80;
    memcpy(longname + n, "tail", 4); n += 4;
    longname[n] = 0;
    vm_prewarm(L, longname, 2, sc);
    idx = log_wait(log, mark, pred_source_len, (const void *)(uintptr_t)1000,
		   ETW_WAIT_MS);
    if (idx < 0 || !log_msg(log, idx, &m)) {
      fail(sc, "no SourceLoad for the overlong name");
    } else {
      size_t tlen = wcslen(m.url_tail);
      if (m.url_chars > 4095)
	fail(sc, "source url is %d units, over the documented cap",
	     m.url_chars);
      if (m.url_chars == 0 || tlen == 0 || m.url_tail[tlen-1] != 0x2026)
	fail(sc, "overlong name not marked as truncated (%d units)",
	     m.url_chars);
      /* The truncation must never leave a lone high surrogate behind. */
      else if (tlen >= 2 && m.url_tail[tlen-2] >= 0xD800 &&
	       m.url_tail[tlen-2] <= 0xDBFF)
	fail(sc, "truncation split a surrogate pair");
    }
  }
  /* Stripped chunk: no line info, so Line must be 0. */
  {
    static const char strip_src[] =
      "local f = function() local s=0\n"
      "for i=1,100000 do s=s+i end\nreturn s end\n"
      "local g = loadstring(string.dump(f, true), '@etw-stripped.lua')\n"
      "return g()\n";
    double v = 0;
    int mark = log_count(log), idx;
    EventMsg m;
    luaL_loadbuffer(L, strip_src, strlen(strip_src), "@etw-stripsrc.lua");
    if (vm_call_work(L, &v) != 0) fail(sc, "stripped chunk checksum");
    idx = log_wait(log, mark, pred_name_has, L"etw-stripped.lua:",
		   ETW_WAIT_MS);
    if (idx < 0 || !log_msg(log, idx, &m)) {
      fail(sc, "no MethodLoad for the stripped chunk");
    } else if (m.line != 0) {
      fail(sc, "stripped chunk reported Line=%u, want 0", m.line);
    }
  }
  if (log_bad(log)) fail(sc, "malformed/overflowed/foreign event log");
  lua_close(L);
  {
    const char *why = NULL;
    session_stop(&s, &why);
    if (why) fail(sc, "%s", why);
  }
  if (!failures) ok(sc);
}

/* -- hold-for-profile mode ----------------------------------------------- */

static int hold_for_profile(void)
{
  lua_State *L = vm_new();
  double v = 0;
  ULONG64 start;
  /* Prewarm first: the ETW_PROFILE_READY signal must mean "the traces a
  ** profiler attaches to already exist", which is what the rundown replay
  ** (and therefore mid-session symbolization) depends on. */
  vm_load_work(L, "@etw-a.lua");
  lua_pushvalue(L, -1);
  if (vm_call_work(L, &v) != 0) return 1;
  printf("ETW_PROFILE_READY pid=%lu traces=%d\n",
	 (unsigned long)GetCurrentProcessId(),
	 (int)vm_traces_count(L));
  fflush(stdout);
  (void)getchar();			/* Wait for a newline. */
  start = GetTickCount64();
  while (GetTickCount64() - start < 10000) {
    int i;
    for (i = 0; i < 20; i++) {
      lua_pushvalue(L, -1);
      if (vm_call_work(L, &v) != 0) return 1;
    }
  }
  printf("checksum=%.0f\n", v);
  fflush(stdout);
  lua_close(L);
  return v == WORK_CHECKSUM ? 0 : 1;
}

typedef struct Scenario {
  const char *name;
  void (*fn)(void);
} Scenario;

static const Scenario scenarios[] = {
  { "collector-first", scenario_collector_first },
  { "empty-cache", scenario_empty_cache },
  { "trace-first-idle", scenario_trace_first_idle },
  { "trace-first-running", scenario_trace_first_running },
  { "keyword-matrix", scenario_keyword_matrix },
  { "two-sessions", scenario_two_sessions },
  { "capture-repeat", scenario_capture_repeat },
  { "flush-reprewarm", scenario_flush_reprewarm },
  { "single-flush-gc", scenario_single_flush_gc },
  { "two-vms-coroutine", scenario_two_vms_coroutine },
  { "concurrent-close", scenario_concurrent_close },
  { "names-and-lines", scenario_names_and_lines },
};

int main(int argc, char **argv)
{
  const char *only = NULL;
  size_t i;
  atexit(cleanup_sessions);
  for (i = 1; i < (size_t)argc; i++) {
    if (strcmp(argv[i], "--hold-for-profile") == 0)
      return hold_for_profile();
    if (strncmp(argv[i], "--only=", 7) == 0)
      only = argv[i] + 7;
  }
  for (i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
    if (only != NULL && strcmp(only, scenarios[i].name) != 0) continue;
    scenarios[i].fn();
  }
  if (only != NULL && failures == 0)
    printf("scenario %s passed\n", only);
  if (failures) {
    fprintf(stderr, "%d ETW symbol check(s) failed\n", failures);
    return 1;
  }
  printf("all ETW symbol scenarios passed\n");
  return 0;
}

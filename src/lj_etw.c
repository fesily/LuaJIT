/*
** Windows ETW JIT symbol client.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_etw_c
#define LUA_CORE

#include "lj_obj.h"

#if LJ_HASETW

/* Windows Vista is the baseline for the ETW provider API (EventRegister).
** An explicitly lower target is a build error instead of a silent partial
** implementation (the default): define LUAJIT_ENABLE_ETW to compile it in.
*/
#if defined(_WIN32_WINNT) && _WIN32_WINNT < 0x0600
#error "lj_etw.c requires _WIN32_WINNT >= 0x0600 (Windows Vista). Raise the target or drop LUAJIT_ENABLE_ETW."
#endif
#if !defined(_WIN32_WINNT)
#define _WIN32_WINNT 0x0600
#endif
#if !defined(WINVER)
#define WINVER 0x0600
#endif

#include <windows.h>
#include <evntprov.h>
#include <evntrace.h>

#if defined(_MSC_VER)
/* msvcbuild.bat compiles lj_*.c without a library list. */
#pragma comment(lib, "advapi32.lib")
#endif

#include "lj_gc.h"
#include "lj_str.h"
#include "lj_debug.h"
#include "lj_dispatch.h"
#include "lj_jit.h"
#include "lj_etw.h"

/* -- ETW JIT symbol protocol ---------------------------------------------
**
** This emits the Microsoft-JScript ("Chakra") JIT symbol protocol -- the same
** wire protocol Windows and V8 use to make dynamically generated code
** resolvable in WPA/PerfView CPU samples. The provider GUID and the event
** descriptors below come from the manifest already registered by Windows
** (resourceFileName %SystemRoot%\system32\Chakra.dll), so nothing is
** installed and no manifest or PDB is produced here.
**
** The publisher is off by default at runtime and is switched globally from
** Lua with jit.profile.etw(true|false) (query with etw()). Enabling also
** flushes the calling VM's traces so every live trace has a record; disabling
** reports the unloads at once and stops publishing. Every VM keeps one small
** context while the switch is off, so any of them can turn it on later
** without being recreated.
**
** Wire layouts (little-endian, pointer-width follows the emitting process):
**
**   Method events: uintptr_t ScriptContextID, uintptr_t MethodStartAddress,
**     uint64_t MethodSize, uint32_t MethodID, uint16_t MethodFlags,
**     uint16_t MethodAddressRangeID, uint64_t SourceID, uint32_t Line,
**     uint32_t Column, NUL-terminated UTF-16 MethodName.
**   Source events: uint64_t SourceID, uintptr_t ScriptContextID,
**     uint32_t SourceFlags, NUL-terminated UTF-16 Url.
**   Context events: uintptr_t ScriptContextID. Init/complete: no payload.
**
** Each field is a separate EVENT_DATA_DESCRIPTOR so C struct padding can
** never change the on-wire layout. Consumers (e.g. PerfView's generated
** JScriptTraceEventParser) read the fields above in exactly this order.
*/

/* {57277741-3638-4a4b-bdba-0ac6e45da56c}: Microsoft-JScript. */
#define LJ_ETW_PROVIDER_GUID \
  { 0x57277741, 0x3638, 0x4a4b, { 0xbd, 0xba, 0x0a, 0xc6, 0xe4, 0x5d, 0xa5, 0x6c } }

#define LJ_ETW_LEVEL		4	/* win:Informational */
#define LJ_ETW_KW_RUNTIME	0x1	/* JScriptRuntime. */
#define LJ_ETW_KW_START		0x2	/* JScriptStartRundown. */
#define LJ_ETW_KW_END		0x4	/* JScriptEndRundown. */

/* EVENT_DESCRIPTOR: Id, Version, Channel, Level, Opcode, Task, Keyword. */
#define ETW_EVT(id, op, task, kw) \
  { (USHORT)(id), 0, 0, LJ_ETW_LEVEL, (UCHAR)(op), (USHORT)(task), (ULONGLONG)(kw) }

static const EVENT_DESCRIPTOR etw_evt_dcstart_init =
  ETW_EVT(1, 14, 3, LJ_ETW_KW_START);
static const EVENT_DESCRIPTOR etw_evt_dcstart_complete =
  ETW_EVT(2, 12, 3, LJ_ETW_KW_START);
static const EVENT_DESCRIPTOR etw_evt_dcstop_init =
  ETW_EVT(3, 15, 3, LJ_ETW_KW_END);
static const EVENT_DESCRIPTOR etw_evt_dcstop_complete =
  ETW_EVT(4, 13, 3, LJ_ETW_KW_END);
static const EVENT_DESCRIPTOR etw_evt_method_dcstart =
  ETW_EVT(5, 10, 3, LJ_ETW_KW_START);
static const EVENT_DESCRIPTOR etw_evt_method_dcstop =
  ETW_EVT(6, 11, 3, LJ_ETW_KW_END);
static const EVENT_DESCRIPTOR etw_evt_ctx_dcstart =
  ETW_EVT(7, 10, 4, LJ_ETW_KW_START);
static const EVENT_DESCRIPTOR etw_evt_ctx_dcstop =
  ETW_EVT(8, 11, 4, LJ_ETW_KW_END);
static const EVENT_DESCRIPTOR etw_evt_method_load =
  ETW_EVT(9, 10, 1, LJ_ETW_KW_RUNTIME);
static const EVENT_DESCRIPTOR etw_evt_method_unload =
  ETW_EVT(10, 11, 1, LJ_ETW_KW_RUNTIME);
static const EVENT_DESCRIPTOR etw_evt_ctx_load =
  ETW_EVT(11, 10, 2, LJ_ETW_KW_RUNTIME);
static const EVENT_DESCRIPTOR etw_evt_ctx_unload =
  ETW_EVT(12, 11, 2, LJ_ETW_KW_RUNTIME);
static const EVENT_DESCRIPTOR etw_evt_source_dcstart =
  ETW_EVT(39, 12, 4, LJ_ETW_KW_START);
static const EVENT_DESCRIPTOR etw_evt_source_dcstop =
  ETW_EVT(40, 13, 4, LJ_ETW_KW_END);
static const EVENT_DESCRIPTOR etw_evt_source_load =
  ETW_EVT(41, 12, 2, LJ_ETW_KW_RUNTIME);
static const EVENT_DESCRIPTOR etw_evt_source_unload =
  ETW_EVT(42, 13, 2, LJ_ETW_KW_RUNTIME);

/* Longest source name in UTF-16 code units (excluding the NUL). */
#define LJ_ETW_MAX_SOURCE_WCHARS	4095

/* -- Diagnostics ---------------------------------------------------------
** Failures never abort state creation or trace compilation. Each failure
** class is reported once to the debugger instead.
*/

enum {
  ETW_FAIL_REGISTER,
  ETW_FAIL_THREAD,
  ETW_FAIL_EVENT,
  ETW_FAIL_RECORD,
  ETW_FAIL_WRITE,
  ETW_FAIL_IDS,
  ETW_FAIL__MAX
};

static volatile LONG etw_diag_seen[ETW_FAIL__MAX];

/* Append a NUL-terminated message plus a decimal code to a fixed buffer. */
static void etw_diag_write(const char *op, unsigned long code, int have_code)
{
  char buf[160];
  char *p = buf;
  const char *q;
  char digits[11];
  int n = 0;
  q = "LuaJIT ETW: ";
  while (*q) *p++ = *q++;
  q = op;
  while (*q && p < buf + sizeof(buf) - 16) *p++ = *q++;
  if (have_code) {
    q = " failed (";
    while (*q) *p++ = *q++;
    do { digits[n++] = (char)('0' + (code % 10)); code /= 10; } while (code);
    while (n) *p++ = digits[--n];
    *p++ = ')';
  }
  *p++ = '\n';
  *p = '\0';
  OutputDebugStringA(buf);
}

static void etw_fail(int cls, const char *op, unsigned long code)
{
  if (InterlockedCompareExchange(&etw_diag_seen[cls], 1, 0) == 0)
    etw_diag_write(op, code, 1);
}

/* -- Provider lifetime and symbol cache ----------------------------------
**
** One provider is shared by every loaded LuaJIT VM in the process. Lock
** order is always lifecycle lock -> cache lock; the trace hooks and the
** worker thread only ever take the cache lock. The worker thread never
** dereferences Lua state, Lua strings or GC objects: it only replays the
** self-contained records built on the VM thread.
*/

typedef struct EtwContext {
  struct EtwContext *next;	/* Cache list (cache lock). */
  struct EtwContext *prev;
  struct EtwContext *wnext;	/* Write batch chain (cache lock). */
  void *id;			/* ScriptContextID == global_State pointer. */
  int published;		/* Participating in the current runtime state. */
  int refs;			/* List membership plus active writers. */
} EtwContext;

typedef struct EtwRecord {
  struct EtwRecord *next;	/* Cache list (cache lock). */
  struct EtwRecord *prev;
  struct EtwRecord *wnext;	/* Write batch chain (cache lock). */
  void *context_id;		/* Owning ScriptContextID. */
  void *mcode;			/* Start of the trace machine code. */
  uint64_t size;		/* Size of the trace machine code. */
  uint64_t source_id;		/* Generation; stable for this trace. */
  uint32_t method_id;		/* Low 32 bits of the generation. */
  uint32_t line;		/* Starting line (0 if there is no line info). */
  int method_chars;		/* UTF-16 units, excluding the NUL. */
  int source_chars;
  int retired;			/* Unload already reported; owner frees it. */
  int refs;			/* T ownership + list membership + writers. */
  WCHAR strings[1];		/* Method name, then source URL. */
} EtwRecord;

/* Nodes borrowed (or handed over) so that ETW writes happen outside the cache
** lock. Every chained node owns one reference, so a concurrent unlink or free
** on the owning VM cannot invalidate the payload while it is being written.
*/
typedef struct EtwBatch {
  EtwRecord *recs;
  EtwContext *ctxs;
} EtwBatch;

static INIT_ONCE etw_once = INIT_ONCE_STATIC_INIT;
static SRWLOCK etw_lifecycle_lock;
static SRWLOCK etw_cache_lock;

static REGHANDLE etw_provider = 0;	/* Lifecycle lock (0: not running). */
static HANDLE etw_wake = NULL;		/* Auto-reset event (lifecycle lock). */
static HANDLE etw_worker = NULL;	/* Worker thread (lifecycle lock). */
static HANDLE etw_stop_done = NULL;	/* Manual-reset, created once. */
static volatile LONG etw_stop = 0;	/* Worker stop request. */
static volatile LONG etw_pending = 0;	/* Snapshot request bit. */
enum { ETW_DOWN, ETW_UP, ETW_STOPPING };
static int etw_state = ETW_DOWN;	/* Lifecycle lock. */
static int etw_vms = 0;			/* Listed VM contexts (lifecycle lock). */
static int etw_published = 0;		/* Published contexts (lifecycle lock). */
static volatile LONG etw_on = 0;	/* Runtime switch; default off. */
static EtwContext *etw_contexts = NULL;	/* Cache lock, all VMs. */
static EtwRecord *etw_records = NULL;	/* Cache lock, published records. */
static EtwRecord *etw_retired = NULL;	/* Cache lock, awaiting owner free. */
static uint64_t etw_generation = 0;	/* Cache lock. */
static int etw_callback_token;		/* Static callback context. */

static const uint16_t etw_zero16 = 0;
static const uint32_t etw_zero32 = 0;

static BOOL CALLBACK etw_once_cb(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
  UNUSED(once); UNUSED(param);
  InitializeSRWLock(&etw_lifecycle_lock);
  InitializeSRWLock(&etw_cache_lock);
  etw_stop_done = CreateEventW(NULL, TRUE, FALSE, NULL);
  if (ctx) *ctx = NULL;
  return TRUE;
}

static void etw_ensure_locks(void)
{
  InitOnceExecuteOnce(&etw_once, etw_once_cb, NULL, NULL);
}

/* Acquire the lifecycle lock, waiting out a stop that another thread is
** finishing. The join happens outside the lock, so this wait can always make
** progress; the final acquire is a best-effort fallback for cleanup paths.
*/
static void etw_lifecycle_acquire(void)
{
  int tries;
  for (tries = 0; tries < 8; tries++) {
    AcquireSRWLockExclusive(&etw_lifecycle_lock);
    if (etw_state != ETW_STOPPING) return;
    ReleaseSRWLockExclusive(&etw_lifecycle_lock);
    if (!etw_stop_done) break;
    WaitForSingleObject(etw_stop_done, 5000);
  }
  AcquireSRWLockExclusive(&etw_lifecycle_lock);
}

/* -- Write batches -------------------------------------------------------
**
** The cache lock only ever covers list/flag mutations. Payloads are handed to
** a batch, the lock is dropped and the ETW writes happen afterwards, so a VM
** committing or freeing a trace is never blocked behind an EventWrite (and
** behind ETW buffer backpressure in particular).
*/

static void etw_batch_init(EtwBatch *b)
{
  b->recs = NULL;
  b->ctxs = NULL;
}

/* Cache lock held: take a borrowed reference (snapshot) or hand over an
** unlinked list reference (unpublish/freestate). */
static void etw_batch_chain_rec(EtwBatch *b, EtwRecord *r)
{
  r->wnext = b->recs;
  b->recs = r;
}

static void etw_batch_chain_ctx(EtwBatch *b, EtwContext *c)
{
  c->wnext = b->ctxs;
  b->ctxs = c;
}

/* Drop one reference per chained node and free what reached zero. Called
** without the cache lock; the chained order is reversed, which does not
** matter because consumers key records by MethodID/SourceID.
*/
static void etw_batch_release(EtwBatch *b)
{
  EtwRecord *r, *rn, *rfree = NULL;
  EtwContext *c, *cn, *cfree = NULL;
  AcquireSRWLockExclusive(&etw_cache_lock);
  for (r = b->recs; r; r = rn) {
    rn = r->wnext;
    if (--r->refs <= 0) { r->wnext = rfree; rfree = r; }
  }
  for (c = b->ctxs; c; c = cn) {
    cn = c->wnext;
    if (--c->refs <= 0) { c->wnext = cfree; cfree = c; }
  }
  ReleaseSRWLockExclusive(&etw_cache_lock);
  while (rfree) {
    rn = rfree->wnext;
    HeapFree(GetProcessHeap(), 0, rfree);
    rfree = rn;
  }
  while (cfree) {
    cn = cfree->wnext;
    HeapFree(GetProcessHeap(), 0, cfree);
    cfree = cn;
  }
}

static int etw_enabled(REGHANDLE h, const EVENT_DESCRIPTOR *evt)
{
  return h != 0 && EventEnabled(h, evt) != FALSE;
}

static void etw_desc(EVENT_DATA_DESCRIPTOR *d, const void *p, ULONG n)
{
  d->Ptr = (ULONGLONG)(ULONG_PTR)p;
  d->Size = n;
  d->Reserved = 0;
}

static void etw_write_event(REGHANDLE h, const EVENT_DESCRIPTOR *evt)
{
  ULONG rc;
  if (!etw_enabled(h, evt)) return;
  rc = EventWrite(h, evt, 0, NULL);
  if (rc != ERROR_SUCCESS) etw_fail(ETW_FAIL_WRITE, "EventWrite", rc);
}

static void etw_write_ctx(REGHANDLE h, const EVENT_DESCRIPTOR *evt,
			  const EtwContext *c)
{
  EVENT_DATA_DESCRIPTOR d[1];
  ULONG rc;
  if (!etw_enabled(h, evt)) return;
  etw_desc(&d[0], &c->id, (ULONG)sizeof(void *));
  rc = EventWrite(h, evt, 1, d);
  if (rc != ERROR_SUCCESS) etw_fail(ETW_FAIL_WRITE, "EventWrite", rc);
}

static void etw_write_method(REGHANDLE h, const EVENT_DESCRIPTOR *evt,
			     const EtwRecord *r)
{
  EVENT_DATA_DESCRIPTOR d[10];
  ULONG rc;
  if (!etw_enabled(h, evt)) return;
  etw_desc(&d[0], &r->context_id, (ULONG)sizeof(void *));
  etw_desc(&d[1], &r->mcode, (ULONG)sizeof(void *));
  etw_desc(&d[2], &r->size, 8);
  etw_desc(&d[3], &r->method_id, 4);
  etw_desc(&d[4], &etw_zero16, 2);	/* MethodFlags. */
  etw_desc(&d[5], &etw_zero16, 2);	/* MethodAddressRangeID. */
  etw_desc(&d[6], &r->source_id, 8);
  etw_desc(&d[7], &r->line, 4);
  etw_desc(&d[8], &etw_zero32, 4);	/* Column. */
  etw_desc(&d[9], r->strings, (ULONG)((r->method_chars + 1) * sizeof(WCHAR)));
  rc = EventWrite(h, evt, 10, d);
  if (rc != ERROR_SUCCESS) etw_fail(ETW_FAIL_WRITE, "EventWrite", rc);
}

static void etw_write_source(REGHANDLE h, const EVENT_DESCRIPTOR *evt,
			     const EtwRecord *r)
{
  EVENT_DATA_DESCRIPTOR d[4];
  const WCHAR *url = r->strings + r->method_chars + 1;
  ULONG rc;
  if (!etw_enabled(h, evt)) return;
  etw_desc(&d[0], &r->source_id, 8);
  etw_desc(&d[1], &r->context_id, (ULONG)sizeof(void *));
  etw_desc(&d[2], &etw_zero32, 4);	/* SourceFlags. */
  etw_desc(&d[3], url, (ULONG)((r->source_chars + 1) * sizeof(WCHAR)));
  rc = EventWrite(h, evt, 4, d);
  if (rc != ERROR_SUCCESS) etw_fail(ETW_FAIL_WRITE, "EventWrite", rc);
}

/* Publish a full snapshot of the live symbol cache. Called by the worker
** thread with the cache lock held, so the replayed addresses can never be
** stale or reordered against a concurrent load/unload.
*/
static void etw_snapshot(REGHANDLE h)
{
  EtwBatch b;
  EtwContext *c;
  EtwRecord *r;
  int do_start, do_load, do_stop;
  /* EventEnabled is a lock-free query, so the block gating happens before any
  ** borrowing. Sessions may subscribe to any combination of the runtime
  ** (0x1), start rundown (0x2) and end rundown (0x4) keywords.
  */
  do_start = etw_enabled(h, &etw_evt_dcstart_init);
  do_load = etw_enabled(h, &etw_evt_method_load);
  do_stop = etw_enabled(h, &etw_evt_dcstop_init);
  if (!do_start && !do_load && !do_stop) return;
  etw_batch_init(&b);
  AcquireSRWLockExclusive(&etw_cache_lock);
  for (c = etw_contexts; c; c = c->next) {
    if (c->published) { c->refs++; etw_batch_chain_ctx(&b, c); }
  }
  for (r = etw_records; r; r = r->next) { r->refs++; etw_batch_chain_rec(&b, r); }
  ReleaseSRWLockExclusive(&etw_cache_lock);
  if (do_start) {
    etw_write_event(h, &etw_evt_dcstart_init);
    for (c = b.ctxs; c; c = c->wnext)
      etw_write_ctx(h, &etw_evt_ctx_dcstart, c);
    for (r = b.recs; r; r = r->wnext) {
      etw_write_source(h, &etw_evt_source_dcstart, r);
      etw_write_method(h, &etw_evt_method_dcstart, r);
    }
    etw_write_event(h, &etw_evt_dcstart_complete);
  }
  if (do_load) {
    for (c = b.ctxs; c; c = c->wnext)
      etw_write_ctx(h, &etw_evt_ctx_load, c);
    for (r = b.recs; r; r = r->wnext) {
      etw_write_source(h, &etw_evt_source_load, r);
      etw_write_method(h, &etw_evt_method_load, r);
    }
  }
  if (do_stop) {
    etw_write_event(h, &etw_evt_dcstop_init);
    for (c = b.ctxs; c; c = c->wnext)
      etw_write_ctx(h, &etw_evt_ctx_dcstop, c);
    for (r = b.recs; r; r = r->wnext) {
      etw_write_source(h, &etw_evt_source_dcstop, r);
      etw_write_method(h, &etw_evt_method_dcstop, r);
    }
    etw_write_event(h, &etw_evt_dcstop_complete);
  }
  etw_batch_release(&b);
}

/* Standard PENABLECALLBACK. Only records a request: no locks, no allocation,
** no VM walk, no sampling control. The worker thread decides what to emit.
*/
static void WINAPI etw_enable_cb(LPCGUID source, ULONG code, UCHAR level,
				 ULONGLONG anykw, ULONGLONG allkw,
				 PEVENT_FILTER_DESCRIPTOR filter, PVOID ctx)
{
  UNUSED(source); UNUSED(level); UNUSED(anykw); UNUSED(allkw);
  UNUSED(filter); UNUSED(ctx);
  if (code == EVENT_CONTROL_CODE_ENABLE_PROVIDER ||
      code == EVENT_CONTROL_CODE_CAPTURE_STATE) {
    InterlockedExchange(&etw_pending, 1);
    if (etw_wake) SetEvent(etw_wake);
  }
}

static DWORD WINAPI etw_worker_proc(LPVOID param)
{
  UNUSED(param);
  while (!etw_stop) {
    if (WaitForSingleObject(etw_wake, INFINITE) != WAIT_OBJECT_0) break;
    if (etw_stop) break;
    if (InterlockedExchange(&etw_pending, 0) != 0) {
      /* The handle stays valid until the join that precedes unregistering. */
      REGHANDLE h = etw_provider;
      if (h) etw_snapshot(h);
    }
  }
  return 0;
}

/* Start the provider, worker thread and wake event. Lifecycle lock held. */
static int etw_module_start(void)
{
  static const GUID guid = LJ_ETW_PROVIDER_GUID;
  ULONG rc;
  if (etw_state == ETW_UP) return 1;
  if (etw_state == ETW_STOPPING) return 0;	/* Retry once the stop lands. */
  InterlockedExchange(&etw_pending, 0);
  InterlockedExchange(&etw_stop, 0);
  etw_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
  if (!etw_wake) {
    etw_fail(ETW_FAIL_EVENT, "CreateEvent", GetLastError());
    return 0;
  }
  rc = EventRegister(&guid, etw_enable_cb, &etw_callback_token, &etw_provider);
  if (rc != ERROR_SUCCESS) {
    etw_fail(ETW_FAIL_REGISTER, "EventRegister", rc);
    etw_provider = 0;
    CloseHandle(etw_wake); etw_wake = NULL;
    return 0;
  }
  /* Callbacks can arrive from EventRegister on, so the wake event and the
  ** static callback context are already valid here. The worker is started
  ** only after the registration handle has been published.
  */
  etw_worker = CreateThread(NULL, 0, etw_worker_proc, NULL, 0, NULL);
  if (!etw_worker) {
    etw_fail(ETW_FAIL_THREAD, "CreateThread", GetLastError());
    EventUnregister(etw_provider);
    etw_provider = 0;
    CloseHandle(etw_wake); etw_wake = NULL;
    return 0;
  }
  etw_state = ETW_UP;
  return 1;
}

/* Stop phase 1 (lifecycle lock held): ask the worker to exit and hand back
** its handle so the join can happen *outside* the lock.
*/
static HANDLE etw_module_stop_begin(void)
{
  HANDLE w;
  if (etw_state != ETW_UP) return NULL;
  etw_state = ETW_STOPPING;
  InterlockedExchange(&etw_stop, 1);
  if (etw_wake) SetEvent(etw_wake);
  w = etw_worker;
  etw_worker = NULL;
  return w;
}

/* Join the worker without holding the lifecycle lock, so a VM create/close or
** another enable never waits behind ETW buffer backpressure. Correctness
** still requires the worker to be gone before EventUnregister, so a timeout
** is reported once and then waited out instead of silently hanging.
*/
static void etw_module_stop_join(HANDLE w)
{
  if (!w) return;
  if (WaitForSingleObject(w, 5000) == WAIT_TIMEOUT) {
    etw_fail(ETW_FAIL_THREAD, "worker join", WAIT_TIMEOUT);
    WaitForSingleObject(w, INFINITE);
  }
  CloseHandle(w);
}

/* Stop phase 2 (lifecycle lock held): unregister and release the wake event. */
static void etw_module_stop_finish(void)
{
  if (etw_state != ETW_STOPPING) return;
  if (etw_provider) {
    EventUnregister(etw_provider);
    etw_provider = 0;
  }
  if (etw_wake) { CloseHandle(etw_wake); etw_wake = NULL; }
  InterlockedExchange(&etw_pending, 0);
  etw_state = ETW_DOWN;
  if (etw_stop_done) SetEvent(etw_stop_done);
}

/* Stop the worker thread and unregister the provider. Lifecycle lock held,
** cache lock NOT held (the worker may still need the cache lock to leave).
*/
/* -- Chunk name to UTF-16 ------------------------------------------------- */

static int etw_is_high_surrogate(WCHAR c)
{
  return c >= 0xD800 && c <= 0xDBFF;
}

static int etw_u32_digits(uint32_t v)
{
  int n = 1;
  while (v >= 10) { v /= 10; n++; }
  return n;
}

static WCHAR *etw_put_u32(WCHAR *p, uint32_t v)
{
  WCHAR digits[10];
  int n = 0;
  do { digits[n++] = (WCHAR)(L'0' + (v % 10)); v /= 10; } while (v);
  while (n) *p++ = digits[--n];
  return p;
}

/* Convert up to maxchars UTF-16 code units of a chunk name. Invalid bytes
** use the Windows replacement behaviour. Long names keep a prefix and get a
** trailing ellipsis; a surrogate pair is never split. NULs inside the name
** are replaced, since ETW consumers read a NUL-terminated string.
*/
static WCHAR *etw_utf16_alloc(const char *s, size_t len, int maxchars,
			      int *out_chars)
{
  WCHAR *w = (WCHAR *)HeapAlloc(GetProcessHeap(), 0,
				(maxchars + 2) * sizeof(WCHAR));
  size_t off = 0;
  int n = 0, truncated = 0;
  if (!w) return NULL;
  while (off < len && n < maxchars) {
    WCHAR tmp[132];
    size_t step = len - off;
    int got, i;
    if (step > 128) step = 128;
    /* End the step at a UTF-8 character boundary: never split a sequence
    ** across MultiByteToWideChar calls. */
    while (off + step < len && ((unsigned char)s[off + step] & 0xc0) == 0x80)
      step++;
    got = MultiByteToWideChar(CP_UTF8, 0, s + off, (int)step, tmp, 132);
    if (got <= 0) { truncated = 1; break; }
    for (i = 0; i < got; i++) {
      WCHAR c = tmp[i];
      if (c == 0) c = 0xfffd;
      if (n >= maxchars) { truncated = 1; break; }
      if (etw_is_high_surrogate(c) && n + 2 > maxchars) {
	truncated = 1;		/* No room for the low surrogate. */
	break;
      }
      w[n++] = c;
    }
    if (truncated) break;
    off += step;
  }
  if (truncated) {
    if (n > 0 && etw_is_high_surrogate(w[n-1])) n--;
    if (n >= maxchars) n = maxchars - 1;
    w[n++] = 0x2026;		/* '…' */
  }
  w[n] = 0;
  *out_chars = n;
  return w;
}

/* Longest chunk name kept verbatim; longer names are source blobs. */
#define LJ_ETW_MAX_CHUNKNAME_BYTES	512

static const char *etw_chunkname(GCproto *pt, size_t *len)
{
  GCstr *cn = proto_chunkname(pt);
  const char *name = strdata(cn);
  size_t n = cn->len;
  if (n && (name[0] == '@' || name[0] == '=')) {
    *len = n - 1;
    return name + 1;
  }
  /* Unprefixed names are either a bare path (the DST engine passes
  ** "scripts/foo.lua" without '@') or the source text itself (loadstring
  ** without a name). Keep short path-like names, because otherwise every
  ** game-side symbol would degrade to "(string)"; collapse anything that
  ** looks like source so a whole blob never becomes the symbol URL.
  */
  if (n > 0 && n <= LJ_ETW_MAX_CHUNKNAME_BYTES && !memchr(name, '\n', n) &&
      (memchr(name, '/', n) != NULL || memchr(name, '\\', n) != NULL)) {
    *len = n;
    return name;
  }
  *len = sizeof("(string)") - 1;
  return "(string)";
}

/* "LuaJIT::TRACE_<traceno>::<source>:<line>" */
static WCHAR *etw_method_alloc(const WCHAR *source, int source_chars,
			       uint32_t traceno, uint32_t line, int *out_chars)
{
  static const WCHAR prefix[] = L"LuaJIT::TRACE_";
  const int plen = (int)(sizeof(prefix) / sizeof(WCHAR)) - 1;
  int n = plen + etw_u32_digits(traceno) + 2 + source_chars + 1 +
	  etw_u32_digits(line);
  WCHAR *m = (WCHAR *)HeapAlloc(GetProcessHeap(), 0,
				(n + 1) * sizeof(WCHAR));
  WCHAR *p;
  if (!m) return NULL;
  p = m;
  memcpy(p, prefix, plen * sizeof(WCHAR));
  p += plen;
  p = etw_put_u32(p, traceno);
  *p++ = L':'; *p++ = L':';
  memcpy(p, source, source_chars * sizeof(WCHAR));
  p += source_chars;
  *p++ = L':';
  p = etw_put_u32(p, line);
  *p = 0;
  *out_chars = n;
  return m;
}

/* -- Runtime switch ------------------------------------------------------
**
** The provider is process-global and off by default: nothing is published
** until a VM asks for it with jit.profile.etw(true), which also flushes that
** VM's traces so every live trace has a record and no stale identity
** survives. Turning it off reports the unloads immediately and moves the
** records to a retired list: only the owning VM frees them (its traces may
** still point at them), so a foreign VM's GCtrace is never dereferenced here.
*/

/* Report every listed VM context. Lifecycle lock held, module running. */
static void etw_publish_all(void)
{
  EtwBatch b;
  EtwContext *c;
  etw_batch_init(&b);
  AcquireSRWLockExclusive(&etw_cache_lock);
  for (c = etw_contexts; c; c = c->next) {
    if (c->published) continue;
    c->published = 1;
    etw_published++;
    c->refs++;
    etw_batch_chain_ctx(&b, c);
  }
  ReleaseSRWLockExclusive(&etw_cache_lock);
  for (c = b.ctxs; c; c = c->wnext)
    etw_write_ctx(etw_provider, &etw_evt_ctx_load, c);
  etw_batch_release(&b);
}

/* Stop publishing: unload the live records and the contexts. Lifecycle lock
** held (the records stay alive until their owning VM releases them).
*/
static void etw_unpublish_all(void)
{
  EtwBatch b;
  EtwRecord *r;
  EtwContext *c;
  etw_batch_init(&b);
  AcquireSRWLockExclusive(&etw_cache_lock);
  for (r = etw_records; r; r = r->next) {
    r->retired = 1;
    etw_batch_chain_rec(&b, r);		/* hands over the list reference */
  }
  etw_records = NULL;
  for (c = etw_contexts; c; c = c->next) {
    if (!c->published) continue;
    c->published = 0;
    if (etw_published > 0) etw_published--;
    c->refs++;
    etw_batch_chain_ctx(&b, c);
  }
  ReleaseSRWLockExclusive(&etw_cache_lock);
  for (r = b.recs; r; r = r->wnext) {
    etw_write_method(etw_provider, &etw_evt_method_unload, r);
    etw_write_source(etw_provider, &etw_evt_source_unload, r);
  }
  for (c = b.ctxs; c; c = c->wnext)
    etw_write_ctx(etw_provider, &etw_evt_ctx_unload, c);
  etw_batch_release(&b);
}

int lj_etw_setstate(jit_State *J, int on)
{
  HANDLE worker;
  int transition = 0;
  UNUSED(J);
  on = on != 0;
  if (on == (etw_on != 0)) {
    /* Already in the requested state: still publish contexts that appeared
    ** while a stop was finishing, but report "no transition" so the caller
    ** only flushes traces when publishing was actually switched on.
    */
    if (on) {
      etw_ensure_locks();
      etw_lifecycle_acquire();
      if (etw_state == ETW_DOWN) etw_module_start();
      if (etw_state == ETW_UP) etw_publish_all();
      ReleaseSRWLockExclusive(&etw_lifecycle_lock);
    }
    return 0;
  }
  etw_ensure_locks();
  etw_lifecycle_acquire();
  if (!on) {
    if (etw_on == 0) {
      ReleaseSRWLockExclusive(&etw_lifecycle_lock);
      return 0;
    }
    InterlockedExchange(&etw_on, 0);
    etw_unpublish_all();
    worker = etw_module_stop_begin();
    ReleaseSRWLockExclusive(&etw_lifecycle_lock);
    etw_module_stop_join(worker);
    /* This thread owns the stop, so do not wait for our own stop_done. */
    AcquireSRWLockExclusive(&etw_lifecycle_lock);
    etw_module_stop_finish();
    ReleaseSRWLockExclusive(&etw_lifecycle_lock);
    return 1;
  }
  if (etw_module_start()) {
    InterlockedExchange(&etw_on, 1);
    etw_publish_all();
    transition = 1;
  }
  ReleaseSRWLockExclusive(&etw_lifecycle_lock);
  return transition;
}

int lj_etw_getstate(void)
{
  return etw_on != 0;
}

/* -- Trace hooks --------------------------------------------------------- */

void lj_etw_initstate(jit_State *J)
{
  EtwContext *ctx;
  EtwBatch b;
  J->etw_context = NULL;
  etw_ensure_locks();
  /* The context is always allocated and listed, even while the switch is off,
  ** so any VM can turn publishing on later without being recreated. Off costs
  ** one small allocation: no provider, no thread, no per-trace record.
  */
  ctx = (EtwContext *)HeapAlloc(GetProcessHeap(), 0, sizeof(EtwContext));
  if (!ctx) {
    etw_fail(ETW_FAIL_RECORD, "HeapAlloc", ERROR_OUTOFMEMORY);
    return;
  }
  ctx->id = (void *)J2G(J);
  ctx->published = 0;
  ctx->wnext = NULL;
  etw_batch_init(&b);
  etw_lifecycle_acquire();
  if (etw_on && etw_state == ETW_DOWN) etw_module_start();
  AcquireSRWLockExclusive(&etw_cache_lock);
  ctx->prev = NULL;
  ctx->next = etw_contexts;
  if (etw_contexts) etw_contexts->prev = ctx;
  etw_contexts = ctx;
  ctx->refs = 1;			/* list membership */
  etw_vms++;
  if (etw_on && etw_state == ETW_UP) {
    ctx->published = 1;
    etw_published++;
    ctx->refs++;			/* this writer */
    etw_batch_chain_ctx(&b, ctx);
  }
  ReleaseSRWLockExclusive(&etw_cache_lock);
  if (b.ctxs) {
    etw_write_ctx(etw_provider, &etw_evt_ctx_load, ctx);
    etw_batch_release(&b);
  }
  J->etw_context = ctx;
  ReleaseSRWLockExclusive(&etw_lifecycle_lock);
}

void lj_etw_freestate(jit_State *J)
{
  EtwContext *ctx = (EtwContext *)J->etw_context;
  EtwRecord *r, *next;
  EtwBatch b, dead;
  HANDLE worker = NULL;
  int was_published, need_stop;
  if (!ctx) return;
  J->etw_context = NULL;
  etw_ensure_locks();
  etw_batch_init(&b);
  etw_batch_init(&dead);
  etw_lifecycle_acquire();
  AcquireSRWLockExclusive(&etw_cache_lock);
  /* All traces of this VM are gone already, so nothing points at its records
  ** any more: unlink the live ones (their unloads still have to be reported)
  ** and the retired ones (already reported) into the write batches.
  */
  for (r = etw_records; r; r = next) {
    next = r->next;
    if (r->context_id != ctx->id) continue;
    if (r->prev) r->prev->next = r->next;
    else etw_records = r->next;
    if (r->next) r->next->prev = r->prev;
    etw_batch_chain_rec(&b, r);		/* hands over the list reference */
  }
  for (r = etw_retired; r; r = next) {
    next = r->next;
    if (r->context_id != ctx->id) continue;
    if (r->prev) r->prev->next = r->next;
    else etw_retired = r->next;
    if (r->next) r->next->prev = r->prev;
    etw_batch_chain_rec(&dead, r);	/* no events left to send */
  }
  was_published = ctx->published;
  if (ctx->published) {
    ctx->published = 0;
    if (etw_published > 0) etw_published--;
  }
  if (etw_contexts == ctx) etw_contexts = ctx->next;
  if (ctx->next) ctx->next->prev = ctx->prev;
  if (ctx->prev) ctx->prev->next = ctx->next;
  etw_batch_chain_ctx(&b, ctx);		/* hands over the list reference */
  ReleaseSRWLockExclusive(&etw_cache_lock);
  /* Report the unloads outside the cache lock; the provider stays registered
  ** until the stop below finishes.
  */
  if (was_published) {
    for (r = b.recs; r; r = r->wnext) {
      etw_write_method(etw_provider, &etw_evt_method_unload, r);
      etw_write_source(etw_provider, &etw_evt_source_unload, r);
    }
    etw_write_ctx(etw_provider, &etw_evt_ctx_unload, ctx);
  }
  etw_batch_release(&b);
  etw_batch_release(&dead);
  if (etw_vms > 0) etw_vms--;
  need_stop = (etw_published == 0);
  worker = need_stop ? etw_module_stop_begin() : NULL;
  ReleaseSRWLockExclusive(&etw_lifecycle_lock);
  if (worker) {
    etw_module_stop_join(worker);
    /* This thread owns the stop, so do not wait for our own stop_done. */
    AcquireSRWLockExclusive(&etw_lifecycle_lock);
    etw_module_stop_finish();
    ReleaseSRWLockExclusive(&etw_lifecycle_lock);
  }
}

void lj_etw_addtrace(jit_State *J, GCtrace *T)
{
  EtwContext *ctx = (EtwContext *)J->etw_context;
  GCproto *pt;
  const BCIns *startpc;
  const char *name;
  size_t namelen;
  WCHAR *source, *method;
  int source_chars = 0, method_chars = 0;
  uint32_t line, traceno;
  size_t sz;
  EtwRecord *r;
  uint64_t gen;
  if (!ctx || !ctx->published) return;
  /* Metadata is read and converted on the VM thread. */
  pt = &gcref(T->startpt)->pt;
  startpc = mref(T->startpc, const BCIns);
  lj_assertJ(startpc >= proto_bc(pt) && startpc < proto_bc(pt) + pt->sizebc,
	     "start PC out of range");
  name = etw_chunkname(pt, &namelen);
  source = etw_utf16_alloc(name, namelen, LJ_ETW_MAX_SOURCE_WCHARS,
			   &source_chars);
  if (!source) {
    etw_fail(ETW_FAIL_RECORD, "HeapAlloc", ERROR_OUTOFMEMORY);
    return;
  }
  traceno = (uint32_t)T->traceno;
  line = (uint32_t)lj_debug_line(pt, proto_bcpos(pt, startpc));
  method = etw_method_alloc(source, source_chars, traceno, line,
			    &method_chars);
  if (!method) {
    etw_fail(ETW_FAIL_RECORD, "HeapAlloc", ERROR_OUTOFMEMORY);
    HeapFree(GetProcessHeap(), 0, source);
    return;
  }
  sz = offsetof(EtwRecord, strings) +
       (size_t)(method_chars + 1 + source_chars + 1) * sizeof(WCHAR);
  r = (EtwRecord *)HeapAlloc(GetProcessHeap(), 0, sz);
  if (!r) {
    etw_fail(ETW_FAIL_RECORD, "HeapAlloc", ERROR_OUTOFMEMORY);
    HeapFree(GetProcessHeap(), 0, source);
    HeapFree(GetProcessHeap(), 0, method);
    return;
  }
  r->context_id = ctx->id;
  r->retired = 0;
  r->mcode = (void *)T->mcode;
  r->size = (uint64_t)T->szmcode;
  r->line = line;
  r->method_chars = method_chars;
  r->source_chars = source_chars;
  memcpy(r->strings, method, (size_t)method_chars * sizeof(WCHAR));
  r->strings[method_chars] = 0;
  memcpy(r->strings + method_chars + 1, source,
	 (size_t)source_chars * sizeof(WCHAR));
  r->strings[method_chars + 1 + source_chars] = 0;
  HeapFree(GetProcessHeap(), 0, method);
  HeapFree(GetProcessHeap(), 0, source);

  AcquireSRWLockExclusive(&etw_cache_lock);
  if (etw_generation >= (uint64_t)0xffffffffu) {
    /* Never wrap around: a reused MethodID/SourceID would silently associate
    ** a new trace with an old lifetime. */
    if (InterlockedCompareExchange(&etw_diag_seen[ETW_FAIL_IDS], 1, 0) == 0)
      etw_diag_write("trace id counter exhausted; new traces are not symbolized",
		     0, 0);
    ReleaseSRWLockExclusive(&etw_cache_lock);
    HeapFree(GetProcessHeap(), 0, r);
    return;
  }
  etw_generation++;
  gen = etw_generation;
  r->source_id = gen;
  r->method_id = (uint32_t)gen;
  r->wnext = NULL;
  r->refs = 2;			/* T->etw_entry ownership + list membership */
  r->prev = NULL;
  r->next = etw_records;
  if (etw_records) etw_records->prev = r;
  etw_records = r;
  r->refs++;			/* this writer */
  T->etw_entry = r;
  ReleaseSRWLockExclusive(&etw_cache_lock);
  /* Writes happen outside the cache lock; our reference keeps the payload
  ** alive even if another thread unpublishes the record meanwhile.
  */
  etw_write_source(etw_provider, &etw_evt_source_load, r);
  etw_write_method(etw_provider, &etw_evt_method_load, r);
  AcquireSRWLockExclusive(&etw_cache_lock);
  r->refs--;			/* drop the writer reference */
  ReleaseSRWLockExclusive(&etw_cache_lock);
}

void lj_etw_deltrace(jit_State *J, GCtrace *T)
{
  EtwRecord *r = (EtwRecord *)T->etw_entry;
  int retired, dead;
  UNUSED(J);
  if (!r) return;
  T->etw_entry = NULL;
  AcquireSRWLockExclusive(&etw_cache_lock);
  if (r->retired) {
    if (r->prev) r->prev->next = r->next;
    else etw_retired = r->next;
  } else {
    if (r->prev) r->prev->next = r->next;
    else etw_records = r->next;
  }
  if (r->next) r->next->prev = r->prev;
  retired = r->retired;
  r->refs--;			/* drop the T->etw_entry ownership */
  dead = (r->refs == 0);
  ReleaseSRWLockExclusive(&etw_cache_lock);
  /* A retired record already reported its unload when publishing stopped. */
  if (!retired) {
    etw_write_method(etw_provider, &etw_evt_method_unload, r);
    etw_write_source(etw_provider, &etw_evt_source_unload, r);
  }
  if (dead) HeapFree(GetProcessHeap(), 0, r);
}

#endif

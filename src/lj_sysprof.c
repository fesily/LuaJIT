/*
** Implementation of sysprof - platform and Lua profiler.
*/

#define lj_sysprof_c
#define LUA_CORE

#include "lj_arch.h"
#include "lj_sysprof.h"

#if LJ_HASSYSPROF

#include "lj_obj.h"
#include "lj_debug.h"
#include "lj_dispatch.h"
#include "lj_frame.h"

#if LJ_HASJIT
#include "lj_jit.h"
#include "lj_trace.h"
#endif

#include "lj_wbuf.h"
#include "lj_profile_timer.h"
#include "lj_symtab.h"

#include <errno.h>
#include <signal.h>
#if LJ_TARGET_WINDOWS
#include <stdlib.h>
/*
** There is no async signal delivery on Windows: the profile timer runs in
** a separate thread which steals the sample point from the profiled thread
** (SuspendThread + GetThreadContext), so the profiled frames are unwound
** from a saved CONTEXT instead of from the current stack.
*/
#else
#include <pthread.h>
#include <execinfo.h>
#endif

/*
** Number of profiler frames we need to omit during stack
** unwinding. This is only relevant for the POSIX backend, where the
** sample is taken from the signal handler running on the profiled stack.
**   +------------------------+
** 0 | default_backtrace_host |
**   +------------------------+
** 1 | stream_backtrace_host  |
**   +------------------------+
** 2 |  stream_{guest/host}   |
**   +------------------------+
** 3 |      stream_event      |
**   +------------------------+
** 4 | sysprof_record_sample  |
**   +------------------------+
** 5 | sysprof_signal_handler |
**   +------------------------+
*/
#if LJ_TARGET_WINDOWS
/*
** The host stack is unwound from the saved CONTEXT of the profiled thread,
** so the sampler frames are not present on it at all.
*/
#define SYSPROF_HANDLER_STACK_DEPTH 0
#else
#define SYSPROF_HANDLER_STACK_DEPTH 6
#endif
#define SYSPROF_BACKTRACE_FRAME_MAX 512

/* Check that vmstate fits in 4 bits (see streaming format) */
#define vmstfit4(st) ((st & ~(uint32_t)((1 << 4) - 1)) == 0)

enum sysprof_state {
  /* Profiler is not running. */
  SPS_IDLE,
  /* Profiler is running. */
  SPS_PROFILE,
  /*
  ** Stopped in case of stopped or failed stream.
  ** Saved errno is set at luaM_sysprof_stop.
  */
  SPS_HALT
};

/* Machine code ranges of the JIT traces that can appear in a sample. The
** array is sized from the trace cache limit (JIT_P_maxtrace), which is the
** upper bound of J->sizetrace.
*/

struct sysprof {
  global_State *g; /* Profiled VM. */
#if LJ_TARGET_WINDOWS
  DWORD thread; /* Id of the profiled thread. */
  HANDLE thread_handle; /* Handle to suspend/inspect the profiled thread. */
  uintptr_t stack_low; /* Stack bounds of the profiled thread. */
  uintptr_t stack_high;
  int frozen; /* The profiled thread is suspended by the sampler. */
  int prologue_c_pending; /* C symbols are dumped by the profiler thread. */
  uintptr_t vm_func[4]; /* Function entries of the VM assembly. */
  int nvm_func;
  uint8_t *own_buf; /* Buffer owned by the deferred writer (or NULL). */
  uint8_t *spare_buf; /* Spare stream buffer for the deferred write. */
  uint8_t *pending_buf; /* Full buffer waiting for the writer. */
  size_t pending_len;
#else
  pthread_t thread; /* Profiled thread. */
#endif
  volatile sig_atomic_t state; /* Internal state. */
  struct lj_wbuf out; /* Output accumulator. */
  struct luam_Sysprof_Counters counters; /* Profiling counters. */
  struct luam_Sysprof_Options opt; /* Profiling options. */
  luam_Sysprof_writer writer; /* Writer function for profile events. */
  luam_Sysprof_on_stop on_stop; /* Callback on profiling stopping. */
  luam_Sysprof_backtracer backtracer; /* Backtracing function for the host stack. */
  lj_profile_timer timer; /* Profiling timer. */
  int saved_errno; /* Saved errno when profiler failed. */
#if LJ_HASJIT
  /*
  ** Snapshot of the trace machine code ranges. It is maintained on the
  ** profiled thread and only read by the sampler: the JIT state must not be
  ** walked from another thread, it may be suspended in the middle of a
  ** reallocation of the trace array.
  */
  struct sysprof_mcode {
    uintptr_t start;
    uint32_t size;
    uint32_t traceno;
  } *mcode;
  uint32_t nmcode;	/* Entries in use. */
  uint32_t mcode_cap;	/* Allocated entries; never shrinks while profiling. */
#endif
};
/*
** XXX: Only one VM can be profiled at a time.
*/

static struct sysprof sysprof = {0};

#if LJ_TARGET_WINDOWS
/* Context of the profiled thread captured at the current sample point. */
static CONTEXT sysprof_sample_ctx;
static int sysprof_sample_ctx_valid;
#endif

/* --- Stream ------------------------------------------------------------- */

static const uint8_t ljp_header[] = {'l', 'j', 'p', LJP_FORMAT_VERSION,
                                      0x0, 0x0, 0x0};

static int stream_is_needed(struct sysprof *sp)
{
  return sp->opt.mode != LUAM_SYSPROF_DEFAULT;
}

static int is_unconfigured(struct sysprof *sp)
{
  return sp->backtracer == NULL || sp->on_stop == NULL || sp->writer == NULL;
}

static void stream_prologue(struct sysprof *sp)
{
#if LJ_TARGET_WINDOWS
  /*
  ** Only the Lua symbols are dumped here: the profiled thread owns the GC
  ** heap. The C symbols of the loaded images are dumped by the profiler
  ** thread before the first sample (see stream_prologue_c), which is the
  ** expensive part of the prologue.
  */
  lj_symtab_dump_lua(&sp->out, sp->g);
#else
  /*
  ** XXX: Must be zero for the symtab module to dump all loaded libraries.
  */
  uint32_t unused_lib_adds = 0;
  lj_symtab_dump(&sp->out, sp->g, &unused_lib_adds);
  lj_wbuf_addn(&sp->out, ljp_header, sizeof(ljp_header));
#endif
}

#if LJ_TARGET_WINDOWS
/* The rest of the stream prologue, called on the profiler thread. */
static void stream_prologue_c(struct sysprof *sp)
{
  lj_symtab_dump_c(&sp->out);
  lj_wbuf_addn(&sp->out, ljp_header, sizeof(ljp_header));
  sp->prologue_c_pending = 0;

  if (LJ_UNLIKELY(lj_wbuf_test_flag(&sp->out, STREAM_ERRIO | STREAM_STOP))) {
    sp->saved_errno = lj_wbuf_errno(&sp->out);
    lj_wbuf_terminate(&sp->out);
    sp->state = SPS_HALT;
  }
}

static void sysprof_startup(void)
{
  struct sysprof *sp = &sysprof;
  if (sp->state == SPS_PROFILE && stream_is_needed(sp) &&
      sp->prologue_c_pending)
    stream_prologue_c(sp);
}
#endif

static void stream_epilogue(struct sysprof *sp)
{
  lj_wbuf_addbyte(&sp->out, LJP_EPILOGUE_BYTE);
}

static void stream_ffunc_impl(struct lj_wbuf *buf, uint8_t ffid)
{
  lj_wbuf_addbyte(buf, LJP_FRAME_FFUNC);
  lj_wbuf_addu64(buf, ffid);
}

static void stream_lfunc(struct lj_wbuf *buf, const GCfunc *func)
{
  lj_assertX(isluafunc(func), "bad lua function in sysprof stream");
  const GCproto *pt = funcproto(func);
  lj_assertX(pt != NULL, "bad lua function prototype in sysprof stream");
  lj_wbuf_addbyte(buf, LJP_FRAME_LFUNC);
  lj_wbuf_addu64(buf, (uintptr_t)pt);
  lj_wbuf_addu64(buf, (uint64_t)pt->firstline);
}

static void stream_cfunc(struct lj_wbuf *buf, const GCfunc *func)
{
  lj_assertX(iscfunc(func), "bad C function in sysprof stream");
  lj_wbuf_addbyte(buf, LJP_FRAME_CFUNC);
  lj_wbuf_addu64(buf, (uintptr_t)func->c.f);
}

static void stream_ffunc(struct lj_wbuf *buf, const GCfunc *func)
{
  lj_assertX(isffunc(func), "bad fast function in sysprof stream");
  stream_ffunc_impl(buf, func->c.ffid);
}

static void stream_frame_lua(struct lj_wbuf *buf, const cTValue *frame)
{
  const GCfunc *func = frame_func(frame);
  lj_assertX(func != NULL, "bad function in sysprof stream");
  if (isluafunc(func))
    stream_lfunc(buf, func);
  else if (isffunc(func))
    stream_ffunc(buf, func);
  else if (iscfunc(func))
    stream_cfunc(buf, func);
  else
    /* Unreachable. */
    lj_assertX(0, "bad function type in sysprof stream");
}

static void stream_backtrace_lua(struct sysprof *sp, uint32_t vmstate)
{
  global_State *g = sp->g;
  struct lj_wbuf *buf = &sp->out;
  cTValue *top_frame = NULL, *frame = NULL, *bot = NULL, *end = NULL;
  lua_State *L = NULL;

  lj_assertX(g != NULL, "uninitialized global state in sysprof state");
  L = gco2th(gcref(g->cur_L));
  lj_assertG(L != NULL, "uninitialized Lua state in sysprof state");
  /*
  ** Lua stack may be inconsistent during the execution of a
  ** fast-function, so instead of updating the `top_frame` for
  ** it, its `ffid` is set instead. The first frame on the
  ** result stack is streamed manually, and the rest of the
  ** stack is streamed based on the previous `top_frame` value.
  */
  if (vmstate == LJ_VMST_FFUNC) {
    uint8_t ffid = g->top_frame_info.ffid;
    stream_ffunc_impl(buf, ffid);
  }

  /*
  ** The profiled thread may have been stopped before it reported its first
  ** frame (top_frame_info is only written on a vmstate transition), or right
  ** after a coroutine switch, when cur_L and top_frame_info disagree for a
  ** moment. Neither the sample point nor the stack belongs to us, so the
  ** frame pointer is validated before it is walked: a profiler must never
  ** crash the profiled VM.
  */
  bot = tvref(L->stack) + LJ_FR2;
  end = tvref(L->stack) + L->stacksize;
  top_frame = g->top_frame_info.top_frame;
#if LJ_TARGET_WINDOWS
  /*
  ** The VM does not save/restore the VM state around C calls on Windows
  ** (see save_vmstate/restore_vmstate in vm_x64.dasc), so while a C function
  ** runs the stashed base is not refreshed and may point at slots that have
  ** been reused since. L->base is kept up to date for the call itself, so it
  ** is the better starting point for a C sample.
  */
  if (vmstate == LJ_VMST_CFUNC)
    top_frame = L->base;
#endif
  /*
  ** A frame chain is only usable if it still lies inside the live part of the
  ** stack.
  */
  if (top_frame != NULL && top_frame <= L->top && --top_frame >= bot &&
      top_frame < end) {
    /* Traverse frames backwards. */
    for (frame = top_frame; frame > bot && frame < end;
	 frame = frame_prev(frame)) {
      if (frame_gc(frame) == obj2gco(L) || frame_isvarg(frame))
	continue;  /* Skip dummy frames. See lj_err_optype_call(). */
      /*
      ** The sample point is an arbitrary instruction, and on Windows the VM
      ** state is not restored around C calls (see save_vmstate in
      ** vm_x64.dasc), so a slot may already belong to the next frame. Only a
      ** function cell is dereferenced: anything else is a stale slot.
      */
      if (!tvisfunc((frame) - LJ_FR2))
	continue;
      stream_frame_lua(buf, frame);
    }
  }

  lj_wbuf_addbyte(buf, LJP_FRAME_LUA_LAST);
}

/*
** Start address of the function that contains pc, or 0 if it is unknown.
** Recorded with every host frame so that the two callchains of a sample can
** be merged without any symbol table: the profiles are usually collected
** from stripped binaries. On Windows the PE unwind info is always present,
** even in stripped images; elsewhere the function is left unknown for now
** and the merge falls back to a plain concatenation.
*/
static uintptr_t stream_func_begin(uintptr_t pc)
{
#if LJ_TARGET_WINDOWS
  DWORD64 base = 0;
  PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry((DWORD64)pc, &base, NULL);
  return rf != NULL ? (uintptr_t)(base + rf->BeginAddress) : 0;
#else
  UNUSED(pc);
  return 0;
#endif
}

static void *stream_frame_host(int frame_no, void *addr)
{
  struct sysprof *sp = &sysprof;
  uintptr_t pc = (uintptr_t)addr;
  uintptr_t func = 0;
  /*
  ** We don't want the profiler stack to be streamed, as it will
  ** burden the profile with unnecessary information.
  */
  if (LJ_UNLIKELY(frame_no <= SYSPROF_HANDLER_STACK_DEPTH))
    return addr;
  else if (LJ_UNLIKELY(sp->opt.mode == LUAM_SYSPROF_LEAF &&
                         frame_no > SYSPROF_HANDLER_STACK_DEPTH))
    return NULL;

  lj_wbuf_addu64(&sp->out, pc);
  if (pc != 0)
    func = stream_func_begin(pc);
  /* Offset within the function, plus one; zero means "unknown". */
  lj_wbuf_addu64(&sp->out, func != 0 && pc >= func ? pc - func + 1 : 0);
  return addr;
}

#if LJ_TARGET_WINDOWS && LJ_HASJIT
/* Remember the machine code range of a trace. Profiled thread only. */
static void sysprof_add_mcode(struct sysprof *sp, uintptr_t start, uint32_t size,
			      uint32_t traceno)
{
  if (size == 0 || sp->mcode == NULL || sp->nmcode >= sp->mcode_cap)
    return;
  sp->mcode[sp->nmcode].start = start;
  sp->mcode[sp->nmcode].size = size;
  sp->mcode[sp->nmcode].traceno = traceno;
  sp->nmcode++;
}

/* Snapshot the traces that exist when profiling starts. Profiled thread only. */
static void sysprof_collect_mcode(struct sysprof *sp)
{
  jit_State *J = G2J(sp->g);
  MSize n;

  /*
  ** The trace array never grows beyond JIT_P_maxtrace (lj_trace.c), so that
  ** is the exact bound for the ranges we may have to report. Allocated once,
  ** on the profiled thread, so the sampler never sees the array move.
  */
  sp->nmcode = 0;
  sp->mcode_cap = (uint32_t)J->param[JIT_P_maxtrace] + 1;
  sp->mcode = (struct sysprof_mcode *)malloc(sizeof(*sp->mcode) *
					     sp->mcode_cap);
  if (sp->mcode == NULL) {
    sp->mcode_cap = 0;
    return;
  }
  for (n = 1; n < J->sizetrace; n++) {
    GCtrace *T = traceref(J, n);
    if (T != NULL && T->mcode != NULL)
      sysprof_add_mcode(sp, (uintptr_t)T->mcode, T->szmcode, T->traceno);
  }
}

/* Trace entry whose machine code contains pc, NULL if none. */
static const void *sysprof_trace_at(const struct sysprof *sp, uintptr_t pc)
{
  uint32_t i;
  for (i = 0; i < sp->nmcode; i++) {
    if (pc >= sp->mcode[i].start &&
	pc < sp->mcode[i].start + sp->mcode[i].size)
      return &sp->mcode[i];
  }
  return NULL;
}

/*
** Skip a frame that cannot be unwound generically.
**
** JIT compiled code has no unwind information at all, and the hand-written
** data of the VM assembly is meant for the SEH dispatch path. Both are
** abandoned: the VM entry frame below them is exactly what the Win64 SEH
** contract describes (lj_err.c, buildvm_peobj.c), so its caller is reached
** at a fixed offset:
**
**   L->cframe      == RSP of the entry frame (establisher frame)
**   return address == [cframe + CFRAME_SIZE - 8]
**   caller's RSP   == cframe + CFRAME_SIZE
**
** This does not depend on the trace's own stack usage (spadjust), which is
** only an approximation at call sites inside the trace.
*/
static int sysprof_unwind_entry(const struct sysprof *sp, void **cfp,
				CONTEXT *ctx)
{
  uintptr_t cf = (uintptr_t)*cfp;
  uintptr_t ra, base;
  void *prev;

  if (cf == 0 || cf < sp->stack_low || cf + CFRAME_SIZE > sp->stack_high)
    return 0;
  ra = *(const uintptr_t *)(cf + CFRAME_SIZE - 8);
  if (ra == 0)
    return 0;
  if (RtlPcToFileHeader((PVOID)ra, (PVOID *)&base) == NULL &&
      sysprof_trace_at(sp, ra) == NULL)
    return 0;

  /*
  ** Each skipped frame consumes one entry frame: the next one, if any, is
  ** reached through the cframe chain. Without this the walk would jump back
  ** to the same caller every time the interpreter is entered again.
  */
  prev = cframe_prev(*cfp);
  *cfp = ((uintptr_t)prev >= sp->stack_low &&
	  (uintptr_t)prev + CFRAME_SIZE <= sp->stack_high) ? prev : NULL;

  ctx->Rip = ra;
  ctx->Rsp = cf + CFRAME_SIZE;
  return 1;
}
#endif

#if LJ_TARGET_WINDOWS
/* Is this address inside the VM assembly? */
static int sysprof_is_vm_func(const struct sysprof *sp, uintptr_t addr)
{
  uintptr_t fn = stream_func_begin(addr);
  int i;

  if (fn == 0)
    return 0;
  for (i = 0; i < sp->nvm_func; i++)
    if (sp->vm_func[i] == fn)
      return 1;
  return 0;
}

static void default_backtrace_host(void *(writer)(int frame_no, void *addr))
{
  struct sysprof *sp = &sysprof;
  CONTEXT ctx = sysprof_sample_ctx;
  void *cf;
  DWORD64 frame_base;
  DWORD64 est_frame;
  void *handler_data;
  int level;

  if (!sysprof_sample_ctx_valid)
    return;

  {
    global_State *g = sp->g;
    cf = g != NULL && gcref(g->cur_L) != NULL ?
	 gco2th(gcref(g->cur_L))->cframe : NULL;
  }

  for (level = 0; level < SYSPROF_BACKTRACE_FRAME_MAX; level++) {
    uintptr_t addr = (uintptr_t)ctx.Rip;
    PRUNTIME_FUNCTION func;

    if (addr == 0)
      break;
    /*
    ** Stop at the first frame that does not belong to a PE image. JIT
    ** compiled code has no unwind information at all (see the comment in
    ** lj_err.c), so the chain cannot be continued past it; the frame itself
    ** is still reported, the parser resolves its trace.
    */
    if (RtlPcToFileHeader((PVOID)addr, (PVOID *)&frame_base) == NULL) {
#if LJ_HASJIT
      const void *entry_ = sysprof_trace_at(sp, addr);
      if (entry_ != NULL) {
	if (!writer(level + 1, (void *)addr))
	  return;
	/* Continue on the other side of the trace. */
	if (sysprof_unwind_entry(sp, &cf, &ctx))
	  continue;
      }
#endif
      break;
    }

    if (!writer(level + 1, (void *)addr))
      return;

    /*
    ** The VM assembly is identified by its function entry (see
    ** lj_symtab_vm_functions): its hand-written unwind data is not meant
    ** for generic unwinding, a sample inside it is continued at the caller
    ** of the interpreter's entry frame instead.
    */
    if (sp->nvm_func > 0 && sysprof_is_vm_func(sp, addr)) {
      if (sysprof_unwind_entry(sp, &cf, &ctx))
	continue;
      break;
    }

    func = RtlLookupFunctionEntry(addr, &frame_base, NULL);
    if (func != NULL) {
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, frame_base, addr, func, &ctx,
                       &handler_data, &est_frame, NULL);
    } else {
      /*
      ** A leaf function without unwind data: its caller's return address
      ** is stored at the top of the stack.
      */
      void **retaddr = (void **)(uintptr_t)ctx.Rsp;
      if ((uintptr_t)retaddr < sp->stack_low ||
          (uintptr_t)retaddr + sizeof(void *) > sp->stack_high)
        break;
      ctx.Rip = (DWORD64)(uintptr_t)*retaddr;
      ctx.Rsp += sizeof(void *);
    }

    if ((uintptr_t)ctx.Rsp < sp->stack_low ||
        (uintptr_t)ctx.Rsp >= sp->stack_high)
      break;
  }
}
#else
static void default_backtrace_host(void *(writer)(int frame_no, void *addr))
{
  static void *backtrace_buf[SYSPROF_BACKTRACE_FRAME_MAX] = {};

  struct sysprof *sp = &sysprof;
  int max_depth = sp->opt.mode == LUAM_SYSPROF_LEAF
                  ? SYSPROF_HANDLER_STACK_DEPTH + 1
                  : SYSPROF_BACKTRACE_FRAME_MAX;
  const int depth = backtrace(backtrace_buf, max_depth);
  int level;

  lj_assertX(depth <= max_depth, "depth of C stack is too big");
  for (level = SYSPROF_HANDLER_STACK_DEPTH; level < depth; ++level) {
    if (!writer(level - SYSPROF_HANDLER_STACK_DEPTH + 1, backtrace_buf[level]))
      return;
  }
}
#endif

static void stream_backtrace_host(struct sysprof *sp)
{
  lj_assertX(sp->backtracer != NULL, "uninitialized sysprof backtracer");
  sp->backtracer(stream_frame_host);
  lj_wbuf_addu64(&sp->out, (uintptr_t)LJP_FRAME_HOST_LAST);
}

#if LJ_HASJIT
static void stream_trace(struct sysprof *sp, uint32_t vmstate)
{
  lj_wbuf_addbyte(&sp->out, (uint8_t)vmstate);
  struct lj_wbuf *out = &sp->out;
  uint32_t traceno = sp->g->vmstate;
  jit_State *J = G2J(sp->g);
  GCtrace *trace = traceref(J, traceno);

  GCproto *startpt = gco2pt(gcref(trace->startpt));

  lj_wbuf_addu64(out, traceno);
  lj_wbuf_addu64(out, (uintptr_t)startpt);
  lj_wbuf_addu64(out, startpt->firstline);
  /* The native callers of the trace: the frame in mcode is unwound through
  ** the trace's stack layout, see sysprof_unwind_jit().
  */
  stream_backtrace_host(sp);
}
#endif

static void stream_guest(struct sysprof *sp, uint32_t vmstate)
{
  lj_wbuf_addbyte(&sp->out, (uint8_t)vmstate);
  stream_backtrace_lua(sp, vmstate);
  stream_backtrace_host(sp);
}

static void stream_host(struct sysprof *sp, uint32_t vmstate)
{
  lj_wbuf_addbyte(&sp->out, (uint8_t)vmstate);
  stream_backtrace_host(sp);
}

typedef void (*event_streamer)(struct sysprof *sp, uint32_t vmstate);

static event_streamer event_streamers[] = {
  /* XXX: order is important */
  stream_host,  /* LJ_VMST_INTERP */
  stream_guest, /* LJ_VMST_LFUNC */
  stream_guest, /* LJ_VMST_FFUNC */
  stream_guest, /* LJ_VMST_CFUNC */
  stream_host,  /* LJ_VMST_GC */
  stream_host,  /* LJ_VMST_EXIT */
  stream_host,  /* LJ_VMST_RECORD */
  stream_host,  /* LJ_VMST_OPT */
  stream_host,  /* LJ_VMST_ASM */
#if LJ_HASJIT
  stream_trace  /* LJ_VMST_TRACE */
#endif
};

static void stream_event(struct sysprof *sp, uint32_t vmstate)
{
  event_streamer stream = NULL;

  lj_assertX(vmstfit4(vmstate), "vmstate don't fit in 4 bits");
  stream = event_streamers[vmstate];
  lj_assertX(stream != NULL, "uninitialized sysprof stream");
  stream(sp, vmstate);
}

/* -- Signal handler ------------------------------------------------------ */

/*
** Account the sample. The vmstate is a single aligned word written by the VM,
** so reading it from the sampler thread without stopping the profiled thread
** is as safe as reading it from a signal handler is on POSIX.
*/
static uint32_t sysprof_account(struct sysprof *sp)
{
  global_State *g = sp->g;
  uint32_t _vmstate = ~(uint32_t)(g->vmstate);
  uint32_t vmstate = _vmstate < LJ_VMST_TRACE ? _vmstate : LJ_VMST_TRACE;

  /* Caveat: order of counters must match vmstate order in <lj_obj.h>. */
  ((uint64_t *)&sp->counters)[vmstate]++;

  sp->counters.samples++;

  return vmstate;
}

#if LJ_TARGET_WINDOWS
/*
** Deferred stream writer.
**
** lj_wbuf flushes when it runs out of space, and the flush happens in the
** middle of a sample, i.e. while the profiled thread is stopped. A write()
** there would keep the thread stopped for the whole I/O, so the full buffer
** is swapped for a spare one and is written after the profiled thread has
** been resumed. The spare is the only extra memory the stream needs.
*/
static void sysprof_drain_stream(struct sysprof *sp)
{
  const void *data;
  size_t len, written;

  if (sp->pending_len == 0)
    return;

  data = sp->pending_buf;
  len = sp->pending_len;
  written = sp->writer(&data, len, sp->opt.ctx);

  if (LJ_UNLIKELY(written < len)) {
    sp->out.flags |= STREAM_ERRIO;
    sp->out.saved_errno = errno;
  }
  if (LJ_UNLIKELY(data == NULL)) {
    sp->out.flags |= STREAM_STOP;
    sp->out.saved_errno = errno;
  }

  sp->spare_buf = sp->pending_buf;
  sp->pending_buf = NULL;
  sp->pending_len = 0;
}

static size_t sysprof_deferred_writer(const void **data, size_t len, void *ctx)
{
  struct sysprof *sp = &sysprof;
  uint8_t *spare = sp->spare_buf;

  /*
  ** Outside the frozen window (prologue/epilogue), with no spare buffer or
  ** with a write already pending there is nothing to gain from deferring.
  */
  if (!sp->frozen || spare == NULL || sp->pending_len != 0)
    return sp->writer(data, len, ctx);

  sp->pending_buf = (uint8_t *)*data;
  sp->pending_len = len;
  sp->spare_buf = NULL;
  *data = spare;  /* lj_wbuf continues in the spare buffer. */

  return len;
}

/* Release the buffer owned by the deferred writer. */
static void sysprof_release_stream(struct sysprof *sp)
{
  sysprof_drain_stream(sp);
  free(sp->own_buf);
  sp->own_buf = sp->spare_buf = NULL;
}
#endif

#if LJ_TARGET_WINDOWS
static void sysprof_record_sample(struct sysprof *sp)
#else
static void sysprof_record_sample(struct sysprof *sp, siginfo_t *info)
#endif
{
  uint32_t vmstate;

#if !LJ_TARGET_WINDOWS
  UNUSED(info);
  lj_assertX(pthread_self() == sp->thread,
	     "bad thread during sysprof record sample");
#endif

  vmstate = sysprof_account(sp);

  if (!stream_is_needed(sp))
    return;

  stream_event(sp, vmstate);
  if (LJ_UNLIKELY(lj_wbuf_test_flag(&sp->out, STREAM_ERRIO|STREAM_STOP))) {
    sp->saved_errno = lj_wbuf_errno(&sp->out);
    lj_wbuf_terminate(&sp->out);
    sp->state = SPS_HALT;
  }
}

#if LJ_TARGET_WINDOWS
/*
** Take a sample of the profiled thread. The thread is stopped at an
** arbitrary instruction, its CONTEXT is saved for the host stack unwinder
** and the Lua stack is read from the global state, which the VM keeps
** consistent for the profiler at every vmstate transition (see vm_x64.dasc).
**
** The profiled thread is the only one mutating the Lua VM, so the state
** observed here is the same the POSIX backend observes from SIGPROF.
*/
static void sysprof_take_sample(struct sysprof *sp)
{
  if (SuspendThread(sp->thread_handle) == (DWORD)-1)
    return;

  sp->frozen = 1;

  /*
  ** The profiled thread writes the stream itself when it extends the symbol
  ** table (see lj_sysprof_add_proto): it announces that by leaving
  ** SPS_PROFILE. It is stopped now, so its state says whether the stream is
  ** ours: any other state means a symbol event may be half written.
  */
  if (sp->state != SPS_PROFILE) {
    sp->frozen = 0;
    ResumeThread(sp->thread_handle);
    return;
  }

  sysprof_sample_ctx_valid = 0;
  memset(&sysprof_sample_ctx, 0, sizeof(sysprof_sample_ctx));
  sysprof_sample_ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
  sysprof_sample_ctx_valid =
      GetThreadContext(sp->thread_handle, &sysprof_sample_ctx) != 0;

  sysprof_record_sample(sp);

  sp->frozen = 0;
  ResumeThread(sp->thread_handle);

  /*
  ** The buffer may have been flushed while the profiled thread was stopped:
  ** the write itself is deferred (see sysprof_deferred_writer) and is done
  ** here, when the profiled thread is running again.
  */
  sysprof_drain_stream(sp);
}
#endif

#if LJ_TARGET_WINDOWS
static void sysprof_signal_handler(void)
#else
static void sysprof_signal_handler(int sig, siginfo_t *info, void *ctx)
#endif
{
  struct sysprof *sp = &sysprof;
#if !LJ_TARGET_WINDOWS
  UNUSED(sig);
  UNUSED(ctx);
#endif

  switch (sp->state) {
    case SPS_PROFILE:
#if LJ_TARGET_WINDOWS
      /*
      ** Without a stream there is nothing to unwind for: the sample is the
      ** vmstate alone, so the profiled thread is not stopped at all.
      */
      if (stream_is_needed(sp))
	sysprof_take_sample(sp);
      else
	sysprof_account(sp);
#else
      sysprof_record_sample(sp, info);
#endif
      break;

    case SPS_IDLE:
    case SPS_HALT:
      /* noop */
      break;

    default:
      lj_assertX(0, "bad sysprof profiler state");
      break;
  }
}

/* -- Internal ------------------------------------------------------------ */

static int sysprof_validate(struct sysprof *sp,
                            const struct luam_Sysprof_Options *opt)
{
  switch (sp->state) {
    case SPS_IDLE:
      if (opt->mode > LUAM_SYSPROF_CALLGRAPH) {
        return PROFILE_ERRUSE;
      } else if (opt->mode != LUAM_SYSPROF_DEFAULT &&
                 (opt->buf == NULL || opt->len == 0 || is_unconfigured(sp))) {
        return PROFILE_ERRUSE;
      } else if (opt->interval == 0) {
        return PROFILE_ERRUSE;
      }
      break;

    case SPS_PROFILE:
    case SPS_HALT:
      return PROFILE_ERRRUN;

    default:
      lj_assertX(0, "bad sysprof profiler state");
      break;
  }

  return PROFILE_SUCCESS;
}

static int sysprof_init(struct sysprof *sp, lua_State *L,
                        const struct luam_Sysprof_Options *opt)
{
  const int status = sysprof_validate(sp, opt);
  if (PROFILE_SUCCESS != status)
    return status;

  /* Copy validated options to sysprof state. */
  memcpy(&sp->opt, opt, sizeof(sp->opt));

  /* Init general fields. */
  sp->g = G(L);
#if LJ_TARGET_WINDOWS
  {
    PNT_TIB tib;
    sp->thread = GetCurrentThreadId();
    /*
    ** GetCurrentThread() is a pseudo handle which cannot be used from the
    ** timer thread, so duplicate it. THREAD_SUSPEND_RESUME and
    ** THREAD_GET_CONTEXT are granted by DuplicateHandle for own threads.
    */
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                         GetCurrentProcess(), &sp->thread_handle, 0, FALSE,
                         DUPLICATE_SAME_ACCESS))
      return PROFILE_ERRIO;
    tib = (PNT_TIB)NtCurrentTeb();
    sp->stack_low = (uintptr_t)tib->StackLimit;
    sp->stack_high = (uintptr_t)tib->StackBase;
  }
#else
  sp->thread = pthread_self();
#endif

  /* Reset counters. */
  memset(&sp->counters, 0, sizeof(sp->counters));

  /* Reset saved errno. */
  sp->saved_errno = 0;

  if (stream_is_needed(sp))
    lj_wbuf_init(&sp->out, sp->writer, opt->ctx, opt->buf, opt->len);

  return PROFILE_SUCCESS;
}

/* -- Public profiling API ------------------------------------------------ */

int lj_sysprof_set_writer(luam_Sysprof_writer writer) {
  struct sysprof *sp = &sysprof;

  if (sp->state != SPS_IDLE || writer == NULL)
    return PROFILE_ERRUSE;

  sp->writer = writer;
  if (!is_unconfigured(sp)) {
    sp->state = SPS_IDLE;
  }
  return PROFILE_SUCCESS;
}

int lj_sysprof_set_on_stop(luam_Sysprof_on_stop on_stop) {
  struct sysprof *sp = &sysprof;

  if (sp->state != SPS_IDLE || on_stop == NULL)
    return PROFILE_ERRUSE;

  sp->on_stop = on_stop;
  if (!is_unconfigured(sp)) {
    sp->state = SPS_IDLE;
  }
  return PROFILE_SUCCESS;
}

int lj_sysprof_set_backtracer(luam_Sysprof_backtracer backtracer) {
  struct sysprof *sp = &sysprof;

  if (sp->state != SPS_IDLE)
    return PROFILE_ERRUSE;
  if (backtracer == NULL) {
    sp->backtracer = default_backtrace_host;
#if !LJ_TARGET_WINDOWS
    /*
    ** XXX: `backtrace` is not signal-safe, according to man,
    ** because it is lazy loaded on the first call, which triggers
    ** allocations. We need to call `backtrace` before starting profiling
    ** to avoid lazy loading.
    */
    void *dummy = NULL;
    backtrace(&dummy, 1);
#endif
  }
  else {
    sp->backtracer = backtracer;
  }
  if (!is_unconfigured(sp)) {
    sp->state = SPS_IDLE;
  }
  return PROFILE_SUCCESS;
}

int lj_sysprof_start(lua_State *L, const struct luam_Sysprof_Options *opt)
{
  struct sysprof *sp = &sysprof;

  int status = sysprof_init(sp, L, opt);
  if (PROFILE_SUCCESS != status) {
    if (NULL != sp->on_stop) {
      /*
      ** Initialization may fail in case of unconfigured sysprof,
      ** so we cannot guarantee cleaning up resources in this case.
      */
      sp->on_stop(opt->ctx, opt->buf);
    }
    return status;
  }

  sp->state = SPS_PROFILE;

  if (stream_is_needed(sp)) {
    stream_prologue(sp);
    if (LJ_UNLIKELY(lj_wbuf_test_flag(&sp->out, STREAM_ERRIO|STREAM_STOP))) {
      /* on_stop call may change errno value. */
      const int saved_errno = lj_wbuf_errno(&sp->out);
      /* Ignore possible errors. mp->out.buf may be NULL here. */
      sp->on_stop(opt->ctx, sp->out.buf);
      lj_wbuf_terminate(&sp->out);
      sp->state = SPS_IDLE;
#if LJ_TARGET_WINDOWS
      if (sp->thread_handle != NULL) {
	CloseHandle(sp->thread_handle);
	sp->thread_handle = NULL;
      }
#endif
      errno = saved_errno;
      return PROFILE_ERRIO;
    }
  }

#if LJ_TARGET_WINDOWS
  if (stream_is_needed(sp)) {
    /*
    ** If the allocation fails the writer simply keeps writing in place:
    ** deferring is an optimisation, not a requirement.
    */
    sp->prologue_c_pending = 1;
    sp->own_buf = (uint8_t *)malloc(sp->opt.len);
    /* Touch the buffer now: its page faults must not hit the frozen window. */
    if (sp->own_buf != NULL)
      memset(sp->own_buf, 0, sp->opt.len);
    sp->spare_buf = sp->own_buf;
    sp->pending_buf = NULL;
    sp->pending_len = 0;
    sp->out.writer = sysprof_deferred_writer;
  }
#endif

#if LJ_TARGET_WINDOWS
  /* Frames of the interpreter are recognised by these entries. */
  sp->nvm_func = lj_symtab_vm_functions(sp->vm_func,
					(int)(sizeof(sp->vm_func) /
					      sizeof(sp->vm_func[0])));
#if LJ_HASJIT
  if (stream_is_needed(sp))
    sysprof_collect_mcode(sp);
#endif
#endif

  sp->timer.opt.interval_msec = opt->interval;
  sp->timer.opt.handler = sysprof_signal_handler;
#if LJ_TARGET_WINDOWS
  sp->timer.opt.startup = sysprof_startup;
#endif
  lj_profile_timer_start(&sp->timer);

  return PROFILE_SUCCESS;
}

int lj_sysprof_stop(lua_State *L)
{
  struct sysprof *sp = &sysprof;
  global_State *g = sp->g;
  struct lj_wbuf *out = &sp->out;

  if (SPS_IDLE == sp->state)
    return PROFILE_ERRRUN;
  else if (G(L) != g)
    return PROFILE_ERRUSE;

  lj_profile_timer_stop(&sp->timer);

#if LJ_TARGET_WINDOWS
  if (sp->thread_handle != NULL) {
    CloseHandle(sp->thread_handle);
    sp->thread_handle = NULL;
  }
  /* The timer thread is joined, so no sample can be in flight. */
  sysprof_drain_stream(sp);
#endif

  if (SPS_HALT == sp->state) {
    errno = sp->saved_errno;
    sp->state = SPS_IDLE;
    /* wbuf was terminated when error occurred. */
#if LJ_TARGET_WINDOWS
    sysprof_release_stream(sp);
#endif
    return PROFILE_ERRIO;
  }

  sp->state = SPS_IDLE;

  if (stream_is_needed(sp)) {
    int cb_status = 0;

    stream_epilogue(sp);
    lj_wbuf_flush(out);

    cb_status = sp->on_stop(sp->opt.ctx, out->buf);
    if (LJ_UNLIKELY(lj_wbuf_test_flag(out, STREAM_ERRIO | STREAM_STOP)) ||
        cb_status != 0) {
      errno = lj_wbuf_errno(out);
      lj_wbuf_terminate(out);
#if LJ_TARGET_WINDOWS
      sysprof_release_stream(sp);
#endif
      return PROFILE_ERRIO;
    }

    lj_wbuf_terminate(out);
  }

#if LJ_TARGET_WINDOWS
  sysprof_release_stream(sp);
#endif
#if LJ_HASJIT
  free(sp->mcode);
  sp->mcode = NULL;
  sp->nmcode = sp->mcode_cap = 0;
#endif
  return PROFILE_SUCCESS;
}

int lj_sysprof_report(struct luam_Sysprof_Counters *counters)
{
  const struct sysprof *sp = &sysprof;
  if (sp->state != SPS_IDLE)
    return PROFILE_ERRUSE;
  memcpy(counters, &sp->counters, sizeof(sp->counters));
  return PROFILE_SUCCESS;
}

void lj_sysprof_add_proto(const struct GCproto *pt)
{
  struct sysprof *sp = &sysprof;

  if (sp->state != SPS_PROFILE || sp->opt.mode == LUAM_SYSPROF_DEFAULT)
    return;
#if LJ_TARGET_WINDOWS
  /* The profiler thread still owns the stream while it dumps the C symbols. */
  if (sp->prologue_c_pending)
    return;
#endif

  /*
  ** XXX: Avoid sampling during the symtab extension. That shouldn't have any
  ** significant effect on profile precision, but if it does, it's better to
  ** implement an async-safe queue for the symtab events.
  */
  sp->state = SPS_IDLE;
  lj_wbuf_addbyte(&sp->out, LJP_SYMTAB_LFUNC_EVENT);
  lj_symtab_dump_proto(&sp->out, pt);
  sp->state = SPS_PROFILE;
}

#if LJ_HASJIT
void lj_sysprof_add_trace(const struct GCtrace *tr)
{
  struct sysprof *sp = &sysprof;

  if (sp->state != SPS_PROFILE || sp->opt.mode == LUAM_SYSPROF_DEFAULT)
    return;
#if LJ_TARGET_WINDOWS
  /*
  ** The machine code snapshot must be complete for every trace created while
  ** profiling, including those created before the timer thread finished the C
  ** symbol dump: the sampler reads the snapshot to recognise JIT frames, and
  ** a missing range silently truncates every host chain that runs through it.
  ** Appending is safe here, it does not touch the stream that the timer
  ** thread may still own.
  */
  sysprof_add_mcode(sp, (uintptr_t)tr->mcode, tr->szmcode, tr->traceno);
  if (sp->prologue_c_pending)
    return;
#endif

  /* See the comment about the sysprof state above. */
  sp->state = SPS_IDLE;
  lj_wbuf_addbyte(&sp->out, LJP_SYMTAB_TRACE_EVENT);
  lj_symtab_dump_trace(&sp->out, tr);
  sp->state = SPS_PROFILE;
}
#endif /* LJ_HASJIT */

#if LJ_HASJIT
/*
** The trace is being flushed or freed. Forget its machine code range while
** the sampler may still be scanning the snapshot: the size is cleared first,
** so a racing lookup can no longer match the entry.
*/
void lj_sysprof_del_trace(const struct GCtrace *tr)
{
  struct sysprof *sp = &sysprof;
  uint32_t i;

  if (sp->mcode == NULL)
    return;
  for (i = 0; i < sp->nmcode; i++) {
    if (sp->mcode[i].traceno == (uint32_t)tr->traceno &&
	sp->mcode[i].start == (uintptr_t)tr->mcode) {
      sp->mcode[i].size = 0;
      sp->mcode[i].traceno = 0;
      break;
    }
  }
}
#endif /* LJ_HASJIT */

#else /* LJ_HASSYSPROF */

int lj_sysprof_set_writer(luam_Sysprof_writer writer) {
  UNUSED(writer);
  return PROFILE_ERRUSE;
}

int lj_sysprof_set_on_stop(luam_Sysprof_on_stop on_stop) {
  UNUSED(on_stop);
  return PROFILE_ERRUSE;
}

int lj_sysprof_set_backtracer(luam_Sysprof_backtracer backtracer) {
  UNUSED(backtracer);
  return PROFILE_ERRUSE;
}

int lj_sysprof_start(lua_State *L, const struct luam_Sysprof_Options *opt)
{
  UNUSED(L);
  return PROFILE_ERRUSE;
}

int lj_sysprof_stop(lua_State *L)
{
  UNUSED(L);
  return PROFILE_ERRUSE;
}

int lj_sysprof_report(struct luam_Sysprof_Counters *counters)
{
  UNUSED(counters);
  return PROFILE_ERRUSE;
}

void lj_sysprof_add_proto(const struct GCproto *pt)
{
  UNUSED(pt);
}

#if LJ_HASJIT
void lj_sysprof_add_trace(const struct GCtrace *tr)
{
  UNUSED(tr);
}

void lj_sysprof_del_trace(const struct GCtrace *tr)
{
  UNUSED(tr);
}
#endif /* LJ_HASJIT */

#endif /* LJ_HASSYSPROF */

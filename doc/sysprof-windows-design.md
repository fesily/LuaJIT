# sysprof and jit.profile on Windows: design and v2.1 backport notes

This document describes the Windows port of the platform profiler (`sysprof`)
and the host-callchain support added to the low-overhead profiler
(`jit.profile`), organized by the VM modules that had to change. It doubles
as the porting checklist for moving the work onto a branch based on LuaJIT
v2.1.

Status: implemented and verified on Windows x64 against the tarantool
`master` tree (MinGW build, GC64). The POSIX backend of sysprof is
untouched; the ELF parts of the symbol table are unchanged.

## 1. Sampling architecture

POSIX takes the sample from a signal delivered *on the profiled thread*.
Windows has no analogue, so the sample point is taken from the timer thread:

```
timer thread (LJ_PROFILE_WTHREAD)          profiled thread (the VM)
  Sleep(interval)                          running Lua / GCC / JIT
  SuspendThread(target) ---------------->  stopped at an arbitrary instruction
  GetThreadContext(CONTROL|INTEGER)        (CONTEXT is a register snapshot)
  record sample:
    counters from g->vmstate
    Lua chain from g->top_frame_info
    host chain from CONTEXT (RtlVirtualUnwind)
  ResumeThread(target) ------------------>  runs again
  write/flush outside the frozen window
```

Consequences that shaped the module layout:

* Every read of VM state happens while the thread is stopped, so the
  invariants the POSIX signal handler relies on (`g->vmstate` ordering,
  `top_frame_info`) still hold — except that the sampler is a *different*
  thread, which matters for anything the VM writes lazily (see §3.2).
* The frozen window is time stolen from the profiled VM. Everything that can
  run outside it does: the C symbol table is dumped by the timer thread
  before the first sample, and stream flushes are deferred until after
  `ResumeThread` (`sysprof_deferred_writer`).

`jit.profile` keeps its cooperative sampling (a hook at VM safepoints) and
only adds host stacks: the callback already runs on the profiled thread, so
the stack is read there, synchronously, without stopping anything.

## 2. Data path

```
sampler ──► ljp stream (samples) ──► tools/sysprof/parse.lua ──► report
        └─► ljs symtab (Lua protos/traces, C symbols, VM markers, mcode)
                                      ▲
tools/utils/symtab.lua ───────────────┘   tools/utils/symresolver.lua
                                          (debug info / map file / embedded)
```

The stream is self-describing on purpose: C symbols are collected while the
profile is taken, so the analysis does not need the binaries. Two optional
sources of names can refine it (§8).

## 3. VM modules

### 3.1 `lj_arch.h` — feature gating

```
LJ_HASSYSPROF = 1 for LJ_TARGET_LINUX or (LJ_TARGET_WINDOWS && LJ_TARGET_X64)
LJ_HASRESOLVER remains Linux-only (ELF backend)
LJ_HASMEMPROF remains 0 on Windows
```

**Dynasm must be told about the feature.** The VM assembly only stashes the
Lua frame pointer under `|.if LJ_HASSYSPROF`, and that conditional is
evaluated by dynasm, not by the C preprocessor. Every build system needs to
pass `-D LJ_HASSYSPROF` when the macro is 1:

* CMake: `cmake/SetDynASMFlags.cmake` (already does it for tarantool);
* `src/Makefile`: `DASM_AFLAGS += -D LJ_HASSYSPROF` when
  `$(TARGET_TESTARCH)` contains `LJ_HASSYSPROF 1`;
* `src/msvcbuild.bat` (v2.1): add the same flag to `DASMFLAGS`.

Failure mode if skipped: `g->top_frame_info.top_frame` is never written, the
sampler walks a NULL frame and crashes on the first guest sample.

### 3.2 `lj_obj.h` + `vm_x64.dasc` — the Lua stack snapshot

`global_State` carries `struct lj_sysprof_topframe { uint8_t ffid; TValue
*top_frame; }`, and the interpreter stashes it at every vmstate transition:

```text
set_vmstate_sync_base(st):          # LFUNC, CFUNC, ...
    set_vmstate INTERP              # guard against a non-atomic context store
    top_frame_info.top_frame = BASE
    set_vmstate st

set_vmstate_ffunc:                  # fast functions: BASE is unusable
    set_vmstate INTERP
    top_frame_info.ffid = callee->ffid
    set_vmstate FFUNC
```

This is what makes it safe to read the Lua stack from another thread at an
arbitrary instruction: a sample that observes `INTERP` is streamed without a
Lua chain, and the non-atomic update window is hidden behind that state.

**Windows deviation (must be handled by any port):** `save_vmstate` and
`restore_vmstate` are compiled out on `WIN` in `vm_x64.dasc`. The VM state
and the stashed base are therefore not restored around C calls, so a sample
taken while a C function runs may see a stale `top_frame_info.top_frame`
that points at stack slots which have been reused since.

The sampler defends itself in `stream_backtrace_lua()`:

* for `LJ_VMST_CFUNC` on Windows the walk starts from `L->base` (kept up to
  date for the call) instead of the stashed base;
* the stashed frame must lie in `[stack + LJ_FR2, L->top)` and inside the
  allocated stack, otherwise the Lua chain is emitted empty;
* every step is bounded by the stack end, so a stale chain cannot run away.

A port may either keep this defence or re-enable save/restore on WIN; the
defence is needed for the first case and harmless for the second.

### 3.3 `lj_bcread.c`, `lj_parse.c` — new prototypes

`lj_sysprof_add_proto(pt)` is called right after a prototype is created, so
the symbol table can be extended while profiling (the stream carries
`SYMTAB_LFUNC` events). Both call sites are part of the port.

### 3.4 `lj_trace.c` — trace lifecycle

Three hooks, all on the profiled thread:

| hook | site | purpose |
|---|---|---|
| `lj_sysprof_add_trace(T)` | `trace_save()` | dump the trace record and extend the mcode snapshot |
| `lj_sysprof_del_trace(T)` | `lj_trace_free()` | forget the range of a freed trace |
| `lj_sysprof_del_trace(T)` | `lj_trace_flushall()` | **separate path**: flushall calls `lj_gdbjit_deltrace()` directly, it does not go through `lj_trace_free()` |

Hooking only the free path leaves flushed traces in the snapshot, which
mis-attributes later samples to a dead trace number.

### 3.5 `lj_sysprof.c/.h` — the sampler

Portable parts (shared with the POSIX backend):

* counters by vmstate, `LUAM_SYSPROF_DEFAULT` without a stream;
* the stream: prologue, per-vmstate event layout, symbol extension events,
  epilogue, error/halt handling;
* the Lua callchain walk (`stream_backtrace_lua`);
* `lj_sysprof_add_proto()` / `add_trace()` / `del_trace()` hooks;
* the trace machine code snapshot (see §3.5.1).

Windows parts (`#if LJ_TARGET_WINDOWS`):

| piece | why |
|---|---|
| timer-thread sampling (`SuspendThread`/`GetThreadContext`) | no signals |
| host unwinder (`RtlPcToFileHeader` / `RtlLookupFunctionEntry` / `RtlVirtualUnwind`, leaf fallback) | `backtrace()` only works on the current thread |
| JIT frame and VM assembly frame skip through `L->cframe` (§3.5.2), with a cursor that follows `cframe_prev()` | mcode has no `.pdata`; the VM `.xdata` is for SEH dispatch only |
| deferred writer + spare buffer | a flush must not run while the VM is frozen |
| C-symbol dump on the timer thread (`stream_prologue_c`, timer `startup` hook) | the profiled thread owns the GC heap |
| stream error re-check after `SuspendThread` | the VM writes symbol events itself (see §6) |

#### 3.5.1 Trace machine code snapshot

The sampler must be able to answer "which trace contains this PC" without
touching the JIT state (the thread may be suspended in the middle of a
reallocation of `J->trace`). It keeps a flat array:

```c
struct sysprof_mcode { uintptr_t start; uint32_t size; uint32_t traceno; };
```

* capacity is `J->param[JIT_P_maxtrace] + 1`, the exact bound of
  `J->sizetrace` (`lj_trace.c`), allocated once when profiling starts;
* filled at start (existing traces) and from `lj_sysprof_add_trace()`;
* `lj_sysprof_del_trace()` clears `size` first and `traceno` second, so a
  racing lookup stops matching before the number disappears;
* the sampler only reads it; no allocation happens while sampling.

#### 3.5.2 Unwinding past JIT code

JIT code is not described by `.pdata`. Instead of guessing the trace frame
size (`spadjust` is only an approximation at call sites inside a trace), the
sampler abandons the trace frame and resumes at the VM entry frame below it,
which the Win64 SEH contract pins down:

```text
L->cframe        == RSP of the entry frame (establisher frame)
return address   == [cframe + CFRAME_SIZE - 8]
caller's RSP     == cframe + CFRAME_SIZE
```

The addresses are validated (stack bounds, and the return address must be in
a PE image or in another trace) before the walk continues, so a broken
invariant truncates the chain instead of crashing.

### 3.6 `lj_symtab.c/.h` — symbol table

The module is split by responsibility:

| part | platform | thread | notes |
|---|---|---|---|
| `lj_symtab_dump_proto`, `dump_trace` | all | profiled | serialize Lua-side symbols |
| `lj_symtab_dump_lua` (prologue: header + protos/traces) | Windows | profiled | reads the GC heap |
| `lj_symtab_dump_c` (prologue: VM markers + C symbols + final) | Windows | timer | no Lua state |
| `lj_symtab_cfuncs(cb, ctx)` | Windows | any | reusable C symbol collector: modules → COFF symbol table → export directory → image base |
| ELF backend (`resolve_symbolnames`, `dump_sht_symtab`, ...) | Linux | profiled | unchanged |
| `lj_symtab_dump` (combined) | Linux | profiled | used by memprof and the POSIX prologue |

Rules that the Windows code depends on:

* the C dump uses `malloc`, not the Lua allocator (`lj_mem_new` is not
  thread-safe); no Lua state is touched;
* the symbol table records the load address and path of every image (not
  only as a fallback), because the tools use it to turn load addresses into
  link-time addresses;
* `SYMTAB_VM` marks the function(s) that cover the VM assembly: they are
  found with `RtlLookupFunctionEntry()` on `lj_vm_asm_begin` and
  `lj_vm_profhook`, plus the second half of the blob (buildvm splits `.text`
  at `vm_ffi_call`, so the end of the first entry is the start of the
  second). `lj_symtab_vm_functions()` is shared by the symtab dump and by
  the sampler, which uses the entries to recognise — and skip — interpreter
  frames; the tools then drop them without symbols.

### 3.7 `lj_profile_timer.c/.h` — the timer thread

Two additions to the existing `LJ_PROFILE_WTHREAD` backend:

* an optional `startup` callback, invoked once on the timer thread before
  the first tick. sysprof uses it to dump the C symbols off the profiled
  thread; `jit.profile` leaves it NULL;
* the timer resolution is requested as 1 ms instead of the sampling
  interval, otherwise `Sleep()` overshoots the requested interval several
  times over.

### 3.8 `lj_profile.c/.h` — jit.profile

New, all on the profiled thread at the safepoint (no suspension):

* mode option `h` enables host sampling;
* `profile_capture_mixed()` reads the host stack with
  `RtlCaptureStackBackTrace(skip = 4)`, skipping the profiler's own frames,
  and collects the Lua chain with `lj_debug_frame()`;
* the merge splices host frames in at every C function of the Lua chain,
  matching by **entry address** (from `RtlLookupFunctionEntry`); fast
  functions keep their `[builtin#N]` marker and are followed by the host
  frames of their implementation; VM assembly frames are dropped (identified
  through the same two PE unwind entries as `SYMTAB_VM`);
* `luaJIT_profile_hoststack()` and `luaJIT_profile_mixedstack()` expose the
  raw and merged chains.

### 3.9 `lj_debug.c/.h`

`debug_putchunkname()` became `lj_debug_putchunkname()` and is exported, so
the mixed-stack formatter prints `chunk:line` with exactly the same rules as
`dumpstack`.

### 3.10 API and library surface

| file | addition |
|---|---|
| `luajit.h` | `luaJIT_profile_hoststack`, `luaJIT_profile_mixedstack`, `luaJIT_profile_symtab`, `LUAJIT_PROFILE_HOSTSTACK_MAX` |
| `lib_jit.c` | `jit.profile.hoststack`, `jit.profile.mixedstack`, `jit.profile.symtab` |
| `lib_misc.c` | `misc.sysprof` is registered on Windows (`#if LJ_HASSYSPROF`); the profile file is opened with `O_BINARY` (text mode would expand `0x0a` to `0x0d 0x0a` and corrupt the stream) |
| `luajit.c` | tarantool-only `-t<cmd>` tool runner; needed for `-ts` (§8) |

## 4. Stream formats

Both formats are versioned and the tools accept a range, so an older
profile stays readable.

### 4.1 ljp (event stream), version 4

```text
sample-guest := sample-header stack-lua stack-host
sample-host  := sample-header stack-host
sample-trace := sample-header traceno sym-addr line-no stack-host   (v4+: host chain)
frame-host   := exec-addr func-off                                   (v3+)
func-off     := <ULEB128>  exec-addr - function start + 1, 0 = unknown
```

`func-off` is what makes the Lua/host merge work without symbols; the trace
sample's host chain is what gives JIT samples their native callers.

### 4.2 ljs (symbol table), version 5

| record | payload |
|---|---|
| `SYMTAB_LFUNC` | proto address, chunk name, firstline |
| `SYMTAB_CFUNC` | address, name (also image base + path entries) |
| `SYMTAB_TRACE` | traceno, proto address, line, **mcode, mcode size** (v5+) |
| `SYMTAB_VM` | address of a VM assembly function (v4+) |
| `SYMTAB_FINAL` | – |

## 5. Concurrency rules

| data | writer | reader | rule |
|---|---|---|---|
| `ljp` stream | timer thread (prologue-C, samples), profiled thread (proto/trace events) | – | the profiled thread sets `SPS_IDLE` around a symbol event; the sampler re-checks the state *after* `SuspendThread` and skips the sample if it is not `SPS_PROFILE` |
| `lj_wbuf` buffer | sampler | – | flush swapped out while frozen, written after `ResumeThread` |
| `lj_symtab` | timer thread (C part) | – | `malloc` only, no Lua allocator |
| mcode snapshot | profiled thread | sampler | append + clear-range; the sampler never allocates or walks JIT state |
| `host_frames` (jit.profile) | profiled thread | profiled thread (callback) | no lock needed |

## 6. Where the profiled-thread side must stay single-writer

`lj_sysprof_add_proto()`/`add_trace()` run on the profiled thread and append
to the stream. On POSIX this is safe because the signal handler runs on the
same thread; on Windows the sampler is a different thread, hence:

* the state protocol above, plus
* `prologue_c_pending`, which makes those hooks return early while the timer
  thread still owns the stream (C symbol dump).

## 7. Build system

* `cmake/SetDynASMFlags.cmake`: pass `-D LJ_HASSYSPROF` (already present in
  tarantool).
* `src/msvcbuild.bat` (v2.1): add `-D LJ_HASSYSPROF` to `DASMFLAGS`, and add
  the new translation units to the compile list. Note that a v2.1 tree does
  not have the profiler support files at all: `lj_sysprof.c`, `lj_symtab.c`,
  `lj_profile_timer.c`, `lj_wbuf.c`, `lj_utils_leb128.c`, `lmisclib.h` and
  `lib_misc.c` come from the tarantool fork and must be brought along with
  the code that uses them (`misc.*` registration, `lj_symtab` collector).
* The `src/Makefile` + `src/Makefile.dep` in this tree are **not** part of
  the port: they are the upstream v2.1 makefiles brought in to build the
  tarantool sources with MinGW on Windows. A v2.1 branch has its own
  Makefile and `msvcbuild.bat`.

## 8. Tools

| file | change |
|---|---|
| `tools/utils/symtab.lua` | ljs 3..5, module base entries, VM markers, trace ranges, `trace_at()` |
| `tools/sysprof/parse.lua` | ljp 2..4; deterministic merge by `func-off`; `TRACE [N]` for mcode addresses |
| `tools/utils/symresolver.lua` | C name sources: DbgHelp/PDB (FFI, batched), linker map (GNU/MSVC), embedded table; 64-bit-safe address parsing |
| `tools/sysprof.lua` | `-ts` entry point, `--map=`, `--debug=`, `--addr2line=` (two-pass collection + batched resolution) |
| `doc/ext_profiler.html` | `h` mode, `profile.hoststack`, `profile.mixedstack`, `profile.symtab`, C APIs |

`-ts` needs `luajit.c`'s `-t<cmd>` support (tarantool extension): it does
`require("sysprof")` and calls the returned function with `arg`. A v2.1
branch either ports that option or runs the same module explicitly.

## 9. Port checklist for v2.1

1. `lj_arch.h`: gating. **Then** make every build system pass
   `-D LJ_HASSYSPROF` to dynasm (§3.1). Verifying step: a guest sample must
   produce a non-empty Lua chain; otherwise the VM never stashed BASE.
2. `lj_obj.h` + `vm_x64.dasc`: the stash macros (already in the tarantool
   tree; re-apply to v2.1's dasc) and the `WIN` caveat handling in
   `stream_backtrace_lua()`.
3. `lj_bcread.c`, `lj_parse.c`: `lj_sysprof_add_proto()` call sites.
4. `lj_trace.c`: `add_trace` in `trace_save`, `del_trace` in **both**
   `lj_trace_free` and `lj_trace_flushall`.
5. `lj_sysprof.c/.h`, `lj_symtab.c/.h`, `lj_profile_timer.c/.h`,
   `lj_profile.c/.h`, `lj_debug.c/.h`: new code and the small exported
   helpers.
6. `lib_misc.c`: `#if LJ_HASSYSPROF` registration, `O_BINARY` output.
7. `lib_jit.c`, `luajit.h`: new APIs; `luajit.c`: `-t` runner (optional).
8. Build files (§7).
9. Tools and `doc/ext_profiler.html`.
10. Optional: `test/win-probes/veh-unwind-probe.c` as the unwinder oracle.

Each step has a failure mode worth checking explicitly, because most of them
fail silently: a missing dynasm flag crashes on the first sample, a missing
`del_trace` hook mis-attributes samples after a flush, a missing
`prologue_c_pending` check corrupts the stream, and text-mode file output
corrupts the first symbol with a newline in it.

## 10. Verification

* `misc.sysprof` smoke: `mode="D"` (counters only), `"L"`, `"C"`; the parsed
  event count must equal `samples`, and `tools/sysprof.lua` must parse the
  file.
* Mixed stacks: `jit.profiling` with `-joff` and with JIT; the merged chain
  must show Lua frames, C frames and `[builtin#N]` in program order.
* JIT frames: a workload that calls a C function from a trace must produce
  `lua_pcall; TRACE [N]; ...` chains, and `jit.flush()` in the middle of a
  profile must not crash and must not attribute post-flush samples to the
  dead trace.
* `test/win-probes/veh-unwind-probe.exe 300` must exit 0 and report
  agreement between the manual unwinder and
  `RtlCaptureStackBackTrace` for every sample.

## 11. v2.1 port status

Branch `sysprof-v2.1` (worktree of `fesily/LuaJIT` at `dd3b2436`, LuaJIT 2.1
with GC64, LJ_FR2). Built and exercised on Windows x64 with MSVC
(`msvcbuild.bat`, VS 18 / v145 toolset) and MinGW-w64 gcc 16.2
(`src/Makefile`, MSYS shell).

### 11.1 What the port needed beyond the tarantool tree

* `lj_obj.h`: `struct lj_sysprof_topframe` in `global_State`, the extended
  vmstate enum (`LFUNC`, `FFUNC`, `CFUNC`, `LJ_VMST_TRACE`), and
  `LJ_VMST_C` renamed in the `*.dasc` files;
* `vm_x64.dasc`: `TMPRb` alias, and the `set_vmstate_sync_base` /
  `set_vmstate_ffunc` macros plus their seven LFUNC/CFUNC transition sites
  re-applied to the v2.1 interpreter (the tarantool hunks do not apply: the
  dasc differs);
* build files: `-D LJ_HASSYSPROF` for **dynasm** (`msvcbuild.bat`
  `DASMFLAGS`, `Makefile` `DASM_AFLAGS`), the new objects, `-ldbghelp`
  (`Makefile`), `dbghelp.lib` (MSVC link), `lib_misc.o` in the `libdef`
  input list, and a `$(LJLIB_O): lj_libdef.h` dependency;
* `luajit.h` does not exist in v2.1: the new APIs go into
  `luajit_rolling.h`, from which `host/genversion.lua` generates `luajit.h`;
* `lj_profile.c`: v2.1 keeps `lj_profile_lock()`/`lj_profile_unlock()` for
  `lj_dispatch_update(g, nolock)`; the ported file calls
  `lj_dispatch_update(g, 1)` because it always holds the profile lock;
* `lib_misc.c`: `SBuf` in v2.1 is `char *w, *e, *b` (no `sbufB()`), there is
  no `ssize_t`, and the POSIX I/O names are spelled `_write`/`_open`/
  `_close` by MSVC;
* `luajit.c`: the `-t<cmd>` tool runner (`-ts`), with
  `createargtable(..., argn - 1)` for tools so that `arg[1]` is the input
  file.

### 11.2 Verified on the v2.1 tree

| Check | Result |
|---|---|
| `misc.sysprof.start{mode="D"}` counters | `report()` counts samples and per-vmstate hits |
| `mode="C"` / `"L"`, interpreter (`jit.off()`) | Lua chain with `chunk:line`, C frames (`string.format`, `ffi.meta.__call`, `sqrt`), host frames from `RtlUserThreadStart` down |
| JIT samples (`mode="C"`, JIT on) | `...;lua_pcall;TRACE [N];` plus the trace's callees, callers continue down to `mainCRTStartup` |
| parsed events == `report().samples` | 158/158, 152/152, 500/500, 527/527 on four runs |
| `jit.flush()` every round during a `mode="C"` profile | clean, samples still attributed after the flush, `TRACE [N]` in the report |
| `-ts <file>` (`tools/sysprof.lua`) | parses, prints `chain\tcount`, trace names resolved |
| `jit.profile` `h` mode | `profile.hoststack()`, `profile.mixedstack()` (Lua + native frames), `profile.symtab()` (12000+ C symbols) |
| soak: `misc.sysprof.start`/`stop` in a loop, `jit.off()`, `mode="C"`/`"L"` | ~1500 sessions on MSVC + MinGW, no crash |
| MSVC and MinGW builds | both build the same sources and reproduce the numbers above |

Host symbol names are those of the embedded COFF/export table (nearest
export for internal functions: a `calloc` frame may be shown as
`luaopen_jit`); `--map=` / `--debug=` refine them. A map of one module is
applied to that module only.

### 11.3 Defects found while verifying (fixed)

Four independent bugs produced the symptoms that the tarantool tree did not
show. All of them failed silently or crashed only under load.

**a. Incomplete machine code snapshot.** `lj_sysprof_add_trace()` returned
before updating the snapshot while `prologue_c_pending` (the timer thread
was still dumping C symbols). Every trace compiled in that window — in
practice the first traces of a profile — was then unknown to the sampler:
`RtlPcToFileHeader()` failed, `sysprof_trace_at()` failed, and the walk
stopped at the JIT frame, so all callers below a trace were missing and the
chain degraded into the leaf/VM fallback (the "repeated frames" symptoms).
The snapshot is now updated before the early return; it is plain data, so
appending it does not touch the stream that the timer thread may own.

**b. Generic unwinding through the VM assembly.** The `.xdata` that
`host/buildvm_peobj.c` emits describes the interpreter frame for the SEH
dispatch path; `RtlVirtualUnwind` is not guaranteed to agree with it. Under
load it faulted inside `ntdll!RtlVirtualUnwind` (read at a near-NULL
address) — the sampler crashed the process. VM frames are now recognised by
their function entry (`lj_symtab_vm_functions()`, shared with the symtab
dump) and skipped with the entry-frame contract instead of being unwound.

**c. The skipped frames consumed the wrong entry frame.** The end of a
chain that re-enters the VM (`lua_pcall` → interpreter → ...) made the walk
reuse the same `L->cframe` and loop (`lua_pcall; main; lua_pcall; ...`
until the frame limit). The walk now keeps a cursor into the cframe chain
(`cframe_prev()`) and consumes one entry frame per skipped frame.

**d. A stale Lua frame slot was dereferenced.** A sample taken while the
stack is mid-update (Windows does not restore the VM state around C calls)
can leave a slot that is no longer a function cell; `frame_func()` then read
freed memory and the sampler crashed. Only cells that pass
`tvisfunc(frame - LJ_FR2)` are dereferenced now.

Tool side: a trace sample now keeps the trace at its real depth
(`...;lua_pcall;TRACE [N];sqrt`), and `utils/symresolver.lua` no longer
applies a map to addresses of a different module (it checks that the
resolved link-time address lies inside the map range).

# Interpreter-only tail-call compatibility

Date: 2026-10-19. Base: `f1eb755a` on `dontstarve`.

Lua 5.1 tail-call compatibility is supported only in interpreted functions.
Compiled traces retain ordinary LuaJIT tail-call optimization, without virtual
tail counts or Lua 5.1 debug-frame/hook guarantees. Enabling these macros does
not make compiled call chains compatible.

## Switches

| Switch | Default | Retained behavior |
| --- | --- | --- |
| `LUA_COMPAT_TAILCALL_COUNT` | `1` on x64 with `LUAJIT_ENABLE_LUA51COMPAT`, otherwise `0` | Interpreter counter array, virtual tail levels, tail-return hook events |
| `LUA_COMPAT_TAILCALL_DEBUG` | `LUA_COMPAT_TAILCALL_COUNT` | Extended `debug.getinfo(..., "t").istailcall` support |
| `LUA_COMPAT_TAILCALL_CFRAME` | `1` with `LJ_DS && LJ_TARGET_X64 && LJ_FR2`, otherwise `0` | An explicitly interpreted caller retains its frame for C/FF tails; trailing `RETM` forwards results and preserves local/upvalue lifetime |
| `LUA_COMPAT_DYNAMIC_DISABLE_TAILCALL` | `LUAJIT_ENABLE_LUA51COMPAT` | Exposes `jit.disabletailcall(bool)`; affects subsequently parsed chunks |
| `LUAJIT_DISABLE_TAILCALL` | Undefined | Defining it disables parser tail-call emission for all subsequently parsed source |

`COUNT`, `DEBUG`, and `CFRAME` are independent numeric preprocessor overrides.
The dynamic parser switch does not rewrite existing prototypes or traces.
`LUAJIT_DISABLE_TAILCALL` is checked with `#ifdef`, so even defining it as `0`
disables tail-call emission. Neither parser switch changes the JIT engine.
The previous C/FF bytecode format is retained: `CFRAME=1` still uses dump
version `0x81` and rejects stock `2` and the previous `0x80` format.

For a function whose tail-call debug behavior matters, use `jit.off(fn, true)`.
The second argument disables nested prototypes too; separately defined Lua
callees need their own `jit.off` call. Existing compiled callers outside that
interpreted chain have no Lua 5.1 virtual-frame guarantee.

The x64 VM chooses the C/FF path using the Lua caller executing `CALLT/CALLMT`,
including after `__call` resolution:

| Caller policy | C/FF tail behavior |
| --- | --- |
| Engine on and caller permits JIT | Native frame replacement during warmup, recording and compiled execution |
| Caller has `PROTO_NOJIT` | Retain the Lua caller; the function is excluded from recording |
| Engine off, for sites executed by the interpreter | Retain the Lua caller |
| No-JIT build | Retain the Lua caller |
| `CFRAME=0` | Stock native replacement in every mode |

Lua tail calls always replace their caller. Native replacement closes the
caller's open upvalues before overwriting its slots, for Lua, C and FF callees.
Retained C calls close on the actual return, keeping locals and open upvalues
aliased during callbacks, debug writes and coroutine suspension.

`jit.off(fn[, recursive])` keeps its existing per-prototype behavior: disable
future recording and flush roots anchored in the selected prototypes.
It does not request a global flush for tail-call compatibility, including
when disabling cold functions. Set this policy before the functions warm up.

Already compiled copies in other inlined or linked callers may remain.
If a caller explicitly needs a warm chain to become wholly interpreted,
use the existing operations:

```lua
jit.off(fn, true)
jit.flush()  -- Optional: explicitly discard existing compiled callers.
```

The tests use this explicit cleanup at warm mode-switch boundaries.
`jit.flush(fn)` only covers roots anchored in that prototype and is not a
substitute for clearing all inlined copies. A prototype's own trace list
cannot establish whether it was compiled into another caller. No new JIT
entry point or trace dependency tracking is introduced.
On/flush retain their existing meaning; a mode change cannot recreate a
caller already replaced by an in-flight tail call. Normal C calls and
callbacks run outside machine code; `cont_stitch` handles an explicit full
flush by checking the blacklisted trace. FFI callbacks entered from compiled
FFI calls retain their existing `lj_ccallback_enter` rejection boundary.

Global `jit.off()` retains its existing behavior: it stops compilation but
does not flush existing traces. For a wholly interpreted call chain, use it
before warming the chain, or use `jit.off(); jit.flush()`. Existing machine
code does not acquire compatibility guarantees just because the engine was
disabled. `PROTO_NOJIT` remains runtime state and is not serialized in dumps.

## Removed JIT code

- Tail-count IR updates and the `THREAD_STACK`/`THREAD_TAILCALLS` FLOAD fields.
- C/FF normal-call recording for `CALLT`/`CALLMT`.
- RETM stitching, its continuation, result-count spill/normalization, trace
  metadata, entry-side inheritance, linking and special snapshot restoration.
- `IR_RETFV`, its x64 assembler and optimizer/snapshot barriers.
- Production RETM diagnostics and the `LUAJIT_TEST_RETM` build switch.

Native C-tail calls retain classic `recff_c -> recff_nyi -> recff_stitch`.
Supported FF tails such as `math.abs` record directly. The recorder does not
select or model the retained-frame path. The parser still emits tail calls;
neither a new opcode nor a function compatibility flag is needed.

The existing `UCLO` NYI boundary remains. With the retained-frame bytecode
layout, the parser puts closing work after `CALLT`; the native recorder
checks that immediate `UCLO` suffix before recording frame replacement.
There is no blanket rejection of C/FF or callable-object tails. Original
vararg and stitching-consumer restrictions remain; this is not zero-NYI
support. The bytecode encoding and dump version remain unchanged.

The crash investigation worktree changed the old inline counter writes after
finding that cached `L->stack` and `L->tailcalls` loads can survive stack resize.
Resizing frees the old arrays, making a subsequent generated counter write
stale. Removing that IR sequence removes this write path; this is not evidence
that every game-level JIT/GC crash is resolved. The investigation worktree and
its uncommitted diagnostic changes remain separate.

## Tests

The former JIT frame-parity tests now check interpreter execution, including
functions explicitly disabled while JIT is globally enabled. Native hooks,
upvalue aliases, million-level queries and transactional stack OOM regressions
remain covered. JIT cases check values and side effects, variable C result
counts through 600, callbacks, yields, errors, GC objects and flush/re-record.
They also require ordinary Lua tail calls to compile, then re-enter interpreted
tail-count execution on the same thread. Upvalue and Lua/native hook suites
exercise both global-off and explicit function-off with the engine on.

`test/tailcall/native.lua` associates record events with each exercised wrapper,
requires actual stitch successor links, then triggers a guard exit on those
successors to prove machine-code execution. It covers ordinary C calls, direct
C tails, C `__call`, C `CALLMT` and direct FF tails, with exact side-effect counts.
`modes.lua` verifies that cold-function off and repeated function-off/flush
preserve unrelated traces without a global flush. It covers outer inlining
and links with explicit cleanup, off/on/off transitions, shared prototypes,
recursive off, explicit flushing inside warmed C callbacks, coroutine resume
and dump reload. `dispatch.lua` covers native upvalue ownership,
early-return fixups and mixed Lua/C/FF/callable targets.

`LUAJIT_TEST_TAILCALL=ON` enables the native tail-call and hook modules, without
adding diagnostic fields or counters to production VM structures:

```sh
cmake -S test/tailcall/runner -B build-tailcall -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLUAJIT_NUMMODE=OFF \
  -DLUAJIT_TEST_TAILCALL=ON -DLUAJIT_TEST_STACK_OOM=ON
cmake --build build-tailcall --parallel 4
ctest --test-dir build-tailcall --output-on-failure
```

The RETM evaluation patches and performance harnesses are historical artifacts.
The old implementation, counter, hook and integration reports describe their
original validation and do not extend the current compatibility contract.

## Explicit-flush revision on 2026-10-19

The automatic global flush previously added to function-off has been removed.
The production mode-setting helpers and `luaJIT_setmode` function now match
the base implementation. Only warm tests explicitly request a global flush.
The earlier mode-split and automatic-flush results below are historical
evidence; this revision is validated separately.

Fresh builds of this revision pass all seven configurations on Windows x64
(MSVC/Ninja) and Linux x64 (GCC/Ninja), each with both default and arena GC:

| Configuration | Windows | Linux |
| --- | --- | --- |
| Optimized | 37/37 | 37/37 |
| VM/API assertions | 37/37 | 37/37 |
| `CFRAME=0` | 37/37 | 37/37 |
| `COUNT=0` | 29/29 | 29/29 |
| `CFRAME=0, COUNT=0` | 29/29 | 29/29 |
| `DEBUG=0` | 37/37 | 37/37 |
| `LUAJIT_DISABLE_JIT=ON` | 31/31 | 31/31 |

All 474 CTest executions and 40 superproject parity executions pass.
The new cold-function assertion fails on both earlier automatic-flush
Windows assertion VMs and passes on this revision. The mode tests also
require repeated off and prototype-local flush to preserve existing caller
traces, then verify compatibility after an explicit global flush. Native
C stitching, C `__call`, C `CALLMT` and FF loop/machine-code checks continue
to pass. All 230 checked Windows/Linux source inputs have matching SHA-256.

This revision's evidence is in the superproject's
`builds/tailcall-mode-split/explicit-flush/`: fresh build/test/parity logs,
`before-*-modes.log`, `mode-api-diff.log`, `source-hashes.json`,
`binary-hashes.json`, and matching-vmdef native IR/machine-code dumps.
Linux builds are in `/tmp/luajit-tailcall-explicit-flush-build-20261019`.
The historical throughput and standalone stress observations below were
not rerun for this API-scope correction.

## Historical automatic-flush validation on 2026-10-19

Fresh Windows x64 MSVC/Ninja and independent Linux x64 GCC/Ninja builds pass
the following matrix. Every configuration includes both default and arena GC.
Macro-off and no-JIT configurations include VM and API assertions.

| Configuration | Windows | Linux |
| --- | --- | --- |
| Optimized | 37/37 | 37/37 |
| VM/API assertions | 37/37 | 37/37 |
| `CFRAME=0` | 37/37 | 37/37 |
| `COUNT=0` | 29/29 | 29/29 |
| `CFRAME=0, COUNT=0` | 29/29 | 29/29 |
| `DEBUG=0` | 37/37 | 37/37 |
| `LUAJIT_DISABLE_JIT=ON` | 31/31 | 31/31 |

This is 474 passing CTest executions, plus 40 passing superproject parity
executions: five scripts, both GC variants, assertion and no-JIT builds on
both platforms. The native tests verify wrapper-associated trace recording,
actual stitch successor links, guard exits on those successors and exact C
side-effect counts. Direct C, ordinary C, C `__call`, C `CALLMT` and direct FF
tails pass. Windows IR/machine-code dumps use each VM's matching `vmdef.lua`.
After requiring a loop link for direct FF recording and a nonempty machine
code buffer, the focused native tests pass another 24 executions across all
JIT-enabled matrix configurations. All 230 source inputs checked for the
Windows and Linux builds have matching SHA-256 hashes; the evidence manifest
also hashes 224 binaries and generated definitions.

The earlier isolated negative control removes only the full flush from function-off.
Both Windows GC variants then fail at "old inlined/linked native copy bypassed
function-off". This establishes the old inlined-copy boundary, rather than
a requirement to flush globally inside production function-off. Warm tests
now explicitly flush when they need that stronger guarantee. The earlier
blanket-rejection tree fails the first
native C-tail trace assertion; the earlier CFRAME=0 control passes it.

The investigation worktree's standalone `gc_tailcall_stress.lua 200000` and
`retm_c_stress.lua 10000` produce `ok 25288895` and `ok 50115000`, respectively,
with both GC variants on both platforms. These checks execute their existing
workloads using the new native paths; they do not exercise the removed RETM
stitch implementation or reproduce the complete DST process.

Five alternating benchmark pairs compare CFRAME=1 and CFRAME=0 assertion
builds with identical VM/API checks, after completing the build matrix.
Each C run performs 1,000,000 calls; each FF run performs 50,000,000 calls.
Times below are median seconds as reported by `os.clock`
(Windows has millisecond resolution):

| Platform / GC | C, CFRAME=1 / 0 | FF, CFRAME=1 / 0 |
| --- | --- | --- |
| Windows default | 0.020000 / 0.019000 | 0.030000 / 0.030000 |
| Windows arena | 0.018000 / 0.018000 | 0.029000 / 0.029000 |
| Linux default | 0.018808 / 0.019311 | 0.029644 / 0.029712 |
| Linux arena | 0.019340 / 0.018983 | 0.029673 / 0.029621 |

The short runs overlap in range and do not establish a performance advantage
or a reproducible regression. They show that both configurations execute the
native paths with comparable throughput in these cases. This is not game
performance evidence.

Evidence is in the superproject's `builds/tailcall-mode-split/`, including
`win-*-{config,build,test}.log`, `linux-logs/`,
`win-noinvalidation-test.log`, `*-native-ir-mcode.log`, the benchmark JSON,
and source/binary hashes. Linux build directories are in
`/tmp/luajit-tailcall-mode-split-build-20261019`.
This matrix did not reproduce the separately reported crash. The historical
arena moving-allocator OOM failure below remains unresolved. macOS, remote CI
and full-game execution have not been validated.

## Historical intermediate validation on 2026-10-17

This section describes the earlier reduction with the blanket C/FF recording
fallback. Its passes did not prove that native C-tail stitching survived.
The mode-split implementation requires separate validation below.

Fresh MSVC/Ninja Windows x64 and GCC/Ninja Linux x64 (Ubuntu WSL) builds each
include both default and arena GC VMs, the native probes and moving-allocator
OOM tests. Macro-off configurations also enable VM/API assertions.

| Configuration | Windows | Linux |
| --- | --- | --- |
| Optimized | 31/31 | 31/31 |
| VM/API assertions | 31/31 | 31/31 |
| `CFRAME=0` | 31/31 | 31/31 |
| `COUNT=0` | 23/23 | 23/23 |
| `CFRAME=0, COUNT=0` | 23/23 | 23/23 |
| `DEBUG=0` | 31/31 | 31/31 |
| `LUAJIT_DISABLE_JIT=ON` | 27/27 | 27/27 |

The final matrix passed 394 CTest executions. No-JIT builds omit the two
unrelated scripts that explicitly enable JIT and the two native JIT modes;
interpreted tail-count, hook, depth, upvalue and native value tests remain.
No-JIT validation also required passing the option into CMake architecture
checks and guarding the existing ETW profile switch's JIT-state access.

The superproject parity tests now run with `-joff`. Their C-tail result checks
also run with JIT enabled, without requiring C-tail traces to compile; frame
shape checks use explicitly disabled functions. All five scripts pass with
both GC variants in assertion and no-JIT builds on Windows and Linux, for
40 passing parity executions.

The investigation worktree's `gc_tailcall_stress.lua` (200,000 iterations) and
`retm_c_stress.lua` (10,000 iterations) also pass with both Windows GC variants.
These are standalone workloads, not a reproduction of the full DST process.
macOS, remote CI and game performance are not validated here.

### Separate moving-allocator GC failure

Before the final matrix, repeated Windows arena `stack_oom` executions
intermittently crashed. The unmodified `f1eb755a` baseline reproduced it on the
fifth process in a 30-process attempt. Matching PDBs and disassembly identify
the same failure in baseline and candidate dumps:

```text
hugeset_find+0x25: cmp qword ptr [r9+rax*8],0  (r9 == NULL)
huge_obj_ismarked -> gc_obj_iswhite -> atomic
  -> lj_gc_fullgc -> lua_gc -> grow_case
```

This existing moving-allocator/arena failure remains unresolved; the final
single-pass matrix does not establish that it disappeared. GC implementation
and stack-allocation ownership code were not changed for this tail-call removal.
Four nonempty WER dumps were preserved, including baseline and candidate.

Windows evidence is in the superproject's
`builds/tailcall-interpreter/`: `win-final-*-test.log`,
`win-baseline-nodebug-test.log`, `oom-crash-dumps/`,
`oom-dump-analysis.log`, `oom-candidate-dump-analysis.log`,
`source-hashes.json` and `binary-hashes.json`.
Linux builds and logs are under
`/tmp/luajit-tailcall-interpreter-build-20261017/final-*`; the final logs are
also preserved in `builds/tailcall-interpreter/linux-final-logs/`.

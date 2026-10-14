# Partial thread initialization and tail-count ownership

Date: 2026-10-14. Base: `96888b492900bc18805ca2dfcb57c1c083c15502`.
Branch: `codex/tailcall-stack-oom`.
This follows the uncommitted stack-resize fix in `stack-resize-oom.md`.

## Failure and fix

`lj_state_new()` links a new thread to the GC before `stack_init()` finishes.
The latter publishes the value stack and `stacksize` before allocating the
parallel tail-count array. If that allocation fails, the unanchored thread
remains available for GC or state-close cleanup. Previously `tailcalls` still
contained allocator poison or old arena-cell contents, and `lj_state_free()`
unconditionally freed it with `stacksize * sizeof(int)`.

The new thread initializes `tailcalls` to NULL before any stack allocation can
throw. NULL remains the ownership marker until `stack_init()` publishes the
successfully allocated and cleared count array. Both `lj_state_free()` and
main-thread `close_state()` now free and account for that array only when its
pointer is non-NULL. The main state is already zeroed in `lua_newstate()`, but
it needs the same conditional cleanup after partial initialization.

This condition is essential: `lj_mem_free()` subtracts the supplied old size
before calling the allocator, including when the pointer is NULL. NULL
initialization alone would still deduct 192 bytes for the unallocated initial
48-element count array and violate the allocator's pointer/old-size contract.
The successfully allocated value stack retains its original size and is
released normally.

The new main-state OOM tests also exposed the arena collector's unconditional
release of an unallocated `ssb`. A failure before its allocation reached the
allocator with NULL and old size 8192. `close_state()` now checks this pointer
before releasing or accounting for it, too.

No state-layout or public API change is needed. These checks apply only to
thread initialization and destruction; ordinary calls, returns, and the
PUC 5.1 tail-count semantics are unchanged.

## Regression

`test/stack_oom.c`, registered by `LUAJIT_TEST_STACK_OOM=ON`, extends the
existing moving/poisoning allocator tests. It rejects initial value-stack and
count-array allocations separately, using their allocation sizes rather than
the GC variants' different object-allocation sequences.

For each allocation, it checks coroutine creation inside `lua_pcall()` in two
independent cases: direct state close, and full GC followed by successful
creation and execution of a replacement coroutine. Full GC must restore the
exact pre-failure `gc.total`. It also checks main-state initialization returning
NULL, complete cleanup, and successful retry using the same allocator.
Every successful initial count array is checked for zeroed entries. Unknown
pointers, NULL with nonzero old size, incorrect sizes, and live blocks at close
are test failures.

Together with the earlier resize regressions, each VM executable now runs 15
cases with counting enabled or 9 with COUNT=0. The original resize cases remain
included. Individual initialization probes can be run with:

```text
stack-oom-default --thread-init 1
stack-oom-default --thread-close 1
stack-oom-default --state-init 1
```

`1` rejects counts; `0` rejects the value stack. Windows executables have the
`.exe` suffix. `--repeat 100` runs the complete suite 100 times.

## Validation

All builds use fresh directories, RelWithDebInfo, and `LUAJIT_NUMMODE=OFF`.
The assert build was rebuilt cleanly after the final included-C-file edit,
avoiding reliance on localized MSVC include-dependency detection.

| Platform | Configuration | CTest |
| --- | --- | --- |
| Windows x64 | Optimized, default + arena | 51/51 |
| Windows x64 | Assert/APICHECK, default + arena | 51/51 |
| Windows x64 | Assert/APICHECK, COUNT=0, default + arena | 47/47 |
| Linux x64 / WSL2 | Optimized, default + arena | 51/51 |
| Linux x64 / WSL2 | Assert/APICHECK, default + arena | 51/51 |
| Linux x64 / WSL2 | Assert/APICHECK, COUNT=0, default + arena | 47/47 |

The Windows/Linux assert executables passed 100 rounds per GC variant:
1500 cases per executable, 6000 total. No fixed-build timeout occurred in this
validation. The previous resize investigation's single arena timeout remains
unexplained and recorded in `stack-resize-oom.md` and its separate evidence.

Negative controls reuse the preserved, unmodified Windows `96888b49` DLLs with
the new probe. The supplied cdb captured a poisoned count pointer at
`lj_state_free()` for default GC, and a NULL pointer with old size 192 at the
same cleanup for arena GC. The default main-state negative control also
captures NULL with old size 192 in `close_state()`, demonstrating the accounting
problem independently of an uninitialized pointer.

The initial assert suites passed 50/51 on both platforms before adding the
`ssb` ownership check. The retained valid cdb capture shows NULL, old size 8192,
and `close_state()` on the stack. The final suites above include that fix.
In the fixed coroutine cdb runs, the failed thread has `stacksize == 48` and
`tailcalls == NULL`; GC skips count cleanup, retry succeeds, and close completes.

Evidence is in the superproject at
`C:/Users/fesil/DontStarveLuaJIT2/builds/tailcall-thread-oom-20261014`:
configure/build/CTest and stress logs, negative/fixed cdb captures, source and
binary hashes, an incremental patch over the previous resize fix, and the
combined patch over `96888b49`. The previous evidence directory is preserved.
Linux execution is local WSL2; macOS and remote CI remain unverified.

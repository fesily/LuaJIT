# Atomic stack and tail-count resize

Date: 2026-10-14. Base: `96888b492900bc18805ca2dfcb57c1c083c15502`.
Branch: `codex/tailcall-stack-oom`.

`resizestack()` previously reallocated and published the value stack before
reallocating `tailcalls`. Failure of the latter invoked `lj_err_mem()` while
`base`, `top`, open upvalues, and `jit_base` could still reference the released
value stack. Reordering the two reallocations would instead leave a resized
count allocation paired with the old `stacksize` if the value-stack realloc
failed; that would break later allocation/free sizes and memory accounting.

The count-enabled path now prepares a separate count array, copies retained
counts, and zeroes newly added entries before attempting the value-stack
realloc. The latter uses `allocf` directly so a failure can free the temporary
counts and restore accounting before `lj_err_mem()` runs. At either failure
point both live arrays, their shared size, and all stack pointers are unchanged.
After both allocations succeed, accounting and the count array are committed,
and the value-stack pointers are relocated with no further throwing allocation.
The count-disabled path keeps its existing single-realloc implementation.

This applies to growth, GC shrink, and overflow relimiting, which share
`resizestack()`. It adds temporary count-array memory during resize and always
copies the retained counts. It does not add work to ordinary calls or returns,
change `lua_State` layout, or alter the public Lua/C API.

## Regression

`test/stack_oom.c` supplies an allocator backed by VirtualAlloc or mmap. Every
successful realloc moves to a fresh mapping and immediately releases the old
mapping; bookkeeping rejects unknown pointers and incorrect old sizes.

The default and arena executables each exercise nine cases with counting
enabled: successful growth and each of its two failures on the main thread and
an active coroutine, plus successful GC shrink and each of its two failures on
a suspended coroutine. They verify preserved stack/count state and accounting,
retries on the same state, nil/count initialization, retained counts on shrink,
open-upvalue relocation and closure results, `jit_base` relocation, and exact
allocation sizes with no live blocks after `lua_close()`.

With counting disabled each executable covers the six corresponding
single-allocation cases. The test is optional and registered with CTest by
`LUAJIT_TEST_STACK_OOM=ON`; it does not instrument the VM.

```powershell
cmake -S test/retm/runner -B build-stack-oom -G Ninja `
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLUAJIT_NUMMODE=OFF `
  -DLUAJIT_TEST_RETM=ON -DLUAJIT_TEST_STACK_OOM=ON `
  "-DCMAKE_C_FLAGS=/DLUA_USE_ASSERT /DLUA_USE_APICHECK"
cmake --build build-stack-oom
ctest --test-dir build-stack-oom --output-on-failure
```

On Linux replace the C flags with `-DLUA_USE_ASSERT -DLUA_USE_APICHECK`.
An optimized build omits those flags. To check the count-disabled path, add
`LUA_COMPAT_TAILCALL_COUNT=0` to the C compiler definitions.

## Validation

| Platform | Configuration | CTest |
| --- | --- | --- |
| Windows x64 | Optimized, default + arena | 51/51 |
| Windows x64 | Assert/APICHECK, default + arena | 51/51 |
| Windows x64 | Assert/APICHECK, COUNT=0, default + arena | 47/47 |
| Linux x64 / WSL2 | Optimized, default + arena | 51/51 |
| Linux x64 / WSL2 | Assert/APICHECK, default + arena | 51/51 |
| Linux x64 / WSL2 | Assert/APICHECK, COUNT=0, default + arena | 47/47 |

The same probe was also run against newly built, unmodified Windows baseline
DLLs from `96888b49`. Rejecting allocation #2 reproduces an access violation in
`lj_err_mem+0x56` in both GC variants. The supplied cdb was used for these
negative controls and for breakpoints at `lj_err_mem` in the fixed builds;
the fixed probes return `lua_checkstack == 0`, successfully retry growth, and
close normally.

One intermediate Windows assert CTest run timed out the arena probe after
30 seconds. CTest terminated that process before a debugger stack could be
captured. The log is retained as `ctest-windows-assert-timeout.log`. The probe
now prints each completed case without buffering and accepts `--repeat N`.
All six final configurations passed, and the Windows/Linux assert executables
each passed 100 consecutive rounds per GC variant (900 cases per executable).
The timeout did not recur, but its cause remains unconfirmed; the successful
runs do not erase that earlier observation.

Evidence lives in the superproject at
`C:/Users/fesil/DontStarveLuaJIT2/builds/tailcall-stack-oom-20261014`:
configure/build/CTest logs, `negative-{default,arenagc}-cdb.log`,
`fixed-{default,arenagc}-cdb.log`, the baseline Git bundle, source patch,
and file hashes. Linux validation is local WSL2 execution; macOS and remote CI
are not verified by this change.

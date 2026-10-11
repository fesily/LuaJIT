# JIT tail-call counter investigation

Date: 2026-10-11. Baseline: `25fb1bfb` on `dontstarve`.
Branch: `codex/tailcall-count-investigation`.
This investigates item 10 in the hot-path review on `docs/hotpath-review`.

## Findings

Both reported problems reproduce. They affect `debug.getinfo()` stack levels
and frame names as well as traceback text; the arithmetic return values in the
fixtures remain correct.

1. `REF_BASE` is the trace's entry base. Inlined calls move `J->baseslot`,
   without moving that IR reference. Using `(REF_BASE - stack) / 8 - 1`
   therefore increments the entry frame's counter instead of the current one.
2. `lj_record_call()` and the C/fast-function arm of `rec_callt()` do not emit
   the zero store that the interpreter's `ins_call` performs. Correcting only
   the offset makes a reused frame inherit and accumulate its previous count.
3. A counter store needs a new snapshot before subsequent guards. Without
   it, a type-check exit can restore the frame before its tail calls, then
   the interpreter executes those calls again. The `guard_exit` fixture
   reports four virtual levels instead of two when only this step is omitted.
4. `lj_record_tailcall()` is also used by `recff_metacall_cp()` for
   `FFH_TAILCALL`, such as `tostring` entering `__tostring`. That internal
   replacement does not elide a Lua caller and the interpreter does not
   increment the counter. Emitting an increment there creates a phantom
   virtual frame in the metamethod.

## Fix

`src/lj_record.c` uses a shared `rec_tailcall_count()` helper for clearing
and incrementing the side array. Its index is:

```text
(REF_BASE - L->stack) / sizeof(TValue) + J->baseslot - 2 - LJ_FR2
```

The entry base corresponds to slot `1 + LJ_FR2`, and the counter belongs
to the slot immediately before the current base. Vararg tail calls adjust
`J->baseslot` back to the real frame before calling this helper.

Both ordinary-call paths clear the new frame. Lua tail calls increment the
reused frame; fast-function metamethod replacements preserve its count.
Counter stores set `J->needsnap` so subsequent exits see the updated frame.
The helper is compiled out when tail-call counts are disabled.

## Reproduction

```powershell
.\builds\tailcall-count\baseline\luajit\luajit.exe test/compat51/tailcall_count_jit.lua nested
.\builds\tailcall-count\fixed\luajit\luajit.exe test/compat51/tailcall_count_jit.lua
```

The test runs each scenario with the JIT disabled and enabled, in fresh
coroutines. It verifies the function owning each virtual level, samples the
stack after the loop to catch stale outer counts, and checks that the heated
calls actually compiled. It covers nested calls, slot reuse, vararg callers,
vararg callees, `__call`, side traces, tail recursion, type-check exits, and
fast-function metamethod replacement. An optional first argument selects one
case. On stock Lua 5.1 it runs the reference interpreter checks.

The baseline fails `nested/jit` with hundreds of unexpected outer levels.
The corrected-offset/no-clear mutant fails `reuse/jit` with inherited levels.
The no-tail-snapshot mutant fails `guard_exit/jit` with four levels instead
of two. The initial offset/clear/snapshot fix without filtering
`FFH_TAILCALL` fails `ff_metacall/jit` with one unexpected level.

## cdb Evidence

Debugged with the user's supplied cdb installation. With a breakpoint on
`lua51DS!lj_debug_frame` and the `nested` fixture, the side array contained:

| Frame counter slot | Interpreter | Baseline JIT | Fixed JIT |
| --- | ---: | ---: | ---: |
| 5, loop driver | 0 | 1992 (`0x7c8`) | 0 |
| 12, leaf | 2 | 2 | 2 |

The final iteration exits to the interpreter, which accounts for the leaf's
correct two levels even in the baseline. The loop driver's 1992 extra levels
come from compiled iterations writing the wrong slot.

A breakpoint on `lua51DS!lj_record_tailcall` for `ff_metacall` showed
`J->baseslot == 9`, `J->fn->c.ffid == 0x12` (tostring), and this call chain:

```text
lj_record_tailcall
recff_metacall_cp
lj_vm_cpcall
recff_metacall
```

Local logs are under `builds/tailcall-count/`: `cdb-baseline-slots.log`,
`cdb-fixed-slots.log`, and `cdb-ff-metacall.log`.

## Validation

All builds use MSVC x64, CMake/Ninja, and `RelWithDebInfo`. The assertion
configuration additionally defines `LUA_USE_ASSERT` and `LUA_USE_APICHECK`.
Default and arena GC executables were built for each fixed configuration.

| Configuration | Result |
| --- | --- |
| Fixed optimized build | CTest 11/11 |
| Fixed assertion build | CTest 11/11 |
| `LUA_COMPAT_TAILCALL_CFRAME=0` | CTest 11/11 |
| `LUA_COMPAT_TAILCALL_COUNT=0` | Builds both variants; CTest 9/9, count tests omitted |
| Stock Lua 5.1 | All nine reference cases pass |
| Main repository's `tests/lua_vm_parity/` | Optimized default VM 5/5 |
| Default VM `tailcall_debug`, `mainchunk_tailcall`, `getlocal_tail_frame` | Optimized build passes |

The assertion build still fails in `luaL_traceback`, `lj_debug.c:874`,
when `funcV()` is applied to a virtual tail frame's nil function. This is
the pre-existing item C in the hot-path review. It occurs in
`tailcall_lines.lua` and the traceback case of `tailcall_debug.lua`; the
other four parity cases pass. The new counter test walks `debug.getinfo`
directly and passes with assertions. This patch does not change `lj_debug.c`.

The supplied machine's Ninja dependency prefix was corrupted for the
localized `/showIncludes` output, leaving zero recorded include dependencies
for `ljamalg.c.obj`. After detecting a stale DLL during verification, the
default/arena amalgamation objects were explicitly removed and rebuilt.
For future incremental builds on this machine, inspect
`ninja -t deps` or use a clean build after changing included `.c` files.

## Performance

Seven alternating baseline/fixed runs of `test/hotpath/bench.lua`,
`N = 1000000000`, using the same CMake build settings:

| Case | Baseline best / median, seconds | Fixed best / median, seconds |
| --- | ---: | ---: |
| Ordinary Lua calls | 0.817 / 0.846 | 0.796 / 0.851 |
| Lua tail recursion | 0.708 / 0.917 | 0.712 / 0.757 |

The best times are close. Tail-recursion samples varied substantially,
so the median difference is not evidence of a speedup. These probes did
not establish a stable performance regression, and do not measure game
performance.

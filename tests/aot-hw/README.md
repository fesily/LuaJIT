# jit.aot measurement tools

Diagnostic tools for the `jit.aot` compiler in `src/jit/aot.lua`. This is
**not** part of the test suite — correctness lives in `tests/aot.lua`
(79 checks, portable). Everything here only *measures*.

None of these tools are required to use `jit.aot`.

## bench.lua — portable benchmark

    LUA_PATH='./src/?.lua;;' ./src/luajit tests/aot-hw/bench.lua

Needs the LuaJIT build in `./src` and a C compiler (`$CC`, default `cc`).
Compares the interpreter (JIT permanently disabled per prototype), the JIT and
the AOT module, 1e8 iterations per case, min of 5 runs, and verifies that all
three produce identical results.

Cases are picked so the integer ones stay inside int32 for **every**
iteration (`[-2^31, 2^31-1]`). That matters: a loop such as `s = s + i` with
`s` reaching 5e9 silently turns into a double loop (and `s = s*2 + i` turns
into `+inf`), so it no longer measures the integer path at all.

| case | body | int32 safe |
|---|---|---|
| `int1` | `s = s + 1`, n = 1e6 | yes |
| `int2` | `s = s + i`, n = 40000 (max 8.0e8) | yes, asserted |
| `fp1`  | `x = x * 0.999999 + 0.5` | n/a, all doubles |

Cases live in `cases.lua` (shared with `workload.lua`).

## workload.lua — one untimed run, for the profiling tools

    LUA_PATH='./src/?.lua;;' ./src/luajit -joff tests/aot-hw/workload.lua fp1 interp

Runs the chosen case for the chosen engine once (1e8 iterations in total,
times an optional multiplier argument). Use a multiplier of ~10 when sampling
hardware counters: a 1 kHz sample needs a workload that runs about a second,
otherwise the `cycle` median is dominated by partial windows.
`jit.aot` is only required for the `aot` engine, so that an interpreter
profile starts at the loop under test rather than inside module-load loops.

## counters.sh — hardware counters (macOS only)

Requires macOS, a full Xcode installation and an accepted license
(`sudo xcodebuild -license accept`). Uses Instruments' *CPU Counters*
template (guided "CPU Bottlenecks" mode, 1 ms sampling, EL0) through
`xcrun xctrace`, then summarises `MetricTable` with `counters_parse.py`.

    tests/aot-hw/counters.sh int1 fp1      # case ... (default: int1 fp1)

Per case it records `interp`, `aot` and `jit` (1e9 iterations each, override
with `MULT=...`), and prints the median of each metric over all samples.

How to read the output:

* `cycle` — PMU cycle count per sample window. `median(cycle) * samples /
  iterations` estimates cycles per loop iteration (sampling based, ±10%);
  the same value also gives the effective P-core frequency (≈4.0–4.4 GHz on
  the M4 measured here).
* `useful`, `delivery`, `processing`, `discarded` — Apple's model of *lost
  µop bandwidth*: retired, front-end starvation, window full / completion
  latency, and branch misprediction + pipeline restart respectively.
  **`useful` is not speed**: a very short loop has little independent work
  to fill the window and can show a low `useful` fraction while being the
  fastest variant (that is the JIT here).

## singlestep.py — lldb single-step tracer (macOS/arm64)

Ground truth for the instructions that actually execute: one loop iteration,
counted by single-stepping. Needs `lldb` (works with the command line tools)
and `LUA_PATH='./src/?.lua;;'` in the environment, because the workload mode
of `bench.lua` requires `jit.aot`.

    LUA_PATH='./src/?.lua;;' lldb -- ./src/luajit -joff \
        tests/aot-hw/bench.lua workload fp1 aot
    (lldb) settings set target.disable-aslr true
    (lldb) command script import tests/aot-hw/singlestep.py
    (lldb) hw_trace name:aot_run 1200 -joff tests/aot-hw/workload.lua fp1 aot

For the interpreter, break inside a bytecode handler instead. The process
must already be stopped at the entry point, and the offset comes from
`disp_probe.c`:

    LUA_PATH='./src/?.lua;;' lldb -- ./src/luajit
    (lldb) process launch -s -- -joff tests/aot-hw/workload.lua fp1 interp
    (lldb) command script import tests/aot-hw/singlestep.py
    (lldb) hw_trace atentry:-103008 800

Set `HW_TRACE_DUMP=/tmp/iter.txt` to also write the per-instruction listing.
</br>`hw_trace` walks until the hottest PC appears three times and then reports
one cycle: instruction count, branches, taken branches, and which image the
instructions came from.

## disp_probe.c — interpreter handler addresses (macOS only)

Prints the addresses of the `FORI`/`FORL`/`MULVN`/`ADDVN` bytecode handlers of
the running interpreter, both relative to the image base and to `lua_pcall`
(the latter is what `hw_trace atentry:` expects). Uses dyld, so macOS only;
the numbers are per-build and must be regenerated after rebuilding LuaJIT.

    cc -O2 -std=c99 -fPIC -Isrc -bundle -undefined dynamic_lookup \
       tests/aot-hw/disp_probe.c -o /tmp/disp_probe.so
    ./src/luajit -joff -e 'assert(package.loadlib("/tmp/disp_probe.so",
      "luaopen_disp_probe"))()()'

## Numbers measured on this machine

arm64 Apple M4, Apple clang -O2, `-ffp-contract=off`, 1e8 iterations/case,
generated modules built from this tree:

| case | interpreter | JIT | AOT | AOT/interp |
|---|---|---|---|---|
| `int1` | 2.13 ns | 0.23 ns | 2.04 ns | 0.96 |
| `int2` | 2.26 ns | 0.23 ns | 2.11 ns | 0.93 |
| `fp1`  | 2.29 ns | 1.22 ns | 1.92 ns | 0.84 |

`fp1` with CPU Counters (per loop iteration): 9.6 cycles for the AOT vs 10.2
for the interpreter; `useful` 0.563 vs 0.726, `processing` 0.437 vs 0.238,
`delivery` and `discarded` 0.000 for the AOT (no front-end or mispredict
loss). `singlestep.py` counts 46 instructions and one taken branch per iteration
for the AOT loop (interpreter: 63 instructions, 13 branches, 7 taken).

All numbers are specific to this tree, architecture, compiler and build
configuration; the generated modules are bound to the same configuration.

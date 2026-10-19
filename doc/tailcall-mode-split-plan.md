# Tail-call behavior selected by caller JIT mode

Date: 2026-10-19.
Status: explicit-flush revision validated on Windows and Linux; uncommitted.
Worktree: `.claude/worktrees/luajit-tailcall-interpreter` in the superproject.
Branch: `codex/tailcall-interpreter-only`.
Base: `f1eb755aa6741ebfd59240bde96955631e7b2cd4`.

## Objective and starting point

Keep Lua 5.1 tail-call compatibility in explicitly interpreted functions and
restore ordinary LuaJIT tail-call recording, classic C stitching, and direct
fast-function recording elsewhere.

Continue from the existing, uncommitted reduction in this worktree. It has
already removed JIT tail-counter stores, RETM stitching, RETFV, and their
metadata. Its blanket C/FF tail-call recording rejection is an intermediate
fallback, not an acceptable final implementation.

The earlier 394 CTest executions and 40 parity checks are historical evidence
for that intermediate tree. They do not establish native C stitching and must
not be reused as validation of the implementation proposed here.

## Behavior contract

The relevant prototype is the Lua function executing CALLT/CALLMT: the caller
being replaced or retained. A C callee does not have that Lua prototype.
The decision applies after callable-object resolution as well.

| Configuration and caller mode | Interpreted tail-call behavior | Recording behavior |
| --- | --- | --- |
| CFRAME disabled | Existing stock path | Existing native recorder |
| CFRAME enabled, JIT enabled, caller permits JIT | Native frame replacement, including during warmup | Native recorder; eligible C calls stitch and supported FFs record directly |
| CFRAME enabled, caller has PROTO_NOJIT | Retain caller for C/FF; Lua tail calls still reuse frames | Caller is excluded from recording |
| CFRAME enabled, engine globally off | Retained C/FF caller for instructions executed by the interpreter | No new recording; see existing-code caveat below |
| CFRAME enabled, build has no JIT | Existing interpreted compatibility | No recorder |

Conceptual predicate for the C/FF retained-frame decision:

```text
CFRAME enabled AND
  (no JIT in build OR engine off OR caller has PROTO_NOJIT)
```

Use this stable policy in the VM. Do not select a different frame model merely
because the recorder is active or a function happens to be hot.
Keep CALLT/CALLMT generation enabled, preserving Lua tail recursion.
Reuse PROTO_NOJIT rather than adding a second function-level compatibility flag.
COUNT and DEBUG remain independent switches with interpreted implementations;
compiled traces do not maintain or materialize virtual tail counts.

Global jit.off() does not flush previously compiled code in the current fork.
A read-only probe on the existing assertion VM observed 1,000 trace exits
after globally disabling compilation. Consequently, full interpreted-chain
compatibility requires a cold start with JIT off, or jit.off() plus jit.flush(),
or the function-level policy with explicit cleanup when already warm.
Neither global nor function-level jit.off() automatically flushes all traces.
Compiled outer callers do not gain historical virtual-frame guarantees.

## Phase 1: Record baseline failures and mode boundaries

Extend the existing test/tailcall harness before changing dispatch:

1. Capture native C tail calls, ordinary fixed-result C calls, and FF tails.
   Inspect trace events, linktype, and links, alongside values and call counts.
2. Demonstrate the current CFRAME=1 failure with return probe.values(n) and
   return math.abs(-n); use CFRAME=0 as the native-behavior control.
3. Add explicit jit.off(fn, true), engine-off plus flush, and mode-switch cases.
4. Warm an outer caller with an inlined wrapper, then disable the wrapper.
   Test whether the old caller can still execute its compiled wrapper body.
5. Record closure/upvalue, CALLMT, callable-object, and bytecode-layout cases.
   Separate pre-existing UCLO/vararg restrictions from the blanket rejection.

Completion: reproducible tests identify the native optimization gap and the
existing inlined-copy boundary, including explicit cleanup for warm tests.

## Phase 2: Split the VM path and restore the native recorder

Primary files: src/vm_x64.dasc, src/lj_record.c, src/lj_arch.h.

1. In CALLT/CALLMT, read the caller's mode before overwriting its function or
   frame slots. Preserve argument count, argument source, callee, PC and KBASE.
2. For an eligible caller, use the stock LuaJIT frame-replacement route for
   C/FF as well as Lua callees. Restore the stock fast-function KBASE handling,
   which the current CFRAME build omits.
3. For an interpreted compatibility caller, retain the existing C/FF normal
   call route and parser-provided return sequence.
4. Apply the same decision after __call resolution. Include C __call and FF
   metamethod replacement; do not infer the caller's policy from the callee.
5. Remove the generic non-Lua/callable rejection in lj_record_ins().
   Keep lj_record_tailcall() native. Restore no retained-frame recording code.
6. Leave recff_c -> recff_nyi -> recff_stitch and ordinary cont_stitch intact.
   Retain native restrictions on unsupported continuation consumers.

Completion: simple C tails produce a stitch with an actual successor link,
FF tails compile directly, and explicitly disabled wrappers retain their frame.

## Phase 3: Preserve upvalue ownership without extending the JIT

Primary files: src/vm_x64.dasc and src/lj_parse.c; narrow recorder checks only
where needed to preserve the existing UCLO boundary.

Current CFRAME source emits CALLT/CALLMT, then UCLO when necessary, then RETM.
fs_fixup_ret() can also place closing work on the return path. A native tail
call skips that suffix, so the existing suffix alone is insufficient.

1. Keep compatibility C/FF upvalues open throughout the C call and close them
   on the actual return, preserving debug.setlocal/setupvalue aliasing.
2. Before native frame replacement, close caller-owned open upvalues. Reuse or
   extend the existing interpreter close slow path; cover C/FF and Lua callees,
   fixed and vararg callers, and callable objects.
3. Keep the recorder's existing inability to compile UCLO. If a tail site
   requires closing under the current parser layout, report/fall back at the
   equivalent UCLO boundary before recording a conflicting frame replacement.
   Do not implement compiled upvalue closing or disguise this as a C-call ban.
4. Preserve argument/source registers and reload movable stack pointers after
   calling the closing helper. Validate the final captured value after return,
   errors, yield/resume, stack growth and GC.
5. Audit early returns preceding the first child prototype and their
   fs_fixup_ret() transformations.

Prefer retaining the current bytecode encoding and CFRAME dump version 0x81.
Do not rewrite bytecode when toggling JIT mode. Revisit the dump version only
if implementation changes an invariant required to interpret old bytecode.
PROTO_NOJIT remains runtime policy and is not newly serialized in dumps.

Completion: escaped closures survive both paths correctly; no new JIT
compatibility IR, helper calls, trace fields, or snapshot metadata are added.
Existing native UCLO NYI remains an explicit supported fallback boundary.

## Phase 4: Preserve mode API behavior and make cleanup explicit

Primary file: test/tailcall/modes.lua. Production mode-setting code is unchanged.

Current luaJIT_setmode() aborts recording before changing mode.
setptmode() updates PROTO_NOJIT and flushes roots associated with that proto.
lj_trace_flushproto() alone does not inspect references from other trace roots;
its name is not evidence that every inlined or linked use is invalidated.

1. Keep jit.off(fn) limited to the existing prototype mode and root-flushing
   behavior. Do not add compatibility-driven global flushing to luaJIT_setmode.
2. Verify that disabling a cold prototype and repeatedly disabling a warm one
   leave unrelated inlined/linked caller traces available, with no global
   flush event. The target prototype's trace list does not describe all uses.
3. Warm compatibility tests explicitly call jit.off(fn) followed by jit.flush()
   when they need existing native copies discarded. Reuse existing safe flush
   machinery for callbacks and stitch continuations. Do not add a new API or
   trace dependency tracking for this two-call sequence.
4. jit.on(fn) restores native behavior for future tail calls. jit.flush(fn)
   alone does not change PROTO_NOJIT or the selected compatibility policy.
5. Keep the global-off existing-code caveat explicit. To test the full global
   compatibility contract, disable the engine and flush at the safe boundary.
6. A change made inside a C callback cannot recreate an already elided frame.
   Existing retained calls return using their actual frame layout; modes must
   not reinterpret an in-flight return as if a different call path had run.
7. Cover recursive mode changes, separate callees not nested in the wrapper,
   coroutine suspension/resumption, and multiple closures sharing a prototype.

Completion: function-off preserves the existing API scope, explicit flushing
clears old copies where requested, and switching does not corrupt frames or
replay/skip native calls.

## Acceptance cases

| Area | Required evidence |
| --- | --- |
| Classic C tails | Under CFRAME=1 and engine on, eligible fixed-argument wrappers produce a stitch and execute a linked successor; actual C calls occur exactly once |
| Fast-function tails | A supported FF such as math.abs compiles directly; a trace elsewhere is insufficient proof |
| Lua tails | Native compilation still works; interpreted tail recursion remains bounded in physical stack usage |
| Native restrictions | Unsupported consumers, UCLO and vararg shapes retain their original safe fallback; no zero-NYI claim |
| Interpreted compatibility | Explicitly off wrapper remains visible during C/FF calls with correct locals, upvalue aliases, logical levels and hooks |
| Dispatch | CALLT/CALLMT, Lua/C/FF alternation at one site, C __call, Lua __call and FF metamethod replacement |
| Results | 0/1/3/4/259/300/515/600 results, nil padding, fixed receivers, MULTRET, object identity and call counts |
| Recovery | Guard exits before/after a tail call, linked traces, flush, re-record, errors, yield/resume and C callbacks |
| Switching | Cold off preserves unrelated traces; warm cleanup is explicit; outer inlining, links, on/off/on, engine off with/without flush, callback toggles |
| Ownership | Stack growth, GC churn, escaped closures and transactional allocation-failure regression |
| Bytecode | Source and dump roundtrip, runtime modes after reload, existing version rejection |
| Static removal | No JIT tail-count stores/materialization, RETM stitch continuation, RETFV or compatibility trace metadata |

Trace assertions must identify the exercised wrapper/call site and verify the
successor link. Do not accept merely traceinfo(1) being non-NULL, or compare
debug frames of compiled callers against interpreted compatibility semantics.
Production diagnostics are not required; use the existing C probe and trace
introspection/event interfaces.

## Validation and completion order

1. Build fresh Windows x64 assertion/API-check VMs for both GC variants.
   Run focused native/compatibility/upvalue/mode-switch tests after each phase.
2. Once the focused acceptance cases pass, run fresh Windows and independent
   Linux source builds for release, assert, CFRAME=0, COUNT=0, both disabled,
   DEBUG=0, and no-JIT. Each build includes both GC variants.
3. Run root lua_vm_parity scripts with interpreted compatibility; additionally
   exercise explicit function-off while the engine is on. Keep JIT value and
   native-stitch checks separate from debug-frame assertions.
4. Re-run the investigation worktree's standalone stress workloads using exact
   final binaries. Preserve identities and any nonempty dumps. These runs do
   not substitute for full DST reproduction.
5. Check targeted C-tail and FF throughput against CFRAME=0 in paired runs.
   Report observations without borrowing earlier performance acceptance.
6. Update tailcall-interpreter-only.md, macro comments, active test names and
   CI to the final contract. Mark intermediate validation as historical.
7. Run diff checks and verify source/binary hashes and main-worktree state.
   Deliver the diff, final acceptance results, unresolved limits and evidence.
   Commit/integrate/push only when separately requested.

Use sequential fresh builds when generated source files are shared, and pair
each VM with its own generated vmdef.lua.
The known intermittent arena moving-allocator OOM crash, reproduced on the
unchanged baseline, remains a separate issue. Preserve and report a recurrence;
a passing single matrix does not establish that issue is fixed.

macOS, full-game performance, and full DST crash reproduction require their own
execution evidence and are not implied by local Windows/Linux tests.

## Implementation evidence on 2026-10-19

- The original CFRAME=1 build fails the new wrapper-specific native C-tail
  stitching test; the original CFRAME=0 control passes it.
- VM dispatch now selects retained C frames by engine/caller mode and restores
  the stock FF KBASE path. All native callees share pre-replacement upvalue
  closing. The recorder only rejects a deferred UCLO suffix.
- Function-off retains its original prototype-local behavior. Global cleanup
  is explicit in warm tests. The earlier negative control establishes the
  inlined-copy boundary without making global flushing a production policy.
- The earlier automatic-flush Windows/Linux matrices passed all seven configurations:
  474 CTest executions plus 40 superproject parity executions. Both
  investigation stress workloads produce the expected values with both
  assertion VMs on both platforms.
- Final cross-platform counts, source/binary identities and throughput
  observations are recorded in tailcall-interpreter-only.md.

## Explicit-flush revision evidence on 2026-10-19

- The mode-setting section of lj_dispatch.c exactly matches the base. There
  is no compatibility-driven global flush in jit.off or luaJIT_setmode.
- The new cold-prototype preservation assertion fails on both earlier
  automatic-flush Windows assertion VMs.
- This revision passes 474 fresh Windows/Linux CTest executions and 40
  superproject parity executions across the same seven configurations.
  Both GC variants preserve native C stitching and direct FF recording.
- Warm tests explicitly request full cleanup, including inside a stitched
  callback. Cold and repeated off calls preserve unrelated traces.
- All 230 checked source inputs match between Windows and Linux copies.
  New evidence is in builds/tailcall-mode-split/explicit-flush.

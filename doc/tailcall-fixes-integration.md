# Tail-call fixes: mainline integration

Historical validation: as of 2026-10-17, tail-call compatibility is
[interpreter-only](tailcall-interpreter-only.md). JIT-specific guarantees and
the old RETM diagnostic build commands below no longer apply.

Date: 2026-10-14. Integration base: `96888b49`.
Integration branch: `codex/tailcall-fixes-integration`.
Target: LuaJIT `dontstarve`, followed by the superproject `master` gitlink.

The integration preserves the independently validated source snapshots and
keeps the original investigation worktrees intact. Changes are committed in
dependency order:

| Commit | Change |
| --- | --- |
| `e3a6cdf2` | Close caller upvalues according to the actual Lua/C tail-call branch; reject old dump layouts with CFRAME version `0x81`. |
| `7cccb882` | Observe the Lua caller before tail-call frame reuse; emit virtual tail-return hook events. |
| `1f80790b` | Remove the million-level debug limit and preserve representable tail depth. |
| `b7b7768b` | Make stack/count resizing transactional and partial thread cleanup conditional on allocation ownership. |

The OOM probe registration is combined with the hook/depth registrations.
All tests use fresh build directories. Windows uses MSVC/Ninja; Linux uses
an independent source checkout and GCC/Ninja under local WSL x64.
Both use RelWithDebInfo and `LUAJIT_NUMMODE=OFF`.
`LUAJIT_TEST_STACK_OOM=ON` is enabled throughout.
`LUAJIT_TEST_RETM=ON` enables the RETM and native hook probes except in no-JIT
builds, where that option is OFF.

## Combined validation

| Configuration | Windows x64 | Linux x64 / WSL |
| --- | --- | --- |
| Optimized | 61/61 | 61/61 |
| VM/API assertions | 61/61 | 61/61 |
| CFRAME=0 | 61/61 | 61/61 |
| COUNT=0 | 53/53 | 53/53 |
| CFRAME=0 and COUNT=0 | 53/53 | 53/53 |
| No JIT, applicable tests | 17/17 | 17/17 |

Each configuration includes default and arena VM variants: 612 successful
CTest executions in total. The no-JIT configuration excludes the four
pre-existing JIT-dependent tests matched by
`tailcall_count_jit|function_args_alloc|str_order51`.
Its moving-allocator OOM tests remain enabled.

The five superproject parity scripts pass on both platforms in optimized
and assertion builds, for both VM variants: 40/40 executions. These cover
metamethod names, tail-call lines, main-chunk tail calls, xpcall tail calls,
and C-tail results including new-dump roundtrip and old-dump rejection.

The 16 relevant source/test files have identical SHA256 hashes in the
Windows and Linux tested trees. Evidence in the superproject is retained at
`builds/tailcall-integration-20261014/`: validation scripts, per-configuration
configure/build/CTest logs, parity logs, `source-hashes.json`,
`linux-source-hashes.log`, and a Git bundle of the semantic-fix commits
used to bootstrap the Linux checkout. The OOM overlay is covered by the
source hashes and the final mainline commit.

Original PUC comparisons, negative controls, debugger evidence and stress
runs remain documented in the individual upvalue, hook, depth, stack-resize,
and thread-initialization investigation reports. This integration reruns
the combined functional matrix, not those entire investigations.

## Remaining limits

Existing native traces do not acquire call/return hook entry points when
those hooks are enabled after compilation. The previously documented
limitation remains; see `tailcall-hook-parity.md`.

No performance measurements are added by this integration. The busy-host
3% ordinary-stitch acceptance bound remains inconclusive; these functional
results do not convert it into a performance pass.

Linux evidence is local WSL execution. macOS and remote CI execution remain
unverified. No public Lua/C API changes are introduced; old CFRAME bytecode
dump layouts are deliberately rejected.

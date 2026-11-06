"""Single-step tracer for jit.aot (macOS/arm64, lldb).

Load it into lldb and trace one loop iteration of either the AOT module or
the asm interpreter, counting dynamically executed instructions and taken
branches. No hardware counters are involved; this is ground truth for the
instructions that actually ran.

    # interpreter: stop at the entry point first, then break in a handler
    LUA_PATH='./src/?.lua;;' lldb -- ./src/luajit
    (lldb) process launch -s -- -joff tests/aot-hw/bench.lua workload fp1 interp
    (lldb) command script import tests/aot-hw/singlestep.py
    (lldb) hw_trace atentry:-103008 800      # offset from lua_pcall, see disp_probe.c
    (lldb) hw_trace name:aot_run 900 -joff tests/aot-hw/bench.lua workload fp1 aot

Modes:
  name:<symbol>       pending breakpoint on a symbol (AOT module: aot_run)
  atentry:<offset>    break at lua_pcall+offset in the already stopped
                      process (interpreter bytecode handler; the offset comes
                      from disp_probe.c, and the process must have been
                      stopped first with "process launch -s -- ...")

The trace walks until the hottest PC has been seen three times, then reports
one cycle of the loop: instruction count, branch count, taken branches, and
the per-image split. Set HW_TRACE_DUMP=1 to also write the instructions.
"""
import os
from collections import Counter

import lldb


def _valid(a, target):
    if not a.IsValid():
        return None
    la = a.GetLoadAddress(target)
    if la in (0, 0xFFFFFFFFFFFFFFFF, lldb.LLDB_INVALID_ADDRESS):
        return None
    return la


def _find_main_module(target):
    exe = target.GetExecutable().GetFilename()
    for i in range(target.GetNumModules()):
        mod = target.GetModuleAtIndex(i)
        if mod.GetFileSpec().GetFilename() == exe:
            return mod
    return None


def _walk(target, thread, start, cap, dump):
    path = []
    for _ in range(cap):
        pc = thread.GetFrameAtIndex(0).GetPC()
        ins = target.ReadInstructions(lldb.SBAddress(pc, target), 1)
        mn = ins.GetInstructionAtIndex(0).GetMnemonic(target) if ins.GetSize() else ""
        path.append((pc, mn))
        thread.StepInstruction(False)
        if thread.GetProcess().GetState() != lldb.eStateStopped:
            print("process stopped at step", len(path))
            break
    print("stepped %d instructions" % len(path))
    counts = Counter(pc for pc, _ in path)
    for pc, cnt in counts.most_common(3):
        print("  hot pc 0x%x x%d" % (pc, cnt))
    if not counts:
        return
    hot = counts.most_common(1)[0][0]
    idx = [i for i, (pc, _) in enumerate(path) if pc == hot]
    if len(idx) < 3:
        print("no cycle found (PC 0x%x seen %d times)" % (hot, len(idx)))
        return
    seg = path[idx[1]:idx[2]]
    branches = taken = 0
    for i, (pc, mn) in enumerate(seg):
        if mn[0] == "b" or mn in ("br", "blr", "ret", "cbz", "cbnz"):
            branches += 1
            nxt = seg[i + 1][0] if i + 1 < len(seg) else hot
            if nxt != pc + 4:
                taken += 1
    print("one iteration: %d instructions, %d branches (%d taken)"
          % (len(seg), branches, taken))
    imgs = Counter()
    for pc, _ in seg:
        imgs[target.ResolveLoadAddress(pc).GetModule().GetFileSpec().GetFilename()] += 1
    print("  by image:", dict(imgs))
    if dump:
        with open(dump, "w") as f:
            for pc, _ in seg:
                ins = target.ReadInstructions(lldb.SBAddress(pc, target), 1)
                txt = ""
                if ins.GetSize():
                    i0 = ins.GetInstructionAtIndex(0)
                    txt = i0.GetMnemonic(target) + " " + i0.GetOperands(target)
                f.write("0x%x  %s\n" % (pc, txt))
        print("  instructions written to", dump)


def hw_trace(debugger, command, result, internal_dict):
    argv = command.split()
    mode = argv[0]
    cap = int(argv[1], 0)
    args = " ".join(argv[2:])
    dump = os.environ.get("HW_TRACE_DUMP")
    target = debugger.GetSelectedTarget()

    if mode.startswith("atentry:"):
        off = int(mode[8:], 0)
        proc = target.GetProcess()
        if not proc.IsValid() or proc.GetState() != lldb.eStateStopped:
            print("hw_trace: launch and stop first: "
                  "process launch -s -- <luajit args>")
            return
        mod = _find_main_module(target)
        if mod is None:
            print("main module not found")
            return
        sym = mod.FindSymbol("lua_pcall")
        base = _valid(sym.GetStartAddress(), target) if sym.IsValid() else None
        if base is None:
            print("lua_pcall not found in", mod.GetFileSpec().GetFilename())
            return
        start = base + off
        print("handler 0x%x (lua_pcall 0x%x %+d)" % (start, base, off))
        bp = target.BreakpointCreateByAddress(start)
        debugger.HandleCommand("continue")
        print("  bpHits=%d state=%s pc=0x%x" % (
            bp.GetHitCount(), target.GetProcess().GetState(),
            target.GetProcess().GetSelectedThread().GetFrameAtIndex(0).GetPC()))
    else:
        sym = mode[5:] if mode.startswith("name:") else mode
        target.BreakpointCreateByName(sym)
        debugger.HandleCommand("run " + args)
        start = target.GetProcess().GetSelectedThread().GetFrameAtIndex(0).GetPC()
        print("breakpoint %s hit at 0x%x" % (sym, start))

    proc = target.GetProcess()
    thread = proc.GetSelectedThread()
    if thread.GetFrameAtIndex(0).GetPC() != start:
        print("did not stop at the requested address")
        proc.Kill()
        return
    for bp in list(target.breakpoint_iter()):
        bp.SetEnabled(False)      # otherwise stepping re-hits it
    _walk(target, thread, start, cap, dump)
    proc.Kill()


def __lldb_init_module(debugger, d):
    debugger.HandleCommand("command script add -f singlestep.hw_trace hw_trace")

# Lua 5.1 尾调用 hook 的事件与时机

日期：2026-10-14。源码基线为
`96888b492900bc18805ca2dfcb57c1c083c15502`，隔离分支为
`codex/tailcall-hook-parity`，工作树为
`.claude/worktrees/luajit-tailcall-hooks`。

本工作树包含此前 `codex/tailcall-upvalue-alias` 工作树的未提交修复。
这项依赖使 Lua 调用者的 upvalue 能保持开放直到 callee call hook
结束，并在真实 Lua 尾调用复用帧之前关闭。旧 dump 仍按该修复拒绝，
CFRAME dump 版本为 `0x81`；本次 hook 修复未再改变编码或 dump 版本。
依赖的细节与回归记录见 [upvalue 关闭时机](tailcall-upvalue-close.md)。
两个分配失败修复仍在独立的 `codex/tailcall-stack-oom` 工作树中。

## 原因与改动

基线 `lj_dispatch_ins()` 在返回指令之前只发出 `LUA_HOOKRET`，
没有消费该帧已有的虚拟尾层计数并发出 `LUA_HOOKTAILRET`。
同时，x64 `BC_CALLT` 先搬移参数、覆盖旧帧，然后才进入通常的
callee call hook 分发。hook 因此已经看不到实际调用者。

PUC 5.1 的参考是主仓库 `src/lua51/src/ldo.c` 的
`luaD_precall()`、`callrethooks()` 和 `luaD_callhook()`，以及
`src/lua51/src/lvm.c` 的 `OP_TAILCALL`。其 Lua 分支先创建 callee
帧并调用 hook，随后关闭 caller 的 upvalue、复用 caller 帧。
返回时先发普通 return，再逐层发 tail return；每次先递减计数，
重新检查返回 hook 掩码后才进入下一层。

实现分为两个部分：

- `lj_dispatch_ins()` 在普通 return 后按当前计数发尾返回事件。
  变参函数的计数来自 `FRAME_VARG` 下方的实际返回帧。帧位置保存为
  栈偏移，每次 hook 返回后重新定位，允许 callback 扩栈。
  hook 内重入不消费外层计数；禁用或替换 hook 在下一事件立即生效。
- 新增 x64 GC64 `lj_dispatch_tailcall()` 与 `BC_CALLT_HOOK` 分支。
  仅在实际 Lua callee、call hook 开启且 hook 不在执行中时进入。
  临时建立普通 callee 帧并保留 caller，准备缺失参数，调用 hook；
  callback 结束后才关闭 caller 的 upvalue、剥离 caller 变参伪帧、
  增加一次尾层、搬移参数并进入 callee 的解释器函数头。

call hook 对缺失参数的 `debug.setlocal()` 修改会进入实际 callee。
`memmove()` 处理参数源和目标的重叠；CALLMT 的展开数量来自运行时，
不限制为字节码中的固定数量。callee hook 不会再执行第二次。
C/FF callee 继续走保留调用者的普通调用路径，不额外增加虚拟尾层。

共用的 `callhook()` 同时保存和恢复 `L->top` 的栈偏移，防止原生 hook
的临时栈值或扩栈改变实际参数与返回值。
原生 `LUA_HOOKTAILRET` 的 `lua_Debug.i_ci` 为零，已有调试 API
将其解释为无函数、无局部变量的虚拟尾帧。

未启用 call hook 时仍走原有汇编尾调用与 recorder 路径。
新增慢路径在 callee hook 返回后直接选择 `IFUNCF/IFUNCV`；
进入 callee 函数体的循环是否使用已有 trace，仍受原有 JIT 机制管理。

## 复现结果

原始脚本为主仓库
`builds/tailcall-review-20261014/hooks.lua`。基线退出码为 1：
`middle`、`leaf` 的 call hook 父级均为 `tail`，尾返回事件为零。
修复后的优化/断言、default/arena、Windows/Linux 八组输出逐行匹配
原版 PUC 5.1：

```text
value 17
call:outer:parent=main
call:middle:parent=outer
call:leaf:parent=middle
return:leaf:parent=tail
tail return:leaf:parent=tail
tail return:leaf:parent=main
```

实际日志使用制表符分隔 `value` 和 `17`。

## 回归测试

`test/compat51/tailcall_hooks.lua` 注册为 default/arena 两项 CTest，
同时运行 JIT 关闭及 fresh trace cache 下的 JIT 开启模式。
无 JIT 构建仅运行解释器模式。完整纯 Lua 回归也在 PUC 5.1 上通过。

覆盖固定参数、变参、八种混合尾链、CALLMT、300 个展开参数、尾部 nil、
Lua `__call`、C/FF 尾调用，以及 200 层尾递归。call-only、return-only
和组合掩码分别校验事件顺序与父级函数；hook 可禁用、替换自身，
修改 caller 局部变量和 callee 缺失参数，并触发扩栈、GC、错误展开
及 coroutine yield/resume。首个 tail return 中抛错后，后续新调用
仍产生正确事件，不复用旧计数。

`test/hooks/probe.c` 是原生 hook 探针。default/arena 各链接匹配的 VM，
使用独立模块目录，随 `LUAJIT_TEST_RETM=ON` 测试构建启用。
探针强制 `lua_checkstack(L, 600)`，读取传入的 `lua_Debug`，在 hook
结束前留下临时栈值，并测试嵌套保护调用和不同 coroutine 上的重入。
context 按 state 存于注册表，嵌套调用恢复原 context；hook 执行中的
递归事件抑制保持本 fork 的已有行为。

原生测试验证 call hook 的真实父级、虚拟尾帧没有函数和局部变量、
hook 错误展开，以及尾链返回的 300 个值和尾部 nil。
Lua hook 和原生 hook 均验证多结果对象身份与结果数量。
探针在调用 `lua_getinfo()` 前保存事件码，避免依赖该 API 调用后的
`ar.event` 内容。

## 平台矩阵

日志与命令保存在主仓库 `builds/tailcall-hooks-20261014/`。
`validate-windows.ps1` 和 `validate-linux.sh` 使用 clean build，
每个构建独立生成 VM 定义。Linux 使用本机 WSL x64 和独立源码副本，
不与 Windows 构建共享生成文件。最终测试扩充后重新运行全部适用
CTest，Linux 重跑命令另见 `retest-linux.sh`。

| 构建 | Windows x64 | Linux x64 / WSL |
| --- | --- | --- |
| 优化，RETM 测试计数开启 | 55/55 | 55/55 |
| 优化 + VM/API 断言 | 55/55 | 55/55 |
| CFRAME=0 | 55/55 | 55/55 |
| COUNT=0 | 51/51 | 51/51 |
| CFRAME=0、COUNT=0 | 51/51 | 51/51 |
| 无 JIT，适用测试 | 13/13 | 13/13 |

每个构建均包含 default/arena，共 560 项最终 CTest 执行。
主仓库五项 parity 在两个平台的优化/断言与两个 VM 中通过 40/40 次。
CFRAME=0 不要求保留 C wrapper 及开放局部变量别名；COUNT=0
仍要求 call hook 父级可见，但不要求虚拟 tail return 事件。

无 JIT 配置关闭 RETM 原生探针，排除四项旧的 JIT 专用测试：
两个 `tailcall_count_jit`、`function_args_alloc` 和 `str_order51`。
13/13 是适用测试的结果。新增纯 Lua hook/upvalue 回归均执行通过。
本次没有出现需要 cdb 定位的 VM 崩溃，没有重新测量性能。
macOS 尚未实际执行，保持未验证。

## 已有 trace 的 hook 边界

不得将上述 fresh trace cache 结果解释为已经支持所有编译路径 hook。
`builds/tailcall-hooks-20261014/precompiled-control.lua` 先编译一个
调用尾链的循环，再开启 `cr` hook。实际基线和修复版均存在两条 trace；
已编译调用的 call、return、tail return 计数都为零。

关闭 JIT 并 flush 后，基线产生 `3000 / 1000 / 0`，修复版产生
`3000 / 1000 / 2000`。这证明本次修复了 Lua 尾调用解释器的事件与时机，
没有为已编译、内联的调用增加原本不存在的 hook 分发。
对应日志为 `precompiled-baseline.log`、`precompiled-fixed.log`。
需要完整调试事件时，应在运行目标代码前关闭 JIT 并清空 trace。

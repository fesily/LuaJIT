# 百万级尾调用的完整逻辑深度

日期：2026-10-14。隔离工作树为
`.claude/worktrees/luajit-tailcall-depth`，分支为
`codex/tailcall-depth-limit`，提交基线为
`96888b492900bc18805ca2dfcb57c1c083c15502`。

本工作树复制了 `codex/tailcall-hook-parity` 中尚未提交的 hook 与
upvalue 修复，便于验证新的 call hook 路径在百万次尾调用后仍保留
正确计数。原 worktree 未修改。依赖的说明见
[hook 修复](tailcall-hook-parity.md) 和
[upvalue 关闭时机](tailcall-upvalue-close.md)。
以上描述为独立验证时的工作树状态。2026-10-14 已将这些修复与两个
OOM 修复整理到整合分支；提交和组合验证见
[主干整合记录](tailcall-fixes-integration.md)。

## 复现与原因

用户的 `builds/tailcall-review-20261014/deep_count.lua` 执行
1,000,005 次 Lua 尾调用，直接查询最后一个虚拟尾层。
基线在 JIT 开启和关闭时都输出 `first=tail, last=nil` 并失败；
主仓库原版 PUC Lua 5.1 输出 `first=tail, last=tail`。

有两处人为截断：

- `lj_debug_frame()` 将读取的计数限制到
  `LJ_TAILCALL_COUNT_MAX=1000000`。汇编和 JIT 已写入更大的正确计数，
  但层级查询只能看到前一百万层。
- `frame_tailcalls_inc()` 的 C helper 在一百万处停止递增。
  新增的 `lj_dispatch_tailcall()` call hook 路径使用这个 helper，
  因而只删除 debug 读取处的截断仍不够。

PUC 的依据是 `src/lua51/src/ldebug.c` 的 `lua_getstack()`：
按实际 `ci->tailcalls` 跳过被消除的帧，不对层级查询施加输出阈值。

## 实现与边界

本次独立的生产逻辑改动位于 `src/lj_debug.c` 与 `src/lj_frame.h`：

- 删除 debug 层级查询中的百万上限和该常量。
- C helper 在完整的正值 `int32` 范围内递增，仅在 `INT32_MAX`
  防止 C 有符号整数溢出。

保持已有 `int[]` 数组、x64 汇编和 recorder 的 32 位计数表示。
本次没有扩大计数或公共 `lua_getstack()` 层级参数的整数表示范围；
超过该表示范围的递归不在本次兼容保证内。
Lua/C 公共 API、字节码编码和 dump 版本均未因本次改动改变。

traceback 仍使用已有的 `TRACEBACK_LEVELS1=12` 与
`TRACEBACK_LEVELS2=10`，格式化时省略中间段。本次不增加新的格式化
限制，也不通过修改原始计数或 debug 查询结果来限制输出。
百万级 traceback 的回归检查输出行数保持有界，且输出之后末端
虚拟层依然可直接查询。

## 正式回归

`test/compat51/tailcall_depth.lua` 在 COUNT 开启时为 default/arena
分别注册 CTest，覆盖 0、1、999,999、1,000,000、1,000,005 次尾调用，
以及 1,000,005 次变参尾调用。

检查真实 leaf、第一层、旧阈值附近、最后一层、下一实际调用者及栈外
层级；虚拟尾帧必须为 `what="tail"`、`func=nil`，没有局部变量。
还查询公共 API 的最大正整数层级，确认不存在的层保持 nil。
traceback 不得扩展为百万行，格式化不能消费或截断计数。

JIT 开启测试通过 trace start/stop 事件确认目标递归函数确实编译，
并检查 trace 仍存在。控制函数与检查函数使用解释器执行；
变参用例不宣称其递归路径已全部编译。

原生 `hookprobe.frame()` 直接调用 `lua_getstack()/lua_getinfo()`，
与 Lua `debug.getinfo()` 对相同层级的结果逐项比较。
`hookprobe.count()` 在实际执行的 coroutine 上安装原生 hook，
用固定大小的计数 context 记录事件，不为每个事件分配日志表。
call-only、return-only 和组合掩码均运行百万级尾链。
return-only、组合掩码及变参组合掩码都得到
**1,000,005 个 tail return**；call-only 路径保持完整末端深度。

新测试按 COUNT 构建开关注册。COUNT=0 构建继续执行其余回归。
无 JIT 构建执行新增纯 Lua 深度测试，不访问不存在的 `jit.opt`，
也不要求编译事件。

## 验证记录

材料目录为主仓库 `builds/tailcall-depth-20261014/`。
`validate-windows.ps1` 与 `validate-linux.sh` 保存配置、clean build、
CTest、原始复现和主仓库 parity 的命令。Linux 使用独立的 WSL x64
源码与构建目录。最后修正测试脚本的无 JIT 入口后重新运行完整的
适用 CTest 矩阵，Linux 重跑入口为 `retest-linux.sh`。

| 构建 | Windows x64 | Linux x64 / WSL |
| --- | --- | --- |
| 优化 | 59/59 | 59/59 |
| 优化 + VM/API 断言 | 59/59 | 59/59 |
| CFRAME=0 | 59/59 | 59/59 |
| COUNT=0 | 51/51 | 51/51 |
| CFRAME=0、COUNT=0 | 51/51 | 51/51 |
| 无 JIT，适用测试 | 15/15 | 15/15 |

每组同时包含 default/arena，共 588 项最终 CTest 执行。
无 JIT 排除四项已有的 JIT 专用测试：
两个 `tailcall_count_jit`、`function_args_alloc`、`str_order51`；
原生 RETM/hook 模块随测试构建选项关闭。

原始复现在两平台、优化/断言、default/arena、JIT 开关组合中
通过 16/16，输出逐行匹配 PUC。
完整纯 Lua 新回归也在 PUC 5.1 上通过。
主仓库五项 parity 通过 40/40 次，继承的 hook/upvalue 和正式 RETM
回归均包含在上述 CTest 中。

原生百万事件日志见 `native-depth-windows-assert.log`；
Linux 对应日志为 `native-depth-linux-assert.log`。
初次无 JIT 测试脚本失败保存在 `*-nojit-first.log`，
最终通过结果在 `*-nojit-ctest.log`。
源码、匹配的 VM 定义和二进制哈希保存在 `hashes.json`、
`hashes-linux.log`。

本次没有发生需要 cdb 定位的 VM 崩溃，没有重新测量性能。
已有编译 trace 动态开启 call/return hook 的限制仍见 hook 文档；
本测试中的百万事件用例在关闭 JIT 并 flush 后执行。
macOS 尚未实际执行，保持未验证。

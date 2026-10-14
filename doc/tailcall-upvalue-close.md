# C 尾调用的 upvalue 关闭时机

本改动基于 `96888b492900bc18805ca2dfcb57c1c083c15502`，位于隔离分支
`codex/tailcall-upvalue-alias`。目标是修复保留的 C 尾调用者帧中，局部变量
与闭包 upvalue 的关联提前断开的问题。按用户要求删除旧 dump 兼容处理，
保留本项目的 PUC Lua 5.1 帧与尾层语义。

## 原因与实现

原解析器对含子函数的返回发出 `UCLO -> CALLT/CALLMT -> RETM`。
当被调用者实际为 C/FF 时，VM 保留 Lua 调用者帧，但 upvalue 已被复制到
独立存储。因此 `debug.setlocal` 修改帧中的 `x` 后，闭包仍读取旧值。

PUC Lua 5.1 的 `OP_TAILCALL` 在 `PCRLUA` 分支复用帧前关闭 upvalue；
`PCRC` 分支保留帧，等待 `OP_RETURN` 关闭。这也是当前修复的时序：

| 路径 | 关闭时机 |
| --- | --- |
| Lua 尾调用 | x64 `BC_CALLT_Z` 确认 Lua callee 后，在移动参数与复用帧前关闭 |
| C/FF 尾调用，已知含子函数 | 解析器发出 `CALLT/CALLMT -> UCLO -> RETM` |
| 首次子函数之前编码的 C 尾调用 | return fixup 只补返回端的 `UCLO`，保留原调用位置 |
| 普通返回、块作用域退出 | 保持已有 `UCLO` 处理 |
| C 调用抛错 | 由已有错误展开关闭仍开放的 upvalue |

`fs_fixup_ret()` 不再搬移 CFRAME 模式下的尾调用指令，避免把被保留的
调用者 PC 移出原局部变量的调试生命周期。其返回端 fixup 仍生效。
Lua 分支新增的关闭慢路径保存参数数目，调用 `lj_func_closeuv()`，
随后恢复 BASE、参数源地址和调用参数数目；变参帧在关闭后再按原路径移动。

删除原型中的 `BC_UCLO` 尾调用目标识别与延迟关闭逻辑，以及 `BC_RETM`
回看上一条调用并关闭的逻辑。VM 不再解释旧布局的特殊含义。

## 字节码与 JIT 边界

CFRAME 开启时，dump 版本改为 `0x81`。版本 `0x80` 的旧关闭布局和版本
`2` 的 stock dump 均在加载阶段拒绝；没有转换或兼容执行路径。
CFRAME 关闭时仍为 stock 版本 `2`。Lua/C 公共 API 与字节码编码不变。
旧 dump 必须从源码重新生成。

recorder 在 `PROTO_CHILD` 尾调用处以 `BC_UCLO` NYI 回落，延续此前关闭
upvalue 路径不能录制的限制。此处不新增编译关闭 upvalue 的能力。
不含子函数的 `CALLT/CALLMT -> RETM` 仍使用正式 RETM stitch 路径。
含关闭动作的路径不满足该入口的相邻字节码条件，保持解释执行。

测试中的栈检查 helper 与受调试写入的 getter 使用解释器执行，包装函数
内部循环和周边路径继续加热并检查 trace 存在。这不证明含 `UCLO` 的
尾调用返回已编译；这条路径明确回落。正式 RETM 原生探针另行验证不含
关闭动作的编译返回。

原因是现有 JIT 对 immutable upvalue 的常量化独立于本修复。
普通调用对照 `immutable-control.lua` 在原基线和修复版都得到
`compiled=1, interp=99`：已有 trace 不会因调试 API 写入自动失效。
本改动没有修改这一通用 JIT 调试限制。

## 回归范围

`test/compat51/tailcall_upvalue.lua` 注册为 default/arena 两项 CTest，
覆盖解释器与加热后的周边 JIT 路径：

- `pcall`、`CALLMT`、包装函数内部热循环及 return fixup。
- 真实 C `table.sort` 回调、C `__call` 与 FF 元方法替换。
- `debug.setlocal` 与 `debug.setupvalue` 双向别名检查。
- 同一包装函数交替尾调 Lua/C，错误处理器与 C 回调抛错展开。
- 栈增长、GC 对象身份、yield/resume、300 个返回结果。
- Lua 尾递归、300 个变参、普通块作用域关闭。
- 新 dump 往返和旧版本拒绝。

CFRAME 关闭构建运行 stock 模式，验证 Lua 尾调用和块作用域关闭，
不要求保留 C 尾调用者帧。无 JIT 构建执行解释器用例。

## 验证记录

材料目录：主仓库 `builds/tailcall-upvalue-20261014/`。
`validate-windows.ps1` 与 `validate-linux.sh` 记录完整构建和测试命令。
Linux 使用本机 WSL x64；共享源码的构建串行运行，包含 C/H/DynASM
变更后的 clean build。每个构建生成自己的 VM 定义。

2026-10-14 最终矩阵如下。每个构建同时包含 default/arena 两种 GC VM：

| 构建 | Windows x64 | Linux x64 / WSL |
| --- | --- | --- |
| 优化，RETM 测试计数开启 | 51/51 | 51/51 |
| 优化 + VM/API 断言 | 51/51 | 51/51 |
| CFRAME=0 | 51/51 | 51/51 |
| COUNT=0 | 47/47 | 47/47 |
| CFRAME=0、COUNT=0 | 47/47 | 47/47 |
| 无 JIT，适用测试 | 11/11 | 11/11 |

共 516 项 CTest 执行。无 JIT 配置未注册 RETM 探针，并排除了四项要求
主动开启 JIT 的旧测试：两个 `tailcall_count_jit`、`function_args_alloc`
和 `str_order51`。未经筛选的失败日志单独保留；此处的 11/11 不是全部
已注册用例通过的声明。新增 upvalue 回归在无 JIT 两种 VM 中均执行通过。

主仓库五项 parity 在每个平台的优化/断言、default/arena 中共通过
40/40 次；原始 `upvalue_alias.lua` 在这些组合中通过 8/8 次。
PUC Lua 5.1 通过原始复现和完整新增回归。
新版本 dump 的往返及旧版本拒绝均通过；实际基线生成的
`baseline-wrapper.ljbc` 返回 `cannot load incompatible bytecode`。

最后新增的交替 callee 和错误展开用例之后，完整 CTest 矩阵再次运行，
结果见各平台 `*-ctest.log`。本次没有重新测量性能，也没有更改此前
忙碌宿主机中的性能验收结论。macOS 尚未实际执行。
两项分配失败修复保留在独立的 `codex/tailcall-stack-oom` 分支，
不属于此改动。

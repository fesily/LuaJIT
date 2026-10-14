# C 尾调用从 RETM 缝合 trace：方案评估

2026-10-14 补充：保留的 C 尾调用者帧现在于调用返回后关闭 upvalue；
旧 dump 布局不再兼容，CFRAME dump 版本为 `0x81`。含关闭动作的
尾调用仍解释执行，普通 RETM 缝合回归通过。详见
[关闭时机修复与验证](tailcall-upvalue-close.md)。

2026-10-14 hook 补充：Lua 尾调用的 call hook 现在先观察保留的 caller，
随后才关闭 upvalue 并复用帧；普通 return 后按虚拟尾层发 tail return。
Windows/Linux x64 的完整矩阵与原生扩栈、多结果探针通过。
已有编译 trace 的 hook 限制及验证记录见
[尾调用 hook 修复](tailcall-hook-parity.md)。

日期：2026-10-11。
补充评估：2026-10-12，目标调整为保留 PUC 5.1 行为并对齐上游 C 函数缝合覆盖。
历史实验与性能评估基线：`cbaddf73f94eab95c5e697b21fec179a69670093`。
当前工作树基线：`c04ae378c9d1b79086fe7de5ec86dde0deb518fd`
（2026-10-12 快进更新；已使用全新目录完成重建和回归验证）。
新工作树：`.claude/worktrees/luajit-retm-eval`。
分支：`codex/retm-stitch-evaluation`。

## 结论

2026-10-12 已将方案实现到本隔离工作树的 `src/`，包括专用
`cont_stitch_retm`、固定接收者归一化、每次执行独立的数量 spill、
入口侧 trace 的 spill 搬移及 x64 `IR_RETFV`。
正式测试、平台矩阵和当前验收状态见
[实现与验证记录](retm-stitch-implementation.md)。
下文保留早期原型的分析和历史数据；它们不作为当前基线的验收证据。

建议继续方案 b：保留 PUC Lua 5.1 的 C 尾调用帧，在 C 函数返回后，
让缝合 trace 的首条字节码为 `RETM`，通过已有 `IR_RETF` 返回下层 Lua 帧。
隔离原型证明该路径可运行，不必第一步就增加通用的动态多结果 IR 或改写
整个快照格式。

但必须在进入缝合 trace **之前**检查实际返回数量，数量变化时直接解释执行
原来的 `RETM`。不能只删除 `recff_nyi()` 的 `RETM` 禁止条件。
历史原型的补丁保存在 `test/retm_eval/guarded-prototype.patch`。
当前固定接收者已用归一化替代实际数量完全相等的策略；
真正 MULTRET 仍有完整宽度数量保护。当前改动没有提交或合并。

原型的数量检查是安全基础。当前实现已补齐固定接收者面对变化或大量
C 结果、变参包装函数的动态返回，以及实际执行编译后续路径的测试。
Windows/Linux x64 的功能矩阵和实际上游覆盖对照已通过，详见实现记录。

本文件下文约 6% 的改善属于 cbaddf73 上的历史原型数据，不描述当前
c04ae378 实现。当前性能验收单独记录配对比值、95% 区间和失败样本。
忙碌宿主机中的 Linux arena 基线对基线控制测量也超过普通 stitch 的
3% 上界，因此该环境不能为这项容差提供可靠验收；尚不能声明最终可合入。

## 两个问题的区别

原热路径文档的 #2 / §3.2 是 **C 尾调用阻止 trace 缝合**。
#10 / §3.5 才是 **尾调用计数槽位错位和普通调用不清零**。

#10 已由基线中的 `cbaddf73` 修复，包括当前帧偏移、清零、计数更新后的
快照，以及 FF 元方法替换不增加虚拟尾层。本次重新运行
`test/compat51/tailcall_count_jit.lua`，基线 JIT/解释器及原版 Lua 5.1
参照均通过。详情见 `doc/tailcall-count-jit.md`。

本评估重点是用户选择的 #2 方案 b，并确认它不会破坏 #10 的修复。

## 原因和已有能力

PUC 实现的依据在主仓库 `src/lua51/src/lvm.c` 的 `OP_TAILCALL`：
`PCRLUA` 复用帧并增加 `tailcalls`；`PCRC` 保留调用者帧，继续执行后续返回。
当前 fork 的解析器在 `CALLT/CALLMT` 后追加 `RETM`，
解释器和 `rec_callt()` 对 C/FF 被调用者走普通调用路径，复刻这一行为。

阻断点是 `src/lj_ffrecord.c:162` 的 `recff_nyi()`：
看到 C 函数返回位置是 `RETM`，直接将 trace 停成 `LJ_TRLINK_RETURN`。

现有系统已经具备大部分能力：

| 部件 | 已有能力 | 本方案需要的补充 |
| --- | --- | --- |
| `recff_stitch()` | 插入 continuation，退出 trace 后执行真实 C 函数 | 只对解析器追加的尾调用 `RETM` 放开限制 |
| `cont_stitch` | 将 C 结果搬回调用位置，链接后续 trace | 链接前检查完整宽度的 `MULTRES` |
| `lj_dispatch_stitch()` | 按下一条字节码校正 `L->top` | 对此路径传入 `RETM` 本身，不能继续传 `pc-1` |
| `rec_setup_root()` | 接受普通 CALL 后的缝合入口 | 支持 `RETM`，初始化实际活跃槽数并检查上限 |
| `lj_record_ret()` | 返回下层 Lua 帧，发出 `IR_RETF` 和目标帧 guard | 保留 `cur.startins=RETM`，使它识别返回入口 |
| `lj_snap_restore()` / `lj_trace_exit()` | 已处理 `RETM` 的栈顶和多结果恢复 | 匹配数量的路径可以复用；不匹配必须在入口前回落 |
| `trace_stop()` / `trace_abort()` | 为普通缝合入口链接、黑名单 | 正确区分缝合 `RETM` 和 down-recursion 返回入口 |

普通缝合逻辑把 `startpc` 放在已经执行的 CALL 上，setup 再将 PC 前移。
这里要直接把 `startpc` 放在尚未执行的 `RETM` 上，并保留首个快照的 PC。
setup 后由下一次正常 dispatch 记录 `RETM`，不要额外立即记录一次造成重复。

原型的 `-jdump=bis` 输出确认：

```text
TRACE 2 ... stitch gcinfo
TRACE 3 start 2/stitch bench2.lua:27
0003  RETM     1   0
...
0002 > p64 RETF ...
...
TRACE 3 stop -> 1
```

## 必须处理的三个边界

### 1. 返回数量变化不能使用原来的初始快照

`RETM` 的结果数来自运行时 `MULTRES`。trace 记录一次后，其槽数、结果
SLOAD 和快照都已按那次观察特化。

如果记录时返回 3 个值，后来返回 0 个值，直接进入同一 trace 会读取
残留槽，或者在 guard 退出时按旧的 3 个结果恢复栈顶。原型实际复现了
`first result, n=0`，固定参数、变参和参数展开的调用都会受影响。

第一阶段建议在 `GCtrace` 保存完整宽度的 `stitch_multres`，
在 `cont_stitch` 跳转到机器码前比较它与真实 `MULTRES`。
0 可以作为普通缝合入口的哨兵，因为真实值是“结果数 + 1”，零结果对应 1。
不匹配直接走 `cont_nop`，由解释器执行当前 `RETM`；C 函数已经执行，
不重放 C 调用，也不经过按旧数量特化的快照恢复。

不要只检查结果类型，也不要截断成 8 位。
多种返回数量的 trace 缓存可以以后再做，第一阶段只缓存一套数量。

### 2. 在 snapshot #0 生成前检查 recorder 槽数

无槽位上限检查的原型在 3 个结果切到 300 个结果时崩溃。
cdb 将崩溃映射到 `src/lj_snap.c:85-87` 的 `snapshot_slots()`：

```text
snapshot_slots -> snapshot_stack -> lj_record_setup
 -> trace_state -> lj_vm_cpcall -> lj_trace_ins
 -> lj_dispatch_stitch -> lj_cont_stitch

nslots = 0x12f (303)
s = 0x102 (258)
J->maxslot = 0x12d (301)
J->baseslot = 2
sizeof(J->slot)/sizeof(J->slot[0]) = 0x102 (258)
```

故障指令是：

```asm
cmp byte ptr [r9+rcx*8+5], 47h
```

扫描越过 `J->slot` 后将相邻字段当作 TRef，访问无效 IR 地址。
另一轮也触发了 `rec_check_slots: slot overflow`。
`LJ_MAX_JSLOTS` 是 250，实际数组含额外槽共有 258 个。
必须在 `rec_setup_root()` 中、`lj_snap_add()` 之前拒绝过大的入口，
安全回落到解释器。300 个结果仍须正确转发，不能因为无法编译就丢弃。

### 3. recorder 中不能直接从当前 cframe 取 MULTRES

第一版保护原型使用了
`cframe_multres_n(cframe_raw(J->L->cframe))`。
断言构建和非断言构建的结果不一致，后者仍返回错误值。

原因是 `lj_trace_ins()` 使用 `lj_vm_cpcall()` 运行 recorder；
此时 `L->cframe` 已是临时保护调用的帧，不是 interpreter 的原始帧。
正确的观察数量应从 dispatch 已校正的 `L->top` 推导：

```c
J->maxslot = (BCReg)(J->L->top - J->L->base);
J->cur.stitch_multres = J->maxslot - bc_a(ins) - bc_d(ins) + 1;
```

修正后，最终非断言和断言构建均通过数量变化探针。

## 原型覆盖和限制

原型只放开 `CALLT/CALLMT` 后的 `RETM`，保留普通 `CALLM`、
`TSETM` 等限制，以及 `error`、`debug.sethook`、`jit.flush`
等特殊内建函数的现有处理。

非变参 Lua 调用者即使使用 `CALLMT` 展开参数，也能从 `RETM`
进入后续 trace。变参 **调用者函数** 的返回要处理 `FRAME_VARG`；
当前 `lj_record_ret()` 仍会结束到解释器，探针结果正确但不能获得完整
缝合收益。不要把支持 `CALLMT` 当作已解决 `FRAME_VARG`。

早期最小原型保留了这一回落；按 2026-10-12 的完整对齐目标，
变参帧返回已是必做项。
`return prefix, C()` 的普通多结果 `RETM` 不属于本次放开的范围。

在 PUC 兼容方面，真实 C 调用时 Lua 包装帧仍存在；两层 Lua 尾调用后再
尾调 C 的探针仍报告两个虚拟尾层，C 尾调用本身没有额外虚拟层。
错误处理器、C→Lua 回调和 yield 探针也保持调用者帧及结果语义。

## 验证

构建均为 MSVC x64 / GC64、CMake/Ninja、`RelWithDebInfo`。
断言构建额外定义 `LUA_USE_ASSERT` 和 `LUA_USE_APICHECK`。
默认和 arena 两个 VM 的现有 CTest 均已编译运行。
新的原生 C 探针目前只链接默认 VM。

| 检查 | 最终结果 |
| --- | --- |
| 基线 CTest | 11/11 |
| 最终保护原型断言 CTest | 11/11，包含两个 GC 变体的尾计数测试 |
| 默认 VM 的新 C 探针 | 非断言、断言均通过 |
| 新探针覆盖 | 固定参数、参数展开、变参调用者、普通带前缀返回、0/1/3/4 数量变化、300 结果、C 回调栈、已有 Lua 虚拟尾层、错误、yield、flush |
| 不同初始数量 | 分别从 0、1、3、4 个结果热启动，数量变化检查通过 |
| 主仓库 5 个 parity 用例 | 最终非断言原型 5/5 |
| 断言 parity 用例 | 4/5；在评估基线 `cbaddf73` 上，`tailcall_lines.lua` 与基线同样在 `luaL_traceback` 失败；主分支后来已由 `9f457b6d` 修复，待更新原型基线后重验 |
| PUC Lua 5.1 参照 | `tailcall_count_jit.lua` 的 9 个解释器场景通过 |
| 无数量保护的变异原型 | 零结果转发失败；300 结果崩溃 |
| `git apply --check` | 最终实验补丁可应用到本 worktree 基线 |

尚未进行游戏进程性能测试、Linux/macOS 构建或其他 CPU 后端验证。
arena 编译和现有测试通过不代表原生 C 探针已覆盖 arena。

## 性能评估

最终数量推导修正后，`bench2.lua tailc 20000000` 交替运行 7 轮：

| 构建 | 最优，秒 | 中位数，秒 |
| --- | ---: | ---: |
| 基线 `cbaddf73` | 0.487 | 0.655 |
| 保留 CFRAME、从 RETM 缝合的原型 | 0.457 | 0.613 |
| 基线关闭 CFRAME 的性能对照 | 0.366 | 0.567 |

每次计算结果均为 `92499994`。
最优值和中位数的改善约 6%，但单次耗时在约 0.36–0.76 秒间波动，
不能据此声称有稳定的游戏加速。原型也没有达到关闭 CFRAME 的对照水平。
之前短样本里更大的加速不作为最终结论。

`bench.lua tailc` 不适合验收此问题：其中 `debug.getmetatable`
在当前源码实际有 recorder（`lib_debug.c` 的 `LJLIB_REC(.)` 和
`recff_debug_getmetatable()`），trace 直接形成 loop，不经过本次缝合。
后续应使用 `bench2.lua` 的 `gcinfo` 或本探针的真实 C 函数。

性能后续重点：continuation 插槽及结果搬运、重新进 trace 的开销、
`IR_RETF` 的帧切换/guard，以及入口数量检查。
实验原型为了简单从后续 trace 对象读取数量，多了一次 trace 表查询；
正式实现可评估将期望数量保存到已持有的前驱 trace，减少查询，
但必须处理链接更新、flush 和黑名单生命周期。

## 对齐上游的范围

本次源码参照为本地上游提交
`c6ffc141a8762b41703f9287d63d93622a13dd8f`，日期 2026-09-08。
它的 `recff_nyi()` 同样不允许后续入口为 `CALLM`、`CALLMT`、
`RETM`、`TSETM`，并保留特殊内建函数、`minstitch`、Lua 调用帧等限制。
对齐覆盖不要求把上游本身禁止的普通动态多结果消费者一并放开。
包装函数自己的 `CALLMT` 参数展开与 C 返回后的下一条 `CALLMT` 是两回事。

上游 Lua 尾调用先移除包装帧，C 返回后可直接衔接下层调用者的固定结果
消费。PUC 兼容保留包装帧后，新增的 `RETM` 不能让这些原本可缝合的
场景永久回落。

| 场景 | 当前原型 | 完整对齐所需 |
| --- | --- | --- |
| 非变参包装函数、固定数量 C 结果 | 已验证 `RETM -> IR_RETF -> 调用者` | 正式实现和退出恢复验收 |
| 包装函数通过 `CALLMT` 展开参数 | 已有成功缝合路径 | 保持覆盖，区分实际变参帧 |
| 变参包装函数 `function(...) return C(...) end` | 后续 trace 停成 `return` | 编译跨越入口已有的 `FRAME_VARG` 并继续下层调用者 |
| 外层固定接收结果，C 实际结果数量变化 | 只链接一次观察到的数量，变化时解释执行 `RETM` | 按固定接收结果语义处理，不只依赖实际数量完全相等 |
| 外层只接收一个值，C 返回 300 个值 | 入口槽数检查安全终止录制 | 丢弃值不能强制全部占用 recorder 槽 |
| 上游禁止的普通 `*M` 后续消费者 | 保持原限制 | 保持正确解释执行，超出本次覆盖对齐范围 |

2026-10-12 用现有 release 二进制补做了两项路径比较。关闭 CFRAME 的
fork 对照不是原版上游，结果只作为源码分析的补充证据：

- `probe.lua vararg`：保护原型的后续 trace 在包装函数返回处停成
  `return`；关闭 CFRAME 的对照后续 trace 从外层赋值之后接回循环。
- 外层只接收一个值、C 依次返回 300/301/600 个值：两者均得到
  `603013000`。对照后续 trace 接回循环；保护原型三次缝合录制均以
  `trace too deep` 终止，对应入口槽数上限检查。

因此原来的 300 结果正确性探针并不能证明大量结果下覆盖对齐：
它通过 `pack(fixed(n))` 要求保留全部结果，本来就是另一类消费者。

## 建议实施顺序

1. 隔离分支已于 2026-10-12 快进到 `c04ae378`，包含 `9f457b6d`
   的虚拟尾层 traceback 修复和新增 CTest 注册。实验补丁仍可应用；
   后续保持 CFRAME 和尾计数兼容开关开启，在新基线上重新构建和测试。
2. 将安全原型整理为正式实现。三个已解决边界必须保留：完整宽度数量
   检查先于进入 trace、快照 #0 前的槽位上限检查、从 dispatch 校正的
   `L->top` 推导数量。明确 RETM 缝合入口与 down-recursion 的区别，
   验证创建、链接、录制失败、黑名单、单 trace flush 和整体 flush。
3. 设计固定接收者的返回处理。这是当前单一数量入口之外的主要工作。
   应分别处理外层请求固定 N 个结果和真正的 MULTRET。优先研究按固定
   N 个结果特化的 RETM/RETF 路径，让少返回补 nil、多返回丢弃可以继续
   缝合。仅增加少量实际数量变体仍不能解决固定接收者面对 300/600 个
   结果的问题。RETM 前的 frame/type guard 失败必须保留原始结果及实际
   数量；不能先裁掉结果，再用旧快照恢复到尚未执行的 RETM。
4. 支持入口已经存在的 `FRAME_VARG`。当前 `lj_record_ret()` 的处理
   假定变参帧属于本 trace，直接取消回落会破坏 framedepth/baseslot
   假设。需要处理真实帧跨度、BASE、结果位置、目标 Lua 帧 guard 和
   两侧快照。现有 x64 `asm_retf()` 只按调用指令 A 加固定帧头移动
   BASE，不能直接跨过入口已有的变参帧；应评估返回 IR 或入口处理的
   补充，并验证不同参数数目、缺少参数及 `arg` 兼容路径。
5. 将探针接入两个 GC 变体的 CTest。除了值和数量，还须识别目标 C
   调用点的 RETM 后续 trace，检查其返回并接回外层路径。`stitch
   traces > 0` 不足以验收：变参原型已有 stitch trace，仍然解释返回。
   配合 trace/record 事件、IR/快照检查及强制退出场景验证执行路径；
   不能只验证存在某条无关的 stitch trace。
6. 完成恢复与兼容矩阵。覆盖 RETF 前后 frame/type guard 失败、变化
   的结果数量和类型、Lua/C 尾链、FF 元方法、C 回调重入、栈增长、
   GC、错误、yield、flush。每次 C 副作用只执行一次，实际 C 调用时
   包装帧仍存在，Lua 虚拟尾层计数不变。验证 CFRAME/COUNT 关闭构建，
   Windows/POSIX x64，并根据目标平台覆盖 macOS ABI。
7. 验收覆盖和性能。用同工具链编译的实际上游作覆盖参照，再测普通
   缝合和 RETM 缝合，避免给所有普通 stitch 热路径额外增加 trace
   查询和数量检查。覆盖、恢复、兼容通过后，再用稳定采样及实际游戏
   C 绑定评估收益。保留 PUC 帧带来的固有开销应单独说明。

这套顺序以安全原型为基础逐项补齐能力，不把“值正确但解释返回”视为
上游覆盖对齐，也不预先要求增加通用动态多结果 IR。固定接收者的入口
与退出方案仍需实现验证，不能将本节的设计方向当作已完成代码。

补充的大结果固定接收者探针使用同一个 `retmprobe` 模块，脚本如下；
分别用保护原型及关闭 CFRAME 的对照运行，并通过 `-jv` 比较路径：

```lua
local probe = require("retmprobe")
jit.opt.start("hotloop=3", "hotexit=2")
local function wrapper(n) return probe.values(n) end
local sum = 0
for i = 1, 2000 do
  local n = i < 1980 and 300 or (i % 2 == 0 and 301 or 600)
  local a = wrapper(n)
  assert(a == n * 1000 + 1)
  sum = sum + a
end
assert(sum == 603013000)
```

## 本地材料与复现

原生探针和外壳工程在本 worktree 的 `test/retm_eval/`。
实验源码副本、二进制和日志在主仓库 `builds/retm-stitch-eval/`：
`unsafe-src` 是无保护变异，`guarded-src` 是最终保护原型。
当前正式源码已应用并扩展实验补丁；本节列出的旧目录和日志仅供历史复现。

关键日志：

- `cdb-big-interactive.log`：反汇编、调用栈、实际槽数。
- `guarded-release-retm-dump.log`：最终 RETM/RETF 和初始快照。
- `guarded-assert-final-ctest.log`：最终断言 CTest。
- `guarded-release-*.log` / `guarded-assert-final-*.log`：C 探针。
- `bench-final.csv`：最终 7 轮性能原始数据。

示例，从主仓库根目录运行：

```powershell
$env:LUA_PATH = "$pwd/builds/retm-stitch-eval/guarded-release/luajit/variants/default/?.lua;;"
$env:LUA_CPATH = "$pwd/builds/retm-stitch-eval/guarded-release/?.dll;;"
.\builds\retm-stitch-eval\guarded-release\luajit\luajit.exe `
  .claude/worktrees/luajit-retm-eval/test/retm_eval/probe.lua counts
.\builds\retm-stitch-eval\guarded-release\luajit\luajit.exe `
  .claude/worktrees/luajit-retm-eval/test/retm_eval/probe.lua luatails
```

要重新编译，使用 `test/retm_eval/CMakeLists.txt` 作为外壳，设置
`LJ_EVAL_SOURCE` 指向待评估源码，构建 `luajit`、`luajit-arenagc`、
`retmprobe`。构建后将生成的 `variants/default/vmdef.lua` 复制到
`variants/default/jit/vmdef.lua` 再使用 `-jv/-jdump`。

本机 Ninja 对本地化 `/showIncludes` 的依赖记录仍为 0，
并实际遇到生成版本头变化后旧对象/`vmdef.lua` 的版本不匹配。
改 included `.c/.h` 或版本头后应使用全新构建目录；
不要把复用的旧 DLL、旧 buildvm 或旧生成模块的测试当成当前源码验证。

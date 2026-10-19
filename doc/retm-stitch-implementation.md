# 保留 PUC 5.1 帧的 RETM 缝合：实现与验证

2026-10-17：本文保留为历史记录。RETM JIT 支持已删除，当前范围见
[仅解释器尾调用兼容](tailcall-interpreter-only.md)。

日期：2026-10-12。
隔离分支：`codex/retm-stitch-evaluation`。
基线：`c04ae378c9d1b79086fe7de5ec86dde0deb518fd`。
覆盖参照：实际上游 `c6ffc141a8762b41703f9287d63d93622a13dd8f`。
2026-10-14 按用户指示整合到主干，整合基线为包含表构造器拆分的
`2353c551fefa8c969d775eb390b22dc7e60e8eef`。下文的 c04 构建、补丁和
性能记录保留为历史证据；整合验证另列于文末。合入不等于繁忙环境中的
性能验收通过，远端 CI、macOS 和实际游戏性能仍未验证。

## 阶段状态

| 阶段 | 状态与证据 |
| --- | --- |
| 重验基线 | 从实际 c04ae378 bundle 建立独立源码；Windows/Linux 优化及断言构建各 33/33 CTest（13 项旧测试、两个 GC 各十项原生探针），各 10/10 主仓库 parity |
| 重验历史原型 | 新基线上的全新 Windows 断言构建 13/13 CTest；十个原生探针模式通过 |
| 正式入口 | 专用 continuation 与三种入口元数据；普通 stitch continuation 保留原路径；RETM 不改写为 JLOOP |
| 固定接收者 | N=0/1/4，C 返回 0/1/3/4/300/600；少返回补 nil，丢弃结果不占 recorder 槽；稳定类型数量变化继续编译返回 |
| 变参返回 | x64 RETFV 校验帧类型和目标 PC，使用运行时帧跨度；包含 CALLMT、参数长度变化、缺少固定参数及兼容 arg 表 |
| 正式测试 | 两个 GC 变体分别链接独立 probe DLL/so；36 项原生 CTest；入口、编译完成、回落、返回前后退出计数仅测试构建启用 |
| 恢复与兼容 | 类型/目标 guard、入口侧 trace、返回后退出、C 副作用次数、回调重入、独立 state、coroutine、GC 对象、错误、yield/resume、单 trace/整体 flush、失败/黑名单/重新录制均通过 |
| 平台与上游 | Windows 本机及 Ubuntu WSL 的五配置矩阵通过；两平台的实际上游覆盖参照通过；新增 CI 同矩阵 |
| 性能 | 使用关闭测试计数的构建；Windows 数值符合门槛，忙碌宿主机的 Linux arena 普通 stitch 尚不能可靠验收，详见性能验收节 |
| macOS | 未验证；没有实际执行证据 |

早期 cbaddf73 的二进制、vmdef 和七轮性能数据未用于本次验收。

## 实现边界

`is_tailcall_retm()` 仅接受紧邻 CALLT/CALLMT、A 相同、D=0 的解析器附加
RETM。普通 CALLM/CALLMT/RETM/TSETM 消费者及特殊 FF、minstitch 等原有
限制保持。带前缀的 `return prefix, C()` 测试要求不产生这种入口。

GCtrace 的 `stitch_entry` 区分普通入口、RETM 精确数量入口和 RETM 固定
接收入口。`trace_retm_pc()` 使用已有的 `startpc` 指向尚未执行的 RETM；
入口侧 trace 也从同一个 RETM 开始，不保存重复的 PC。该指针所在
prototype 与 `startpt` 相同，沿用 startpt 的 GC 标记生命周期。`retm_retref` 限定
返回前快照范围，`retm_wanted` 保存固定 N，`retm_spill` 指向专用原生槽。
没有改变 SnapShot/SnapEntry 编码、字节码编码、dump 格式或公共 Lua/C API。
两个 GC 变体的 buildvm、fold 表和 vmdef 都由新构建生成。

GC64 的 wanted/spill/retref/真实数量复用 unused_gc64 及其指针对齐间隙的
八字节；入口种类复用尾部 padding。固定请求 N 来自八位的 BC B-1，
最多 254，因此 wanted 可用八位保存；retref 与现有 SnapShot.ref 一样
保存为 IRRef1，真实数量仍是完整 32 位。GCtrace 不增长，mcode、
traceno、link、gclist/ir 等原有字段偏移保持。静态断言保护 ir 偏移；
指定 cdb 的实际 PDB 显示 default GCtrace 仍为 0x78，生产 jit_State
仍为 0xc38，GG_State 仍为 0x1958。新增 IR 链索引的两个字节由原 ktrace
位置提供，ktrace 移到 retryrec 后、trace 指针前的对齐空隙。
slot、penalty、热计数表及后续 dispatch 的偏移与基线一致。
日志：`layout-baseline-cdb.log`、`layout-candidate-cdb.log`。

x64 ELF 的 VM 代码块改为 64 字节对齐，普通 `cont_stitch` 入口也独立
64 字节对齐，避免新增 recorder/assembler 代码改变链接布局时，这个
原有热入口跨越不同的指令缓存行。普通 continuation 的指令序列不变。
新 continuation 置于普通 stitch 和退出代码之后；FFI 仍是最后一个
子程序，以保留原有的特殊 unwind 范围。PE/Mach-O 不执行新增对齐。
该布局调整的诊断配对改善约 1%，在繁忙宿主机上不能把全部改善归因于
对齐；不以诊断构建的结果代替最终构建对 c04 的验收。

固定 N 从下层 Lua 调用指令的 B-1 取得，录制和机器入口采用相同补 nil
规则；补齐前检查空间。空间不足时机器入口回到解释器。保留真实结果
数量及原始丢弃值，直至返回提交。300/600 个原始结果、接收一个值时
只为该值建立引用，入口槽数上限按有效结果检查。

真实 MULTRES 经 r10d 传入，在第一条 guard 前写入 allocator 预留的两个
32 位 spill 槽之一。普通 spill 分配不能复用该槽。入口侧 trace 按
父子 spadjust 差搬移数量，在首个子 guard 前初始化。保护恢复调用前
已经从 ExitState 取得原始数量并设好 L->top，不读取保护调用临时 cframe。
返回前的入口 guard 没有改写 BASE/结果槽，恢复保留原包装帧和完整结果；
返回后的退出使用普通快照。直接到解释器的尾部也从该槽传回 MULTRES。
数量状态不存于共享 jit_State 临时字段。

入口侧 trace 的临时数量搬移先使用子帧的原生地址，再恢复父 RSP，
随后执行原有的寄存器搬移与正式栈调整。子帧扩展至少覆盖数量槽的
完整八字节保留区，并保持 16 字节对齐，使早期写入落在父 RSP 以下，
不会覆盖仍然活跃的父 spill。超过 16 位 spadjust 时安全终止录制。
这是代码审查发现的重叠风险保护，不声称已经复现了父 spill 损坏。

不同固定 N 的接收者不能复用同一入口侧 trace 的归一化槽图：
目标 PC guard 退出后，若 N 不同，该侧 trace 安全终止录制，由调用点
形成自己的根入口。相同 N 的类型侧 trace 可继承数量状态并继续编译。
这项保守限制不会把原始结果截断；guards 用例从接收 1 个值切换到
接收 4 个值，并验证原始多结果。

IR_RETFV 保留普通 RETF，先检查 FRAME_VARG，再按动态帧跨度定位原始
Lua 返回帧，检查返回 PC 后调整 BASE/jit_base。入口外的 vararg 帧
不计入 framedepth。SLOAD 合并、BASE CSE、快照恢复消除和 down-recursion
展开计数都纳入 RETFV 屏障。RETM 入口限定在 x64/GC64（LJ_FR2）的
帧格式；其他 CPU 后端及非 GC64 帧格式保持原有解释回落，不生成该路径。

## 最终矩阵

每个构建同时覆盖 default/arena，原生模块输出目录和 import library
目录均按变体分开。关闭尾计数时，四项明确依赖虚拟尾层的旧 CTest
不注册；其余测试保留。CFRAME=0 是关闭 PUC C 包装帧语义的辅助对照，
因此该配置不要求包装帧存在，也不以 PUC 虚拟尾层数作验收。

| 配置 | Windows x64 | Linux x64 (Ubuntu WSL) |
| --- | --- | --- |
| 优化，CFRAME/COUNT 开启 | 49/49 | 49/49 |
| 优化加 ASSERT/APICHECK | 49/49 | 49/49 |
| CFRAME=0 | 49/49 | 49/49 |
| COUNT=0 | 45/45 | 45/45 |
| CFRAME=0，COUNT=0 | 45/45 | 45/45 |
| 主仓库 parity，优化，两 GC 变体 | 10/10 | 10/10 |
| 主仓库 parity，断言，两 GC 变体 | 10/10 | 10/10 |

新 probe 验证 C 副作用次数，且固定热路径数量变化时 compiled-return
计数在接收 0、1、4 个值时分别增加超过 1000、没有数量回落。
目标 RETM 必须存在并链接到调用者，
变参用例必须包含 RETFV。guards 要求入口侧 trace、返回前退出和返回后
退出计数均非零。重入用例包含 1200 次外层 C 回调、1200 次内层尾调
及十二个独立 Lua state 各 1200 次调用，总 C 次数为 16800。

全部十八个正式模式均按实际调用安排断言 C 副作用的精确总数，包括
frames/error/flush/yield、16800 次 reentry 和 72000 次 coroutine；
不以打印的计数或“大于零”代替次数一致性。

spills 用例对固定参数与变参包装各运行一次：下层 Lua 固定接收 100 个
结果，C 交替返回 300/600 个值，两个动态生成的调用者具有不同返回 PC；
其中一个调用者还使用 16 个 double 参数的 FFI 调用制造原生栈压力。
变参包装同时改变实际参数长度。要求入口侧 trace、返回前退出及编译
完成计数增加，变参用例还必须存在 RETFV；总 C 副作用次数为 14400。
两平台、两 GC 的 spills、blacklist 和 multret 在最终布局的断言构建中
分别重复 30 次通过。独立入口对齐后的两平台五配置清理重建和全量复验
日志为 `handler-{windows,linux}-{assert,release,nocframe,nocount,noboth}-build.log`
及 `*-ctest.log`；parity 日志为 `handler-parity-*`，最终压力日志为
`handler-*-stress.log` 及 `handler-*-<配置>-spills-repeat.log`。
精确总调用次数断言随后加入 Lua 测试，重新运行完整矩阵，日志为
`acceptance-{windows,linux}-<配置>-ctest.log`；回调、重入、coroutine、
flush、yield、生命周期及 spills 十八项测试在两平台断言构建中各重复
30 次通过，记录为 `acceptance-*-effects-stress.log`。

直接解释器退出的 interp 探针由 C 使用 LUA_MULTRET 调用 Lua 包装函数：
该包装函数返回到 C 帧，因此 RETM trace 在返回提交前停成 RETURN。
必须实际进入这种尚未完成 RETM 的 trace、触发数量回落，并验证全部结果、
尾部 nil 和恰好 3000 次 C 副作用。该路径明确要求存在没有 retm_retref
的 RETM/RETURN trace，不以一般结果正确代替路径证据。

真正 MULTRET 的数量变化包含 259/515，二者与预热的三个结果具有相同的
低八位。必须保持全部结果及真实数量，并实际触发数量入口回落。
预热扫描循环后再训练带动态参数的 consume 函数，使 RETM 后续 trace
可以链接到已编译的扫描函数；入口计数、返回完成计数和数量回落计数
均须增加，C 副作用次数必须恰为 1500。

### 验收映射

| 正式探针 | 主要覆盖 | 编译或恢复证据 |
| --- | --- | --- |
| fixed / vararg / expanded | 固定接收 0/1/4，实际返回 0/1/3/4/300/600；CALLMT、动态变参跨度、缺少参数、兼容 arg | 每种固定接收数量均要求超过 1000 次编译返回、无数量回落；vararg 要求 RETFV |
| multret / guards | 完整宽度数量变化、类型变化、不同接收者和目标返回 PC | 实际数量回落、入口侧 trace、返回前后退出；完整值与 C 调用次数 |
| interp | 返回到 C 帧，RETM trace 直接交给解释器完成返回 | 实际进入未提交返回的 trace、数量回落、完整结果与 3000 次副作用 |
| spills | 100 个固定结果、300/600 个实际结果、16 参数 FFI、固定及变参包装 | 编译返回、目标帧 guard 与入口侧 trace；14400 次 C 副作用 |
| frames / luatails / error / yield | 包装帧可见性、两个 Lua 虚拟尾层、错误处理器、yield/resume | 原生 C 回调中的帧扫描、精确结果及尾层数，与 PUC 参照分工验证 |
| reentry / coroutine / gc | C→Lua 回调、十二个独立 state、交替 coroutine、对象结果及 GC | 独立 state 编译完成、C 调用次数与逐个结果断言 |
| lifecycle / flush / blacklist / prefix | 单 trace/整体 flush、重录制、录制失败和黑名单、普通 RETM 消费者边界 | 重录制后的编译返回、C 副作用、prefix 不产生 RETM stitch 根入口 |

测试计数通过原生 probe 读取内部结构，不新增公共 Lua/C API。
它们只在 `LUAJIT_TEST_RETM=ON` 时编译；普通 stitch 和生产构建不执行
这些计数。独立 coverage 脚本另外按 trace 事件定位目标函数和字节码位置，
检查入口为 RETM、存在 RETF/RETFV 且链接调用者。

## 实际参照

实际上游使用独立源目录、独立 VM 和匹配其 import library 的原生模块，
没有用关闭 CFRAME 的 fork 代替上游。Windows 使用 msvcbuild amalg，
Linux 使用 stock Makefile/GCC。相同 coverage.lua 中，C 交替返回
300/600 个值，外层接收一个值；三个场景的校验和均为 2250005000。

| 场景 | 上游 Windows/Linux | 当前实现 Windows/Linux，两 GC |
| --- | --- | --- |
| 固定参数包装 | CALL 后续入口，stitch 并链接调用者 | RETM，RETF，链接调用者 |
| 变参包装 | CALL 后续入口，stitch 并链接调用者 | RETM，RETFV，链接调用者 |
| CALLMT 参数展开 | CALL 后续入口，stitch 并链接调用者 | RETM，RETF，链接调用者 |

当前两平台、两 GC 另由正式 probe 的根入口、IR 与运行时返回完成计数验收。
最终生产构建的独立覆盖日志为
`handler-fork-{windows,linux}-{default,arenagc}-coverage-{fixed,vararg,expanded}.log`。
每次使用该构建本次生成的 `variants/<GC>/vmdef.lua`；
coverage 运行通过 `package.preload["jit.vmdef"]` 指向这份文件，避免加载旧模块。

原版 Lua 5.1 原生模块已重新链接，三个大结果场景及十个旧探针模式均通过，
包括包装帧可见性、两层 Lua 虚拟尾层、错误处理、yield/resume。
PUC 校验语义；兼容基线校验回归；实际上游校验编译覆盖。

## 调试与生命周期

增加参数长度变化测试时，RETFV 的连续机器码发射曾超过 assembler
red zone。使用指定 cdb 抓取 assertion、lj_asm_trace 调用栈及反汇编，
定位后在新 IR 的发射中加入中途 checkmclim。完整断言矩阵复验通过。
日志：`retfv-redzone-cdb.log`。

`jit.flush()` 清除 trace，并不自动重启被惩罚的字节码。黑名单探针
恢复 maxrecord 后显式 jit.on(drive/fixed, true)，并重设 hotloop=3，
重新启用和重置热计数后验证录制。flush 清除惩罚缓存，并不重置原有
热计数表；不重设时，在 1200 次调用内未必触发重新录制。
没有改写上游 flush/blacklist 语义。

COUNT=0 的 spills 探针曾出现结果正确但没有目标侧 trace 的覆盖失败。
改为连续训练两个接收者各 800 次，随后交替返回 PC；hotexit=1、
tryside=16。仍严格要求入口侧 trace、返回前退出和编译完成，未放宽
验收条件。两平台五配置两 GC 的该探针分别重复 30 次通过。

## 性能验收

测试计数关闭；基线为独立构建的实际 c04ae378。Windows 使用
MSVC 19.51，Linux 使用 GCC 13.3；两边均为 RelWithDebInfo/O2、
NUMMODE=OFF。每个场景的 baseline/candidate 使用相同工具链、工作量、
预热与 CPU affinity。Windows affinity mask=4、AboveNormal；
Linux taskset CPU=2。使用 os.clock 测量循环本身，交替交换执行先后。
LuaJIT `lib_os.c` 直接调用 C clock；本机 MSVC CRT 的计时是墙钟，
Linux/glibc 的计时是进程 CPU 时间。不能声称 Windows 的测量排除了
调度等待。两平台各睡眠一秒的控制实验分别增加 1.012 秒和 0.000038 秒，
记录为 `clock-platform-control.log`。两平台的比值分别比较，不混合时间量。
统计独立配对总耗时比的对数均值，以 Student t 构造双侧 95% 区间，
报告几何均值和区间。比值为候选/基线；普通 stitch 上界不得超过
1.03，三个 RETM 工作负载的上界必须小于 1。不用最优样本作验收。

Windows 的 15 对测量，每项 C 探针 15000000 次、bench2 20000000 次，
预热 1000000 次，两次进程执行之间冷却 1000 ms。结果如下：

| GC | 场景 | 配对耗时比 | 95% 区间 | 数值门槛 |
| --- | --- | --- | --- | --- |
| default | ordinary | 1.003390 | [0.984254, 1.022897] | 通过 |
| default | fixed | 0.941204 | [0.924023, 0.958705] | 通过 |
| default | vararg | 0.874730 | [0.861884, 0.887768] | 通过 |
| default | bench2 tailc | 0.893447 | [0.883343, 0.903666] | 通过 |
| arena | ordinary | 0.978376 | [0.932260, 1.026773] | 通过 |
| arena | fixed | 0.930647 | [0.915182, 0.946373] | 通过 |
| arena | vararg | 0.883468 | [0.872782, 0.894285] | 通过 |
| arena | bench2 tailc | 0.932529 | [0.923889, 0.941249] | 通过 |

原始数据：`performance-layout-windows-{default,arenagc}.csv/json`。
ELF 对齐不改变 Windows 的生成路径。最终清理重建的两个 PE DLL，
34648/38498 行 .text 字节均与测量时一致；记录为
`handler-windows-{lua51DS,lua51DS_arenagc}-text.log`。
这组结果符合数值门槛，不代表游戏进程中的性能已经验证。

最终 Linux 独立入口对齐布局的测量如下。普通 stitch 为 30 对，每对
20 个 3000000 次的小块，预热 10000000 次；三个 RETM 负载为 15 对，
每对五个块，fixed/vararg 每块 10000000 次、bench2 每块 20000000 次，
均预热 10000000 次。进程在每对内常驻，块间交替、无冷却。

| GC | 场景 | 配对耗时比 | 95% 区间 | 数值门槛 |
| --- | --- | --- | --- | --- |
| default | ordinary | 0.994509 | [0.980422, 1.008799] | 本轮符合 |
| default | fixed | 0.904316 | [0.870449, 0.939501] | 通过 |
| default | vararg | 0.873785 | [0.860688, 0.887082] | 通过 |
| default | bench2 tailc | 0.876866 | [0.859323, 0.894767] | 通过 |
| arena | ordinary | 1.012498 | [0.996630, 1.028619] | 本轮符合；控制组不足以验收 |
| arena | fixed | 0.915294 | [0.889569, 0.941763] | 通过 |
| arena | vararg | 0.876508 | [0.865899, 0.887247] | 通过 |
| arena | bench2 tailc | 0.924607 | [0.916854, 0.932425] | 通过 |

原始独立配对数据为 `performance-handler-linux-{default,arenagc}.csv/json`
及 `performance-handler-tail-linux-{default,arenagc}.csv/json`，
全部块数据在对应的 `*-blocks.csv` 中。普通 stitch 不因这一轮的数值
符合门槛而覆盖旧失败记录或控制组的不确定性，最终合入验收仍未完成。

### 调度干扰与控制测量

宿主机同时运行很多进程。Windows 的调度等待直接计入当前计时；
Linux 进程 CPU 时间排除了大部分未运行的等待，但仍无法排除频率变化、
缓存/内存竞争和 WSL 虚拟 CPU 调度的影响。
连续的 Linux arena 普通 stitch 数据中，基线 50000000 次循环的
CPU 耗时从约 0.65 秒变化到 1.44 秒；不能只靠延长循环或取消冷却
把这一组不确定结果记成通过。

新增常驻进程分块测量：每个独立配对重新创建两个 VM，分别预热
10000000 次；普通 stitch 交替执行 20 块，每块 3000000 次。
每组两边各自累计块耗时后形成一个比值，30 个独立配对用于区间计算。
组内块只用于诊断，不当作 600 个独立样本；没有删去异常块或配对。
Linux 的模块目录及 LD_LIBRARY_PATH 分别指向对应 VM。

用同一个 c04 VM、同一原生模块在两个进程中执行完全相同的工作量，
得到下列基线对基线控制测量：

| GC | 对数配对均值对应的比值 | 95% 区间 | 对 3% 容差的含义 |
| --- | --- | --- | --- |
| default | 1.010668 | [0.999032, 1.022439] | 出现约 1% 估计偏差；不能据此校正候选比值 |
| arena | 1.007546 | [0.982938, 1.032770] | 同一代码也超过 1.03，当前环境不能可靠验收该容差 |

原始数据为 `performance-control-aa-linux-{default,arenagc}.csv/json`，
全部块数据另存 `*-blocks.csv`。控制组不参与候选比值的修正或筛选。
因此 Linux arena 普通 stitch 的最终性能结论保持“无法可靠判定”，
需要在低干扰环境复测；当前实现不能据此声明最终可合入。

旧布局的 `performance-aligned-linux-*`、`performance-acceptance-linux-*`、
`performance-ordinary-{steady,continuous}-linux-*`、分块的
`performance-blocked-linux-arenagc` 均完整保留。其普通 stitch 多轮区间
上界超过 1.03；其中连续 arena 一轮为 1.057157
[0.992472, 1.126059]。它们不能被后续一次符合门槛的测量抹去。
独立入口对齐前后诊断为 0.990844 [0.982033, 0.999733]，
数据为 `performance-handler-diagnostic-linux-arenagc.csv/json`。
这个诊断比较的是两种候选布局，不是 c04 基线验收。

Windows 分块脚本的 1000000 次短块冒烟为 1.052230
[0.963525, 1.149100]，不符合 3% 门槛；记录为
`performance-blocked-windows-smoke.*`，同样保留。更短的 1000 次块
在 Windows clock 分辨率下出现零耗时，原始输出在
`*-smoke-low-resolution.*`。脚本现明确拒绝零耗时并要求增加迭代数，
不把 NaN 作为有效配对。冒烟用于验证脚本和计时限制，不取代上述
较长工作量的测量；繁忙 Windows 宿主机也需要低干扰环境的验收记录。

## 复现与材料

主仓库材料目录：`builds/retm-implementation/`。
Windows 最终构建：`final-windows-{assert,release,nocframe,nocount,noboth}`。
Linux 本地目录：`/tmp/retm-final-linux-{assert,release,nocframe,nocount,noboth}`。
Linux 验证是本机 WSL 实际执行，尚未在 GitHub hosted runner 执行。
CI 工作流 `.github/workflows/retm.yml` 已加入相同五配置、两个 OS；
未推送，因此没有远端 CI 执行记录。

```powershell
cmake -S test/retm/runner -B build-retm -G Ninja `
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLUAJIT_NUMMODE=OFF -DLUAJIT_TEST_RETM=ON
cmake --build build-retm --target luajit luajit-arenagc retmprobe-default retmprobe-arenagc
ctest --test-dir build-retm --output-on-failure
```

Unix 相同参数；断言构建添加
`-DCMAKE_C_FLAGS="-DLUA_USE_ASSERT -DLUA_USE_APICHECK"`。
生产/性能构建保留 `LUAJIT_TEST_RETM=OFF`，没有运行时诊断计数开销。

审查顺序可以按运行边界分为：`lj_ffrecord.c` / `lj_dispatch.c` /
`vm_x64.dasc` 的入口与 continuation；`lj_jit.h` / `lj_record.c` /
`lj_trace.c` 的槽图、元数据及生命周期；`lj_asm.c` / `lj_snap.c` 的独立
数量槽和恢复；`lj_ir.h` / `lj_asm_x86.h` / `lj_opt_fold.c` 的动态变参返回；
最后是测试、CI 和本记录。入口与恢复的安全保护应作为同一组评审，
不能单独放开 RETM 后再补数量保护。

记录包括各配置 configure/build/ctest 日志、两平台 parity 输出、
实际上游 coverage 日志、PUC 输出、性能 CSV/JSON 及 cdb 日志。
macOS、实际游戏进程性能与其他 CPU 后端不作已验证声明。

### 审查补丁与证据索引

材料目录中的 `retm-stitch-c04.patch` 包含全部三十个改动/新增文件，
以 c04ae378 为应用基线；另按顺序提供三组补丁：

1. `01-runtime-core.patch`：正式入口、数量保护、动态变参返回、
   recorder/assembler/快照恢复及 ELF 布局。入口与恢复放在同一组，
   避免应用一个尚无数量保护的入口。
2. `02-tests-build-ci.patch`：测试构建选项、独立原生模块、正式测试、
   参照及性能工具、历史探针和 CI。依赖第一组。
3. `03-evaluation-docs.patch`：历史评估更新、本实现记录与限制。

完整补丁和分组补丁均在独立 c04 源码上执行 git apply --check；
这些是审查分组，没有形成提交，也没有合并或推送。
`source-sha256.csv` 覆盖全部三十个源文件，`binary-sha256.csv` 记录
验收使用的两平台 VM 和原生模块；`generated-vmdef-sha256.csv`
记录本次生成的生产 vmdef。`validation-index.csv` 索引最终十个矩阵
通过结果，`performance-index.csv` 索引测量及控制组，保留所有失败轮次。

`baseline.bundle` 的 HEAD 为真实 c04ae378。需要独立复建时，在新的目录
初始化 Git，fetch 该 bundle 的 HEAD，checkout FETCH_HEAD，再应用完整
补丁；不要仅复制无 Git 元数据的旧源码目录复建版本号。随后使用前述
runner 构建命令。Linux 本机源码副本已同步到相同的三十个文件哈希。

原定性能验收条件仍未全部完成：普通 stitch 需要低干扰环境性能验收，
远端 Windows/Linux CI 需要实际执行；macOS 保持未验证。

## 2026-10-14 主干整合

用户在完成 CALL 续接与当前 RETM 实现的直接对比后要求合入主干。
整合保留 RETM 入口方向，没有合入封存的 `cont_stitch_return` 方案。
主干 `2353c551` 的表构造器拆分和相关测试一并纳入整合构建。

2026-10-12 的直接配对测量使用相同 c04 基线、工具链和公开 C 探针，
每场景 15 个独立配对、每对五块交替执行，两种 GC、Windows 与 WSL。
RETM 相对 CALL 的单结果耗时少 3.8%–5.8%，变参包装少 4.7%–8.8%；
bench2 tailc 则多 2.1%–7.4%。普通 stitch 三种配置及 Linux arena
四结果的区间跨过 1，保留全部异常样本后均为无法判定。不能以这些
数字声明 3% 回退门槛通过，也不能隔离 CFRAME 对上游的成本。

完整记录在主仓库 `builds/retm-implementation/call-vs-retm/report.md`，
包括 26 个汇总、390 个独立配对、1950 个双边块记录及 119 项证据索引。
上游独立对照在 `builds/retm-implementation/upstream-comparison/`。

本次提交同时修正 `test/retm/measure.ps1`：WSL worker stdin 使用 LF，
并检查退出码。原脚本的 CRLF 使退出命令成为 `quit\r`，在已输出计时
数据之后触发断言；原性能样本没有回写，校验、块总和和区间均已复核。

整合矩阵及主仓库 parity 的实际结果将在验证完成后记录。

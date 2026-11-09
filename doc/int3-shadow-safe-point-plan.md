# INT3 full-shadow 与安全点挂载实施计划

## 1. Context：目标与边界

将现有 INT3 从覆盖 `proto_bc(pt)` 改为覆盖**仅供解释器新取指使用的完整 `BCIns` shadow**。原数组始终保留语义指令；其他模块不再需要为读取 INT3 恢复原指令。

本计划针对当前源码基线 `7912f84220128b7f9cad1f9ddac6ec48388d2d57`。保留 `src/lj_int3.c` 的 `static Int3State int3_state`、进程级单 session 和跨 VM 排他规则，不移植其他 worktree 的 per-VM session 重构。

已确认的用户选择：

- 自动延迟挂载：安全调用同步兑现；允许的录制／编译上下文先登记，下一安全点兑现。
- 挂载发布前允许漏掉命中；发布后不允许因 shadow 过期而改变程序行为。
- 缓存映射，优先普通取指热路径；不逐指令追溯当前函数的 proto，不逐指令扫描断点表。

不在范围内：扩大可断点 opcode 集合、异线程／信号处理器调用、并发 session、重做 trace 清理算法、重构 profiler、增加显式公共 commit API。

本文是实施计划，**尚未实施，也没有构建／测试通过的声明**。文中的新增字段、接口及结果码均为拟实施设计。

## 2. 正确性不变量

1. `proto_bc(pt)`、`fn->pc`、VM 的 `PC`、栈帧返回 PC、continuation PC、snapshot PC 始终属于原数组。不得把 shadow 地址存入这些位置；`PC2PROTO` 和 `proto_bcpos` 的含义不变。
2. shadow 与原数组等长，包含完整的 `BCIns[sizebc]`。只有 armed 槽的 shadow opcode 为 `BC_INT3`，其余位与当前语义指令一致。
3. 新取指可以读 shadow；同一指令的 hook／慢路径重读和操作数窥读仍读 canonical。不得把“读一次字节码”一概改成新派发。
4. mounted proto 已清理所有执行／内联它的 trace，并受 `PROTO_NOJIT` 和现有 hot／stitch／jit.on 入口保护。pending proto 尚不受此限制。
5. 任何 pending 或 mounted 请求都持有对应 `GCROOT_INT3+i`。映射撤销、shadow 释放及 NOJIT 释放完成后，才清除最后的根。
6. 同一 proto 共享一个 owner 和一个 shadow；NOJIT 所有权属于 owner，不再属于某个 breakpoint entry。
7. 兑现路径不分配、不执行 Lua、不触发 GC。INT3 命中交付前先复制 canonical 指令；callback 中 cancel／stop 后，本次指令仍只执行一次。
8. 挂载期间仍会执行的 `ISNEXT` 自修改必须同步原数组和 shadow。安全点和禁编译不能替代这一同步。

## 3. Approach：状态与接口

### 3.1 数据布局

在 `#if LJ_HASPROFILE` 下增加以下字段，避免 profile-disabled 构建承担新增布局与取指成本：

|位置|新增字段|含义|
|---|---|---|
|`GCproto`|`uintptr_t int3ofs`|原数组到 shadow 的字节偏移；0 表示未发布|
|`global_State`|`uintptr_t int3bias`|当前解释器 Lua 帧的热路径缓存偏移|
|`global_State`|`MRef int3pc`|缓存对应的 canonical 函数入口；用于发布／撤销时精确失效|

偏移以整数地址计算：`(uintptr_t)shadow - (uintptr_t)proto_bc(pt)`；映射也通过 `uintptr_t` 加法后转换回指针。不得对两个独立数组做 C 指针相减，也不得把偏移截断成 `MRef`／32 位整数。

`int3pc` 不持有 shadow，也不是新的 GC 所有权。它只作地址相等比较；切帧时覆盖，撤销全部请求时归零，不通过过期缓存解引用 proto。新增这一键避免公共 API 被传入另一个 coroutine 的 `L` 时，错误地按该 `L` 或不可靠的 `g->cur_L` 重建正在运行帧的缓存。

初始化：`lj_parse.c` 的 `fs_finish()`、`lj_bcread.c` 的 `bcread_proto()` 将 `pt->int3ofs` 置零；`global_State` 随现有初始化归零。字段不要插在标为仅 debug/traceback 的字段区内。

session 继续使用固定容量表，不增加动态容器：

- entry：owner 索引、`bcpos`、pending／mounted 状态；原有逐槽 `ins` 与 `nojit` 删除。
- owner：`GCproto *pt`、`BCIns *shadow`、分配字节数、请求引用数、mounted 状态、`owns_nojit`。
- entry 和 owner 表均最多 `LJ_INT3_MAX` 项；pending+mounted 请求总数仍最多 32。
- 同 `(pt,bcpos)` 重复 arm 幂等，先查重复再判断 FULL，不重复分配、flush 或取得所有权。

### 3.2 内存与错误

首个 proto 请求接受前预留 `pt->sizebc * sizeof(BCIns)`，一个 proto 只分配一次。不把 shadow 纳入 `sizept`，不改变 dump/debug 数据的序列化布局。GCproto 自身增大后的内存偏移仍由现有 `sizeof(GCproto)` 计算。

计划新增公共结果 `LUAJIT_INT3_NOMEM = 7`；现有 0–6 的编号和四个公共函数签名不变。

预留直接调用 VM allocator：`g->allocf(g->allocd, NULL, 0, bytes)`。NULL 时返回 NOMEM，不建立 entry/owner、不设根、不改 flags 或 dispatch。成功时沿用 `lj_mem_realloc` 的地址检查与 `g->gc.total` 记账规则；释放使用 `lj_mem_freevec`。地址不满足本构建的指针约束时，先释放该块再返回 NOMEM。不得在只读 INT3 callback 内通过 `lj_mem_new` 抛 Lua OOM。

预留阶段**不复制字节码**：pending 期间 trace 仍可改写原数组，复制必须发生在安全点清理之后。

### 3.3 内部接口与旧接口清理

新增最小接口：

```c
LJ_FUNC int lj_int3_has_pending(global_State *g);
LJ_FUNC void lj_int3_commit(lua_State *L);
LJ_FUNCA BCIns *LJ_FASTCALL lj_int3_isnext(lua_State *L,
                                         const BCIns *pc);
```

owner 查找、发布／撤销缓存、ISNEXT 双写均留为 `lj_int3.c` 内部 helper，不引入通用字节码写入框架。

保留 `lj_int3_hit`、`lj_int3_jit_on_blocked`、`lj_int3_active`、`lj_int3_close`；`jit_on_blocked` 只把 mounted owner 视为禁编译对象，保留子 proto 检查。pending-only session 仍与 profiler 互斥。

完成 cutover 后删除 `lj_int3_bcins`、`lj_int3_pending`、`lj_int3_bcwrite_view`、`lj_int3_ins`，不要留下直接转发的兼容 wrapper。具体迁移见第 8 节。

## 4. 自动延迟挂载与兑现安全点

### 4.1 公共 API 行为

`luaJIT_int3_arm` 的 OK 表示**请求已接受**，不保证已经发布。返回前完成 session、函数类型、bcpos、mutable-op、重复／容量检查和必要的内存预留；成功登记后立即设 GC 根。

|调用上下文|arm 的行为|
|---|---|
|宿主安全调用、普通解释器 C 调用|登记后立即尝试 commit|
|普通 Lua debug hook|允许 arm；满足 JIT 安全条件时立即兑现，否则等待|
|自己的 INT3 callback|保持现有允许 arm／cancel／stop 的能力；仅在安全时兑现|
|RECORD／OPT／ASM 上下文|仅接受 pending，不 flush、不改 NOJIT|
|RECORD／OPT／ASM 内的同步 VM event|仅登记 pending；不能在 event 内兑现|
|GC、profiler、其他 VM event、trace exit／实际运行的机器码|BUSY|
|异线程、信号处理器|不支持，不增加锁或 atomics|

判定 VM 状态使用现有取反编码（例如 `~LJ_VMST_RECORD`），不能直接比较未取反枚举。对 VM event 的例外严格限制为上述录制／编译状态；不能普遍放开 `HOOK_VMEVENT`。自己的 callback 保留现有 self-inflicted mask 例外。

`start`、`cancel`、`stop` 的上下文限制不随 arm 一起放宽：普通 hook、录制／编译／外部 VM event 中仍按现有约束返回 BUSY。安全上下文中的 cancel／stop 必须清理 pending 与 mounted；不新增 recorder 内 pending-only cancel 的特殊规则。

### 4.2 不能只设置 DISPMODE_INS

源码中 `vm_inshook` 在没有用户 line/count mask 时直接跳过 C 调用。因而仅强制 `DISPMODE_INS|DISPMODE_CALL` **不能保证等待请求进入安全点**。

实施时增加内部 `DISPMODE_INT3 = 0x80`：

1. 有 pending 时把 `INT3|INS|CALL` 加入 `lj_dispatch_update` 计算的 mode。
2. 把 INT3 位纳入整张 instruction dispatch 表重建条件。即使用户 hook 原本已使 INS 为 1，pending 的变化也必须更换 dispatch target。
3. pending 且不在 profiler／recorder 模式时，选用现有 `lj_vm_record` 的“总是进入 `lj_dispatch_ins`”派发骨架，而不是 `lj_vm_inshook`。
4. 七后端的 `vm_record` 前导代码在 `JIT or INT3` 时可用；保留 VM event 屏蔽及用户 hookcount 的既有更新，不调用编译宏关闭时不存在的 recorder 字段／代码。实际 recorder 仍由 C 内 `LJ_HASJIT` 和 `J->state` 判断。
5. call dispatch 继续使用现有 `vm_callhook`，不安装／替换用户 hook。
6. pending 清空后调用 `lj_dispatch_update`，去掉内部位，恢复由原用户 hook/JIT 状态决定的派发表；BC_INT3 始终指向自己的直接 handler。

普通取指不增加 pending poll；只有等待期间临时经过既有 C dispatch。

### 4.3 commit 条件与顺序

在 `lj_dispatch_ins` 和 `lj_dispatch_call` 完成 recorder／用户 hook 工作之后、返回汇编之前调用 commit。call 路径必须覆盖 hot-call 的 `goto out`，并在最终读取 FUNC opcode／选择静态目标之前兑现。

commit 只在所属 session 有 pending、无 GC/profile/外部 VM event，且下列 JIT 条件成立时工作：

- `J->state == LJ_TRACE_IDLE`；
- `tvref(g->jit_base) == NULL`；
- `J->patchpc == NULL`。

noJIT 构建省略这些 JIT 字段检查。普通 Lua debug hook及自己的 INT3 callback可按同样条件同步兑现；外部 recorder/compiler event只登记。

按 owner 批量处理：

1. 首次挂载：复用已有 `int3_flush_traces` 清理执行／内联该 proto 的 trace。不要换成会分配并发 VM event 的 `lj_trace_flushall`。
2. 记录 `owns_nojit = !(pt->flags & PROTO_NOJIT)`，设置 PROTO_NOJIT。
3. 将清理后的完整 canonical 数组复制到预留 shadow。
4. 将该 owner 的所有 pending 槽在 shadow 内改为 INT3，保留操作数；标记 entries mounted。
5. 最后发布 `pt->int3ofs`。若 `g->int3pc` 等于 `fn->pc` 对应的 canonical 入口，同步 `g->int3bias`。
6. 已 mounted owner 上新增请求只标记相应 shadow 槽，不重新复制／flush，避免抹掉既有 trap。
7. 清空 pending 后撤销强制 dispatch mode。

不要在 `lj_trace_exit` 内 flush：其返回后的汇编可能继续读取 `GCtrace.startins`、恢复寄存器或处理 `-17`。允许本次退出或被 hook 打断的执行漏断，下一次真正新取指才使用已发布 shadow。

## 5. 缓存映射：新取指与切帧

### 5.1 普通取指

普通取指读取 `canonical_address + g->int3bias`；PC 自身按原规则递增。无 shadow 时 bias 为 0。字段与新汇编使用 `LJ_HASPROFILE`／DynASM `INT3` 一致 gating。

|后端及当前锚点|取指改法／寄存器约束|
|---|---|
|x64 `ins_NEXT`，约 212–219|用 `TMPR` 装入 bias，`mov RCd,[PC+TMPR-4]`；不能抢占 opcode 解码前仍有意义的 RB|
|x86 `ins_NEXT`，约 336–346|用解码前的 ecx 装 bias，eax 取完整指令，随后再由 ah 解码 RA；不要为每条指令压栈|
|ARM `ins_NEXT1/2`，约 120–142|opcode 与完整指令分阶段读取都必须映射；使用该阶段可用 CARG1，显式保持原 canonical PC 的递增|
|ARM64 `ins_NEXT`，约 230–238|使用死临时 TMP2 装 bias，`ldr INSw,[PC,TMP2]` 后 PC 加 4|
|MIPS `ins_NEXT1`，约 240–246|TMP0 组成映射地址，读取 INS 后仅递增 canonical PC|
|MIPS64 `ins_NEXT1`，约 242–248|同上，偏移／地址运算使用完整指针宽度|
|PPC `ins_NEXT1`，约 400–406|TMP2 装 bias，indexed load；保留 split-fetch 的既有时序|

`ins_callt` 的 FUNC header 读取继续使用 canonical：函数头不可 arm，且可能是 C/fast-function 的 synthetic bytecode，不能套用上一 Lua 帧的 bias。Lua header handler 在进入函数体前初始化缓存。

### 5.2 刷新规则

一次刷新同时写 `g->int3pc = canonical_fn_pc` 和 `g->int3bias = pt->int3ofs`。优先合并进已有 `LFUNC->pc`／KBASE 重载块，不新增每指令函数／proto 追溯。

|路径|七后端必须覆盖的位置|注意事项|
|---|---|---|
|Lua 调用、尾调用进入|`BC_IFUNCF`／`BC_IFUNCV` 的 KBASE 初始化块|从 `PC-4+PC2PROTO(int3ofs)` 读取；JIT 关闭分支也必须覆盖|
|返回 Lua caller|`BC_RET_Z`／相应 `vm_return` 回 Lua 分支|先取 fn->pc 对应缓存，再覆盖 KBASE 为常量区|
|metamethod／continuation|`cont_dispatch`|恢复 caller 的缓存|
|可重入 C helper、stitch/profile 返回|`cont_nop` 及不经过它的返回路径|不能假设 C 调用期间缓存未被嵌套 Lua 执行覆盖|
|fast-function 返回|`fff_res`／`fff_res_`|ARM/MIPS/PPC 等必须在 split `ins_next1` 真正取指之前刷新，而非一律放在 BASE 更新后|
|用户 hook／recorder 返回|`lj_dispatch_ins` 返回块、`cont_hook`|使用恢复后的 BASE；同指令重读仍读 canonical|
|trace exit|`vm_exit_interp` 的手写取指|Lua 帧先刷新并映射；C/fast-function synthetic PC 不加 Lua bias|
|fast-function 下层 caller|尾调／trace-exit 中专门恢复下层 KBASE 的分支|不能沿用上层 fast-function 进入时的缓存|
|coroutine resume/yield|核对其最终经过 header、return 或 continuation 的路径|每条到首次新取指的路径必须已刷新，不能只依赖 `cur_L` 更新|
|发布／卸载|INT3 内部发布 helper|仅当缓存键匹配目标 proto 时修改 bias；全量清理将键和偏移归零|

x86 的慢路径刷新若需要 ecx，必须按该点活跃寄存器保存／恢复，不能把普通取指的 scratch 可用性套用于慢路径。Win64 的新代码不得破坏既有非易失寄存器保存契约。

C/fast-function header 本身不把缓存改成 synthetic fn->pc；返回／continuation 路径负责恢复实际 Lua caller。trace exit 对 C/fast-function 的分支则明确使用 canonical 读取，不能从 synthetic PC 反推 GCproto。

`GCtrace.startins` 静态派发、FORI 转换重试、hook continuation 和慢路径的 PC_OP/PC_RD 重读不是新取指，保留 canonical，不重新触发 INT3。

## 6. ISNEXT 同步与写入边界

保留 `int3_mutable_op()` 黑名单，包括 ISNEXT、ITERN、循环／FUNC／RET 类及其现有 patched variants；不改变现有融合／配对指令的派发语义，也不宣称每个字节码槽都存在独立停点。RETM 的既有可 arm 性保持。

同 proto 其他位置 armed 时，ISNEXT 虽不可 arm，仍可能从专用迭代退化为一般迭代。noJIT 下也存在此路径。

七后端的 ISNEXT 失败分支统一调用内部 leaf helper `lj_int3_isnext(L, pc)`，其中 pc 指向 ISNEXT 之后：

1. 从 canonical `pc[-1]` 取得原跳转，计算 canonical target。
2. 把当前 ISNEXT 完整指令的 opcode 改为 JMP，保留跳转和操作数；同时写原数组及已发布 shadow。
3. 把 target 的 ITERN 改为 ITERC；若该后端原本支持 JLOOP 还原，则先取 trace 的完整 startins，再改为 ITERC，并同步完整指令。
4. 返回 canonical target；汇编恢复 ABI 必要状态，再走正常新取指。

helper 无分配、无 GC、无 Lua 调用、无 dispatch 更新。进入前保证 `L->base` 与 BASE 一致，返回后按 ABI 恢复 BASE。PPC 原有 JLOOP 分支 NYI 不借此扩大支持范围。INT3 编译关闭时保留原有失败分支。

**不把全部 JIT writer 改成双写。** 依据当前源码的可达性：

- 挂载前，trace_stop／unpatch／blacklist 可改写 canonical；最终 shadow 只在清理之后复制。
- 挂载后，现有 `lj_trace_hot` 和 `lj_trace_stitch` 在进入 recorder state machine 前检查 `lj_int3_jit_on_blocked`；不能在迁移时删掉这两个入口保护。
- 清理覆盖含有目标 snapshot／内联引用的 trace，因此 mounted proto 不应再成为 hotside 的目标；显式 jit.on 及子树开启继续被拒绝。
- `trace_start` 的 PROTO_NOJIT lazy patch、`trace_stop`、`trace_unpatch`、临时 `J->patchpc` 对 mounted proto 均应不可达；加窄断言验证该前提，不以普遍双写掩盖不变量失效。
- `lj_trace_reenableproto` 必须在映射撤销之后调用。显式 jit.flush/off 只处理仍存在的 trace，不得绕过 mounted 禁编译约束。

ISNEXT 是上述隔离以后仍然可达的解释器写入，必须真实同步，不能删除或只保留注释。

## 7. 命中、撤销、NOJIT 与 teardown

### 命中

`lj_int3_hit` 用 canonical `pc-1` 查 mounted entry。保留 stray trap 错误、topslot 修复、只读 native callback、hook/vmstate 保存恢复规则。

交付前复制 `*canonical_slot`、proto、bcpos 到局部变量；callback 后直接返回这条完整指令给汇编静态派发，既不重读已释放 shadow，也不重新执行 INT3。永久断点不需要逐次恢复／重新布置。

### 撤销

cancel／stop 的正常安全路径按 owner 清理：

1. 先撤销 `pt->int3ofs`，同步／清除匹配的热缓存。
2. 再释放该 owner 的 shadow。
3. 若 owner 为 mounted 且 `owns_nojit`，才调用 `lj_trace_reenableproto` 并释放自己的 PROTO_NOJIT。
4. 最后清 roots、entries、owner，更新计数与派发表。

pending-only owner 只释放预留内存和根，不修改 NOJIT。已有 `jit.off(func)` 不得被 cancel/stop 开启。多个请求不再依靠 entry 之间转移 NOJIT 所有权，消除“先撤销拥有者，最后幸存者无法释放”的旧缺陷路径。

### 关闭

在 `lua_close` 切换到 main thread 后、正常 finalizer 循环前调用一次 `lj_int3_close`；`close_state` 不再重复调用（初始化失败路径不可能存在 session，`lj_int3_close` 本身仍幂等）。关闭不交付 callback；卸载后 finalizer 执行不得再读取已释放 shadow。

## 8. 文件级实施顺序与干净切换

|顺序|文件|实施内容|
|---|---|---|
|1|`src/lj_obj.h`、`lj_parse.c`、`lj_bcread.c`|gated metadata、初始化；不改变 canonical PC 定义|
|2|`src/lj_int3.c/.h`|固定 owner/entry 状态、预留、pending、commit、缓存发布、ISNEXT leaf、命中／撤销生命周期|
|3|`src/lj_dispatch.c`|内部 INT3 mode、可靠 C 派发、两个 commit 出口、保留 mounted jit.on 保护|
|4|七个 `src/vm_*.dasc`|映射取指、所有缓存刷新点、pending 派发骨架、ISNEXT helper；删除旧 `vm_int3_cont` 及同指令 trap 恢复分支|
|5|`src/lj_dispatch.h`|移除 GOT 里的 `lj_int3_ins`；按现有 GOT 规则加入 MIPS 等使用的 `lj_int3_isnext`|
|6|`src/lj_trace.c`|保留 hot/stitch 保护；将旧 logical-view 读取改为 canonical；删除基于物理 INT3 的 pending 判断及失效注释，不改 flush 算法|
|7|`src/lj_meta.c`|删除 `meta_bc_op` 等专为 BC_INT3 恢复的分支及调用；慢路径直接读 canonical opcode／instruction|
|8|`src/lj_debug.c`、`lib_jit.c`|原 logical-view 调用改为直接读取原数组|
|9|`src/lj_bcwrite.c`|删除 `lj_int3_bcwrite_view` 调用及只为它存在的参数／局部值，保留现有 JIT unpatch 和 endian 处理|
|10|`src/lj_state.c`|finalizer 前卸载，保留失败关闭路径|
|11|`src/Makefile`、`msvcbuild.bat`、`xb1build.bat`|一致传递 DynASM INT3 功能条件|
|12|`src/luajit_rolling.h`、`doc/ext_c_api.html`、`int3-smoke.c`|更新 OK／延迟／NOMEM 契约，修正旧 BUSY 断言，增加行为回归|

导出内部符号迁移前按 LSP references 检查；本次研究中 clangd 只返回部分声明，不能视为完整调用图。需要用上述确定的调用点和精确符号搜索补足；最终不得残留四个旧 logical-view helper 的声明、实现、调用、GOT 项或旧汇编标签。`lj_dispatch_topslot` 的 global_State 参数若迁移后无用，一并迁移所有调用者，勿保留无意义参数。

公共生成头只改 `luajit_rolling.h`，不要手工编辑生成的 `luajit.h`。`lj_bcread` 对外来 BC_INT3 的拒绝保留：合法 dump 不携带 shadow。

### 构建条件

`src/Makefile` 紧邻 JIT／FFI flag 派生处添加：

```make
ifneq (,$(findstring LJ_HASPROFILE 1,$(TARGET_TESTARCH)))
  DASM_AFLAGS+= -D INT3
endif
```

这是已有 `LJ_HASPROFILE` 的 DynASM 映射，不增加公共配置宏。七后端用 `.if INT3` 对应 C 的 `#if LJ_HASPROFILE`。

`msvcbuild.bat` 的 x64、x86 两份 DASMFLAGS 以及 `xb1build.bat` 的 DASMFLAGS 补上 INT3，覆盖当前脚本正常 profile-enabled 配置。不得无条件给 profile-disabled 目标加该 flag；如调用方自行定制禁用 profile，C 编译条件和 DynASM 条件必须一起调整。其他 OS_OTHER 脚本不机械增加 INT3。

## 9. Verification：行为验收

复用 `int3-smoke.c`，不建立第二套 harness。保留有行为价值的现有测试，更新因契约改变而错误的断言；不测试源码文本、wrapper 转发或字段名。

|范围|必须证明的行为|
|---|---|
|canonical 不受污染|arm 前后 `jit.util.funcbc`、dump/reload、debug 行号与实际程序结果一致；trap 只由新取指触发|
|安全挂载|已有相关 trace 被清理，无关 trace 保留；mounted 目标不再录制／内联；cancel 后能重新编译|
|自动延迟|真实 record／compile event 内 arm 返回 OK；event 内未发布；回到 dispatch 安全点后可重复命中；无用户 hook、只有用户 call hook、已有 line/count hook 三种模式均兑现|
|原上下文限制|GC／profiler／无关 VM event 仍 BUSY；hook／recorder 中 cancel/stop 不被意外放宽|
|ISNEXT|先确认 fixture 真有 ISNEXT/ITERN，再替换 next 触发退化；非空与空表结果正确；同 proto 其他断点继续准确命中；noJIT 也覆盖|
|缓存切换|两个 proto（仅一个 mounted）、递归／尾调用、变参、metamethod、普通 C/fast-function、C→Lua 重入、协程交替 yield/resume、JIT exit 后结果与 callback proto/bcpos 准确|
|交付生命周期|同槽重复 arm 幂等、重复命中恰一次；callback cancel/stop 后本次副作用只发生一次，不再命中且无 UAF|
|所有权|同 proto 多槽、跨 proto、预先 jit.off、最后一个请求清理、GC 根、finalizer 前卸载；不得重现 NOJIT 泄漏|
|失败边界|定制 allocator 只拒绝首次 shadow 分配：NOMEM、无部分发布／根／flags 泄漏；既有 owner 新槽不再分配；第 33 个请求 FULL，重复已有槽仍 OK|
|编译关闭|noJIT 保留解释器功能；no-profile API 返回 UNAVAILABLE，新增字段／映射汇编不参与构建|

优先扩展已有 `[4]`、`[5]`、`[8]`、`[9]`、`[10]`、`[13]`、`[14]`，新增 `[16]` 放延迟兑现专项。解释器共有场景不要错误地全放在 `!SMOKE_NO_JIT` 条件内。现有 noJIT harness 不注册 16，直接运行它的 16 会零场景通过，不能用作验证。

`ISNEXT` fixture 必须先用 `find_bcpos_n` 确认 opcode；普通泛型 for 不保证满足 parser 的 `predict_next` 条件。命中次数断言必须针对特定函数／槽，不能以全局 trace 数或“非空”替代。

### 9.1 隔离构建与运行命令

以下为实施完成后的执行命令，本次未执行。所有构建都在临时副本内；不清理或覆盖当前源码树的已有构建产物。

```sh
set -eu
repo=$PWD
check=$(mktemp -d /tmp/luajit-int3-shadow.XXXXXX)

for variant in jit nojit noprofile bare; do
  d="$check/$variant"
  mkdir -p "$d"
  cp -a "$repo/src" "$repo/dynasm" "$repo/.relver" "$d/"
  make -C "$d/src" clean
  flags='-DLUA_USE_ASSERT -DLUA_USE_APICHECK'
  smoke=''
  case "$variant" in
    nojit) flags="$flags -DLUAJIT_DISABLE_JIT"; smoke='-DSMOKE_NO_JIT' ;;
    noprofile) flags="$flags -DLUAJIT_DISABLE_PROFILE"; smoke='-DSMOKE_NO_PROFILE' ;;
    bare) flags="$flags -DLUAJIT_DISABLE_JIT -DLUAJIT_DISABLE_PROFILE"
          smoke='-DSMOKE_NO_JIT -DSMOKE_NO_PROFILE' ;;
  esac
  make -C "$d/src" -j2 BUILDMODE=static CCDEBUG=-g XCFLAGS="$flags"
  cc -O2 -g -funwind-tables -I"$d/src" $smoke \
    "$repo/int3-smoke.c" "$d/src/libluajit.a" \
    -Wl,-E -lm -ldl -pthread -o "$d/int3-smoke"
  (cd "$d" && LUA_PATH="$d/src/?.lua;;" timeout 120s ./int3-smoke)
done

LUA_PATH="$check/jit/src/?.lua;;" \
  "$check/jit/src/luajit" -e 'assert(jit.status())'
LUA_PATH="$check/nojit/src/?.lua;;" \
  "$check/nojit/src/luajit" -e 'assert(not jit.status())'
```

四配置各完整运行一次适用的既有 smoke；no-profile 两配置实际只运行 API stub 验证，不能宣称覆盖了解释器断点。如 `jit.vmdef` 路径由现有 harness 自设，运行时确保它最终指向该变体生成物，而非原树旧文件。

### 9.2 七后端 DynASM 生成

本机 Makefile 一次只生成一个目标后端，不能以本机 x64 编译代替七后端语法检查。复用上一节的临时树：

```sh
t="$check/jit"
mkdir -p "$check/dasm-out"
dasm() { "$t/src/host/minilua" "$t/dynasm/dynasm.lua" "$@"; }
for mode in enabled disabled; do
  extra=''
  test "$mode" = disabled || extra='-D INT3'
  dasm -D ENDIAN_LE -D P64 -D JIT -D FFI -D FPU -D HFABI $extra \
    -o "$check/dasm-out/x64-$mode.h" "$t/src/vm_x64.dasc"
  dasm -D ENDIAN_LE -D JIT -D FFI -D FPU -D HFABI $extra \
    -o "$check/dasm-out/x86-$mode.h" "$t/src/vm_x86.dasc"
  dasm -D ENDIAN_LE -D JIT -D FFI -D DUALNUM -D FPU -D HFABI $extra \
    -o "$check/dasm-out/arm-$mode.h" "$t/src/vm_arm.dasc"
  dasm -D ENDIAN_LE -D P64 -D JIT -D FFI -D DUALNUM -D FPU -D HFABI $extra \
    -o "$check/dasm-out/arm64-$mode.h" "$t/src/vm_arm64.dasc"
  dasm -D ENDIAN_LE -D JIT -D FFI -D DUALNUM -D FPU -D HFABI $extra \
    -o "$check/dasm-out/mips-$mode.h" "$t/src/vm_mips.dasc"
  dasm -D ENDIAN_LE -D P64 -D JIT -D FFI -D DUALNUM -D FPU -D HFABI $extra \
    -o "$check/dasm-out/mips64-$mode.h" "$t/src/vm_mips64.dasc"
  dasm -D ENDIAN_BE -D JIT -D FFI -D DUALNUM -D FPU -D HFABI \
    -D SQRT -D ROUND $extra \
    -o "$check/dasm-out/ppc-$mode.h" "$t/src/vm_ppc.dasc"
done
```

对改动的条件分支补充：x86 P64/non-GC64、x64 WIN、ARM soft-float、ARM64/MIPS/MIPS64 BE，以及上述 profile-enabled 命令去掉 JIT。不得把生成头回写原树。

DynASM 生成只证明解析／生成成功，不证明寄存器活跃性、最终 buildvm 编码或目标运行正确。完整验收需对应目标工具链构建与运行；Win64 跑既有 `[15]` ABI 场景。未有可用工具链／运行器的目标明确标为未运行，不能把语法检查表述为跨平台通过。

## 10. 完成判据与实施风险

完成必须同时满足：canonical 从不被 INT3 覆盖；已接受 pending 能在无用户 hook 时自动兑现；全部新取指／切帧路径使用正确缓存；ISNEXT 双写生效；旧 logical-view 通道完全移除；公共文档与返回值一致；适用 smoke 与条件编译验证有实际输出。

主要风险不是 trap opcode 本身，而是：

- split-prefetch 早于刷新导致读到前一帧 shadow；
- C/fast-function synthetic PC 错套 Lua 偏移；
- 仅设置 INS 位却没有真实 C 安全点；
- callback stop 后继续读已释放 shadow；
- 把 pending 误当 mounted，提前阻断 JIT 或复制过时字节码；
- 删除现有 hot/stitch 保护后，又误以为安全点隔离了所有 JIT 写入。

实施顺序按第 8 节推进，在实现与调用者迁移完整后统一执行第 9 节验证，不在中间态反复构建。只为不确定的消费者可见边界增加永久回归；性能比较使用临时脚本，不添加默认值／源码形状断言。本方案增加一次热路径 bias 读取和每 proto 一份完整 shadow，不预先承诺速度提升。

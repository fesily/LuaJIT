# LuaJIT 2.1 并行 GC 设计文档

> 参考:网易雷火《Lua GC算法并行化探讨》(基于 LuaJIT 2.0.3)。
> 本设计针对 LuaJIT 2.1 (v2.1 分支, GC64),在吸收其阶段划分思想的基础上,
> 采用不同的同步模型:**安全点握手 + 无条件日志屏障 + 重日志收敛**,
> 目标是把雷火方案中"每次 table 写屏障都要 StoreLoad fence"的代价
> 从快路径上彻底移除。

---

## 1. 目标与非目标

### 目标
- 将 GC 周期中开销最大的 **Propagate(标记)** 与 **Sweep(扫描)** 阶段移入独立 GC 线程,与主线程(Lua 逻辑线程)并发执行。
- 主线程上只保留:触发、根扫描、Atomic 收尾、增量 Free(实际释放)、Finalize(__gc 语义保持单线程)。
- 解释器与 JIT 生成代码的写屏障快路径 **不引入内存栅栏指令**(无 mfence / dmb)。
- 单一编译开关 `LJ_CONCURRENT_GC`,关闭时与原版增量 GC 完全一致;运行时可经 `collectgarbage("concurrent", on|off)` 切换并回退。

### 非目标
- 不做多 mutator 线程(Lua 语义仍是单逻辑线程 + 单 GC 线程)。
- 不做并行标记(GC 线程只有一个;多 GC 线程留作后续扩展点)。
- 不改对象模型/不引入分代(与 gen-gc 分支正交,先在 v2.1 增量 GC 基础上做)。
- 第一阶段不并发化字符串表 sweep(见 §7.3,作为 M5 优化项)。

### 前置约束
- **要求 LJ_GC64**。GC64 下 TValue 是单个 8 字节对齐字(高 17 位 tag + 47 位指针),
  一条 store 即完成"类型+值"的写入,天然原子。雷火文档中"TValue 类型域/值域写入顺序"
  一节的问题在 GC64 下不存在,这是选择 2.1 + GC64 的最大红利。
  非 GC64 模式直接禁用本特性(编译期 `#error` 或回退增量)。
- 目标平台:x86-64 (TSO) 与 arm64 (弱序)。同步设计按弱序模型推导,x86 自动满足。

---

## 2. 与雷火方案的核心差异

| 维度 | 雷火 (2.0.3) | 本设计 (2.1) |
|---|---|---|
| 标记期数据竞争 | 颜色检查屏障 + 在屏障路径和 GC 黑化路径插入 StoreLoad fence | 标记期屏障**不读 GC 线程写的颜色做正确性判定**;改为"每个被改写对象本周期至少入一次日志",由安全点重扫收敛,快路径零 fence |
| TValue 撕裂 | 改写代码保证地址域先于类型域生效 | GC64 单字写,天然原子,无需改写 |
| grayagain 的消费 | GC 线程与主线程需同步该链表 | grayagain 链表**主线程独占写**,仅在握手/Atomic(主线程停顿点)移交,现有 VM 汇编入链代码可原样保留 |
| 内部缓冲(array/node/stack)realloc | 文中未展开(remove 场景入 grayagain) | 统一的**延迟释放(epoch reclamation)**规则,覆盖 table resize、栈增长等所有并发期 interior buffer 释放 |
| Sweep 中的链表操作 | Sweep 线程扫描,Free 回主线程 | 同样 Free 回主线程;但 GC 线程对 root 链的摘除规则按"快照头之后单写者"原则形式化(§7.2) |

设计哲学:**与其在每条写屏障上为弱内存序付费,不如把所有"可能漏标"的对象
推迟到主线程停在安全点时重扫一遍**。LuaJIT 增量 GC 本来就有这个机制
(grayagain 在 Atomic 重扫),我们只是把触发条件从 `isblack` 放宽,并增加
中途握手轮次防止 Atomic 停顿变长。

---

## 3. 总体架构

### 3.1 线程模型
- **主线程**:跑 Lua/JIT 逻辑 + GC 的串行阶段。所有 `lj_mem_*` 分配与释放仍只发生在主线程(allocf 无需线程安全——这点继承雷火结论并更进一步:GC 线程连 free 都不做,只做读+标记+建链)。
- **GC 线程**:常驻,条件变量唤醒。只做两类事:并发标记传播、并发 sweep 扫描(出死亡对象清单,不释放)。

### 3.2 状态机

```
            (主线程)         (GC线程)        (主线程,STW)   (GC线程)      (主线程,增量)  (主线程,增量)
GCSpause → GCSrootscan → GCSpropagate_c → GCSatomic → GCSsweep_c → GCSfree → GCSfinalize → GCSpause
                              ↑    |
                              +----+ 周期性握手(drain 日志, 重扫)
```

- `GCSrootscan`:主线程一次性扫完根集(registry、mainthread、当前栈、metatable 表等),把初始灰队列交给 GC 线程。根集小,代价等同现在 `GCSpause→GCSpropagate` 的转换。
- `GCSpropagate_c`:GC 线程独立传播灰对象。主线程正常跑逻辑,写屏障记日志(§5)。期间发生若干次**握手**。
- `GCSatomic`:与现状语义相同,主线程执行(GC 线程此时挂起):最后一轮 drain grayagain/日志、遍历所有 thread、weak 表处理、`lj_gc_separateudata`、翻转 white。不可中断。
- `GCSsweep_c`:GC 线程遍历对象链(string 表除外,见 §7.3),把 otherwhite 死对象摘到 freelist,活对象 makewhite。
- `GCSfree`(新增,对应雷火 Free 阶段):主线程在 `lj_gc_step` 中按配额逐个 `gc_freefunc` 释放 freelist,`g->gc.total` 只在这里减——total 维持主线程单写者。
- `GCSfinalize`:不变,主线程。

`lj_gc_step` 在并发阶段的职责退化为:响应握手请求、推进 Free 配额、检查 GC 线程是否完成并推动状态切换。JIT 的 `asm_gc_check` / `lj_gc_step_jit` 不需要改语义。

### 3.3 安全点与握手(handshake)

- 主线程的安全点 = 进入 `lj_gc_step` 的时刻(分配点 `lj_gc_check`、JIT 的 `asm_gc_check`、以及可选在 BC 循环回边的 hook 检查里加一个廉价标志位测试)。
- GC 线程通过原子标志 `g->gc.hsreq` 请求握手;主线程在安全点看到后进入握手例程:
  1. 把 `g->gc.grayagain`(主线程独占的日志链)整链摘下交给 GC 线程,清空;
  2. 清除被摘对象的 LOGGED 位(由主线程清,见 §5,无竞争);
  3. 处理延迟释放队列中上一 epoch 的 buffer(§6);
  4. 返回继续跑逻辑。整个握手 O(1)+O(清 LOGGED 的链长),微秒级。
- 握手只影响**进度**不影响**正确性**:主线程长时间不到安全点,GC 线程就先干别的(继续传播已有灰对象)或休眠;不会漏标(漏的都在日志里等着)。

---

## 4. 颜色协议与 `marked` 字节的并发访问规则

沿用现有 bit 布局(`lj_gc.h:17-25`),新增:

```c
#define LJ_GC_LOGGED  0x80  /* 并发标记期:本对象已入 grayagain 日志 */
```

(0x80 当前空闲;LOGGED 仅在 table/可传播对象上使用,与 string 的 hash 标志等不冲突。)

**单字节双线程 RMW 规则**(核心,所有正确性的根):

| 操作 | 执行者 | 方式 |
|---|---|---|
| white→gray(清 white 位)、gray→black(置 BLACK) | GC 线程(标记期) | `atomic_fetch_or / fetch_and`(relaxed 即可) |
| 置 LOGGED + black2gray(屏障慢路径) | 主线程 | `atomic_fetch_*`(relaxed) |
| 屏障快路径读 marked | 主线程 | 普通 load(可读到旧值,见 §5 的正确性论证) |
| 清 LOGGED、makewhite、翻 white | 主线程在握手/Atomic/Free(GC 线程挂起或在不相交对象集上) | 普通写 |
| sweep 期 makewhite | GC 线程 | 普通写(标记已结束,主线程此期间不写 marked,屏障在 sweep 期按现状不触发——`lj_gc_barrierback` 的前提 isblack 在 sweep 期对象只会从 black 变 white) |

之所以 RMW 必须原子:GC 线程置 BLACK 与主线程置 LOGGED 可能并发命中同一字节,
普通 read-modify-write 会互相丢位。其中"主线程丢 GC 的 BLACK"会让对象退回灰
(保守,多扫一遍,安全);但"主线程把 GC 已清掉的 white 位写回去"会让活对象
在 sweep 中被误判 dead——这是必须用原子 RMW 堵死的致命路径。
x86 上 `lock or byte` 在屏障**慢路径**(每对象每周期最多一次)中,代价可忽略。

---

## 5. 写屏障设计(本方案的核心)

### 5.1 问题回顾
雷火文中的竞争:主线程"写 A[1]=B 后读 A.color 判断是否触发屏障"与 GC 线程
"写 A.color=Black 后读 A 的元素"形成经典的 StoreLoad 乱序,双方都可能读到旧值,
导致 B 漏标。他们的解法是两边都加 fence——代价落在每次 table 写上。

### 5.2 本方案:屏障触发条件放宽 + 重日志收敛

并发标记期(`g->gc.cmark != 0`)的 table 写屏障逻辑:

```c
/* 主线程,概念伪码 */
if (isblack(t)) {            /* 现有路径:black2gray + 入 grayagain */
  barrierback(t);            /* (改用原子 RMW 清 BLACK、置 LOGGED) */
} else if (g->gc.cmark && !(t->marked & LJ_GC_LOGGED)) {
  /* 新增慢路径:不管 t 当前是白是灰,只要本周期没入过日志就入 */
  atomic_or(&t->marked, LJ_GC_LOGGED);
  push_grayagain(t);
}
```

**正确性论证**(为什么不需要 fence):
1. 主线程对 marked 的读可以任意陈旧——读旧只会导致**多入一次日志**(慢路径),不会漏。
   唯一能让主线程跳过入链的是 LOGGED 位,而 LOGGED 只由主线程自己置、
   只在主线程停在握手/Atomic 时由主线程自己清:对该位而言是单线程程序,无可见性问题。
2. GC 线程并发遍历 t 时,主线程可能同时改 t 的 array/node。GC64 下每个 TValue
   是 8 字节对齐原子读写,GC 线程读到的要么旧值要么新值,不会撕裂:
   - 读到**旧值**:旧值对象本周期本来可达(开始快照时在 t 里),标记它最多产生浮动垃圾,安全;
   - 读到**新值**:正确标记;
   - **漏读新值**(GC 已扫过该槽):该写必然触发过屏障 → t ∈ grayagain 日志 →
     下一次握手 drain 后 t 被**整表重扫**;重扫时如又被并发改写,t 重新入日志
     (drain 时已清 LOGGED)→ 下一轮再扫。
3. 收敛性:握手 drain 若干轮后,最后一轮在 **Atomic(主线程自己执行,天然 STW)**
   完成——此时没有并发写者,重扫结果即最终结果。中途轮次只为摊薄 Atomic 时长,
   轮数有限(实现上 2~3 轮即可,或按日志增量自适应),不影响终止性。
4. 雷火文中"table remove / 数组左移与扫描指针赛跑"的场景被同一机制覆盖:
   remove 触发写屏障(改写了槽位)→ 入日志 → 安全点整表重扫,无需为 remove 单独处理。

### 5.3 各类屏障的落点改动

| 屏障 | 现状 | 改动 |
|---|---|---|
| `lj_gc_barrierback`(C inline, `lj_gc.h:82`) | isblack 时入 grayagain | 触发条件加 `cmark && !LOGGED` 分支;RMW 原子化 |
| `lj_gc_anybarriert / barriert / objbarriert` 宏 | `if (isblack)` | 改为 `if (marked & (LJ_GC_BLACK\|maybe)) → slowpath`,慢路径函数化 `lj_gc_barrierback_c(g, t)` |
| VM 汇编 `barrierback` 宏(`vm_x64.dasc:378`,arm64 同理) | test BLACK → 内联入链 | fast path 增加两条指令:`cmp byte [DISPATCH+gl(gc.cmark)],0; jne →慢路径 call lj_gc_barrierback_c`;BLACK 命中仍可走原内联路径,但 black2gray 须改 `lock and`(或统一走 C 慢路径,屏障本来就罕中) |
| JIT `asm_tbar`(`lj_asm_x86.h:1931`) | 内联 test BLACK + 入链 | 同上策略:增加 cmark 标志检查跳到 C call(IR_TBAR 生成的代码本来就有 call 形态的 OBAR 先例);**注意已编译 trace 在开启/关闭 concurrent 模式时无需 flush**,因为 cmark=0 时新增检查恒短路 |
| `asm_obar` / `lj_gc_barrieruv`(IR_OBAR) | 已是 C call | 只改 C 函数体 |
| `lj_gc_barrierf` | white child 前推 | 标记期:对 child 的 white→gray 必须用原子 RMW(与 GC 线程竞争同一字节);或简化为也把 parent 入日志 |
| `lj_gc_closeuv` | 强制 BLACK + 标记 | 并发期改为入日志,Atomic 统一处理 |
| `lj_gc_barriertrace` | trace flag 置位入 grayagain | 同 barrierback 处理 |

注:解释器快路径新增的两条指令(load 全局 byte + 分支)在 cmark=0 时分支
完美可预测,实测预期开销 < 1%,远低于 per-store fence。

---

## 6. 内部缓冲的延迟释放(epoch reclamation)

GC 线程标记期会裸读这些非 GCobj 的 interior buffer:
- `GCtab->array` / `GCtab->node`(`lj_tab_resize` 会 realloc 并 free 旧块)
- `lua_State->stack`(`lj_state_growstack` realloc;open upvalue 的 `uvval()` 指进栈)
- `GCproto` 等创建后不变,无此问题。

规则:**`g->gc.cmark != 0` 期间,主线程对上述 buffer 的释放不直接 `lj_mem_free`,
而是挂入 `g->gc.deferfree` 队列(记录 ptr+size);该队列在握手/Atomic 时
(GC 线程确认不再持有旧指针后,即完成当前对象遍历的 epoch 边界)由主线程统一释放。**

GC 线程读到旧 buffer 的内容 = 改写前的旧值,按 §5.2 论证安全(浮动垃圾);
新 buffer 中的新内容由屏障日志兜底。读 `t->array` 指针本身也是 8 字节原子 load。

实现上 GC 线程遍历单个对象期间持有 epoch 计数,主线程握手时按"队列项 epoch < 当前 epoch"释放,简单计数即可,无需 hazard pointer。

---

## 7. 各阶段详细设计

### 7.1 并发标记(GCSpropagate_c)

- GC 线程拥有自己的灰队列(初始 = 根扫描结果)。`g->gc.gray` 一分为二:
  `gray`(GC 线程私有)与 `grayagain`(主线程私有日志,握手时整链移交)。
  **现有所有入 grayagain 的代码路径(含 VM 汇编)的数据结构形态不变**——这是
  把改动面压到最小的关键决策。
- thread(lua_State)沿用现状:永不变黑(`lj_gc.c:350`),只入 grayagain,
  全部留到 Atomic 由主线程遍历——栈是最热的数据,本来就不该并发扫,现状语义恰好正确。
- 遍历 trace(`gc_traverse_trace`):trace 的 IR 常量创建后不可变;
  trace 链接/patch 改的是 mcode 与 link 字段,被 `lj_gc_barriertrace` 日志覆盖。
  主线程在 trace 中执行时不响应握手(`jit_base` 检查,继承 gen-gc 分支同款经验),
  握手只发生在解释器/退出 trace 后的安全点。
- 升灰/置黑全部原子 RMW(§4)。
- 终止条件:GC 线程灰队列空 && 最近一次握手移交的日志为空 → 通知主线程可进 Atomic。

### 7.2 并发 sweep(GCSsweep_c)

对象 root 链(`g->gc.root`,头插单链)的并发协议:

- Atomic 末尾主线程记录快照头 `sweepsnap = gcref(g->gc.root)`。
- 此后主线程 `lj_mem_newgco` 继续头插**新对象**(newwhite=curwhite,本周期天然不死),
  只写 `g->gc.root` 头指针与新对象自己的 nextgc;**绝不触碰 sweepsnap 及其后的节点**。
- GC 线程从 `sweepsnap` 开始遍历:活对象 makewhite(此时主线程不再动 marked,普通写安全);
  死对象从链上摘除,推入 `g->gc.freelist`(GC 线程私有构建,完成后整链发布,
  release store;主线程 Free 阶段 acquire load 后消费)。
- 边界情况:`sweepsnap` 节点本身若死,其前驱 cell 是主线程领地(root 头或某新对象的
  nextgc,且会随头插移动)——GC 线程**不摘第一个节点**,留给主线程 Free 阶段开头处理
  (O(1) 特判)。
- `gc_sweep` 现有的"thread 顺带 sweep openupval 链"(`lj_gc.c:411`)**移出 GC 线程**:
  openupval 链主线程高频改动,改为主线程在 Atomic 里(或 Free 期间增量)自己 sweep
  ——该链很短(活跃 open upvalue 数),代价可忽略。
- GCSfree:主线程每次 `lj_gc_step` 按 `g->gc.freenum`(可调参数,默认每步 ~100 个)
  调 `gc_freefunc` 释放,`gc.total` 同步扣减。释放完进 GCSfinalize。

### 7.3 字符串表(GCSsweepstring)

2.1 的字符串表是带 tag 位的分链 hash(`g->str.tab`),intern 在主线程头插,
且 `lj_str_resize` 会整表 realloc(现有代码已在 GCSsweepstring 期间禁止 resize,
`lj_str.c:136`)。

- **M1~M4 阶段:string sweep 保留在主线程,维持现有增量行为**
  (GCSsweepstring 状态保留,每步扫若干链)。字符串是叶对象,sweep 单链成本低,
  先不为它引入链级同步。
- M5 优化(可选):移入 GC 线程,协议为原子链游标 `sweepstr`:
  intern 时若目标链号 ≥ 游标则正常头插(该链未扫或已扫完均安全——新串 curwhite);
  等于游标的链用单字节自旋锁互斥;resize 全程禁止(沿用现状)。

### 7.4 Atomic(主线程,STW)

内容与现状 `atomic()` 基本一致,补充:
1. 命令 GC 线程停在围栏(condvar);
2. 最后一轮 drain grayagain + 重扫日志对象(§5.2 收敛终轮);
3. 遍历全部 thread、weak 表清理、`lj_gc_separateudata`;
4. 翻转 currentwhite;清全部 LOGGED 位?——不需要遍历:LOGGED 与 white 位一样
   采用"语义随周期翻转"会增加复杂度,简单做法是 drain 时随手清(日志链上的对象
   才可能有 LOGGED,链外不会有,Atomic drain 完即全清);
5. 设置 sweep 快照,唤醒 GC 线程进入 GCSsweep_c。

Atomic 时长 = 终轮日志重扫 + 线程栈扫描 + weak/udata 处理。日志重扫已被中途
握手轮摊薄,预期与现版本 atomic 同量级。

### 7.5 Finalize

不变。__gc 在主线程、原序执行,语义与单线程版完全一致。

---

## 8. 数据结构与代码改动清单

### GCState 新增字段(`lj_obj.h`)

```c
  /* LJ_CONCURRENT_GC */
  uint8_t  cmark;        /* 并发标记进行中(VM/JIT 快路径读) */
  uint8_t  concmode;     /* 用户开关 */
  GCRef    freelist;     /* sweep 产出的待释放对象链(GC线程→主线程) */
  MRef     sweepsnap;    /* sweep 快照头 */
  MRef     deferfree;    /* 延迟释放队列(epoch) */
  uint32_t epoch;        /* 握手代数 */
  /* 以及:线程句柄、mutex/condvar、hsreq 原子标志、统计计数 */
```

`cmark` 须放进 `GG_State`/DISPATCH 可寻址范围,供 `vm_*.dasc` 与 JIT 后端用
`DISPATCH_GL()` 寻址。

### 新文件
- `lj_atomic.h` — C11 atomics / MSVC interlocked / GCC builtins 三选一的薄封装
  (relaxed or/and/load/store + acquire/release)。
- `lj_gcthread.c/h` — GC 线程生命周期、握手协议、灰队列传播循环、并发 sweep 循环。
  (pthread / Win32 双实现,参考 `LUAJIT_OS` 宏体系。)

### 修改文件
| 文件 | 改动 |
|---|---|
| `lj_gc.c` | 状态机加 GCSrootscan/GCSfree;atomic() 加围栏与终轮 drain;gc_sweep 拆出"只摘不放"版本给 GC 线程;`lj_mem_newgco` 不变(头插协议天然兼容) |
| `lj_gc.h` | LJ_GC_LOGGED;屏障宏/inline 改造(§5.3);状态枚举 |
| `lj_tab.c` | resize 的旧 buffer 走 deferfree |
| `lj_state.c` | 栈 realloc 走 deferfree;lua_newstate/lua_close 创建/汇合 GC 线程 |
| `lj_func.c` | closeuv 并发期入日志 |
| `vm_x64.dasc` / `vm_arm64.dasc` | barrierback 宏加 cmark 检查→C 慢路径;`lj_gc_barrieruv` 调用点不变 |
| `lj_asm_x86.h` / `lj_asm_arm64.h` | asm_tbar 加 cmark 检查与 call 出口 |
| `lj_api.c` / `lib_base.c` | `lua_gc` 增加 LUA_GCCONCURRENT;collectgarbage("concurrent") |
| `lj_str.c` | M5 前不动(resize 禁令已存在) |

---

## 9. 模式切换与回退

- 开启:仅允许在 GCSpause 切入(否则先跑完当前周期,与 gen-gc 分支 entergen 的策略一致)。
- 关闭/出错回退:握手点设置 `concmode=0`,当前周期由主线程以增量方式接管剩余阶段
  (GC 线程灰队列移交回 `g->gc.gray` 即可——两种模式共享同一套对象状态,这是
  本设计刻意保持的性质:**并发模式只是"谁在执行 propagate/sweep"不同,
  颜色协议是同一个**)。
- `lua_close` / 致命错误:围栏汇合 GC 线程后再 freeall。

---

## 10. 实施里程碑

1. **M1 基础设施**:`lj_atomic.h`、GC 线程骨架、握手机制、`LJ_CONCURRENT_GC` 开关、
   cmark 进 DISPATCH。验证:线程起停、握手延迟统计。
2. **M2 并发标记(解释器 only,-joff)**:屏障 C 路径改造 + 日志收敛 + epoch 延迟释放。
   验证:全测试集 + assert 构建(`LUAJIT_USE_ASSERT`)+ 高压力 alloc/写混合 stress。
3. **M3 Atomic 整合与正确性收口**:weak 表、udata、closeuv、栈扫描;
   GCDEBUG 模式(每次握手做全堆一致性校验:无 black→white 引用)。
4. **M4 并发 sweep + Free 阶段**:freelist 协议、快照头特判、openupval 移出。
5. **M5 JIT 支持**:vm_*.dasc barrierback、asm_tbar/asm_obar、`lj_gc_step_jit` 语义复核、
   trace 执行期不握手(jit_base 检查)。
6. **M6 字符串表并发 sweep(可选)** + 参数调优(握手频率、free 配额、触发阈值)。
7. **M7 实战验证**:Don't Starve 工作负载(见 memory 中游戏测试流程)、
   服务器型长跑 benchmark,对比指标:主线程 GC 占用、最大暂停、周期时长、浮动垃圾量。

测试工具:TSAN 只能覆盖纯解释器构建(JIT mcode 不可插桩),解释器构建必须 TSAN 全绿;
JIT 路径靠 assert + GCDEBUG 校验 + 压力测试。

---

## 11. 风险与应对

| 风险 | 应对 |
|---|---|
| 浮动垃圾增多(日志收敛+保守标记) | 周期略提前触发(threshold 系数);统计 estimate 校准 |
| Atomic 终轮日志过大 → 暂停尖刺 | 自适应握手:日志长度超阈值即发起握手,多轮摊薄 |
| 主线程长期不到安全点(纯计算循环) | 不影响正确性;可选在 hookcheck/循环回边加 cmark 检查 |
| `marked` 原子 RMW 在 arm64 上的 LL/SC 竞争 | 每对象每周期最多一次慢路径,竞争窗口极小 |
| 隐藏的 interior buffer 释放点遗漏 | 在 `lj_mem_free` 加 debug 钩子:cmark 期间 free 非 deferfree 来源的 buffer 即 assert |
| JIT 生成代码与 C 屏障语义漂移 | 屏障逻辑单点化:汇编只做"BLACK 内联 + cmark 跳 C",复杂逻辑全在 C |
| 与 gen-gc 分支未来合并 | 颜色协议未变、grayagain 形态未变;gen 的 age 位与 LOGGED(0x80)不冲突需提前预留核对 |

---

## 12. 预期收益

- Propagate + Sweep(GC 周期 90%+ 的工作量)移出主线程;主线程每周期只剩
  根扫描 + Atomic + 增量 Free + Finalize。
- 写屏障快路径零 fence、零原子指令,cmark 关闭时仅多一条可预测分支;
  对比雷火方案省去了所有 table 写路径上的 StoreLoad 栅栏。
- 增量/并发同一套颜色协议,可运行时切换、可安全回退,降低上线风险。

---

## 12.5 实现状态(M1–M3 + JIT 已落地)

代码已落地(编译开关 `LUAJIT_ENABLE_CONCGC`,限 x64+GC64+POSIX),
范围 = M1 基础设施 + M2 并发标记 + M3 状态机/Atomic 整合 + M5 的
JIT/VM 汇编部分;**并发 sweep(M4)未做**(per §13 决策点,sweep 仍为
主线程增量)。新文件 `lj_atomic.h`、`lj_gcconc.h/c`;开关
`collectgarbage("concurrent" [,false])` / `lua_gc(L, LUA_GCCONCURRENT, on)`。

实现与本设计的主要偏差(均为实现期发现的简化或修正):

1. **日志载体复用 grayagain + gclist,GC 线程灰队列改用 malloc 向量
   (jobs)**。设计原文让 GC 线程拥有 gray 链——但链表节点是对象内嵌的
   gclist 字段,主线程日志也要用它,两者冲突。落地方案:并发期 gclist
   全部归主线程日志使用(VM 汇编入链代码形态不变,这是 §5.3 的目标),
   GC 线程的工作队列、weak 表、线程、closed upvalue 收集全部走
   GC-线程私有的 malloc 向量(jobs/weakv/threadv/uvv/ssb)。
2. **屏障触发条件**:`lj_gc_needbarrier(g,o)` = cmark ? `!LOGGED` :
   `isblack`。比设计 §5.2 的"isblack ∥ cmark 再查 LOGGED"少一层;
   LOGGED 位主线程独占决策(单线程语义),置位用 `lock or`(与 GC 线程
   的颜色原子 RMW 同字节)。`marked` 字节的**读**在 LJ_CONCGC 下全部
   走 relaxed atomic load(`gcmarked()` 宏),消除 C11 UB(TSAN 全绿)。
3. **closed upvalue 与 USETV/USETS 汇编不改**:gc_mark_conc 把每个被
   标记的 closed upvalue 推入 uvv,gc_conc_finish 单线程重读一次
   uv->tv。这使"汇编屏障因陈旧颜色漏触发"无害——值要么在重读时被标,
   要么 upvalue 本身还白(死)。lj_gc_closeuv 并发期无条件入日志。
4. **延迟释放简化为 pause 括号**(park/resume),未实现 epoch 队列:
   `lj_tab_resize`、`lj_trace.c` 的 trace 向量扩容/flushall 用
   `lj_concgc_pause_begin/end` 包住 realloc+free;`lj_err_throw` 里
   unwind 时强制 unpause。线程栈 realloc 无需括号(GC 线程从不并发
   遍历线程,threadv 全部推迟到 Atomic)。
5. **weak 表**:并发期发现的 weak 表只进 weakv、保持灰(不碰 weak 位
   ——它们与 FINALIZED/CDATA_FIN 别名),finish 时经正常
   gc_traverse_tab 重走一遍补位并挂 gc.weak。
6. **fullgc/模式切换**:lj_gc_fullgc 在 cmark 时先 stopmark、清全部
   LOGGED、清向量再快进 sweep;fullgc 全程强制同步(concmode 临时清零)。
   `collectgarbage("concurrent", false)` 中途关闭时同步收敛到 GCSatomic。
7. **TValue 撕裂**:gc_marktv 以单条 64 位 relaxed atomic load 快照槽位
   (GC64 单字),其余各遍历路径共用该宏。
8. **JIT**:`asm_tbar`(lj_asm_x86.h)生成运行时 cmark 检查 + LOGGED
   `lock or` 内联路径(无 C call);TBAR 消除(gcstep_barrier)的不变量
   改述为"LOGGED 只在 GC step 边界被清",与原 grayagain 不变量同构,
   折叠规则无需改动。trace 执行期间 GCSatomic 仍拒绝运行(jit_base 检查
   继承原版)。
9. **measured**:churn 基准(8e5 表分配+字符串,JIT on)主线程 wall
   时间较增量模式约 -25%;-joff 约 -15%。全测试集(505 项)与基线
   失败集一致;TSAN(-joff)0 报告;assert 构建全绿。

---

## 13. 分配器演进:与 LuaJIT 3.0 arena GC 提案的关系

> 参考:《LuaJIT 3.0 new Garbage Collector》(Mike Pall 原 wiki,tarantool 存档)
> 与本仓库 `gc/arenagc` 分支(fsfod 风格移植:1MB arena、16B cell、
> block/mark 双位图、bump+fit 分配器,~7000 行改动,WIP,仅 x86/x64)。

### 13.1 结论

arena 分配器对并行 GC 是**结构性增强**:它不是让现有方案跑得更快,
而是直接**消解**本设计中最难的三个同步问题。但 arena 重写本身工程量
巨大且未收敛,因此采用两轨制:**并发标记(M1–M3)按分配器无关的方式
先行落地;并发 sweep(M4)降级为"分配器决策点"——若 arena 落地,
M4 整体作废,sweep 退回主线程位图增量扫描。**

### 13.2 arena 对并发各难点的消解对照

| 本设计的难点 | 现分配器下的方案 | arena 下的形态 |
|---|---|---|
| `marked` 字节双线程 RMW(§4,唯一致命竞争) | 慢路径原子 fetch_or/and | **消失**:mark bit 在 arena 元数据区,GC 线程独占写;gray bit 内联在对象中,主线程独占写。写者域按位置天然分离,零原子指令 |
| root 单链表并发 sweep(§7.2 快照头协议、首节点特判) | GC 线程摘链 + freelist 发布 | **消失**:无 nextgc 链表。sweep = 对位图做 `block&mark` 字级运算,不触对象本体,以内存带宽速度完成 → 不需要 GC 线程参与,主线程增量做即可 |
| Free 阶段移交(freelist 跨线程发布/消费) | release/acquire 发布整链 | **消失**:回收即翻位图位,无 per-object free;只剩 finalizer 对象(udata/cdata)仍走主线程队列,空 arena 整块还 OS |
| interior buffer 延迟释放(§6 epoch) | deferfree 队列 | **弱化**:table array/node 也是 arena block,翻位回收,旧 block 在本周期内位图上仍是活的(标记快照语义),epoch 机制由 arena 语义免费提供;仅 huge block(>阈值)仍需 deferfree |
| 握手移交粒度(per-object 日志链) | grayagain 整链移交 | **粗化**:SSB(sequential store buffer)溢出时按 arena 归并到 per-arena gray stack;GC 线程按 arena 领工作,同步单位从对象升到 arena,天然批量化、缓存友好 |
| 多 GC 线程扩展(本设计非目标) | 灰队列难以切分 | **打开**:per-arena gray stack + gray queue(优先队列)就是现成的工作窃取单元 |

### 13.3 协议同构性:为什么迁移是平滑的

本设计的核心机制在 quad-color/arena 体系下都有直接对应物,概念不作废:

- `LJ_GC_LOGGED` 位 ≈ quad-color 的 **gray bit**(语义同为"本周期已入日志,
  勿重复入队"),且同样是"主线程写、主线程在安全点清"的单写者协议;
- `grayagain` 主线程私有日志链 ≈ **SSB**,握手 drain ≈ SSB overflow flush;
- 屏障快路径"读自己写的位,允许陈旧" ≈ quad-color 写屏障只查 gray bit
  (2~3 条指令,不触 mark 位图,不污染缓存)——3.0 提案为单线程增量设计的
  这个屏障,恰好就是并发安全所需的形态,唯一需要的改造是:
  **GC 线程遍历完 dark-gray 对象后不清 gray bit**(避免写主线程的字节),
  改为主线程在握手/Atomic 时批量清——与现设计 LOGGED 的清理时机完全一致。

因此 M1–M3 产出的握手框架、日志收敛论证、安全点机制、TSAN/GCDEBUG 测试
设施,在 arena 迁移后全部保留;作废的只有 M4 的链表 sweep 协议。

### 13.4 决策与排期

1. **不把并发标记押在 arena 重写上**:`gc/arenagc` 是对象模型级重写
   (布局、VM 汇编、JIT 后端全动),自身未稳定;并发叠加在未收敛的重写上,
   两类 bug 会互相伪装,调试成本超线性。
2. **M1–M3 照常推进**(分配器无关);M4 改为决策点:
   - arena 分支若在 M3 完成前达到"全测试集绿 + 单线程性能不回退",
     则后续在 arena 上做:跳过链表 sweep,直接进入"SSB 化日志 + 位图 sweep
     留主线程"的形态(净工程量反而减少);
   - 否则按 §7.2 原案在现分配器上交付,arena 迁移作为 vNext。
3. **近期最小分配器强化**(无论走哪条路都值得做,改动小):
   - `deferfree` 队列(§6,已在设计内);
   - Free 阶段按 size class 分桶批量释放,提升释放局部性;
   - `lj_mem_newgco` 增加每类型 freelist 复用(可选,缓解 Free 阶段
     还给 allocf 又立刻要回来的抖动)。
4. **提前对齐的接口设计**:M1 的 GC 线程工作队列接口按"工作单元句柄"
   抽象(现在 = 灰链节点,将来 = arena 灰栈),避免迁移时重写线程框架。

### 13.5 风险补充

| 风险 | 应对 |
|---|---|
| arena 分支长期不收敛,两轨变双倍维护 | M3 末设硬决策点,逾期即按原案交付,arena 改为 vNext 迁移 |
| quad-color gray bit 与并发清理的细节(GC 线程不清 gray bit 导致重复入队) | 重复入队无害(幂等重扫);量大时握手清理,与 LOGGED 同款论证 |

## 附录:GC stats 插桩(实测验证模块)

构建时加 `-DLUAJIT_GC_STAT=1`(与 `-DLUAJIT_ENABLE_CONCGC` 并存)启用相位级
计时。flag 关闭时全为 no-op,`lj_gcstat.c` 退化为 stub,生产构建与测试套件零影响
(已验证:concgc / concgc+stat / 纯构建 三种配置均 505 passed / 21 failed 同基线)。

Lua API(`require("jit.util")`):
- `gcstat()` → 表,含 `cycle_count`、`drain_rounds`、`gcthread_bursts`、
  `bytes_total`、`bytes_peak`,及 `phases.<name>.{ns_total,ns_max,count}`。
- `gcstat_reset()` — 清零计数器并重置 epoch。
- `gcstat_dump(filename [, {append=bool, label=str}])` — 追加一行 CSV。

相位:`mark_start`、`drainlog`、`conc_park`、`conc_finish`、`atomic`(mutator 侧)
与 `gcthread_mark`(GC 线程后台标记)。关键验证指标 = `gcthread_mark.ns_total`
对 mutator 侧相位之和的比值(offload ratio)。

### 实测结论(2026-06-13,churn / 大稳定堆 两类负载)

**插桩推翻了"标记已卸载到 GC 线程"的乐观假设。** 两类负载下:
- `gcthread_mark` 仅 0.02–0.9 ms,offload ratio ≈ **0.00–0.01**(后台几乎不干活)。
- `atomic` 相位主导一切(55→190+ ms),且并发模式的 atomic **总耗时反而比增量模式高
  2–3 倍**(cycles 也多:55→93)——并发周期被频繁触发,但每轮 GC 线程还没标几个对象
  就被 `conc_park` 拉回,绝大部分标记工作落回 STW 的 atomic 收敛阶段。

控制实验:将 `CONCGC_DRAINSTEP` 由 8 扫到 512(park 频率降 64 倍),`gcthread_mark`
纹丝不动(0.89→0.88ms),cycles 恒为 92。**排除"测量伪影/线程被握手饿死"假设** ——
GC 线程确实在运行(262 bursts / 91 cycles),只是每周期约 180 对象、~12µs 即 markdone,
周期太碎、触发太频繁,后台标记器无法摊薄成本。

即:churn benchmark 上 conc wall < inc wall(0.85 vs 1.18s)**不是因为标记被并行掉了**,
更可能来自周期调度差异/堆峰值更低(11.7MB vs 17.8MB)。先前 −30% 的"标记并发"解释站不住。

待修方向:并发周期触发阈值(concmode 下 gc.pause/stepmul)使周期数量虚高、单周期工作集过小;
应让并发周期**启动更少、运行更久**(灰集预估够大时才转并发),后台标记器才有实质工作集。

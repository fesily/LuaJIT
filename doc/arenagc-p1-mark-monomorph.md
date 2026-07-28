# P1 · Mark 热路径：POD 单态遍历 + 批量 gray pop + 可选头预取 · 设计稿（v2）

> 范围：major GC（Arena / `LJ_HASGCMARK`）**propagate / mark** 阶段性能。
> 基线：本树 GCARENA（T3a open-UV、T3b 头部瘦身、ClearMarks 分片已落地）。
> 对照：`doc/gc-arena-perf-baseline.md`（bench 协议）；`doc/gc-mark-sweep-adaptation-plan.md`（已否决项历史）。
>
> **本交付 = P1-2（POD 单态遍历）+ P1-3a（批量 gray pop）+ P1-3b（可选头预取，编译开关默认关）+ Trav 混装 TAB 快路径。**
> **不做**：P2 SIMD sweep、§1.7 JIT bump、分代 nursery、改 isdead、改 classic `lj_gc.c`、per-type Trav arena、hugegray 重写、`gc_propagate_arena_udata`。

---

## 0. 路线位置

| 阶段 | 状态 |
|---|---|
| §1.2 finalizer / fin_queue | ✅ |
| §1.3 open UV 向量 + atomic fullsweep 删除 | ✅ |
| §1.1 / §1.8 头部瘦身（nextgc / gclist） | ✅ T3b |
| §1.5 rebuild_clearmarks 分片 | ✅ |
| **P1 mark 指针追逐** | **← 本项（v2）** |
| P2 SIMD sweep / §1.7 JIT bump / P4 pacing | 后续 |

---

## 1. 背景与动机

### 1.1 为何 mark 仍是瓶颈

- 位图 sweep 已是强项（B2 优于 classic）。
- 拖留回归集中在 **mark / propagate**：触对象头、读边、入 gray 栈。
- T3b 已减头字段（少 8–16B/对象）；下一步是 **减少间接跳转 + 改善 mark 内局部性**。

### 1.2 现状代码（关键）

| 组件 | 行为 | 行 |
|---|---|---|
| `propagatemark` | 清 GRAY → `gct` switch → `gc_traverse_*`；TAB/THREAD 各带 keep-gray 语义 | `lj_gc_arena.c:1078-1148` |
| `gc_propagate_arena` | 抽空 **一个** arena 的 gray 栈，逐 cell `propagatemark` | `lj_gc_arena.c:1225-1241` |
| GCSpropagate onestep | 优先 hugegray；否则 `gc_grayarena_pop` 取 gray 堆上最长栈的 arena，整栈 drain | `lj_gc_arena.c:1197-1218` |
| Arena 类 | Trav（混装 tab/thread/proto…）、POD（func/proto 经 `newgcot_pod`）、Udata、CdataV、NonTrav | `lj_arena.h:83-110` |

```c
// lj_gc_arena.c:1225 — 热路径现状
static size_t gc_propagate_arena(global_State *g, GCArena *a) {
  size_t m = 0;
  while (!arena_gray_empty(a)) {
    GCCellID1 cellid = arena_gray_pop(a);
    GCobj *o = (GCobj *)arena_cellptr(a, cellid);
    if (LJ_UNLIKELY(!(o->gch.marked & LJ_GC_GRAY))) continue;
    size_t c = propagatemark(g, o);  // 内含 gct switch
    gcstat_add(g, mark_cost, c);
    m += c;
  }
  return m;
}
```

### 1.3 分配已部分按类分离

| 宏 | 类 | 典型对象 | 出处 |
|---|---|---|---|
| `lj_mem_newgcot` | Trav | table、thread、… | — |
| `lj_mem_newgcot_pod` | POD | C/L 闭包、proto | `lj_func.c:226,239`；`lj_bcread.c:349`；`lj_parse.c:1730` |
| `lj_mem_newudata` | Udata | userdata | — |
| `lj_mem_newcdatav` | CdataV | VLA cdata | — |
| strings | NonTrav | openaddr | — |

**关键**：Trav arena **仍混装**多种 `gct`（至少 tab + thread + 非 pod 的 traversable）。POD arena 经 `newgcot_pod` 分配 **FUNC + PROTO**（闭包在 `lj_func.c`，proto 在 `lj_parse.c` 与 `lj_bcread.c`）。

---

## 2. 四项正交改动

| 代号 | 改动 | 单独收益 |
|---|---|---|
| **P1-2** | POD arena 特化 drain，少 `gct` 间接跳转 | 分支预测 / i-cache |
| **P1-3a** | 批量 pop N=8，本地拷贝后再遍历 | 软件流水，改善 mark 字局部性 |
| **P1-3b** | 批量指针解析后可选 `__builtin_prefetch(o,0,3)` 头预取（编译开关默认关） | 缓解对象体跨 cache line |
| **Trav TAB** | 混装路径对 `gct==~LJ_TTAB` 直调 TAB helper | 热路径多数是 table |

落地顺序（**严格**）：M1 审计 → P1-2 POD → P1-3a 批量 → P1-3b 预取 → Trav TAB。P1-3a 先于 P1-3b，以隔离批量本身的收益；P1-3b 报 ON/OFF 差值。

---

## 3. P1-2 · POD 单态遍历（FUNC + PROTO only）

### 3.1 目标形态

```c
static size_t gc_propagate_arena(global_State *g, GCArena *a) {
  if (a->flags & ArenaFlag_PODOnly)
    return gc_propagate_arena_pod(g, a);   // FUNC / PROTO only
  /* Trav / mixed: 保持通用 propagatemark；可选 TAB 快路径（§6） */
  return gc_propagate_arena_mixed(g, a);
}
```

`gc_propagate_arena_pod`：

1. `while (!arena_gray_empty)` pop（P1-3a 落地后改批量）
2. skip non-GRAY（重复表项 / 已处理，与 `propagatemark` 入口一致）
3. `gray2black`
4. `gct == ~LJ_TFUNC` → `gc_traverse_func`；`gct == ~LJ_TPROTO` → `gc_traverse_proto`
5. **意外 gct** → fallback `propagatemark`（安全网，非热路径）；`LUA_USE_ASSERT` 下 `lj_assertG` 报警

### 3.2 共享 helper（防语义漂移）

POD 特化必须与 `propagatemark`（`lj_gc_arena.c:1078-1148`）共享以下语义，避免复制粘贴漂移：

- 早退 `if (!(marked & LJ_GC_GRAY)) return 0`（重复表项 no-op）
- `gray2black`
- TAB keep-gray：`gc_traverse_tab` 后 **仅当 `t->marked & LJ_GC_WEAK` 才 `black2gray`** —— 用 **header WEAK 位**，**不**用 traverse 返回值（FFI_FIN 合成非零 weak 但无 `LJ_GC_WEAK`，会留 mark∧GRAY 残留）。见 `propagatemark` TAB 分支注释 `lj_gc_arena.c:1093-1103`。
- THREAD permanent-gray：`black2gray` + `gc_graythread_push` + `gc_traverse_thread`，见 `lj_gc_arena.c:1113-1137`。

POD 路径 **不处理** TAB/THREAD（POD arena 不会出现二者，assert）。共享部分抽成 `lj_gc_arena.c` 内 static inline / 薄 wrapper，供特化与 `propagatemark` 复用。**不改** hugegray 循环、**不改** `lj_gc.c`。

### 3.3 POD-only 的语义边界

POD arena 内容由 `lj_mem_newgcot_pod` 路由保证为 FUNC / PROTO。PROTO 在 `lj_parse.c:1730` 与 `lj_bcread.c:349` 均走 `newgcot_pod`，故 POD arena 含 PROTO。特化路径覆盖二者即可。

### 3.4 成功标准（P1-2）

- 语义与现 `propagatemark` 一致（weak keep-gray、THREAD permanent-gray、重复 gray no-op）
- 正确性套件全绿（§7）
- B3 不回退（硬门）

---

## 4. **不做 udata drain（明确）**

`gc_mark`（`lj_gc_arena.c:243-256`）对 `gct == ~LJ_TUDATA` 直接 `gray2black(o)`（清出生 GRAY）后 mark metatable / env / buffer，**不** `white2gray`、**不** `arena_gray_push`。即 UDATA 是 **leaf-marked**，从不进 gray 栈。

`ArenaFlag_UdataOnly` 仅用于 sweep / 分离 / 记账（`lj_gc_arena.c:627,1589,2912,3124,3142,3154,3172`），**没有** UDA gray 栈可 drain。

**结论：本设计不实现 `gc_propagate_arena_udata`，也不声称 UDATA 被单态遍历。** 任何“udata drain monomorph”是死路。

`ArenaFlag_UdataOnly` 现有引用保留；本设计 **不新增** 针对它的传播 dispatcher 或 drain 特化。

---

## 5. P1-3a · 批量 gray pop（N=8）

### 5.1 与已否决 edge prefetch 的区别

`doc/gc-mark-sweep-adaptation-plan.md`（~L299, L352）记录：软件 prefetch **已否决**，实测无收益。理由：子表顺序分配被硬件预取器覆盖，深链为串行指针追逐，prefetch 无效。

**本项不同**。P1-3a 是 **worklist MLP**：gray 栈是连续 `GCCellID1`，批量 pop 是顺序读；批量出栈后先解析指针、（可选）触头，再遍历。这是对 **独立工作项** 的软件流水，**不是** 在 `gc_traverse_*` 内对 **子边** 做 `__builtin_prefetch`。两者机制与对象完全不同：

| 项 | 对象 | 时机 | 结论 |
|---|---|---|---|
| 已否决 edge prefetch | 子表 / 子边 | `gc_traverse_*` 内单点 `__builtin_prefetch(o)` | 无收益，回滚 |
| P1-3a worklist batch | gray 栈 cell（独立工作项） | drain 内批量出栈 + 批量解析 | 本项 |

**绝不** 在 `gc_traverse_*` 内加 `__builtin_prefetch` 子边。

### 5.2 算法（单 arena drain 内）

```text
N = 8  // 命名常量 / enum，不排序
while !empty:
  k = min(N, stack_depth)
  // 硬规则：在第一次 propagate 调用前，把 k 个 cellid / GCobj* 全部拷到本地数组
  for i in 0..k-1:
    local[i] = arena_gray_pop(a)         // 或同时解析 arena_cellptr
  // 此后 greytop 已下移；同 arena push 可能复用该存储 —— 不得再从 gray 栈内存读 batch[i]
  for i in 0..k-1:
    o = resolve(local[i])
    if !(o->gch.marked & LJ_GC_GRAY)) continue   // 跳非 GRAY（重复/已处理）
    propagate(o)  // POD 特化或 propagatemark
```

### 5.3 本地拷贝硬规则（不可违反）

`greytop` 在 pop 后下移，gray 栈存储被释放。遍历期间同 arena 子对象 push 会 **覆盖同一块栈存储**。因此：

- **在第一次 propagate 调用之前**，把本批所有 `GCCellID1`（或解析后的 `GCobj*`）拷进本地数组。
- `greytop` 下移后，**永不** 从 gray 栈内存按索引回读 batch 成员。
- 处理本地拷贝时逐项 skip 非 GRAY（重复表项 / 已处理）。

违反此规则 = UAF / cellid 被覆盖 → 悬挂或损坏。

### 5.4 同 arena re-push 与 stale InGrayHeap（预期，勿“修”）

- 同 arena 子对象在批处理期间 push，落在 **当前 live 栈** 上，下一轮迭代处理 —— 语义 OK（栈内 LIFO 保持于剩余栈；批内顺序不必等于单 pop LIFO）。
- 同 arena re-push 发生在 arena 已被 `gc_grayarena_pop` 取下之后，可能重新置 `ArenaFlag_InGrayHeap` / 插 heap 项，之后看起来 **stale**（已被 drain 空）。**这是预期行为。** `gc_grayarena_pop`（`lj_gc_arena.c:1197-1218`）每轮先 detach root、清 `InGrayHeap`，再检查 `ArenaFlag_TravObjs && !arena_gray_empty`，空则跳过取下一个 root。
- **不要** 在 drain 内 ad-hoc 清 `InGrayHeap`“修复”stale 项。依赖 `gc_grayarena_pop` 的 skip 即可。

### 5.5 边界

- 尾部 `depth < N`：处理剩余。
- **不**改 GCSpropagate step 粒度（仍一 arena 整栈 drain）。
- **不**排序（v1 不做；可选实验仅在测得收益时）。

### 5.6 风险

| 风险 | 缓解 |
|---|---|
| 批量过大增寄存器压力 | N=8 |
| greytop 存储复用 → UAF | 本地拷贝硬规则（§5.3） |
| stale InGrayHeap | 依赖 `gc_grayarena_pop` skip，勿 ad-hoc 清（§5.4） |
| 改 drain 顺序影响可复现性 | 同 arena 内顺序可变；全局仍按 arena 堆 |

---

## 6. P1-3b · 可选头预取（编译开关默认关）

### 6.1 形态

在 P1-3a 批量指针解析后，**编译期**门控：

```c
#ifdef LUAJIT_GC_MARK_HEADER_PREFETCH
  for (i = 0; i < k; i++)
    __builtin_prefetch(resolve(local[i]), 0, 3);  // 读，高局部性
#endif
```

- 默认 build：**无** `LUAJIT_GC_MARK_HEADER_PREFETCH` → 与 T4（P1-3a）二进制等价。
- A/B：加该 flag rebuild，3× bench B1/B3/B10，记 ON vs OFF。
- 若 ON 对 B1 无收益（≤ 噪声），默认保持关；**不删**代码（留实验位）。

### 6.2 这仍是 worklist MLP，不是 edge prefetch

预取对象 = **工作项自己的对象头**（批量出栈后解析的 `GCobj*`），**不是** `gc_traverse_*` 内的子边。与 §5.1 一致，与已否决 edge prefetch 不同。使用处注释须写明“worklist MLP, not edge prefetch; see gc-mark-sweep-adaptation-plan.md rejection + §5.1”。

### 6.3 成功标准（P1-3b）

- 默认 build：`rg LUAJIT_GC_MARK_HEADER_PREFETCH` 只出现在 `#ifdef` / 文档。
- ON vs OFF 的 B1/B3/B10 中位数表存 evidence。
- 默认（OFF）build 正确性套件全绿。

---

## 7. Trav 混装 TAB 快路径（P1-2 之后）

混装 drain（非 PODOnly）：`gct == ~LJ_TTAB` 走共享 TAB prop helper（weak keep-gray 正确，§3.2）；非 TAB → 现有 `propagatemark`（覆盖 THREAD/FUNC/PROTO/TRACE）。

- **不丢** THREAD permanent-gray（非 TAB 走 `propagatemark`）。
- **不用** traverse 返回值判 keep-gray（用 header WEAK 位）。
- 可选 type-run（peek 下一个同 gct）**仅当** B1 在 P1-3a+TAB 后仍 soft-fail 才考虑 —— 不必为完成本计划而做。

---

## 8. 验证

### 8.1 正确性套件（arena assert build，须全绿）

| 脚本 | 关注点 |
|---|---|
| `test/gc/thread_permgray_residual_assert.lua` | THREAD permanent-gray |
| `test/gc/openuv_dead_thread_mark_assert.lua` | openuv + mark |
| `test/gc/thread_openupval_sweep_assert.lua` | openuv sweep |
| `test/gc/openuv_vector_v31_assert.lua` | openuv vector |
| `test/test_weak_stacks.lua` | weak 栈 / keep-gray |
| `test/test_finalizer_order.lua` | finalizer |
| `test/gc/rebuild_stress.lua` | rebuild/clearmarks |
| `test/test_gc_invariants.lua` | 通用不变式（基线绿才跑） |
| `test/test_gc_adversarial.lua` | barrier/SSB / 对抗 gray churn（批量改栈时序） |
| `test/test_jit_tbar.lua` | JIT tbar / barrier 路径（需 JIT 时不强 `-joff`） |

### 8.2 Bench 协议（`doc/gc-arena-perf-baseline.md`）

- 二进制：arena Release-like，`-joff`，**无** timing probe。
- 调用：`./src/luajit -joff test/bench_gc_focused.lua`。
- **3 外部顺序 run**；脚本内 `RUNS=7`；报 B1/B3/B10 **median-of-medians**。
- **只**与 **T2 pre-series 基线**（同机/同 flag）比。**不**在 T3/T4 代码落地后再立 gate 基线。

### 8.3 门控

- **B3**：不回退超噪声（约 ≤2% 或落在改前 max–min 内）—— **硬**。
- **B1 / B10**：改善或持平 vs 基线 —— **软**（持平须记录；明显回退才查）。B1 主推力：批量 + TAB 快路径（非 POD 单独）。
- 遥测：`collectgarbage("stats").mark_cost` 同量级即可；`mark_calls` 可能因特化绕过外层 `propagatemark` 入口而变 —— **不**以 `mark_calls` 相等作硬语义等价证明。

### 8.4 classic smoke

- 非 GCARENA build + `git diff --exit-code -- luajit/src/lj_gc.c`（须空）。

### 8.5 可选 ASAN

- Debug FSANITIZE 可用时：permgray + openuv 脚本跑 ASAN。`test/test_arena*.c` 聚焦分配器，非本 claim 必需。

---

## 9. 风险评级

| 项 | 级 | 说明 |
|---|---|---|
| 语义漂移（weak/THREAD） | 中 | 特化必须共享 helper，不抄全；fallback propagatemark |
| greytop 存储复用 UAF | 中 | 本地拷贝硬规则（§5.3） |
| stale InGrayHeap | 低 | `gc_grayarena_pop` skip（§5.4） |
| 性能无收益 | 中 | B1/B10 软；先测再深挖 table-only arena |
| 实现复杂度 | 低–中 | 主要在 `lj_gc_arena.c` |

**总体：中低风险、高对齐路线图。**

---

## 10. 落地记录（实现后填，T7）

| 项 | 值 |
|---|---|
| POD 内 gct 分布 | _TBD（M1 审计，T2）_ |
| Trav 内 TAB 占比 | _TBD（M1，T2）_ |
| Batch N | 8（默认；若需扫 4/16 另记） |
| Prefetch 默认 | OFF（`LUAJIT_GC_MARK_HEADER_PREFETCH` 未定义） |
| B1 delta vs T2 基线 | _TBD_ |
| B3 delta vs T2 基线 | _TBD_ |
| B10 delta vs T2 基线 | _TBD_ |
| mark_calls 变化 | 记录但不作 gate |
| pre-series 基线文件 | `.omo/evidence/arenagc-p1-mark-hotpath/task-2-pre-series-baseline.txt` |

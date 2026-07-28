# rebuild_clearmarks 分片 · 设计稿（v1）

> 范围：major GC（Arena / `LJ_HASGCMARK`）路径下，把 rebuild 末段
> **`rebuild_clearmarks`** 从 one-shot 非让出改为 **可分片**，消除 sweep/rebuild
> 侧最后的粗粒度暂停窗口。
>
> 基线：local `dontstarve` tree（T3a open-UV 向量 + T3b 头部瘦身已落地）；
> 构建开关 `LUAJIT_ENABLE_GCARENA`（`LJ_HASGCMARK`）。
>
> 对照：`ARENAGC_MAJOR_GC_GAPS.md` §1.5 / §4（旁支若有）；
> 代码 `lj_gc_arena.c` rebuild 子阶段、`lj_gc.h` `gc_obj_isdead`、
> `lj_arena.c` `lj_arena_gc_markinit`。
>
> **本交付止于 ClearMarks arena 循环分片 + hugeset 末尾 one-shot + 验证。**
> **不**做：惰性 demote 并入下周期 markinit、D5 isomorphic encoding、分代 nursery。

---

## 1. 背景与动机

### 1.1 现状

Rebuild 子阶段枚举（`lj_gc_arena.c`，`g->gc.rebuildphase`）：

| 子阶段 | 可分片？ | 职责 |
|---|---|---|
| `Rebuild_Prologue` | 武装游标后即进 HugeScan | 重置 HugeScan 游标 / gen 快照 |
| `Rebuild_HugeScan` | ✅ `GCSWEEP_BITMAP_MAX` 槽/片 | free 死 huge；survivor **保留** slot MARK |
| `Rebuild_Epilogue` | one-shot（几乎空） | T3b 后无 root 锚定；仅推进 phase |
| **`Rebuild_ClearMarks`** | ❌ **one-shot** | 见下 |
| `Rebuild_Done` | — | `sweepphase → SweepPhase_Done` |

Dispatcher（`gc_rebuild_rootchain`）**仅**对 Prologue / HugeScan 在 phase 未推进时
`return` 让出：

```c
if ((phase == Rebuild_Prologue || phase == Rebuild_HugeScan) &&
    g->gc.rebuildphase == phase)
  return;
```

**ClearMarks 在同一 onestep 的 `for(;;)` 内必跑完**——这是目前 sweep/rebuild
侧唯一的不可分片窗口（Gaps §1.5 残留）。

### 1.2 `rebuild_clearmarks` 做什么

实现约 `lj_gc_arena.c:1914`，顺序固定：

1. **Arena 循环（主成本）**  
   对 `i ∈ [0, arenastop)`：  
   - I1：`a->swept_gen == g->gc.epoch`  
   - `mark[w] &= ~a->block[w]`（`UnusedBlockWords .. wtop`）  
   每 1MB arena 触碰约 2×16KB 位图（mark 读写 + block 读）。  
   **1GB 堆 ≈ 1024 arena ≈ 32MB 内存流量**，one-shot 暂停粗估 **2–5ms**，
   随堆线性增长。

2. **Hugeset 循环**  
   对 live slot 清 `HUGESET_MARK`。HugeScan 为重启安全**全程保留** survivor
   MARK，到这里才清。

3. **断言**  
   - I1 已在循环内；  
   - `g->gc.total` 进入前后不变（**证明本阶段不 free**）。

### 1.3 关键语义：ClearMarks 不是「第二次全堆 demote」

**D3** 之后，普通 arena 的幸存者 demote 已在位图 sweep 的 **per-arena
free-complete** 完成：

```text
swept_gen = epoch;   /* FIRST */
gc_arena_demote_survivors(a);  /* mark &= ~block */
```

调用点分布在 NonTrav / CdataV / Trav / POD 等 free-complete 路径（约 5 处）。

Bitmap sweep **跳过** `arena_is_current`（nursery：本周期 born 或已 swept）：

```text
if (arena_is_current(g, a)) { ai++; continue; }
```

因此 ClearMarks 的 arena 循环对**绝大多数已 free-complete 的 arena 是幂等空转**
（mark 在 allocated cells 上已为 0）。真正清的是 **残差**：

| 残差来源 | 说明 |
|---|---|
| **Nursery / current arena** | 本周期未走 free-complete demote；周期内新分配后被栈重扫 / `fin_queue` carry-over / 其它 mark 路径置位的 MARK |
| **Hugeset survivor MARK** | HugeScan 故意保留；ClearMarks 是 demote 点之一（与下周期 `markinit` 对称） |

**Open UV 不是第三类特例**：bitmap 对 `!closed` UPVAL **skip free**，但
free-complete 的 `demote_survivors` 仍对 **所有** `block=1` cell 做
`mark &= ~block`，故 open UV 的 bitmap mark 若在已 sweep 的 arena 上，已在
free-complete 清掉。Open UV residual 只可能落在 **nursery arena**。

**不能省掉 ClearMarks**：nursery 依赖它；hugeset survivor 依赖它；
`lj_gc_checkheap` 在 `GCSpause` 要求 TravObjs mark 空 + huge MARK 清。

### 1.4 动机

| 问题 | 目标 |
|---|---|
| ClearMarks one-shot 暂停随堆线性涨 | 单 step 位图流量有上界（与 inc pause 1.1–2.1ms 对齐） |
| Gaps §1.5 唯一 rebuild 粗窗口 | 关闭；dispatcher 与 HugeScan 同构 |
| 为 gen / D5 留接口 | 游标化 demote，不绑死惰性 demote 语义 |

---

## 2. 分片安全性论证

### 2.1 isdead 同构（无新危险窗口）

```text
isdead ≜ !mark ∧ other(meta)
other(arena) = swept_gen != epoch
other(huge)  = huge_swept_gen != epoch
```

（`lj_gc.h` `gc_obj_isdead`。）

GCSsweep 期间 mutator **本就**带着「部分 arena 已 demote、部分未 demote」的
混合态（per-arena free-complete 时序不齐）。ClearMarks 分片只是让更多 arena
**更久**停留在「current 上仍有 residual mark」——对 isdead：

- current ∧ residual mark → `other=false` → **isdead 仍 false**  
- 已 demote 的 other 已在 free-complete 变为 current  

与现状同构；**不改变任何对象的生死判定**。

### 2.2 无 death window / 无 finalizer 交互

- ClearMarks **不调用** `gc_freefunc`（有 `total` 不变断言）。  
- rebuild Prologue 的 death-window gotcha（过早分片丢 finalizer）与 **本期无关**。  
- `GCSfinalize` 仅在 `SweepPhase_Done` 之后进入（ClearMarks 完成 → Done）。  
- fin_queue carry-over：ClearMarks demote 其 MARK；下周期 `gc_mark_fin_queue`
  再 root。分片只推迟 demote 时刻，finalize 仍在全量 ClearMarks 之后。

### 2.3 Hugeset 写屏障

屏障读对象头 GRAY / 调 C，**不读** slot `HUGESET_MARK`。ClearMarks 清 slot
MARK 与屏障无交互。

### 2.4 checkheap / markinit 双保险

- `lj_gc_checkheap` stuck-mark 仅在 **`GCSpause`**。  
- 状态机保证：ClearMarks 未 `Rebuild_Done` 不会进 pause。  
- 下周期 `lj_arena_gc_markinit` 再次全量 `mark &= ~block` + huge MARK 清
  （幂等）。ClearMarks 的即时义务是 **pause 不变式 + 周期间干净**，不是唯一 demote 路径。

**结论：风险评级低。**

---

## 3. 推荐设计

### 3.1 范围划分

| 部分 | 策略 | 理由 |
|---|---|---|
| **Arena 循环** | **分片** | 主成本；索引游标稳定（`arenas[]` 追加只增不重排本周期语义） |
| **Hugeset 循环** | **末尾 one-shot** | 槽数 ≈ huge 对象数，通常极小；rehash 会使槽游标失效（HugeScan 用 `hugesetgen` 快照重启，为 ClearMarks 引入同套逻辑不值得） |

**禁止**：把 hugeset 拆成多片；在 arena 片中途清 hugeset。

### 3.2 游标与武装

`GCState` 新增：

```c
MSize rebuild_clarena;  /* ClearMarks: next arena index to demote */
```

（与 `rebuild_hugehi` / `rebuild_hugegen` 并列。）

| 时机 | 动作 |
|---|---|
| `rebuild_epilogue` → `Rebuild_ClearMarks` | `rebuild_clarena = 0`；可选采样 `total_before` 存字段或静态于函数首次进入 |
| 每片 `rebuild_clearmarks` | 见 §3.3 |
| `Rebuild_Done` | 游标可留脏；下周期 prologue 不依赖它 |

**注意**：`rebuild_clarena` 加在 `GCState` 中。该字段 **无** dasc/JIT 定址依赖
（与 T3b 的 `root`/`sweep` 不同），但仍建议改后 clean rebuild；勿依赖陈旧
`host/buildvm_arch.h`。

### 3.3 每片算法（伪代码）

```c
#define GCSWEEP_CLEARMARKS_ARENAS  16  /* 8–16；可调 */

static void rebuild_clearmarks(global_State *g)
{
  GCArena **arenas = mref(g->gc.arenas, GCArena *);
  MSize i, n, stop;
  static /* or g->gc field */ GCSize total_before;
  int first_slice = (g->gc.rebuild_clarena == 0 && /* first entry */);

  lj_assertG(g->gc.rebuildphase == Rebuild_ClearMarks, ...);
  lj_assertG(g->gc.state == GCSsweep, ...);

  if (/* first entry this phase */)
    total_before = g->gc.total;

  stop = g->gc.arenastop;           /* R1: 每片重读 */
  n = 0;
  for (i = g->gc.rebuild_clarena; i < stop && n < GCSWEEP_CLEARMARKS_ARENAS; i++, n++) {
    GCArena *a = arenas[i];
    lj_assertG(a->swept_gen == g->gc.epoch, "I1 ...");  /* 随片 */
    if ((GCCellID)a->celltop > MinCellId) {
      uint32_t w, wtop = arena_blockidx((GCCellID)a->celltop - 1);
      for (w = UnusedBlockWords; w <= wtop; w++)
        a->mark[w] &= ~a->block[w];
    }
  }
  g->gc.rebuild_clarena = i;

  if (i < stop)
    return;  /* 同 phase 让出；见 §3.4 */

  /* ---- 最后一片：hugeset one-shot + 收尾 ---- */
  {
    GCRef *slots = mref(g->gc.hugeset, GCRef);
    if (slots != NULL) {
      MSize hi, hmask = g->gc.hugesetmask;
      for (hi = 0; hi <= hmask; hi++) {
        uintptr_t u = gcrefu(slots[hi]);
        if (hugeset_slot_live(u))
          setgcrefp(slots[hi], (void *)(u & ~(uintptr_t)HUGESET_MARK));
      }
    }
  }
  lj_assertG(g->gc.total == total_before, "ClearMarks freed memory ...");
  g->gc.rebuildphase = Rebuild_Done;
}
```

**R1 新 arena**：周期内 append 的 arena 位图全零、`swept_gen == epoch`，
demote 幂等；I1 不受影响。每片读 `arenastop` 即可覆盖追加。

### 3.4 Dispatcher（**实现必改，否则分片无效**）

```c
if ((phase == Rebuild_Prologue || phase == Rebuild_HugeScan ||
     phase == Rebuild_ClearMarks) &&
    g->gc.rebuildphase == phase)
  return;  /* still in same phase → yield one onestep */
```

注释同步：Epilogue 仍 one-shot；**ClearMarks 可分片**。

### 3.5 预算与成本返回

| 项 | 建议 |
|---|---|
| **Arena/片** | 默认 **16**（可 8–16）；约 256–512KB 位图流量 / 片，目标几十 µs–亚 ms |
| **常量名** | `GCSWEEP_CLEARMARKS_ARENAS`（与 `GCSWEEP_BITMAP_MAX` 并列） |
| **onestep cost 现状** | rebuild 分支固定 `return GCSWEEPMAX*GCSWEEPCOST`，**与实际工作无关** |
| **本交付** | 分片后 **单 step 墙钟上界**下降（主收益）；cost 记账可仍固定 |
| **可选（P4）** | cost ∝ 本片 arena 数，改善 pacer；**不**阻塞本交付 |

勿在验收文案中写「成本已与预算成正比」，除非 P4 已做。

### 3.6 断言与统计归位

| 项 | 规则 |
|---|---|
| I1 | **每访问一个 arena** 时 assert |
| total 不变 | **第一片**采样 `total_before`；**最后一片**（hugeset 后）比较 |
| `gcstat_inc(rebuild_clearmarks)` | 现状每进 phase 计 1；分片后改为 **每片 +1**，或拆 `rebuild_clearmarks_slices` |
| 进度日志 | `ClearMarks done` 仅最后一片打印 |

### 3.7 可选 v1.1：current-only 过滤（**不在 v1 必做**）

严格必要 demote 的是 **本周期未 free-complete 的 current arena**（+ hugeset）。
已 demote 的 arena 上空转。

v1：**全量分片**（与现语义 1:1，易审）。  
v1.1：可只 demote `arena_is_current` 的 arena，并对非 current 加 debug 断言
「allocated mark residual == 0」。1GB 堆但 current 很少时 ClearMarks 常单片完成。

---

## 4. 非目标（明确不做）

1. **惰性 demote 并入 `lj_arena_gc_markinit`**（消灭 ClearMarks arena 循环）  
   → D5 isomorphic / gen 地基；牵动 `checkheap` stuck-mark、verify、pause 语义。  
2. **Hugeset ClearMarks 分片**  
3. **改 isdead / epoch / free-complete 顺序**  
4. **classic（`!LJ_HASGCMARK`）**  
5. **强制 P4 pacing 重标定**（可观察、不阻塞）

---

## 5. 工作分解

### C1 — 状态与常量（小）

- [ ] `lj_obj.h`：`MSize rebuild_clarena`  
- [ ] `GCSWEEP_CLEARMARKS_ARENAS`（默认 16）  
- [ ] `rebuild_epilogue`：`rebuild_clarena = 0` 后 `rebuildphase = ClearMarks`

### C2 — `rebuild_clearmarks` 游标化（核心）

- [ ] 片循环 + 未完成则 return（phase 不变）  
- [ ] 完成则 hugeset one-shot + total assert + `Rebuild_Done`  
- [ ] I1 / total / 日志 / gcstat 按 §3.6

### C3 — Dispatcher

- [ ] ClearMarks 纳入 yield 条件  
- [ ] 注释：ClearMarks 可分片；hugeset 仍 one-shot 于末片

### C4 — 卫生（低优先，可同 PR）

- [ ] 过时注释中 `gc_atomic_sweep_openupvals` / 「ClearMarks one-shot」表述更新  
- [ ] Gaps §1.5 若存在：标注 ClearMarks 已分片

### C5 — 验证（见 §6）

---

## 6. 验证矩阵

### 6.1 功能 / 回归

| 套件 | 关注点 |
|---|---|
| openuv 全家（181 量级） | 无回归 |
| `udata_finalize_assert` / finalizer_order | fin carry-over demote 时序 |
| `rebuild_stress` | 100-iter 超时是否缓解（观察项，非硬门槛） |
| `inc_pause_assert`（若有） | ClearMarks 相关暂停上界应改善 |
| 全套 `test/gc` | 无 flaky |

### 6.2 构建

- x64 `LUAJIT_ENABLE_GCARENA` + `LUA_USE_ASSERT`  
- x64 GCARENA + ASAN（cmake FSANITIZE）  
- classic 无 GCARENA smoke（本改不碰）

### 6.3 可选游戏

- Debug+ASAN `jit_gen` 专用服：LOADING + World generated + ASAN_errors=0

### 6.4 建议 debug 探针（assert 构建）

- ClearMarks 进行中：`state==GCSsweep && rebuildphase==ClearMarks`  
- 不得进入 `GCSfinalize` / `GCSpause`（状态机已保证；可加 canary）

---

## 7. 风险表

| 风险 | 严重度 | 缓解 |
|---|---|---|
| Dispatcher **漏** ClearMarks yield | **高**（分片无效） | §3.4 必做；code review 清单 |
| Hugeset 中途清 / 分片 hugeset | 中 | 设计禁止；仅末片 |
| `total_before` 每片重采 | 中 | 仅第一片采样 |
| gcstat 暴涨 | 低 | 每片计数或改名 |
| rebuild cost 仍固定 | 低 | 墙钟已改善；P4 另做 |
| `rebuild_clarena` 未武装 | 中 | epilogue 置 0 |
| 与 gen/nursery 文档混淆 | 低 | §4 非目标写死 |

---

## 8. 与路线图关系

```text
§1.5 残留: rebuild_clearmarks one-shot
    → 本设计 C1–C5（major-only 分片）
    → 更远: 惰性 demote / D5 isomorphic / gen
```

§4 建议路线中「§1.5 `rebuild_clearmarks` 分片」本交付关闭 major 侧该条；
gen 模式另开设计。

---

## 9. 验收清单（DoD）

- [ ] ClearMarks 在 `arenastop` 大时 **多 onestep** 完成（日志/gcstat 可见多片）  
- [ ] 单 step 内 arena demote 数 ≤ `GCSWEEP_CLEARMARKS_ARENAS`  
- [ ] Hugeset MARK 仅在最后一片清；pause 后 checkheap stuck-mark 绿  
- [ ] `total` 不变断言仍在末片  
- [ ] I1 随片检查  
- [ ] GC 回归 + openuv 绿；ASAN 可选绿  
- [ ] 无对象生死语义变更（无新 free 路径）

---

## 10. 落地记录

| 项 | 值 |
|---|---|
| 默认 `GCSWEEP_CLEARMARKS_ARENAS` | **16** |
| 游标 | `GCState.rebuild_clarena`；epilogue 武装 0 |
| Dispatcher yield | Prologue / HugeScan / **ClearMarks** |
| total 断言 | **per-call**（非跨片 first→last）。跨片 mutator 会涨 total，设计稿 §3.6 原「首片采样末片比较」会误报，实现改为每片入口采样、片末比较。 |
| openuv 全家 | 181 绿 |
| rebuild_stress | 多场景 OK（长跑） |
| deep/inc_pause | 绿（total 断言修正后）；`inc_pause` worst_ms 仍可能贴近 5ms 阈值（环境敏感） |
| v1.1 current-only | 未做 |

---

## 附录 A · 关键路径（实现时核对）

| 符号 / 点 | 文件（约） |
|---|---|
| `rebuild_clearmarks` | `lj_gc_arena.c` |
| `gc_rebuild_rootchain` yield | 同文件 dispatcher |
| `gc_arena_demote_survivors` | 同文件 |
| free-complete demote 调用点 | bitmap sweep 各分支 |
| `gc_obj_isdead` | `lj_gc.h` |
| `lj_arena_gc_markinit` | `lj_arena.c` |
| `GCState.rebuildphase` / HugeScan 游标 | `lj_obj.h` |
| checkheap stuck-mark | `lj_gc_checkheap` `GCSpause` 分支 |
| `gc_mark_fin_queue` | `lj_gc_arena.c`（ClearMarks demote 后下周期 re-root） |

## 附录 B · 与用户分析的对照

| 用户论点 | 本设计 |
|---|---|
| 主成本 arena 位图流量 | §1.2 采纳 |
| D3 后多数幂等、nursery 必要 | §1.3 采纳；并澄清 open UV residual ⊆ nursery |
| 分片无 death window / isdead 同构 | §2 采纳 |
| 只分片 arena、hugeset one-shot | §3.1 采纳 |
| 游标 + 预算 8–16 | §3.2–3.5 采纳 |
| R1 重读 arenastop | §3.3 采纳 |
| I1 随片、total 末片 | §3.6 采纳 |
| 惰性 demote 不做 | §4 采纳 |
| 成本与预算成正比 | **降级为可选 P4**；固定 cost 下墙钟仍改善 |
| Dispatcher yield | **显式列为必做**（审查补强） |

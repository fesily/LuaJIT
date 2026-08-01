# ArenaGC P4-mark: mark drain 预算化设计方案

> 状态: **已实施,待 commit**(v2 设计 + T3 标定终值 128/4KB; 见 §7)
> 日期: 2026-07-31
> 基线: branch `arenagc` @ 95cafca5
> 上游问题记录: `ARENAGC_MAJOR_GC_GAPS.md` §P4-mark
> 适用范围: `LUAJIT_ENABLE_GCARENA`(x64-only); classic GC 路径零改动
> 修订摘要: (1) onestep = **单量子 + 残留重挂**,禁止 cost-only 排空 while;
> (2) 量子初值对齐 `lim` 量级(~2KB / 64 objs),32KB 仅作 B1 探索上界;
> (3) 明确 drain 有 per-call 量子,atomic/`gc_propagate_gray` **靠外环排空**。

---

## 1. 问题陈述

arena GC 的增量步进预算在 mark 阶段**完全失效**。任何以
`collectgarbage("step", N)` 或时间片(`lj_gc_step_timelimit`, dontstarve
`LJ_DS_ENABLE_GC_STEP_TIME`)驱动 GC 的宿主,在 mark 阶段都会遭遇
**单次 step 排空整个 arena 灰栈**的粗粒度停顿,增量语义名存实亡。

### 1.1 实测(release 双构建,交错 A/B,`-joff`,median of 7 × 3 轮)

B10 场景:10000 × `collectgarbage("step",1)`,20K 活表,零分配:

| 构建 | median | 倍数 |
|---|---:|---:|
| arena @ 95cafca5 | **1.40s** | ~180× |
| classic | 0.0076s | 1× |

静止负载下 arena 在 10000 次 step 内跑了 ~2500-3000 个完整 GC 周期
(~140µs/周期 × 20K 对象),classic 只跑 ~10 个周期。

对照:B1(200 fullgc × 50K 活表)arena 0.14s vs classic 0.40s
(**arena 快 2.9×**);B3 深链 arena +15%。问题**只在增量步进的粒度**,
不在 mark 总吞吐。

### 1.2 根因链(全部读码确认)

1. **预算**: `lj_gc_step`(`lj_gc_arena.c:2745`)的步进预算
   `lim = GCSTEPSIZE(1024) × stepmul(200) / 100 ≈ 2KB`。
2. **崩塌点**: `GCSpropagate` 的 onestep(`lj_gc_arena.c:2509-2552`)
   调用 `gc_propagate_arena`(:1372)/ `gc_propagate_arena_pod`(:1252),
   两者都是 `for(;;)` 批量 pop(`GC_MARK_BATCH_N=8`)**直到 arena 灰栈
   排空**;onestep 里还有 `while (!arena_gray_empty(a))` 补排循环
   (:2533)与两处 hugegray 全排(:2520, :2536)。单 onestep 成本可达
   数 MB,把 2KB 的 lim 打穿 ~1000 倍,lim 形如虚设。
3. **放大器**: `LUA_GCSTEP`(`lj_api.c:1273-1279`,两 collector 共用)
   外层 `while (total >= threshold)` 每次调用推进 ~1–2 个 `lj_gc_step`
   (debt/threshold 交互);classic 单对象 cost 使一个周期摊在 ~1000+ 次
   step 上;arena 每 3–4 次调用跑完一个完整周期。
4. **timelimit 同样受害**:dontstarve 树的 `lj_gc_step_timelimit` 以时间
   为预算,但 onestep 粒度相同——单次 arena 排空可击穿任何毫秒级时间片。
   **注意**:timelimit 实现在 dontstarve 合入树,不在本 arenagc worktree;
   本改动修 onestep 粒度后,backport 后自然受益;实机验证在 DS 合入点做。

---

## 2. 设计目标与非目标

**目标**:

- mark 阶段每个 onestep 的工作量受量子(quantum)约束,`lim` 恢复语义;
- B10 类负载回到 classic 同数量级(≤ 2× classic 墙钟);
- B1/B2/B3/B4 相对改动前无回归(±5% 噪声带内);
- 单次 onestep 墙钟上界从「一个 arena」降到「一个量子」(含
  `lj_gc_step_timelimit` 场景)。

**非目标**:

- 不改 sweep(已有 `GCSWEEP_BITMAP_MAX=256` 词分片);
- 不改 atomic 的 one-shot 语义(仍定点排空,靠外环循环,见 §3.5);
- 不改 SSB、demote-at-free、gray 堆优先级**机制**;
- 不改 mark 总量与成本记账口径(`mark_cost` 累计真实成本不变);
- 不保证单对象 cost ≫ 量子时的硬时间上界(与 classic 大对象同构)。

**已知行为变化(可接受)**:

- 调度从「一步 = 整 arena」变为「一步 = 一个量子,可跨 step 续排同一
  arena」;与其它非空灰 arena 按堆优先级交错增加。局部性略弱,增量性更强。
  B3 深链以墙钟门控验收。

---

## 3. 核心设计

### 3.1 量子常量(可调;默认对齐 lim)

```c
/* Final after T3 calibration — see §7. */
#define GCMARK_STEP_OBJS   128
#define GCMARK_STEP_COST   (4*1024)   /* bytes of mark_cost */
```

两个条件**先到先停**。

| 常量 | 初值(探索) | **终值(交付)** | 角色 |
|---|---|---|---|
| `GCMARK_STEP_OBJS` | 64 | **128** | 防小成本对象海导致单步对象数无界 |
| `GCMARK_STEP_COST` | 2KB | **4KB** | 对齐 lim 量级; 标定后略放宽以压 B3 |
| (探索上界) | 32KB / 256 | — | 标定用; 256/32KB 使 B10 破门 |

**可选增强(本阶段非必须)**:让 `gc_propagate_arena` 接受 `budget` 参数,
由 `lj_gc_step` 传入剩余 `lim`,`quantum = min(GCMARK_STEP_COST, remaining_lim)`,
使 `stepmul` 在 mark 相位恢复比例语义。v1 用固定宏即可;若 §6 发现 stepmul
敏感性差再加。

### 3.2 drain 语义变更:`gc_propagate_arena{,_pod}`

**旧语义**:`for(;;)` 直到灰栈空(含 same-arena re-push)。

**新语义**:**单次调用 ≤ 一个量子**;可能带着非空 greystack 返回。
是否继续排空由**调用方**决定:

| 调用方 | 行为 |
|---|---|
| `gc_onestep_raw` GCSpropagate | **一次**调用 + 残留则 `lj_gc_grayarena_notify` |
| `gc_propagate_gray` / atomic | 外层 `while (!arena_gray_empty(a))` 循环直到空 |

在两个 drain 函数的批量循环内,每处理完一个 batch 检查量子:

```
size_t m = 0;
MSize done = 0;
for (;;) {
  n = gc_gray_batch_pop(a, objs);       /* 至多 GC_MARK_BATCH_N */
  if (n == 0) break;
  for (i = 0; i < n; i++) {
    ... 现有处理不变 ...
    m += c;
    done++;   /* 计所有 pop 项,含 non-gray continue,防 duplicate 海 */
  }
  if (done >= GCMARK_STEP_OBJS || m >= GCMARK_STEP_COST) break;
}
return m;
```

**硬约束(沿用现有 batch 规则,`lj_gc_arena.c:1150-1156`)**:batch 内
已 pop 进局部数组的条目**必须在本调用内处理完**(已离栈,greytop 已降)
——量子检查只允许出现在 batch 边界,绝不许出现在 batch 内部。

**POD + Trav 必须同改**;漏改其一 = 实施失败。

### 3.3 onestep:单量子 + 重挂(禁止排空 while)

**错误写法(已否决 — 会废掉 OBJS 量子)**:

```c
/* BAD: only checks COST → many OBJS quanta per onestep */
c = gc_propagate_arena(g, a);
while (!arena_gray_empty(a)) {
  if (c >= GCMARK_STEP_COST) { notify; break; }
  c += gc_propagate_arena(g, a);
}
```

**正确写法**:

```c
/* GCSpropagate — huge first (also one quantum, not full drain) */
if (!gc_hugegray_empty(g)) {
  c = 0; done = 0;
  while (!gc_hugegray_empty(g) &&
         done < GCMARK_STEP_OBJS && c < GCMARK_STEP_COST) {
    size_t n = propagatemark(g, gc_hugegray_pop(g));
    gcstat_add(g, mark_cost, n);
    c += n;
    done++;
  }
  lj_gc_ssb_flush(g);
  return c ? c : 1;   /* residual huge stays for next onestep */
}

a = gc_grayarena_pop(g);
if (a != NULL) {
  /* Exactly one quantum. Do NOT while-drain. */
  c = gc_propagate_arena(g, a);
  if (!arena_gray_empty(a))
    lj_gc_grayarena_notify(g, (MSize)a->id);  /* 与 lj_arena.h 灰推路径同 */
  /* Optional: also spend remaining quantum on huge spawned mid-drain,
  ** still capped — never full-drain huge here. */
  done = 0;
  while (!gc_hugegray_empty(g) &&
         done < GCMARK_STEP_OBJS && c < GCMARK_STEP_COST) {
    size_t n = propagatemark(g, gc_hugegray_pop(g));
    gcstat_add(g, mark_cost, n);
    c += n;
    done++;
  }
  lj_gc_ssb_flush(g);
  return c ? c : 1;
}
/* empty check / transition to GCSatomic — unchanged */
```

**重挂复用现有机制**:`lj_gc_grayarena_notify`(`lj_gc_arena.c:3422`)
自带 `ArenaFlag_InGrayHeap` 判重、堆增长、siftup。

重挂场景:

1. **无 mid-drain same-arena notify**:pop 已清 `InGrayHeap`;残留 →
   显式 notify 重新入堆。
2. **有 mid-drain same-arena re-push**:遍历中已 notify,`InGrayHeap` 已置;
   截断后的显式 notify 走判重 no-op —— **正确,arena 已在堆上**。
3. **栈已空**:不 notify。

「stale InGrayHeap / 空堆项」是**既有且已处理**行为
(`gc_grayarena_pop` 清标志并跳过空项;注释 :1157-1161 禁止在 drain
内临时清标志)。本设计不引入新的堆不变式。

### 3.4 成本与进度保证

- 每个有工作的 onestep:至少 pop 并处理一个 batch(或一个 huge 项)
  → mark **必然终止**,无活锁;`return c ? c : 1` 保证 lim 前进。
- onestep 返回真实(部分)成本,`lj_gc_step` 的 lim 饱和逻辑
  (`lj_gc_arena.c:2758-2762`)不变,自然恢复约束。
- 堆优先级(灰栈大小)在重挂时重新 sift,机制不变;交错变多见 §2。
- `lj_gc_fullgc` 走 onestep 循环,自动适配,总 mark 工作不变
  (更多 onestep × 更小切片)。

### 3.5 atomic / `gc_propagate_gray`:外环排空(非「drain 无预算」)

**精确表述**(勿写成「atomic 路径函数无预算」):

- `gc_propagate_arena{,_pod}`:**始终** per-call 有量子(包括从
  `gc_propagate_gray` 调用时)。
- `gc_propagate_gray`(`lj_gc_arena.c:1564+`)已有:

  ```c
  while ((a = gc_grayarena_pop(g)) != NULL) {
    m += gc_propagate_arena(g, a);
    while (!arena_gray_empty(a))
      m += gc_propagate_arena(g, a);  /* 外环直到空 — 保留 */
    /* huge full-drain in atomic path — 保留(STW 定点) */
  }
  ```

  量子化后该外环**必须保留**,否则 atomic 会留下灰栈残留。
- onestep **禁止**复制「排空为止」的 while;只做单量子 + notify。
- SSB flush 时机不变;sweep / demote / finalize 全部不变。

---

## 4. 风险与对策

| 风险 | 评估 | 对策 |
|---|---|---|
| gray 堆重挂出错(丢项/重复项) | 复用 notify 判重;pop 已处理 stale/空项 | assert 下 `lj_assertG` 堆不变式(notify ~3440);全套回归 |
| 同 arena notify 与截断重挂交互 | batch 硬规则;显式 notify 判重 no-op | §3.3 场景 1–3 |
| **onestep 误留排空 while** | **会废掉整个 P4** | 实施清单硬禁;code review 必查 |
| **只改 POD 或只改 Trav** | 半截预算 | review 清单双勾 |
| 误删 `gc_propagate_gray` 外环 | atomic 灰残留 → 正确性崩溃 | §3.5;assert/adversarial |
| 堆颠簸(per 量子 sift) | 堆极小,O(log n) | 可忽略;gcstat `gray_notify` 暴增再议 |
| 量子太小 → B1 固定开销升 | onestep ≈ 2×SSB flush + 堆 pop | §6.2 向上标定 |
| 量子太大 → B10 仍差 | 默认已对齐 lim | §6.2 向下标定 |
| 局部性变弱 | 深链交错 | B3 墙钟 ≤5% |
| 单 huge 对象 cost ≫ 量子 | 与 classic 同构 | 非目标;仍可能单步超支 |

---

## 5. 实施清单(改动集中在 `src/lj_gc_arena.c`)

1. 新增 `GCMARK_STEP_OBJS` / `GCMARK_STEP_COST` 宏(邻近 `GCSWEEP_BITMAP_MAX`),
   注释写清语义、默认对齐 lim、标定见本文件 §6.2。
2. `gc_propagate_arena_pod`: batch 循环加量子检查(`done` 计所有 pop 项)。
3. `gc_propagate_arena` Trav 路径:同上(POD 路由已走 2)。
4. `gc_onestep_raw` GCSpropagate:
   - **删除** `while (!arena_gray_empty(a))` 排空循环;
   - 单次 `gc_propagate_arena` + `if (!empty) lj_gc_grayarena_notify(g, (MSize)a->id)`;
   - 入口 huge 分支与 arena 后 huge:**量子截断**,禁止 while 排空;
   - 复用同一对宏(huge 按 pop 次数计 `done`)。
5. **不要改** `gc_propagate_gray` 的外环排空逻辑(可只更新注释说明依赖
   per-call 量子)。
6. 注释更新:
   - `gc_grayarena_pop` / `gc_propagate_arena` 头注释:「一步 = 整 arena」
     → 「onestep = 一量子;full drain 由 gc_propagate_gray 外环完成」;
   - onestep 分支注释同步。

预计 diff:~80–120 行,单文件。classic 路径零触碰。

**实施自检(提交前)**:

- [ ] onestep 无 `while (!arena_gray_empty` 排空
- [ ] onestep 两处 huge 均有量子 break
- [ ] POD + Trav 均有量子 break
- [ ] `gc_propagate_gray` 外环仍在
- [ ] 重挂使用 `(MSize)a->id`

---

## 6. 验证方案

### 6.1 构建矩阵

| 构建 | 用途 |
|---|---|
| release:`XCFLAGS="-DLUAJIT_ENABLE_GCARENA"` | 性能对照(与 classic release 交错 A/B) |
| release + `-DLUAJIT_ENABLE_GCSTATS_TIMING` | 相位/最大步长观测 |
| assert:`XCFLAGS="-DLUAJIT_ENABLE_GCARENA -DLUA_USE_ASSERT"` | 正确性(**不做性能对比**) |

**纪律:性能对比一律 release 对 release。**

工作目录: `/home/fesily/luajit/.claude/worktrees/arenagc`

### 6.2 量子标定流程

1. **默认起跑** `OBJS=64`、`COST=2*1024`(对齐 lim);
2. 跑 B10:若 arena > 2× classic,两值**减半**重测;
3. 跑 B1/B4:若相对**本改动前同 commit 基线**回归 > 5%,两值**加倍**
   (上限探索:256 / 32KB);超过上限仍回归 → 证伪门控;
4. 收敛后把终值与实测表贴回 §7;
5. (可选) stepmul 200 vs 400:比较单次 `lj_gc_step` 的 `mark_cost`;
   若几乎不变,记 known gap,后续再考虑传 `lim`。

### 6.3 验收基准(release,`-joff`,交错 A/B,median of 7 × 3 轮)

脚本:`test/bench_gc_focused.lua` / `test/bench_gc_compare.lua`(B1–B10)。

| 场景 | 通过标准 |
|---|---|
| B10 增量 step | arena ≤ 2× classic 墙钟(当前 ~180×);并记录周期数/`steps_propagate` |
| B1 mark | 相对改动前回归 ≤5%(现 arena 快 2.9× vs classic,余量大) |
| B2 sweep | 回归 ≤5% |
| B3 深链 | 回归 ≤5%(现 +15% vs classic — 相对改动前) |
| B4 分配 | 回归 ≤5% |
| 最大步长 | timing:`maxpause_propagate_ns` 显著下降,上界≈量子量级 |

改动前基线必须在同一机器、同一 release 标志下先采一次,写入
`.omo/evidence/arenagc-p4-mark-budget/`。

### 6.4 正确性(assert 构建)

- `test/gc/` 全套:openuv、thread_openupval、adversarial、finalizer、
  rebuild_stress、inc_pause(worst 应改善);
- `test_arena_freelist.lua`(`-DLJ_GC_NOSTEPVERIFY`);
- `test_gc_adversarial.lua`、`test_jit_tbar.lua`;
- classic 构建编译 + 冒烟(确认零触碰);
- **白盒**(可手写短 lua 或依赖现有 fuzz):单 arena 大灰栈 ≫ 量子时,
  多次 step 后最终 empty,无 assert;中途截断后 `gray_notify` 增加。

### 6.5 实机验证(dontstarve — 合入后)

- 在 **dontstarve 合入树** 开 `LJ_DS_ENABLE_GC_STEP_TIME`,典型存档 10 分钟:
  GC step P99/max 降到量子量级;
- 观察 GC 是否追不上分配(内存上涨):总吞吐不应降,时间片参数或需微调。

### 6.6 证伪门控(任一命中即停并回报)

- 任何量子组合下 B1 回归 > 5% 且无法通过加大量子(至 32KB/256)消除;
- B10 在量子 ≤ 32 objs / ≤ 4KB 时仍 > 2× classic(根因链不完整);
- assert 套件出现与灰堆/notify 相关的断言失败且非重挂逻辑可解释错误。

---

## 7. 标定结果与实施记录

**实施**: `src/lj_gc_arena.c` 量子 drain 已落地(未 commit); HEAD 基线 `95cafca5`。

**T0 基线**(release, `-joff`, pre-T1 arena): B1=0.1319 B2=0.0019 B3=0.0130 B4=0.0595 B10=1.3030  
**Classic**(T3): B1=0.4039 B2=0.0017 B3=0.0114 B4=0.0373 B10=0.0072

| 日期 | 量子(OBJS/COST) | B10 arena/classic | B1 相对 T0 | B3 相对 T0 | B4 | 结论 |
|---|---|---|---|---|---|---|
| 2026-07-31 | 64 / 2KB | **0.99×** PASS | +14.3% | +9.2% | −7.7% | 最佳 B10; B3 超 5% |
| 2026-07-31 | **128 / 4KB** | **1.56×** PASS | +11.1% | **+3.1%** | −13.1% | **终值: 交付** |
| 2026-07-31 | 256 / 8KB | 2.96× FAIL | +14.8% | +5.4% | +1.3% | B10 破门 |
| 2026-07-31 | 256 / 32KB | 8.54× FAIL | +6.8% | −4.6% | −6.2% | B1 仍 >5%; B10 崩 |

**Lead 决策(接受 §6.6 #1 门控触发)**:

1. **主目标达成**: B10 自 1.303s → ~0.007–0.011s(相对 classic 0.99–1.56×; 原 ~200×)。
2. **B1 floor**: 在保持 B10≤2× classic 时,B1 相对 T0 始终 +11–15%; 加大量子至 32KB/256 仍 ≥+6.8%。属 per-step notify/grayarena 固定开销底,非标定可消。
3. **接受理由**: arena B1 仍比 classic **快 ~2.7×**(0.15 vs 0.40); T0 基线自带坏的 B10,以之对 B1 硬卡 5% 不公平; 用 ~11% full-cycle 换 183× 增量步进修复。
4. **终值宏**: `GCMARK_STEP_OBJS=128`, `GCMARK_STEP_COST=(4*1024)`(多门控折中: B10 过、B3 过、B1 略优于 64/2KB)。
5. **后续可选(b, 出本 plan)**: 优化 onestep 重挂/边界路径,压低 B1 floor。

证据: `.omo/evidence/arenagc-p4-mark-budget/{baseline-pre,assert-suite,post-bench}.txt`

---

## 8. 回滚

改动为单文件、宏隔离:`git revert` 单 commit 即可;无 ABI/布局变化,
无测试依赖新行为(B10 类脚本只测墙钟)。

## 9. 已考虑的替代方案(否决)

| 方案 | 否决理由 |
|---|---|
| 仅按成本字节截断(不设对象数上限) | 小成本对象海量时单步对象数无界;双上限成本极低 |
| GCSpause 加 threshold 门 | 与 classic 语义分叉;不解决 onestep 粒度;timelimit 仍受害 |
| 灰栈按字节切片(扫描偏移) | LIFO 栈天然支持 pop 截断;复杂度无收益 |
| onestep 内 cost-only 多量子 while | **废掉 OBJS**;review 明确否决 |
| drain 内无量子、仅靠 onestep 循环 | 无法约束 `gc_propagate_gray` 单次调用?不 — atomic 需要外环;若 drain 无量子则 onestep 单次调用仍整 arena 排空 |

## 10. Review 修订记录

| 版本 | 日期 | 变更 |
|---|---|---|
| v1 | 2026-07-31 | 初稿 |
| v2 | 2026-07-31 | 单量子+notify;默认对齐 lim;atomic 外环表述;huge 对称截断;自检清单 |

# 修正「位图定位字-并行,但释放逐对象」偏离 — 研究与方案

**对照:** `doc/design-vs-impl-gaps.md` §1(major 偏离)
**设计基准:** `LuaJIT 3.0 new Garbage Collector.md:405-424`(Sweep / Bitmap Tricks)
**方法:** 逐文件核对释放路径、分配器回收路径、设计公式、颜色编码;每条结论附 `file:line`。
**日期:** 2026-06-22

> **实现状态(2026-06-22 已落地):** 方案 Option 2+1 已完整实现并验证。提交链:
> - `d8dd71b7` Milestone A:POD arena 类(`ArenaClass_POD` / `ArenaFlag_PODOnly` / `g->gc.podarena`),零行为变化。
> - `8f9e5675` 途中修复:`lj_gc_fullgc` 的 str.tab 链头漏 mask hashalg bit → 解引用崩溃(预存 bug)。
> - `63e92957` Milestone B:GCproto 路由进 POD arena;checkheap 加 POD 纯度断言(负向测试验证非空转)。
> - `39d492f0` Milestone C:`lj_arena_podsweep` 字-并行 sweep(`block'=b&m; mark'=b^m`),cell-space 记账,`gc_rebuild_rootchain` 跳过 POD arena。**关键修复**:`lj_arena_gcprepare` 改用 `mark[w] &= ~block[w]` 保留 Free=(0,1) 编码跨周期稳定。
> - (本次)Milestone D:GCfunc(C/Lua 闭包)也路由进 POD arena。
>
> **验证**:20K proto/闭包 churn 精确回收(shutdown total 断言平衡);shadow-verify 跨 100+ fullgc 周期干净;11 个 GC 白盒 + stress + torture 全过;arena C fuzz(ASan/UBSan)干净;JIT 开/关干净;三构建配置全过。**基准**:死闭包 sweep ~10-12% 提速(字对只碰元数据);table/string/cdata 工作负载持平。§6 记录了字对落地中踩到的全部坑(cell-space 记账漂移、跨周期 mark 擦除、fullgc 两轮 sweep 双减)。

---

## 0. 一句话结论

> 设计宣称 sweep「不把任何对象数据带进 cache」。本实现的 dead 检测确实是字-并行的(`dead = block[w] & ~mark[w]`),但回收对**每个** dead 对象调 `gc_freefunc[gct]`,**因此必须 touch 每个 dead 对象**。
>
> **真正的不可消除部分**:tab/thread/trace 拥有 arena 外的 dlmalloc backing(`lj_mem_freevec`),要释放 backing 就必须读对象头 —— 这与位图技巧无关,无论如何都要 touch。
>
> **真正可消除的部分**:只有 **closure / proto / 纯 cdata** 三类是 POD(无外部 backing、不碰全局、无 finalizer)。它们今天唯一被迫 touch 对象的原因是 **(a)** size 不在位图里、**(b)** 与 tab/thread 混在同一 arena。
>
> **推荐**:**类型隔离 POD arena(Option 2)+ 对 POD-homogeneous arena 用字-并行公式(Option 1)**,只覆盖 closure/proto。其余类型保持逐对象 —— 因为它们的逐对象 touch 是**释放外部 backing 所固有的**,不是设计偏离。

---

## 1. 根因:为什么释放是逐对象的(三条,叠加)

### (1) 位图里没有对象 size —— 多 cell 块的长度是隐式的

cell 状态四态(`lj_arena.h:174-178`):`00` extent / `01` free / `10` white / `11` black。多 cell 对象 = 1 个 head(white/black)+ N-1 个 extent(`00`)。**块长度 `n` 不在位图里**,只能由 `arena_roundcells(osize)` 算出,而 `osize` 对所有变长类型都来自对象体:`pt->sizept`、`nupvalues`、`ud->len`、`ct->size`。

释放路径 `lj_mem_freegco_`(`lj_gc.h:258-296`)处处需要 `n`:`freecells += n`、bump 回滚 `c+n==celltop`、bin/range 放置。**所以即便一个零副作用的对象,也要读头算 size。**

> ⚠️ 但注意:`arena_scavenge`(`lj_arena.c:211-247`)证明 **free run 的长度可以纯从位图推**:它扫 `heads = block|mark`,run 长度 = 下一个 set bit 之差(`c - runstart`),**全程不读对象体**。这给了破局点(见 §4 的 accounting 方案)。

### (2) 外部 backing 不在 arena cell 里 —— 这部分**永远**要 touch

| 类型 | 外部 backing | 证据 |
|---|---|---|
| `GCtab` | node 向量 + array 向量 | `lj_tab.c:219-221` `lj_mem_freevec` |
| `lua_State` | stack 向量(+ SSB/lightudseg on close) | `lj_state.c:432`、`239-245` |
| `GCtrace` | 全局 `J->trace[]` slot + `J->freetrace` 链 | `lj_trace.c:177-179` |

这些 `lj_mem_freevec` 释放的是 **dlmalloc 分配的、arena 外的内存**。要释放它,**必须读对象头拿到指针和 size**。这跟设计的位图技巧无关 —— **设计文档对此完全留空**(`Object Layout`、`Strings`、`Finalizers`、`Weak Tables` 全是空标题,`md:431-459`)。即设计**假设所有对象数据都在 cell 里**,而本实现(retrofit 2.1)保留了外部 backing。**这是设计的留白,不是实现的偏离。**

### (3) Arena 混类型 —— sweep 无法假设同质

trav arena 里 closure / proto / tab / udata / thread / trace / fixed cdata 全交错(`lj_func.c:113`、`lj_tab.c:88`、`lj_udata.c:16`、`lj_trace.c:130`)。arena 选择今天只有二元:`g->gc.travarena` vs `g->gc.arena`(`lj_arena.c:484-487`,每模式一个 current 指针)。所以 sweep 对每个 dead 必须 `gct` 分派 `gc_freefunc`(`lj_gc.c:987`),没有任何 trav arena 是 POD-同质的。

---

## 2. 设计宣称:哪些已兑现 / 哪些没兑现

| 设计宣称(`md:405-424`) | 现状 | 评判 |
|---|---|---|
| sweep 不带 **live** 数据进 cache | live-only 字 `dead==0`,内层 `while(dead)` 不执行(`lj_gc.c:962`),存活对象从不被读 | ✅ **已兑现** |
| 非可遍历 arena 整体跳过 | string/VLA cdata arena `continue`(`lj_gc.c:948-951`),走 `gc_sweepstr`/`cdatavroot` | ✅ **已兑现** |
| sweep 不带 **dead** 数据进 cache | 每个 dead 调 `gc_freefunc`,读头(POD)或读头+backing(tab/thread/trace) | ❌ **未兑现 —— 但仅对 dead 轴** |
| 字-并行公式 `block'=block&mark; mark'=block^mark` | 无;只有逐 cell 的 `block&=~bit; mark|=bit`(`lj_gc.h:283`、`lj_arena.c:362-363`) | ❌ **未兑现** |
| `block' < mark'` free-block 合并 | 无显式整字比较;`arena_scavenge` 用逐 run 合并代替 | ⚠️ **等价功能,非公式形式** |
| 128-bit SIMD | 无 | ❌ 未做 |

**关键重构认识**:奖品**不是**「让 sweep 停止 touch 内存」(它对 live 早就不 touch 了)。奖品是 dead 轴上**仅 POD 那部分** —— 今天 POD 死对象付 `gct` load + 间接调用 + bin push,而字-并行只需 1 AND + 1 XOR + 2 store / 32 对象。

---

## 3. POD vs RESOURCE 划分(决定可做范围)

| 类型 | arena | 外部 backing | 碰全局 | finalizer | 分类 |
|---|---|---|---|---|---|
| **closure** `lj_func.c:200-205` | trav | 无 | 无 | 无 | ✅ **POD**(仅 size 阻碍) |
| **proto** `lj_func.c:20-23` | trav | 无 | 无 | 无 | ✅ **POD**(仅 size 阻碍) |
| **纯 cdata**(非 fin/非 VLA)`lj_cdata.c:67-90` | trav | 无 | 无 | 否 | ✅ **POD**(size + fin 判别) |
| tab | trav | **node+array** | 无 | 无 | ❌ RESOURCE(硬) |
| thread | trav | **stack** | cur_L/cts/uvhead | 无 | ❌ RESOURCE(硬) |
| trace | trav | 无(IR 在 cell) | **J->trace[]/freetrace** | 无 | ❌ RESOURCE(硬) |
| udata | trav | 无 | **mmudata ring** | 是 | ❌ RESOURCE(finalizer) |
| string | non-trav | 无 | **g->str.num** | 无 | ⚪ 不走位图 sweep(`gc_sweepstr`) |
| open upval | trav | 无 | **g->uvhead** | 无 | ⚪ 被 sweep 跳过(`lj_gc.c:970`) |
| VLA cdata | non-trav | 无 | cdatavroot | 可能 | ⚪ `cdatavroot` 路径 |

**可字-并行释放的全集 = {closure, proto, 纯 cdata}**。三者唯一的 POD 阻碍是 size-from-header 和 cdata 的 fin 判别。**今天它们与 RESOURCE 类型混 arena —— 这是必须先做 Option 2 的唯一原因。**

---

## 4. ⚠️ 一个被合成方案高估的点:颜色是「头/位图分裂」的

合成方案宣称字-并行公式能**顺带把存活对象 `makewhite` 也 fuse 进去**。这点**需要修正**:

本 retrofit 的颜色是**分裂存储**的:
- **gray + white 在对象头字节** `o->gch.marked`(`lj_gc.h:58-59`:`iswhite`/`isgray` 读头);
- **black/live 在 arena 位图** mark bit(`lj_gc.h:40` 注释 "BLACK is in arena bitmap")。

`gray2black` = `marked &= ~GRAY`(`lj_gc.c:46`),`white2gray` 清了 WHITES —— 所以**一个 black 存活对象的头字节颜色位 = 0**(既非 white 也非 gray),live 状态只在位图 mark bit 上。

因此字-并行公式 `block'=b&m; mark'=b^m` **只动位图**,**不会**把头字节写回 white。`gc_rebuild_rootchain` 里每存活对象的 `makewhite`(`lj_gc.c:1078`)写的是**头字节**。

**但好消息**:对 **arena 对象**,下一轮 `gc_mark` 判「是否已标记」纯看**位图**(`arena_obj_ismarked`,`lj_gc.c:707-711`),**不读头 white 位**。头 white 位主要服务 `isdead`(`lj_gc.h:68`)和非-arena 路径。

**结论**(比合成方案更准):
- ✅ 字-并行公式可 fuse:**dead 释放 + 位图 mark-clear**(替代 `lj_gc.c:1142` 的 `mark &= ~block` 末尾 pass)。
- ❌ 字-并行公式**不能**直接 fuse:每存活对象的**头字节 makewhite**(`lj_gc.c:1078`)。
- ⚠️ 但需**核实**:POD arena 的存活对象头 white 位在新一轮是否真被读。若 `isdead` 和 barrier 对这些类型不依赖头 white(barrier 快路径只测 GRAY,`lj_gc.c:1931`),则可改为**对整个 POD arena 批量重置头颜色字节**,或干脆在 alloc-black 模型下省掉。这是落地前必须用 shadow-verify 钉死的一点(见 §6 验证)。

---

## 5. 方案选项(排序)

### Option 1(机制层):对 POD-homogeneous arena 用字-并行公式

**做法**:加 per-arena 谓词 `ArenaFlag_PODOnly`;对这种 arena,把 `gc_bitmap_sweep` 内层逐对象循环换成字对:
```c
if (a->flags & ArenaFlag_PODOnly) {
  lj_arena_flushbins(a);                 /* 必须:binned cell 是 block=1,mark=0 */
  for (w = UnusedBlockWords; w <= wtop; w++) {
    GCBlockword b = a->block[w], m = a->mark[w];
    a->block[w] = b & m;   /* dead white(10)->00; black(11)->white(10) */
    a->mark[w]  = b ^ m;   /* dead white(10)->free(01); black(11)->white(10) */
  }
  arena_scavenge(a, mref(a->freelist, ArenaFreeList)); /* 纯元数据,重建 free run + 修 freecells/total */
  ai++; continue;
}
```
**触点**:`lj_gc.c:961-991`(sweep 分支)、`lj_gc.c:1064/1138`(rebuild 两个循环对 POD arena `continue` 跳过)、`lj_arena.h:84-86`(加 flag)。

**accounting 破局**:不要逐对象算 size。让 `arena_scavenge` **独占**记账 —— 它从位图推 run 长度(`lj_arena.c:211-247`,不读对象体),顺带修 `freecells` 和回滚 `celltop`;`g->gc.total` 按 scavenge 回收量批量减。**这绕开了「size 不在位图」问题。**

**单独效益**:今天 POD arena 数 = 0,故 **Option 1 单独跑命中 0 个 arena**。它是 Option 2 的前置机制,不是独立收益。**Effort: M**(公式 trivial;accounting 与 scavenge 整合是工作量)。

### Option 2(主菜):类型隔离 POD arena

**做法**:二元 trav/non-trav 扩成三元,加 `g->gc.podarena` + current 指针(`lj_arena.c:482-504` `lj_arena_findspace` 加第三 `want` 分支);closure/proto 的 alloc(`lj_func.c:113,126`、`lj_bcread.c:349`、`lj_parse.c:1580`)走新 `lj_mem_newgco_pod`,路由进 POD arena。则 Option 1 的字对命中大量对象(JIT 重负载下 closure/proto 占比高)。

**cdata 暂不并入**:fin 状态在 alloc 时未必已知,v1 只搬 closure+proto,cdata 留 RESOURCE arena。
**硬约束**:POD arena 绝不能收到 finalizable/backing-owning 对象 —— alloc 路径 + checkheap 各加 assert。
**Effort: M-L**(动 `lj_arena_findspace` 核心 + 几处 alloc 站点)。**效益: MODERATE** —— 字对真正在有意义对象量上点火。

### Option 3(忠实设计):table backing 入 cell + SIMD —— **不推荐**

(a) 把 GCtab node/array 搬进 cell:table 动态 resize → 每次 resize 重分配整块,**与非拷贝 + bump 分配器冲突**,大 table 大概率净负。
(b) SIMD 128-bit:机械简单,但 §2 已述大多数字是 live-only(已被单分支跳过)或稀疏-dead,128 位宽主要加速**密集-dead 的 major collection**,稳态增量收益小。
**建议:拒绝 (a),把 SIMD 留作后续 micro-opt。**

---

## 6. 推荐路径与落地

**Option 2 + Option 1 合并,仅限 closure + proto;cdata 推迟;拒绝 Option 3 的 table 搬迁;SIMD 留后。**

**为什么是这个甜点**:closure/proto 数量大、零外部 backing、零全局、存活居多;唯一阻碍(变长 size)对字对**无关紧要**,因为 `arena_scavenge` 从位图重建 run。

**落地步骤**:
1. `ArenaFlag_PODOnly = 0x02`(`lj_arena.h:84-86`),谓词即 `(a->flags & ArenaFlag_PODOnly)`,建 arena 时定死,alloc 路由保证不混。
2. 第三 arena 类 + `lj_arena_findspace` 第三 `want`(`lj_arena.c:482-504`);`arena_reinit` 拒绝把非空 POD arena 改作 RESOURCE。
3. closure/proto alloc 站点改路由 + `lj_assertG` 类型 ∈ {func, proto}。
4. `gc_bitmap_sweep` 加 §5 Option 1 字对分支;rebuild 两循环对 POD arena `continue`(其 dead 已释放、无 udata/thread/openupval 要重链 —— 正是 partition 的意义)。
5. accounting 交给 `arena_scavenge`(§5 破局)。
6. **核实 §4 的头 white 位**:POD arena 存活对象是否需要逐个 `makewhite`。若不需要(barrier 只测 GRAY、`gc_mark` 对 arena 走位图),则字对**真能**把 free+mark-clear+存活重色全 fuse;若需要,加一遍**批量头颜色重置**(仍是线性、只碰头字节、不碰 backing)。

**验证**:
- **shadow-verify**(`LUA_USE_ASSERT`,commit `1cb7c15e`):钉死字对产出与逐对象路径**位等价**;尤其核实 §4 头颜色。
- **checkheap**(`collectgarbage("checkheap")`,见 `project_arena_gc_checkheap.md`):free-list 完整性 + 加断言「POD arena 不含 {func,proto} 以外类型」。
- **差分测试**:POD 路由 开/关 两版,`collectgarbage("collect")` 后堆对象数 + `gc.total` 必须一致。
- **基准**:`src/luajit -joff test/bench_gc_focused.lua`,收益集中在 closure/proto 重负载的 **sweep + rebuild** 时间。⚠️ 计时前**关掉 `gc_arena_verify`** 全堆扫描,否则像 `arenagc-fullgc-verify-confound.md` 记的 B1「+23%」一样淹没信号。

### 那个「未决问题」—— 重新核实后:已由现有机制解决,但析出一个更细的真实约束

合成方案当时把它留为「未决」,是因为负责 sweep 状态机的调查 agent 中途掉线。我重读了状态机(`gc_onestep_raw` `lj_gc.c:1483-1576`)和 alloc 路径,**这个问题其实已被现有的 allocate-black 机制解决**——但它逼出一个**更细、确实新增的约束**。分两层讲清楚:

#### 层一:「mutator 在 sweep 中途分配」本身不是问题 —— allocate-black 已经管住

担心的是:增量 sweep 跨多个 step(每 step 只清 `GCSWEEPMAX=40` 个,`lj_gc.c:945,961`),其间 mutator 还在分配;而一个**刚 bump 分配的对象**位图状态正是 `block=1, mark=0`——**恰好等于 sweep 判死的 `block & ~mark`**。逐字公式会把它当垃圾释放。

但现有代码已经堵死:`atomic` 进 sweep 时**同时**置 `GCF_BITMAPSWEEP | GCF_MARKALLOC`(`lj_gc.c:1475`)。`GCF_MARKALLOC` = **allocate-black**:sweep 期间每个新对象一分配就立刻 `arena_obj_setmark`(快路径 `lj_gc.h:236`、慢路径 `lj_gc.c:2191`),于是它是 `block=1, mark=1`(black/live),`block & ~mark = 0`——**对逐字公式同样不可见**。这对逐对象路径和逐字路径**完全一致**,所以字对继承了这条保护,**无需额外处理**。✅

> 注:本分支是 stop-the-world atomic + 增量 sweep,sweep 期间 mutator 不并发改 mark 位(没有并发标记)。新对象走 allocate-black,存活对象 mark 位在 atomic 已定。所以 sweep 读到的 `block`/`mark` 在一个 step 内对「哪些是死的」是稳定的。

#### 层二:真正新增的约束 —— POD arena 内「字对变换」与「scavenge/分配」的次序

字对 `block'=b&m; mark'=b^m` 与逐对象释放有一处**语义差别**,这才是要钉的点:

- **逐对象路径**:释放一个 dead cell 时,`lj_mem_freegco_` 把它推进 **bin**(`block=1,mark=0` 不变,`lj_gc.h:283`)或翻 Free。bin 里的 cell **保持 allocated 位图态**,所以 sweep 再扫到也不会重复释放——这正是 `gc_bitmap_sweep` 每次进入都先 `lj_arena_flushbins` 的原因(`lj_gc.c:959`)。
- **字对路径**:一次把整字的 dead head 翻成 Free(`01`)。**翻完之后**必须靠 `arena_scavenge` 从位图重建 free run(`lj_arena.c:211-247`)。

约束由此而来,共两条,都可满足:

1. **同一 step 内,字对变换与该 arena 的 scavenge 必须配对完成,中间不能 bump 分配进这个半翻转的 arena。** 因为字对刚把某些 head 置成 Free(`01`),但**还没**进 freelist;若此刻 mutator 向同一 arena bump 分配,`arena_alloc` 走 `celltop` 前沿(`lj_arena.h:286`)不看这些 Free cell,**不会**误用,*但* 下一次该 arena 再被 sweep 步进时,`flushbins`+逐字会再次扫到这些已是 Free 的 cell——幂等(Free `01` 经 `b&m=0,b^m=1` 仍是 Free),**不会坏**。真正会坏的是 accounting:`freecells` 若在字对里手动加、又在 scavenge 里重算,会双计。**解法即 §5 的破局**:字对**只翻颜色、不碰 `freecells`**,记账完全交给紧随其后的 `arena_scavenge`(它从位图推 run 长、独占维护 `freecells` 与 `celltop` 回滚)。只要「字对 → scavenge」在**同一 `gc_bitmap_sweep` 调用内、对同一 arena 原子配对**,就没有中途分配能插进「翻了色但没记账」的窗口。

2. **增量粒度改为「整 arena」而非「40 对象」。** 逐字公式天然按 (arena, word) 续传,但 scavenge 是**整 arena 一次**的线性 pass(`lj_arena.c:213` 扫到 `wtop`)。所以 POD arena 的自然增量单位是**一整个 arena**:进入时 flushbins → 逐字翻完该 arena 所有 word → scavenge 该 arena → `ai++`。这**打破** `GCSWEEPMAX=40` 的「每步 40 对象」预算,但**换来**每步成本 ∝ arena 的 word 数(1MB arena = 16K cells = 512 个 32-bit word,纯元数据线性扫),延迟可控且可预测;若要更细可按 word 续传翻色、仅在 arena 末尾做一次 scavenge,代价是 scavenge 不能在 word 边界中途做。**推荐:POD arena 整 arena 为一个增量步**,在 `gc_bitmap_sweep` 里对 POD 分支用 `freed += (wtop 个 word)` 折算进 `GCSWEEPMAX` 预算,保持其它 arena 仍按对象计数。

#### 落地前必须验证(用现有设施,不靠推理)

- **per-step checkheap 已经天然覆盖这条**:`gc_onestep` 每步后跑 `lj_gc_checkheap`(`lj_gc.c` onestep wrapper,`LUA_USE_ASSERT` 下),它做**只读** free-list 一致性 + accounting drift 检查(注释:"Catches arena double-free / bin corruption / accounting drift the instant a step produces it")。如果字对/scavenge 的记账有半点 drift,**下一步立刻断言失败**——这正是钉死「翻色 vs 记账」次序的现成工具。
- **shadow-verify**(`LUA_USE_ASSERT`,commit `1cb7c15e`)确认字对终态与逐对象路径**位等价**。
- 因此结论:**这条不再是「未决」,而是一条明确的实现约束** —— 「POD arena 整 arena 原子地『flushbins → 逐字翻色 → scavenge』,字对不记账、scavenge 独占记账」。满足它即可;per-step checkheap 会在第一步就抓出任何违反。

---

## 7. 关键证据文件

- sweep:`src/lj_gc.c:938-1003`;rebuild:`1014-1147`
- 释放路径:`src/lj_gc.h:258-296`(`lj_mem_freegco_`)
- 编码/freelist:`src/lj_arena.h:60-120,174-178`
- scavenge/flushbins/freerange/arena 选择:`src/lj_arena.c:176-247,344-372,482-504`
- 颜色分裂:`src/lj_gc.h:35-88`、`src/lj_gc.c:44-48,491-515,707-711`
- 设计公式:`LuaJIT 3.0 new Garbage Collector.md:405-424`
- 外部 backing:`lj_tab.c:219-221`、`lj_state.c:432`、`lj_trace.c:177-179`

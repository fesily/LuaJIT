# T3b · GC 头部瘦身：删除 `nextgc` + `gclist`（HASGCMARK）

> 范围：major GC（Arena / `LJ_HASGCMARK`）路径下，从对象头与共享布局中
> 拆除 **`GCHeader.nextgc`** 与 **`gclist`**，并退役根链锚 `g->gc.root`
> （及无用的 `g->gc.sweep` 挂钩）。
>
> 基线：local `dontstarve` tree（open-UV 向量 T3a 已落地）；构建开关
> `LUAJIT_ENABLE_GCARENA`（`LJ_HASGCMARK`）。
>
> 对照：`ARENAGC_MAJOR_GC_GAPS.md` §1.1 / §1.8 / §4（若在旁支）；
> open-UV 设计 `ARENAGC_OPENUPVAL_VECTOR_DESIGN.md`（T3a 前置已完成）。
>
> **本交付止于 T3b-3 验证。** 不包含：分代 nursery、`table` array/node 进
> arena、JIT bump 内联分配。

---

## 1. 背景与动机

### 1.1 为何现在可以删

| 字段 | 历史用途 | HASGCMARK 现状 |
|---|---|---|
| **`nextgc`** | 全局/根链穿线；open UV 链；finalizer 环；string chain | 分配 `link=0`；open UV 已迁向量（T3a）；finalizer 已 `fin_queue`；string 已 openaddr。**热路径零读写。** |
| **`gclist`** | classic 灰链 / weak 链 / barrier 链 | 弱表与 gray 已紧凑工作栈；x64 HASGCMARK `asm_tbar` 走 GRAY 位 + C 调用。**代码读者为零。** |
| **`g->gc.root`** | 全堆链锚 | 已退化为 **mainthread 单点锚定**（链长恒 1）；枚举靠 arena / hugeset / strtab。 |

删字段的收益（对齐 P1 mark 回归归因）：

1. 每个可收集对象头少 **8B（nextgc）**；带 `gclist` 的类型再少 **8B**。
2. mark 触头少一条指针依赖；与「头部瘦身」一揽子一致。
3. 根链诊断 / 锚定代码可整段删除，降低维护面。

### 1.2 前置条件（已满足）

- [x] Open UV 不再用 `uv->nextgc`（T3a 向量 + Path L/F）。
- [x] Finalizer 不走 `mmudata`/`nextgc` 环。
- [x] String intern 不走 `nextgc` 链（openaddr）。
- [x] 弱表 / gray 不走对象内 `gclist`。

### 1.3 非目标

- **不改 classic**（`!LJ_HASGCMARK`）：`nextgc` / `gclist` / `gc.root` / `gc.sweep` 行为与布局保持现状。
- **不**在本交付移植 arm/arm64/mips/ppc 的 GCARENA barrier（见 §4 策略 b）。
- **不**强制与 GCupval 死 `prev/next` union 收缩同 commit（可选 T3b-2b）。

---

## 2. 完整消费者清单（HASGCMARK 活读者）

### 2.1 `nextgc` / 根链

| # | 位置（约） | 用途 | T3b 动作 |
|---|---|---|---|
| N1 | `lj_gc_arena.c` `gc_root_chain_count` | 诊断：数链长 | **删** |
| N2 | `lj_gc_arena.c` `gc_root_chain_probe_print` | 诊断：`LUAJIT_GC_ROOT_CHAIN_PROBE` | **删** |
| N3 | `lj_gc_arena.c` `gc_assert_root_anchor_only` | 断言 root==mt && mt->nextgc==NULL | **删** |
| N4 | `rebuild_epilogue` | `g->gc.root = mainthread` | **删** |
| N5 | freeall 末 | `mainthread->nextgc=null` + re-anchor root | **删** |
| N6 | verify 根链扫 string 负控 | 走 `nextgc` | **删** |
| N7 | **`lj_state.c` init** | `setgcref(g->gc.root, L)` + `setmref(g->gc.sweep, &g->gc.root)` | **HASGCMARK 下退役**（审查补漏） |
| N8 | **`close_state` assert** | `g->gc.root == mainthread` | **改/删**（审查补漏） |
| N9 | `GCState.root` / 可能无用的 `sweep` | 字段 | T3b-1：`#if !LJ_HASGCMARK` 或仅 GCMARK 不读写 |
| N10 | `gcnext` 宏 `lj_obj.h` | 链步进 | HASGCMARK 无用户 → 条件化 |
| N11 | classic：`lj_gc.c` / str chain / `lj_func.c` `#else` | 真链 | **不动** |

注释中的 `nextgc`/`root` 表述（`lj_gc.h` 分配 API 说明等）随改随清。

### 2.2 `gclist`

| # | 位置 | 用途 | T3b 动作 |
|---|---|---|---|
| G1 | `GCproto` / `GCfuncHeader` / `lua_State` / `GCtab` / `GChead` / `GCtrace` | 字段定义 | `#if !LJ_HASGCMARK` |
| G2 | `lj_obj.h` GChead 对齐断言链 | classic VM barrier 共享偏移 | `#if !LJ_HASGCMARK` |
| G3 | `lj_jit.h` `GChead.gclist == GCtrace.gclist` | 同上 | `#if !LJ_HASGCMARK` |
| G4 | `lj_asm_x86.h` `asm_tbar` **`#else`** | classic 写 gclist + grayagain | 保留 classic |
| G5 | `lj_asm_{arm,arm64,mips,ppc}.h` `asm_tbar` | **无** HASGCMARK 分支，写 gclist | 策略 b：`#error` |
| G6 | `vm_{arm,arm64,mips,mips64,ppc,x86}.dasc` `barrierback` | 写 `tab->gclist` | 策略 b 或仅 classic 构建 |
| G7 | `vm_x64.dasc` HASGCMARK | 已 GRAY 路径，不依赖 gclist | 无改或仅注释 |
| G8 | `lj_gc_arena.c` 历史注释 | 提及 gclist | 卫生 |

**HASGCMARK 下 C 代码对 `gclist` 的读写：零。** 删除是布局与断言问题，不是语义迁移。

---

## 3. 核心机制

### 3.1 `GCHeader` 条件化

```c
#if LJ_HASGCMARK
#define GCHeader	uint8_t marked; uint8_t gct
#else
#define GCHeader	GCRef nextgc; uint8_t marked; uint8_t gct
#endif
```

要点：

- HASGCMARK 下对象头以 `marked/gct` 起跳；**后续字段 `offsetof` 全面左移**（相对 classic 少 8B 起）。
- 注释「occupies 6 bytes…」仅适用于 classic 32 位 `GCRef` 叙事；GCMARK 路径更新注释，避免误导。
- 所有类型经 `GCHeader` 自动瘦身：`GCstr`、`GCudata`、`GCcdata`、`GCupval`、… 无需逐类型删 `nextgc` 字段名。

### 3.2 `gclist` 条件化

```c
#if !LJ_HASGCMARK
  GCRef gclist;
#endif
```

应用于：`GCproto`、`GCfuncHeader`、`lua_State`、`GCtab`、`GChead`、`GCtrace`。

GChead 共享偏移断言整段：

```c
#if !LJ_HASGCMARK
LJ_STATIC_ASSERT(offsetof(GChead, gclist) == offsetof(lua_State, gclist));
/* ... proto / funcL / tab / trace ... */
#endif
```

**不**在 HASGCMARK 下保留 gclist 占位「以备将来」——无消费者，占位只浪费 mark 带宽。

### 3.3 `g->gc.root` / `g->gc.sweep` 退役

HASGCMARK 下：

| 字段 | 动作 |
|---|---|
| `GCState.root` | `#if !LJ_HASGCMARK` 保留；或 GCMARK 构建不声明 |
| `GCState.sweep` | 位图 sweep 不走 root 链；若确认无读者 → 同条件化 |
| init | 去掉 `setgcref(root)` / `setmref(sweep, &root)` |
| rebuild / freeall | 去掉 re-anchor |
| close_state | 去掉「root 是 mainthread」断言；保留 FIXED/SFIXED 与泄漏断言 |

主线程存活不依赖 root 链：`mainthref` + SFIXED 已足够。

### 3.4 非 x86 后端策略（b）

GCARENA 目前仅 x86/x64 完整（`asm_tbar` HASGCMARK 分支 + dasc）。

**决策：策略 (b) 明确关门**，不做半吊子 gclist 移植：

```c
#if LJ_HASGCMARK
#error "LUAJIT_ENABLE_GCARENA is only supported on x64/x86"
#endif
```

放置点（实现时择一或组合）：

- `lj_arch.h` / 目标选择：非 x86 且 `LUAJIT_ENABLE_GCARENA` → 编译期失败；
- 或各 `lj_asm_{arm,arm64,mips,ppc}.h` 的 `asm_tbar` 入口；
- 对应 `vm_*.dasc` 若仍被 GCARENA 构建拉取，同样 `#error` / 不生成。

策略 (a)（按 x86 移植 GRAY 测试）**明确延后**，不在本交付。

### 3.5 `LJ_DS_LUA_STATE_LAYOUT` 垫片重标定（本仓库必做）

游戏锁死：`sizeof(lua_State)==200`，`reserved@0xb8`，`userdata@0xc0`。

当前 HASGCMARK core 末端（GC64 x64，注释口径）：

| 配置 | pad 基（core 末） |
|---|---|
| HASGCMARK，无 tailcalls | `0x68` |
| HASGCMARK + tailcalls | `0x70` |
| classic，无 tailcalls | `0x60` |
| classic + tailcalls | `0x68` |

T3b-2 删 **`nextgc`（8）+ `lua_State.gclist`（8）** → core 再 −16：

| 配置 | 新 pad 基（预期） |
|---|---|
| HASGCMARK，无 tailcalls | **`0x58`** |
| HASGCMARK + tailcalls | **`0x60`** |

实现时用 `offsetof(lua_State, /* 最后一个 core 字段 */)` 或编译期 `sizeof` 探针校准，**不要只改魔数不跑静态断言**。

```c
LJ_STATIC_ASSERT(sizeof(lua_State) == LJ_DST_LUA_STATE_SIZE);
LJ_STATIC_ASSERT(offsetof(lua_State, reserved) == LJ_DST_LUA_STATE_RESERVED);
LJ_STATIC_ASSERT(offsetof(lua_State, userdata) == LJ_DST_LUA_STATE_USERDATA);
```

### 3.6 Arena cell 记账

- `arena_roundcells(sizeof(T))` 随 `sizeof` 自动变。
- **硬断言必须重核**，至少：
  - `LJ_STATIC_ASSERT(arena_roundcells(sizeof(GCupval)) <= 4);`（`lj_arena.h`）
  - 任何写死 cell 数 / 尺寸常量的注释与测试。
- **可选 T3b-2b**：去掉 GCupval 死 `prev/next` union（uvhead 遗物），进一步缩小；与「字段删」正交，可同 PR 或紧随，但 **须单独说明 cell 变化**。

### 3.7 固定根

| 对象 | 注意 |
|---|---|
| `mainthread`（GG 内嵌） | 无 gclist 业务；init 只设 `marked/gct`；不再写 `nextgc` |
| `strempty` | 仅 GCHeader；无链 |
| 其它 SFIXED | 不依赖 root 链枚举 |

---

## 4. 分阶段执行

### T3b-1 — 根链退役（**无布局变化**）

**目标**：删尽 HASGCMARK 对 `nextgc` 链与 `g->gc.root` 的读写；对象 `sizeof` 不变。

**必做**：

1. 删除 N1–N6 诊断/锚定/verify 链走法。
2. `rebuild_epilogue` / freeall：去掉 root / nextgc 维护。
3. `lj_state` init：HASGCMARK 不写 `gc.root` / 不把 `sweep` 指到 root。
4. `close_state`：去掉 root 断言（或改为不依赖 nextgc 的 FIXED 检查）。
5. `GCState.root` / `sweep`：**T3b-1 只停读写，字段保留**（死字段），避免
   `global_State` offsetof 漂移导致 dasc/buildvm 错位 SIGSEGV。字段删除并入 T3b-2
   （与 `GCHeader`/`gclist` 同批，强制全量 rebuildvm）。
6. 注释与 `LUAJIT_GC_ROOT_CHAIN_PROBE` / `root_chain_probe_assert.lua` 标明 retired。

**禁止**：改 `GCHeader`、动 `gclist`、改 dasc、改 DS pad。

**成功**：GCARENA 构建 + 现有 GC 测试绿；`rg nextgc lj_gc_arena.c` 仅剩注释或为零。

**独立 commit**，便于 bisect。

---

### T3b-2 — 字段条件化 + 布局（**核心**）

**目标**：HASGCMARK 对象无 `nextgc`、相关类型无 `gclist`；classic 布局与行为不变。

**必做**：

1. `GCHeader` 按 §3.1 条件化。
2. 六处 `gclist` 按 §3.2 条件化 + 断言链改造。
3. DS pad 按 §3.5 重标定；静态断言全绿。
4. `arena_roundcells` / GCupval 等 cell 断言重核。
5. 非 x86：策略 b 编译期失败。
6. `gcnext`：`#if !LJ_HASGCMARK`。
7. 卫生：过时注释（含 graythread「until openupval sweep」类残留若仍有）。

**可选 T3b-2b**（可同 PR 或 +1 commit）：

- 收缩 GCupval `prev/next` 死 union；更新 cell 断言与注释。

**禁止**：删 classic 字段；在非 x86 上 silently 编过 GCARENA。

**成功**：

- `offsetof` / `sizeof` 断言绿；
- x64 GCARENA 全量 GC 测试绿；
- x64 classic 构建绿且仍含 `nextgc`/`gclist`；
- 非 x86 + GCARENA → 明确 `#error`。

---

### T3b-3 — 验证矩阵

| 构建 | 期望 |
|---|---|
| x64 `GCARENA` + `LUA_USE_ASSERT` | 测试绿 |
| x64 `GCARENA` + ASAN（cmake FSANITIZE Debug） | 测试绿 + 专用服 load |
| x64 classic（无 GCARENA） | 构建绿；链 GC 仍工作 |
| 非 x86 + GCARENA | **失败且信息明确** |

**测试最低集**：

- openuv 全家（vector / dead_thread / sweep_assert / permgray / uvhead）
- finalizer_order / 既有 arena GC suite
- DST Debug+ASAN：`lua_vm_type=jit_gen`，`LOADING LUA SUCCESS`，`AddressSanitizer` = 0

**性能（可选）**：

- B1 / B10 与 T3b 前基线对比；预期 mark 路径小幅回正。勿与其它改动混测。

---

## 5. 风险与缓解

| ID | 风险 | 严重度 | 缓解 |
|---|---|---|---|
| R1 | `sizeof` 变化破坏 arena cell / freelist 假设 | 高 | 静态断言 + ASAN free/poison；T3b-2 单测 cell |
| R2 | DS `lua_State` pad 算错 → 游戏 ABI 错位 | 高 | 三道 `LJ_STATIC_ASSERT`；Injector 加载后冒烟 |
| R3 | 漏掉某处 `o->nextgc` / `gclist` 读写 | 中 | T3b-1/2 后全树 `rg`；classic 与 GCMARK 分建 |
| R4 | 非 x86 误开 GCARENA 链编译错误难懂 | 中 | 策略 b 统一 `#error` 文案 |
| R5 | `gc.sweep` 仍有隐藏读者 | 低 | T3b-1 前 `rg gc.sweep`；无则条件化 |
| R6 | buildvm / 宿主与 target HASGCMARK 不一致 | 中 | 已有教训：勿让 `host/buildvm_arch.h` 污染 variant；T3b 后回归 buildvm-default |

---

## 6. 与其它工作项的关系

```
T3a open-UV 向量  ──已完成──►  解锁 nextgc 热路径
§1.2 fin_queue     ──已完成──►  解锁 nextgc finalizer
strtab openaddr    ──已完成──►  解锁 nextgc string
弱表工作栈         ──已完成──►  解锁 gclist 语义

T3b-1 根链退役 ──► T3b-2 字段删除 ──► T3b-3 验证
                      │
                      ├─► §1.1 / §1.8 关闭
                      └─► P1-1 mark 头部依赖缩短
```

**不要**与 §1.4 自适应分配、§1.9 table 后备存储、nursery 大改捆绑。

---

## 7. 建议 commit 边界

1. `gc: T3b-1 retire root chain under HASGCMARK`  
   （无布局变化）
2. `gc: T3b-2 drop nextgc/gclist from GC headers under HASGCMARK`  
   （布局 + DS pad + 非 x86 关门）
3. （可选）`gc: T3b-2b shrink GCupval dead uvhead union`  
4. 文档：本文件 + Gaps §1.1/§1.8 勾选（若维护 Gaps）

---

## 8. 验收清单（打勾用）

**T3b-1**

- [ ] `lj_gc_arena.c` 无 root 链 walk / re-anchor
- [ ] `lj_state` init/close 不依赖 `gc.root`
- [ ] `GCState.root`（及无用 `sweep`）HASGCMARK 不可见或只读死
- [ ] GCARENA 测试绿；对象 `sizeof` 与 T3b-1 前一致

**T3b-2**

- [ ] `GCHeader` 无 `nextgc`（HASGCMARK）
- [ ] 六处 `gclist` 仅 classic
- [ ] GChead 断言链仅 classic
- [ ] DS pad 基 `0x58`/`0x60`（或探针校准值）+ 三断言绿
- [ ] `arena_roundcells(GCupval)` 等重核
- [ ] 非 x86 + GCARENA → `#error`
- [ ] classic 构建仍有 nextgc/gclist 且测试可跑

**T3b-3**

- [ ] openuv 181+ 及 GC suite
- [ ] ASAN Debug 专用服 `jit_gen` load + 无 ASan 报告
- [ ] （可选）B1/B10 记录

---

## 9. 附录：审查时补漏项（相对初稿）

初稿只列 `lj_gc_arena.c` 六处 `nextgc`。落地前必须包含：

1. **`lj_state.c` init** 的 `gc.root` / `gc.sweep`；
2. **`close_state`** 的 root 断言；
3. **`GCState.sweep`** 是否与 root 捆绑退役；
4. **DS pad −16** 与 **GCupval cell**；
5. **策略 b** 而非「非 x86 编译后再说」。

---

*文档版本：v1.0 · 对应审查结论「先写设计稿」· 实施前以本文件为权威。*

## 10. 落地笔记（T3b-1/2 执行）

1. **GCState.root/sweep**：T3b-1 只停读写；T3b-2 才 `#if !LJ_HASGCMARK` 删字段（与 GCHeader 同批 + 清 `host/buildvm_arch.h` 全量 rebuild）。单独删字段会 SIGSEGV。
2. **GCfuncHeader**：禁止在宏体里写 `#if`；改为两套完整 `#define`（HASGCMARK / classic）。
3. **GCudata.metatable 共享偏移**：classic 靠 GCtab.gclist 与 GCudata.len 对齐；删 gclist 后仅 `GChead==GCtab` 保留，`GChead==GCudata` 断言改 classic-only。
4. **DS pad**：HASGCMARK `@0x58` / `@0x60(+tailcalls)`。
5. **FFI 测试**：`marked`/`gct` 偏移 0/1（不再是 8/9）。

6. **GCtab/GChead 必须保留 8B spacer before metatable**（HASGCMARK 下名为 `unused_mt_pad`）：
   `vm_x64.dasc` `getmetatable` 假定 `offsetof(GCtab,metatable)==offsetof(GCudata,metatable)`。
   classic 用 `gclist` 对齐 GCudata 的 `len`；删 gclist 会导致 `lj_ff_getmetatable` SEGV。
   gclist 仍可从 GCproto/GCfunc/lua_State/GCtrace 删除。


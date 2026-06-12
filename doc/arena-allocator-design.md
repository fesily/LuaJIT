# LuaJIT Arena 分配器设计文档

> 分支: `worktree-arenagc` (基于 v2.1)
> 范围: **仅 Arena 分配器与 Allocator 层**,不含 quad-color GC / 分代 GC 本体
> 参考: LuaJIT 3.0 New GC 设计图纸 (Mike Pall wiki)、`gc/arenagc` 分支 (fsfod 原型)

---

## 1. 目标与范围

### 1.1 In scope

1. **OS 页层**: 按 1 MB 对齐向操作系统申请/归还大块内存。
2. **Arena 层**: 1 MB arena、16 字节 cell、block/mark 双位图、bump 分配器、segregated-fit 分配器。
3. **Huge block 层**: 超大对象旁路分配 + 哈希表元数据。
4. **Allocator 门面层**: 对接现有 `lj_mem_*` 宏体系,GC 对象走 arena,辅助内存(向量/缓冲区)保留 dlmalloc。
5. 编译期开关 `LUAJIT_ENABLE_GCARENA`,可随时退回原分配器。

### 1.2 Out of scope(后续阶段)

- Quad-color 标记、SSB、write barrier 改造、gray stack/gray queue —— 本阶段**现有 tri-color 链表 GC 原样保留**,arena 位图只服务于分配/释放。
- 分代模式、minor/major sweep 位图算法(但布局上为其预留,见 §5.3)。
- JIT mcode 分配(独立系统,不动)。

### 1.3 成功标准

- `LUAJIT_ENABLE_GCARENA` 打开后全量通过 LuaJIT-test-cleanup 测试集。
- 所有 GC 对象从 arena 或 huge block 分配;`luajit -e "..."` 微基准分配吞吐不低于 dlmalloc 路径。
- 长时间运行(Don't Starve 实测,见记忆中 game testing workflow)无内存泄漏、碎片可控。

---

## 2. 背景调研结论

### 2.1 设计图纸要点(必须满足的硬约束)

- Arena 大小 2 的幂(64 KB–1 MB),**自然对齐到自身大小** → 任意内部指针 `& ~(ArenaSize-1)` 即得 arena 头。
- 1/64 空间作元数据(1.5% 开销),block/mark 位图差分编码:

  | Block | Mark | 含义 |
  |-------|------|------|
  | 0 | 0 | Block extent(对象延伸) |
  | 0 | 1 | Free block |
  | 1 | 0 | White block |
  | 1 | 1 | Black block |

  整块状态只由首 cell 决定:分配只需置 block 位,标记只需置 mark 位。
- 分配器在 **bump** 与 **segregated-fit(best-fit + 有界搜索 + 按需 scavenge)** 之间按碎片压力自动切换。
- Huge block 大小为 arena 整数倍、按 arena 对齐,元数据存外部哈希表,无对象头。
- traversable / non-traversable 对象分 arena 存放。
- sweep 只触碰位图: `block' = block & mark; mark' = block ^ mark`(major)/ `mark' = block | mark`(minor);`block < mark` 可判断字内最后一个块是否 free,用于跨字合并。

### 2.2 `gc/arenagc` 分支现状(fsfod 原型,235 commits,HEAD=ffe96152 "wip")

新增文件: `src/lj_gcarena.{h,c}`、`src/gcdebug.{c,h}`,改造 `lj_alloc.c`(新增 `lj_allocpages/lj_freepages/lj_alloc_memalign`)、`lj_gc.{h,c}`、`lj_obj.h`、`lj_state.c`。

**可直接复用的成果**:
- 常量体系与 `GCArena` union 布局(`lj_gcarena.h:8-150`):1 MB arena、16 B cell、`MinCellId=1024`、位图各 8 KB,头部字段(`celltopid/celltopmax/freelist/extra/greytop...`)复用位图前 128 字节的 unused 区 —— 零额外开销,直接照搬。
- Bump 快速路径(`lj_gcarena.h` `arena_alloc`):3 条操作(读 top、`celltopandmax += n`、置 block 位),O(1)。
- Huge block:`hugeblock_alloc` 按 ArenaSize 对齐分配,**低 20 位为 0 即 huge** 的判别技巧 + 哈希表元数据。
- `lj_allocpages` OS 对齐页分配层(Windows/POSIX)。
- GG_State 自举:`arena_createGG` 让 global_State 本身住进 0 号 arena(`lj_state.c:187-215`)。
- arena 列表管理:`g->gc.arenas` 向量 + swap-remove(`lj_gc.c:1063+`)、`lj_gc_findnewarena` 的空 arena 复用/模式切换逻辑。

**确认的缺口(我们必须补齐的部分)**:

| 缺口 | 位置 | 现状 |
|------|------|------|
| fit 分配慢路径 | `arena_allocslow` (`lj_gcarena.c:267`) | `cell = 0; //arena_findfree(...)` —— bin 命中失败即返回 NULL |
| 空闲合并 | `arena_free` (`lj_gcarena.c:390`) | 相邻 free range 不合并,bin 溢出处理粗糙 |
| 应急回收 | `lj_mem_tryreclaim` | `lua_assert(0 && "TODO")` |
| sweep SIMD | `sweep_avx` | 硬编码 AVX 依赖,可移植性差 |
| 自适应切换 | — | 无碎片压力反馈,bump 满即新建 arena,内存只增不减 |

**结论**:布局、位图编码、huge block、OS 层、自举方案照搬 `gc/arenagc`;**fit 分配器、free/合并、scavenge、自适应策略按本文 §6 重写**。

### 2.3 v2.1 现有分配体系(对接面)

- 全部分配经 `g->allocf(g->allocd, p, osize, nsize)`(lua_Alloc 协议),直接调用点仅 3 处:`lj_gc.h:122`(free)、`lj_gc.c:867`(realloc)、`lj_gc.c:881`(newgco)。
- GC 对象分配入口集中:`lj_mem_newgco`(对象本体,分配后挂 `g->gc.root` 链表)与 `lj_mem_newt`(GCstr/GCudata/GCcdata 变长本体)。
- 辅助内存(table array/hash part、字符串驻留表、栈、缓冲区、向量)经 `lj_mem_realloc/lj_mem_newvec`,**特点是高频 realloc** —— 不适合首期进 arena。
- mcode 已绕过 lua_Alloc 自行 mmap,不受影响。

---

## 3. 总体架构

```
                    ┌────────────────────────────────────────┐
   lj_mem_newobj    │            Allocator 门面               │   lj_mem_newvec
   lj_mem_newgco ──▶│  lj_mem_newgco_t(L, size, gct)         │◀─ lj_mem_realloc
   lj_mem_newt      │  lj_mem_freegco(g, p, size)            │   (aux: 留 dlmalloc)
                    └──────┬──────────────┬──────────────────┘
                           │ size < HugeThreshold             │ size >= HugeThreshold
                    ┌──────▼──────┐               ┌───────────▼──────────┐
                    │  Arena 层    │               │   Huge block 层      │
                    │ bump ⇄ fit  │               │  对齐分配 + 哈希表    │
                    └──────┬──────┘               └───────────┬──────────┘
                           │                                  │
                    ┌──────▼──────────────────────────────────▼──────────┐
                    │       OS 页层  lj_arena_pages_alloc/free            │
                    │  1MB 对齐;批量 reserve + 按需 commit;地址约束      │
                    └────────────────────────────────────────────────────┘
```

新增文件(命名与 `gc/arenagc` 区分,避免合并冲突):

| 文件 | 内容 | 预估规模 |
|------|------|---------|
| `src/lj_arena.h` | 常量、`GCArena`/位图/freelist 结构、内联快速路径 | ~450 行 |
| `src/lj_arena.c` | arena 创建/销毁、fit 分配器、free/合并、scavenge、sweep 位图原语 | ~1200 行 |
| `src/lj_hugeblock.{h,c}` | huge block 哈希表与分配/释放/查询 | ~300 行 |
| `src/lj_arena_os.c` (并入 `lj_alloc.c` 亦可) | `lj_arena_pages_*` OS 层 | ~250 行 |
| 改造 | `lj_gc.h/c`(门面函数)、`lj_obj.h`(GCState 字段)、`lj_state.c`(自举)、各对象分配点宏替换 | 散点 |

---

## 4. OS 页层

### 4.1 接口

```c
/* 返回按 alignment 对齐的 size 字节;handle 回传给 free 用(整块预留的基址) */
void *lj_arena_pages_alloc(global_State *g, size_t size, size_t alignment, void **handle);
void  lj_arena_pages_free(global_State *g, void *handle, void *p, size_t size);
```

### 4.2 策略

1. **批量预留,按需提交**(对应图纸 "Memory managers may request even bigger blocks"):
   - 一次 `mmap(PROT_NONE)` / `VirtualAlloc(MEM_RESERVE)` 预留 **16 MB**(16 个 arena 槽位),对齐到 1 MB;
   - 每次需要新 arena 时 `mprotect(RW)` / `MEM_COMMIT` 一个 1 MB 槽;
   - arena 释放时 `madvise(MADV_FREE)` / `MEM_DECOMMIT` 槽位,保留预留区供复用;整个预留区全空时才真正 unmap。
   - 收益:对齐成本摊薄(对齐 mmap 需要 over-allocate + trim,批量化后每 16 个 arena 只付一次)、地址连续利于 TLB/大页、`ArenaFlag_SplitPage` 语义沿用 `gc/arenagc`。
2. **对齐获取**:预留时申请 `size + alignment` 再裁剪首尾(POSIX);Windows 用 `VirtualAlloc(NULL, ...)` 试探 + `MEM_RESERVE` 重试。复用 `gc/arenagc` 的 `lj_allocpages` 实现骨架。
3. **地址约束**:
   - GC64(主目标):无低地址要求,47-bit 内即可,直接 mmap。
   - 非 GC64:GC 对象指针必须可装入 32 位 GCRef。复用 `lj_alloc.c` 现有 `mmap_probe`/`MAP_32BIT` 逻辑(`LJ_ALLOC_MBITS`),**arena 预留区也走同一探测器**。首期允许非 GC64 下回退到逐 arena 探测(放弃批量预留)。
4. **内存上限**:预留区计数提供天然的 sandbox 粒度(图纸 Q&A 中提到的 per-arena 粒度 limit),挂 `g->gc.arenamax` 可配置。

---

## 5. Arena 层

### 5.1 常量与布局(沿用 `gc/arenagc`)

```c
enum {
  ArenaSize         = 1 << 20,            /* 1 MB,自然对齐 */
  CellSize          = 16,
  ArenaCellMask     = ArenaSize - 1,      /* 低 20 位 */
  ArenaMetadataSize = ArenaSize / 64,     /* 16 KB = 1.5% */
  MinCellId         = ArenaMetadataSize / CellSize,   /* 1024 */
  MaxCellId         = ArenaSize / CellSize,           /* 65536 */
  MaxUsableCellId   = MaxCellId - 2,      /* cell id 必须 < 0xFFFF,留哨兵 */
  ArenaUsableCells  = MaxCellId - MinCellId,          /* 64512 ≈ 1008 KB */

  BlocksetBits      = 32,
  UnusedBlockWords  = MinCellId / BlocksetBits,       /* 32 words = 128 B 头部复用区 */
  MaxBlockWord      = MaxCellId / BlocksetBits,       /* 2048 words = 8 KB / 位图 */

  HugeThreshold     = ArenaSize >> 1,     /* ≥512 KB 走 huge block,见 §7 */
};
```

```c
typedef union GCArena {
  GCCell cells[0];                  /* cell 寻址基址: &cells[cellid] */
  struct {
    union {                         /* ---- 与 mark[] 前 128 B 复用 ---- */
      struct {
        union {
          struct { GCCellID1 celltopid, celltopmax; };
          uint32_t celltopandmax;   /* bump 指针+上限,一次加法同时更新 */
        };
        MRef     freelist;          /* ArenaFreeList*,惰性创建 */
        ArenaExtra extra;           /* id/flags/allocud/finalizers... */
      };
      GCBlockword mark[MaxBlockWord];   /* 8 KB */
    };
    union {                         /* ---- 与 block[] 前 128 B 复用 ---- */
      struct {
        MRef greytop, greybase;     /* 本阶段保留字段不使用(为 GC 阶段预留) */
        GCCellID1 freecount;        /* 总空闲 cell 数(碎片压力信号) */
        GCCellID1 firstfree;        /* 最低空闲 cell id(scavenge 起点) */
        GCBlockword unused[UnusedBlockWords - 4];
      };
      GCBlockword block[MaxBlockWord];  /* 8 KB */
    };
    GCCell cellsstart[0];           /* == &cells[MinCellId],16 B 对齐 */
  };
} GCArena;
```

关键派生操作:

```c
#define ptr2arena(p)    ((GCArena *)((uintptr_t)(p) & ~(uintptr_t)ArenaCellMask))
#define ptr2cell(p)     ((GCCellID)(((uintptr_t)(p) & ArenaCellMask) >> 4))
#define arena_cell(a,i) (&(a)->cells[(i)])
#define arena_blockidx(c)  ((c) >> 5)
#define arena_blockbit(c)  (((GCBlockword)1) << ((c) & 31))
```

### 5.2 Cell 状态编码(沿用图纸/`gc/arenagc`)

本阶段语义收窄为分配器视角:**Extent(00)=对象延伸 / Free(01)=空闲 / Allocated(10)=已分配**。mark 位的 "Black(11)" 留给 GC 阶段;`arena_free` 把 block 位清 0、mark 位置 1,块尾延伸 cell 全部归零。

- 分配 n cells:`block[idx(c)] |= bit(c)`,首 cell 之后的 n-1 个 cell 保持 00 —— **一次位操作完成任意大小分配登记**。
- 释放:首 cell `block 1→0, mark 0→1`;延伸 cell 不动(00 跟在 01 后即空闲延伸);**空闲范围长度信息**另行记录在 freelist(§6.2),位图自身经 `arena_cellextent` 扫描也可恢复。
- 空闲位图一条指令可得:`freemap = mark[i] & ~block[i]`(scavenge 用)。

### 5.3 为后续 GC 预留(本阶段不实现,但不破坏)

- mark 位图、greytop/greybase 字段、`CellIdChunk` 结构保持席位;
- `arena_minorsweep/majorsweep` 的位图变换(§2.1 公式)作为独立纯函数实现并单测,但不接入 GC 主循环 —— 它同时是 scavenge 的合并原语(§6.4),不是死代码;
- 分代模式所需的 "黑块保持" 语义由 minor 公式天然支持(对应记忆中 gen-gc 工作的 trace 必须 OLD 等问题,届时在 GC 阶段处理)。

---

## 6. 分配器(本设计核心,重写部分)

### 6.1 Bump 分配器(快速路径,照搬并内联)

```c
static LJ_AINLINE void *arena_alloc(GCArena *a, MSize size)
{
  MSize ncells = arena_roundcells(size);        /* (size+15)>>4 */
  GCCellID c = a->celltopid;
  if (LJ_UNLIKELY(c + ncells > a->celltopmax))
    return arena_allocslow(a, size);            /* → fit 分配器 */
  a->celltopandmax += ncells;                   /* 同时推进 top */
  a->block[arena_blockidx(c)] |= arena_blockbit(c);
  return arena_cell(a, c);
}
```

三步,O(1),无分支预测污染;`celltopandmax` 的单加法技巧(`celltopid` 在低 16 位)沿用 `gc/arenagc`。

### 6.2 Segregated-fit 分配器(重写)

数据结构 —— 修正 `gc/arenagc` 的 stub,采用**范围式 bin**:

```c
typedef union FreeCellRange {       /* 复用 gc/arenagc 定义 */
  struct { GCCellID1 id; GCCellID1 numcells; };  /* numcells 在高 16 位 */
  uint32_t idlen;                   /* 整体比较 = 按长度排序 */
} FreeCellRange;

typedef struct ArenaFreeList {
  uint32_t binmask;                 /* bit i 置位 ⇔ bins[i] 非空 */
  GCCellID1 *bins[8];               /* bin[i]: 恰好 i+1 cells 的空闲块 id 栈 */
  uint8_t  bincounts[8];
  uint8_t  binsizes[8];             /* 栈容量,按需倍增 */
  FreeCellRange *ranges;            /* >8 cells:按 idlen 升序的有序数组 */
  MSize    rangetop, rangesz;
  GCCellID1 scavpos;                /* 增量 scavenge 的位图游标 */
  uint16_t freecells;
  GCArena *owner;
} ArenaFreeList;
```

bin 0–7 收纳 1–8 cells(16–128 B)的空闲块 —— 覆盖 GCupval/GCfunc/小 GCstr/GCcdata 等绝大多数对象;更大的进 `ranges` 有序数组。

**分配算法**(`arena_allocslow`,n = 所需 cells):

```
1. exact-fit:  bin = min(n,8)-1; binmask 命中 → 弹出,置 block 位,返回。      O(1)
2. next-fit:   ffs(binmask >> bin) 找最小的更大 bin → 弹出、切割,
               余下 (k-n) cells 推回 bin[k-n-1] 或 ranges。                   O(1)
3. range 搜索: ranges 按长度有序,二分找首个 ≥n 的范围(best-fit),
               搜索步数上限 BoundedSearchMax=8(有界搜索,图纸要求);
               命中 → 切割,余量更新回 ranges(原位改 idlen,必要时下沉)。
4. scavenge:   未命中且 scavpos < celltop → 执行一轮增量扫描(§6.3)后重试 1-3,
               每次 allocslow 至多 scavenge ScavengeBudget=64 个位图字(2 KB cells)。
5. 全部失败:   返回 NULL → 门面层换 arena / 新建 arena(§8.2)。
```

**释放算法**(`arena_free(a, p, size)`):

```
1. c = ptr2cell(p); n = roundcells(size);
2. 立即尝试前向合并:若 cell c+n 处于 Free(查位图)且其范围记录可定位
   (在 bin 栈/ranges 中线性查找上限 4 次,找不到就放弃合并 —— 位图仍正确,
    留给 scavenge 兜底),合并成大范围;
3. 置位图: block bit 清 0,mark bit 置 1;
4. 入 bin(n≤8)或 ranges(插入排序,memmove);
5. freecount += n;更新 firstfree = min(firstfree, c)。
```

与 `gc/arenagc` 的关键差异:**位图是唯一真相源,freelist 只是缓存**。bin/ranges 允许不完整(放弃合并、容量满时丢弃最小项),正确性由 scavenge 从位图重建保证 —— 这消除了原分支里 bin 溢出和合并缺失导致的丢失空间问题。

### 6.3 Scavenge(按需、增量、有界)

对应图纸 "bounded-effort scavenging phase that scans the block map":

```c
/* 从 scavpos 起扫描位图,把发现的空闲范围归类入 bin/ranges,
   预算 budget 个 block word,返回是否发现 ≥wantcells 的范围 */
int arena_scavenge(ArenaFreeList *fl, MSize budget, MSize wantcells)
{
  GCArena *a = fl->owner;
  for (; budget-- && fl->scavpos < a->celltopid;) {
    MSize w = arena_blockidx(fl->scavpos);
    GCBlockword freebits = a->mark[w] & ~a->block[w];
    while (freebits) {
      GCCellID c = (w << 5) + lj_ffs(freebits);
      MSize len = arena_cellextent(a, c);   /* 数延伸:到下个非 Extent 位 */
      /* 连续 Free 块在此天然合并:extent 扫描跨过 free+extent 序列时
         把相邻 free 首位也吸收(把后续 free 首 cell 的 mark 位清为 extent) */
      freelist_insert(fl, c, len);
      if (len >= wantcells) { fl->scavpos = c + len; return 1; }
      freebits 清除已处理位;
    }
    fl->scavpos = (w + 1) << 5;
  }
  return 0;
}
```

要点:
- **合并发生在 scavenge**(吸收相邻 free 块、改写次块首 cell 为 extent),而不是 free 时强制 —— free 保持 O(1);
- `sweeplimit/scavpos` 在 GC sweep 后重置为 `firstfree`,使 sweep 释放的空间能被重新发现;
- word 级 `mark & ~block` 一次取 32 cells 的空闲视图,缓存友好(只读元数据区)。

### 6.4 Bump ⇄ Fit 自适应切换(图纸要求,新增)

每 arena 两个信号:

```
frag(a)   = freecount / (celltopid - MinCellId)   /* 已 bump 区域中的空洞率 */
bumpleft  = celltopmax - celltopid
```

策略(在门面层 `findarenaspace` 与 GC 周期边界评估):

| 条件 | 动作 |
|------|------|
| `bumpleft > n` 且 `frag < 25%` | 继续 bump(默认) |
| `frag ≥ 25%` 或 bump 耗尽 | 该 arena 标记 `ArenaFlag_NoBump`,后续分配先走 fit |
| fit 连续 MissMax=16 次未命中 | 摘掉该 arena 的 "当前分配 arena" 身份,换/建新 arena |
| GC sweep 后 `frag < 10%` 且 ranges 中存在尾部大范围 | 若 arena 尾部(`celltopid` 之前)整段空闲,**回卷 celltopid**,重新启用 bump |

回卷(bump-pointer rollback)是对图纸 "switched back to the bump allocator" 的具体化:sweep 位图阶段顺带计算每 arena 最高已分配 cell(`arena_topcell`),`celltopid` 直接降到其上,尾部碎片整体回收 —— 这是 `gc/arenagc` 完全没有的内存归还路径。

### 6.5 应急回收

`arena_allocslow` 与新建 arena 之间插入压力阶梯(替代 `lj_mem_tryreclaim` 的 TODO):

1. 当前 arena fit 失败 → 轮询其他同模式 arena 的 freelist(只查 binmask/ranges 顶,O(arena 数));
2. 仍失败且 `g->gc.total > threshold` → 触发一步 GC(沿用现有 `lj_gc_step`);
3. 新建 arena;OS 拒绝 → 全量 GC + 释放 Empty arena 后重试一次;
4. 仍失败 → `lj_err_mem`。

---

## 7. Huge block 层

沿用 `gc/arenagc` 方案,阈值收紧:

- **阈值**: `HugeThreshold = 512 KB`(> ArenaUsableCells 一半;大于它的对象在 arena 内必然造成 >50% 浪费或根本放不下)。图纸说最优值需实验,先取保守值,留 `g->gc.hugethreshold` 可调。
- **分配**: `lj_arena_pages_alloc(g, round_up(size, ArenaSize), ArenaSize, &handle)` —— 大小取 arena 整数倍、按 arena 对齐。
- **判别**: `((uintptr_t)o & ArenaCellMask) == 0` ⇔ huge(普通对象永远不会落在 arena 起始 —— 那是元数据区;此不变量由 `MinCellId>0` 保证)。
- **元数据**: 开放寻址哈希表 `HugeBlockTab`(key=地址>>20,value={size, gct, flags}),挂在 `global_State`。free/查询 O(1)。
- 计入 `g->gc.total` 与 huge 专属统计 `g->gc.hugemem`。

---

## 8. Allocator 门面层

### 8.1 API 与改动面

保持 lua_Alloc 协议与 `lj_mem_realloc/newvec/growvec`(辅助内存)**完全不动**,仍走 dlmalloc —— 高频 realloc 的 table array/hash、栈、缓冲区不适合位图分配器,这也是 `gc/arenagc` 的选择。

GC 对象本体改走新入口(签名沿用 `gc/arenagc`,便于将来合并其 GC 阶段代码):

```c
GCobj *lj_mem_newgco_t(lua_State *L, GCSize size, uint32_t gct);  /* 带类型 */
void   lj_mem_freegco(global_State *g, void *p, GCSize size);

#define lj_mem_newobj(L, t)      ((t *)lj_mem_newgco_t(L, sizeof(t), gctid_##t))
#define lj_mem_newgcot(L, s, t)  ((t *)lj_mem_newgco_t(L, (s), gctid_##t))
```

```c
GCobj *lj_mem_newgco_t(lua_State *L, GCSize size, uint32_t gct)
{
  global_State *g = G(L);
  GCobj *o;
  if (LJ_LIKELY(size < g->gc.hugethreshold)) {
    GCArena *a = istrav(gct) ? g->travarena : g->arena;
    o = (GCobj *)arena_alloc(a, size);
    if (LJ_UNLIKELY(o == NULL))
      o = findarenaspace(L, size, istrav(gct));   /* §8.2 */
  } else {
    o = hugeblock_alloc(L, size, gct);
  }
  g->gc.total += size;
  return o;
}
```

调用点改造(均为机械替换):

| 对象 | 文件 | 现状 | 改为 |
|------|------|------|------|
| GCstr | lj_str.c:278 | `lj_mem_newt` | `lj_mem_newgcot(L, lj_str_size(len), GCstr)` |
| GCtab | lj_tab.c:88/103 | `lj_mem_newgco`/`newobj` | `lj_mem_newgcot`/`lj_mem_newobj` |
| GCfunc | lj_func.c | `lj_mem_newgco` | 同上 |
| GCproto | lj_parse.c | `lj_mem_newgco` | 同上(各子向量仍 dlmalloc) |
| GCupval | lj_state.c | `lj_mem_newobj` | 不变(宏内部已切换) |
| GCudata | lj_udata.c:14 | `lj_mem_newt` | `lj_mem_newgcot` |
| GCcdata | lj_cdata.c | `lj_mem_newt/newgco` | `lj_mem_newgcot` |
| lua_State | lj_state.c | `lj_mem_newobj` | 不变 |

释放侧:各 `*_free` 函数中的 `lj_mem_free(g, o, size)` 对 GC 对象替换为 `lj_mem_freegco`,内部:

```c
void lj_mem_freegco(global_State *g, void *p, GCSize size)
{
  g->gc.total -= size;
  if (LJ_UNLIKELY(gc_ishugeblock(p)))
    hugeblock_free(g, p, size);
  else
    arena_free(ptr2arena(p), p, size);
}
```

**现有 GC 不感知任何变化**:对象仍带 GCheader、仍挂 `g->gc.root` 链、sweep 仍沿链表调用 free 函数 → 落到 `lj_mem_freegco` → arena 位图更新。tri-color 正确性不受影响。

### 8.2 当前 arena 管理与 `findarenaspace`

`global_State` 新增(沿用 `gc/arenagc` 字段名):

```c
GCArena *arena;        /* 当前 non-traversable 分配 arena */
GCArena *travarena;    /* 当前 traversable 分配 arena */
/* GCState 内 */
GCArena **arenas; MSize arenassz, arenastop;   /* arena 注册表(swap-remove) */
ArenaFreeList *freelists;                       /* 与 arenas 平行的数组 */
GCSize hugethreshold, arenamax, hugemem;
```

`findarenaspace` 流程(综合 `gc/arenagc` 的 `lj_gc_findnewarena` + §6.5 压力阶梯):

```
1. 当前 arena fit(allocslow 含增量 scavenge)→ 成功则返回;
2. 遍历注册表找 Empty/低 frag 且模式匹配的 arena → 设为当前 arena,goto 1;
   (Empty 但模式不符的 arena 可整体转换模式 —— 空 arena 转换零成本)
3. 压力阶梯(§6.5 步骤 2);
4. lj_gc_newarena() 注册并设为当前 → bump 分配必然成功。
```

### 8.3 自举(GG_State 先有鸡还是先有蛋)

沿用 `gc/arenagc` 的 `arena_createGG`:`lua_newstate` 先以 OS 层直接建 0 号 arena,GG_State(lua_State+global_State+dispatch 表)bump 进该 arena,之后 `g->gc.arenas` 注册表再把它登记为 id 0(带 `ArenaFlag_GGArena`,永不销毁)。dlmalloc 实例(`lj_alloc_create`)仍照常创建,服务辅助内存。

自定义 lua_Alloc 用户(`lua_newstate(f, ud)`):辅助内存走用户 f;arena/huge 页**仍走 OS 层** —— 在文档与 luaconf 中明示这一行为变更(图纸亦然:"requested directly from the operating system");`LUAJIT_USE_SYSMALLOC` 构建下整个特性禁用。

---

## 9. 与 `gc/arenagc` 的差异总表

| 维度 | gc/arenagc | 本设计 |
|------|-----------|--------|
| fit 慢路径 | stub(返回 NULL) | exact→next→bounded best-fit→scavenge 四级 |
| freelist | bins 数组裸指针,易丢失空间 | 位图为真相源,freelist 仅缓存,可重建 |
| 空闲合并 | 注释掉 | scavenge 内吸收合并 + sweep 后 bump 回卷 |
| bump↔fit 切换 | 无(bump 满即新建) | frag/missrate 双信号 + NoBump flag + 回卷 |
| 应急回收 | `assert(0) TODO` | 跨 arena 轮询→GC step→full GC→err_mem 阶梯 |
| OS 层 | 逐 arena 对齐 mmap | 16 MB 批量 reserve + commit/decommit |
| sweep SIMD | 硬编码 AVX | 可移植 32/64-bit word 版为基线,SIMD 后置 |
| GC 算法 | 全套替换(未完成) | **不动现有 GC**,位图仅服务分配器 |
| huge 阈值 | ~504 KB(ArenaMaxObjMem>>1) | 512 KB,运行期可调 |

---

## 10. 实现计划

| 里程碑 | 内容 | 验收 |
|--------|------|------|
| M1 OS 层 | `lj_arena_pages_*`,批量 reserve,Linux+Windows | 单测:对齐、复用、decommit;ASAN 干净 |
| M2 Arena 核心 | 布局/位图/bump/`arena_free`/`cellextent` | C 单测(独立于 Lua):随机 alloc/free 模式下位图与影子分配器比对 |
| M3 Fit+scavenge | §6.2–6.4 全量 | 碎片化压测:交替大小 alloc/free 后利用率 ≥85% |
| M4 Huge block | 哈希表+分配/释放 | 单测 + 边界(恰好阈值、>1 arena 倍数) |
| M5 门面接入 | `lj_mem_newgco_t`、调用点替换、自举、`LUAJIT_ENABLE_GCARENA` | LuaJIT-test-cleanup 全过;关闭开关二进制等价 |
| M6 调优 | 自适应参数、bump 回卷、统计接口(`luaJIT 扩展 gcinfo`) | 微基准 ≥ dlmalloc 路径 95%;Don't Starve 实测(见 game testing 记忆) |

依赖关系:M1→M2→M3→M5;M4 可与 M3 并行。M2 起即可在每步用 `gcdebug.c`(从 `gc/arenagc` 移植 `lj_gc_verify` 思路)做位图一致性断言。

## 11. 风险与开放问题

1. **非 GC64 低地址压力**:批量 reserve 在 32 位地址空间可能挤占 mcode 区域 → 缓解:非 GC64 降级为逐 arena 探测分配;主目标平台定为 x64 GC64。
2. **`gc.total` 语义漂移**:arena 有内部碎片,`total`(对象字节)与 RSS(arena 字节)分叉,现有 GC 触发阈值基于 total,可能延迟回收 → 增加 `g->gc.arenatotal` 并在 GC pacing 中取 `max(total, arenatotal*α)`,α 初值 0.75。
3. **GCstr 16 B cell 浪费**:大量 ≤8 字符短串(GCstr 头 16/24 B + 数据)round 到 2–3 cells,平均浪费实测后评估;若 >15% 考虑字符串专用 8 B 子分配(后续阶段)。
4. **realloc 类对象**(table 部件)留在 dlmalloc,意味着双分配器并存的常驻成本(dlmalloc 段 + arena 区)——接受,图纸同样保留 "memory manager" 双层。
5. **哨兵约束**:cell id 0xFFFF 不可用(`MaxUsableCellId`),分配恰好顶满 arena 尾部时注意边界,单测覆盖。
6. **与 gen-gc 工作的汇合**:本分配器位图的 minor/major sweep 原语为分代模式准备;trace 对象(记忆:traces 必须 OLD)将来应进 `ArenaFlag_LongLived` arena —— 字段已预留,本阶段 trace 仍走普通路径。

---

## 附录 A: 参考代码索引

- `gc/arenagc:src/lj_gcarena.h` — 常量/GCArena/CellState/FreeCellRange 定义
- `gc/arenagc:src/lj_gcarena.c:267` — arena_allocslow(stub,本设计 §6.2 替换)
- `gc/arenagc:src/lj_gcarena.c:390` — arena_free(无合并,本设计 §6.2/6.3 替换)
- `gc/arenagc:src/lj_gc.c:1011` — lj_gc_findnewarena(arena 复用/模式切换,沿用)
- `gc/arenagc:src/lj_state.c:187` — arena_createGG 自举(沿用)
- `gc/arenagc:src/lj_alloc.h` — lj_allocpages/lj_freepages OS 接口(扩展为批量 reserve)
- `v2.1:src/lj_gc.h:111-134` — lj_mem_* 宏(辅助内存路径保持不变)
- 设计图纸: 仓库根 `LuaJIT 3.0 new Garbage Collector.md`

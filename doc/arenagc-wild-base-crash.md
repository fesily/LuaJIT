# arenagc 变体：野 `L->base` 导致报错路径访问违例（既有缺陷，2026-10-05 定位）

状态：**已定位并修复（2026-10-05）** —— 根因是 x64 `barrierback` 宏把 `RC/TMPR/ITYPE/RA/BASE` push 进**被调 C 函数的 Win64 shadow space**，被 `lj_gc_barrierback_arena` 序言（`mov [rsp+10h],rbx`）覆盖 ⇒ 弹栈后 `RA`/`BASE` 被污染（ASan 构建下表现为 `L->base = G(L)`）。修复见 `src/vm_x64.dasc`，验证与证据见文末 **追记 14**。此前的中间结论（“`L->base` 被其它写入者破坏 / 寄存器相对值漂移”）都是该污染的下游现象，已被追记 14 取代；而中间确证的阴性结果仍然有效：非栈重分配、非 `jit_base`、非 JIT、Windows 专有、与 `LUA_COMPAT_TAILCALL_CFRAME` 无关。

## 复现

```
builds/ninja-multi-vcpkg/luajit/RelWithDebInfo/luajit-arenagc.exe tests/lua_vm_parity/c_tailcall_values.lua
echo $?        # 修复前: 5（3/3 稳定）；修复后: 0。默认变体 lua51DS.dll/luajit.exe 无此问题
```

触发面是「反复报错 + traceback + `__call`/tostring 元方法」这种形状（`c_tailcall_values.lua` 的 `check_shape` 循环即是）；单独跑其中任一 fixture（`a values` / `shape12` / `shape3` / `pcall0`，200–2000 次）不会崩，需要整文件的分配/循环规模。

已确认**与以下因素无关**（均有实测证据）：

- 与 JIT 无关：把脚本里的 `jit.on()` 强制替换为 `jit.off()` 后同样 exit 5。
- 与本特性无关：`-DLUA_COMPAT_TAILCALL_CFRAME=0` 重编（matched gate-off 二进制）同样复现。
- 与栈重分配无关：插桩 `resizestack`，所有调用实测 `delta == 0`（栈缓冲区从未移动）。
- 与 `jit_base` 无关：崩点时 `g->jit_base == NULL`；`lj_gc_step_jit` 7 次调用取到的 `jit_base` 全部落在栈范围内。

## 崩溃链（cdb + 临时插桩）

```
lj_debug.c debug_varname(191) / debug_framepc(135)   ← AV 读取野指针 [pt+0x50] / 野 PC
  ← lj_debug_slotname / lj_debug_addloc / debug_frameline
  ← lj_err.c lj_err_optype(987/990)
  ← lj_err.c lj_err_optype_call(1018)        （对不可调用值解析 __call 失败）
  ← lj_meta.c lj_meta_call(525)
  ← vm_x64.dasc ->vmeta_call（解释器/汇编侧）
```

根因不变量：**报错时 `L->base` 已不在 Lua 栈范围内**。插桩实测（`lj_err_optype_call` 入口）：

```
L=... base=0x...606CD8 o=0x...606CD8 off=0 pc=0x...606CFC ftype=0
   slot2=(0x...606D2D,0x...606D30) stk=0x...64FDD0 size=166 top=0x...64FF58 inrange=0
```

`base` 落在堆/分配器元数据区（内容为 free-list 样式指针），同一次构建内偏移确定。随后 `curr_func(L)`/`funcproto()`/`frame_pc()` 用这个"帧"推出野原型/野 PC → 访问违例。默认变体同一处野读落进已映射内存，退化为无害的"无名帧"，所以只有 arena 变体崩。

补充：给 `curr_funcisL` 加 TValue tag 校验**不能**修好（崩点顺移到 `err_msgv`→`lj_debug_addloc`→`debug_framepc`，仍是同一个野帧），已回退。

## 写入者（本轮结论）

`L->base` 的 C 侧写入点（`grep "L->base = "`）共 14 处，逐一核对：`lj_err.c` 9 处已全部临时插桩（越界即打印），**0 命中**；`lj_state.c`（resizestack/growstack/init）`delta==0` 或初始化路径；`lj_api.c:1355`（hook 中 yield）与 `lj_ccallback/lj_crecord`（FFI 回调/录制）本用例不会执行；`lj_meta.c:100`（`lj_meta_tailcall`，仅 FFI/`lj_carith` 调用）本用例不会执行。因此**野值由解释器汇编写入**：汇编各处的 `mov L:RB->base, BASE`（`->vm_call_dispatch`/`->vmeta_call`/`->vmeta_*`/`vm_gcstep` 之后等）把当时的 `BASE` 寄存器落进 `L->base`，即**某条汇编路径带着野 `BASE` 在跑**。

## 下一步（抓具体汇编点）

1. 对 `&L->base` 下条件硬件断点，命中条件「新值不在 `[stack, stack+16*stacksize)`」：
   - `cdb -lines -logo <log> -cf <cmds> <exe> <script>`，命令文件里
     `bp lua51DS_gengc!lua_cpcall` 里算 `L`，再 `ba w 8 <&L->base> "条件/打印/继续"`；
   - ⚠️ **偏移更正（追记 5 实测 PDB `dt lua51DS_arenagc!lua_State`，布局由 `LJ_STATIC_ASSERT` 锁定、各变体通用）**：`base=0x10`、`top=0x18`、`maxstack=0x20`、`stack=0x28`、`openuv=0x30`、`env=0x40`、`cframe=0x48`、`stacksize=0x50`、`tailcalls=0x58`、`_dst_pad[88]@0x60`、`reserved[8]@0xb8`、`userdata@0xc0`。下表原值整体**错位一格**（0x18 其实是 top、0x30 其实是 openuv、0x58 其实是 tailcalls），据此做的读数为错标。
  - （原表，作废）字段偏移：`base=0x18`、`top=0x20`、`maxstack=0x28`、`stack=0x30`、
     `openuv=0x38`、`openuvtop=0x40`、`openuvsz=0x44`、`openupval=0x48`、`env=0x50`、`stacksize=0x58`。
   - 现成脚本：`builds/cdb_hits.bat` + `builds/arena_cmds.txt`（cdb 的 `@$` 伪寄存器赋值语法在本机批量模式下未生效，需要交互式或改用硬编码 `L`）。
2. 或直接在 `vm_x64.dasc` 的 `mov L:RB->base, BASE` 系列处加临时越界检查（注意该处 `BASE=rdx`、`RA=rcx` 为 volatile，helper 返回后需 `mov BASE, L:RB->base` 复原）。
3. 定位后按该汇编路径的 `BASE` 来源修复（多半是某个 `BASE` 建立在过期/错误帧上）。

---

## 2026-10-05 追记：隔离工作区 + 三条硬结论

隔离工作区（引擎无关、不依赖主仓库构建）：`C:/Users/fesil/luajit-cframe-exp/`
（luajit 树独立副本 + MSVC/Ninja 独立构建 + cdb 脚本；`build*.bat` / `run*.bat` /
`gcsuite*.bat` / `cdb-run.bat` / `instrument_base_checks.py`；主仓库与 luajit 子模块保持 clean）。

### 1. 复现与崩溃链（与上文一致）
- `build/RelWithDebInfo/luajit-arenagc.exe tests/lua_vm_parity/c_tailcall_values.lua` → 3/3 退出码 `-1073741819` (0xC0000005)。
- ASan（Debug + `/fsanitize=address`）报告同链：`lj_err_optype`(lj_err.c:982，读地址 0x2) ← `lj_err_optype_call`(1018) ← `lj_meta_call`(525) ← `lj_vmeta_call` ← 解释器 `->vmeta_call`。
- `-DLUA_COMPAT_TAILCALL_CFRAME=0` 仍撞同一缺陷（ASan 下先命中 Debug 断言 `lj_debug.c:855 tvisfunc(L1->top-1)`，是该测试在关特性时的预期差异，不是本缺陷）。

### 2. 负结果：`L->base` 不是被「带 BASE 的写」写野的
- 对 `vm_x64.dasc` **全部 70 处 BASE 写入**（`mov/lea/sub/add BASE,...`，含宏内实例；`instrument_base_checks.py`）插入区间检查：`BASE ∈ [L->stack, L->stack + 16*stacksize)`，违反即 `mov eax,<id>; int3`。实现要点：先读 `SAVE_L` 再 `push/pop` 保存 TMPR/ITYPE（SAVE_* 是 `[rsp+…]`，push 后读取会取错槽位）；DynASM 局部标签只能用 1–9 数字，需按站点选安全数字。
- 结果：**0 次触发**，程序照旧 AV ⇒ 任何 `mov L:*->base, BASE` 落进 `base` 的值都在栈内。
- 仅 16..30 号点位插桩时曾抓到一次：`lj_fff_res_+0x40`（`lea BASE,[BASE+RA*8-16]`）处 `BASE = PC - 112`（越界）。后续全量插桩不再命中、崩溃点随构建布局漂移 ⇒ 属于「先有内存破坏，后在不同点位爆掉」，不是 BASE 算式本身。

### 3. 决定性：arenagc 连仓库自带 GC 断言套件都跑不过
- 隔离工作区断言构建（Debug + `LUA_USE_ASSERT/APICHECK`）跑仓库 13 个 GC 测试：**12 个 0xC0000005，1 个 rc=1**。
- 用**主仓库自建**的 `builds/ninja-multi-vcpkg/luajit/RelWithDebInfo/luajit-arenagc.exe` 复跑 3 个：同样失败（rc=3 / rc=5）⇒ 不是隔离副本伪影，是 arenagc（gengc）变体在 Windows/MSVC 上的普遍缺陷，与主 CMakeLists 记录的 “luajit-arenagc currently AVs on tools/modinfo2cpp.lua” 同源。
- 这些 GC 测试不含 C/FF 尾调用代码 ⇒ 与 `LUA_COMPAT_TAILCALL_CFRAME` 无关。
- 打开 GC verify（去掉 `-DLJ_GC_NOSTEPVERIFY -DLJ_GC_NOFULLGCVERIFY`）后 `white1_free_assert.lua`、`udata_finalize_assert.lua` 反而全绿 ⇒ 布局/时序敏感型破坏（heisenbug）。

### 4. 建议的下一步（都在隔离工作区内做）
- A. 对 `lj_gc_arena.c` / `lj_arena.c` 的 GC 提交做 `git bisect`（判据 `white1_free_assert.lua` 通过与否），确认 Windows 上是否曾经绿过。
- B. 审查 arena 的 Windows 端口：页释放语义（`MADV_DONTNEED` 的“下次访问零填充”在 Windows 侧如何保证）、块/cell 对齐与 size 假设、chunk commit/decommit 路径——当前最大嫌疑面。
- C. 走 Linux + ASan 官方车道（`tools/run_debug_asan.sh --suite-only`）拿全插桩报告；MSVC 的 ASan 构建在本仓库不被该车道覆盖，本轮实测其 arena 表现不可信（13/13 在不同字节码处理器处 AV）。

### 5. 追记 2（同日）：缺陷是 Windows 侧专有；两个平台对齐实验均为否定

隔离工作区内用 WSL(Ubuntu 24.04, gcc 13.3) 建 arena 原生平台对照（`luajit-linux/`，`make ASAN=1 XCFLAGS="-DLUAJIT_ENABLE_GCARENA -DLUA_USE_ASSERT"`，`ASAN_OPTIONS=detect_leaks=0:allow_user_poisoning=1:halt_on_error=1`）：

- `c_tailcall_values.lua` → **rc=0 / "ok c_tailcall_values"**；
- GC 套件抽 6 个（white1_free / udata_finalize / huge_swept_tag / stale_gray_survivor / gc_invariants / gc_adversarial）→ **全绿**。

⇒ **同一源码、同一 arena+ASan+assert 配置：Linux 全绿、Windows 12/13 AV**，缺陷为 Windows 侧专有。

平台差异定位（`lj_arena.c`）与两个对齐实验（均**否定**，只改隔离副本，已还原）：

| 平台差异 | 对齐实验（Windows 侧改成 Linux 等价语义） | 结果 |
|---|---|---|
| `arena_os_reserve`：Windows `MEM_RESERVE,PAGE_NOACCESS`（提交前访问即 AV） vs Linux `mmap(PROT_READ\|WRITE)` 即刻可读写 | 改为 `MEM_RESERVE\|MEM_COMMIT,PAGE_READWRITE` | 仍崩 ✗ |
| `arena_os_decommit`：Windows `VirtualFree(MEM_DECOMMIT)`（访问即 AV） vs Linux `madvise(MADV_DONTNEED)`（访问返回 0） | 改为等价“保持可读 + 清零” | 仍崩 ✗ |

⇒ 排除了“访问 reserved/decommitted 页”这一类保护性原因，剩余解释是**内容级损坏**（读到的对象引用被写坏），崩溃点随代码尺寸/堆布局漂移亦与此一致。

写屏障审核（`vm_x64.dasc`，arena 分支由 `LJ_HASGCMARK` 门控，而 `LJ_HASGCMARK` 只在 `LJ_HASGCARENA` 时为 1）：`barrierback`→`lj_gc_barrierback_arena` 保存 rcx/rdx/rax/r10/r11 且 X64WIN 下 BASE=rdx、RA=rcx、RC=rax、TMPR=r10、ITYPE=r11 均在其中；KBASE=rdi、PC=rsi(WIN)/rbx(POSIX)、DISPATCH、RB=rbp 都是 callee-save；Windows 专有的 `xchg CARG2, RB`（WIN 下 CARG2==BASE==rdx）用法正确。**未发现漏保存**。

下一步（缩小后的靶子）：在 Windows 上定位“对象引用被写坏”的写者/来源——以崩溃点野指针所在的**存储**（栈槽/cell）为靶下硬件写断点，或对 `lj_gc_arena.c` 的 epoch/bitmap 状态机做定点插桩。Linux 侧已排除；`hist/probe.sh` 的 bisect 仅覆盖 CMake 端口之后的提交。

### 6. 追记 3（同日）：写者定位——排除两条、把靶子缩到“被写坏的 proto 常量槽”

- **MSVC `/fsanitize=address` 确实定义 `__SANITIZE_ADDRESS__`**（实测预处理输出 `SANADDR_YES`）⇒ 之前的 MSVC-ASan 构建里 arena 的 poison 契约**是生效的**；其报告全是 access-violation、**无 use-after-poison** ⇒ **不存在“读已释放（poisoned）cell”**。（不要用 `-DLUAJIT_USE_ASAN=1` 手动打开：`lj_asan.h` 会直接 `#error`，且全局 CFLAGS 会打到宿主工具编译。）
- **0x5A 模式填充**释放 cell 的 body（EXP-ONLY，已还原）：崩溃值**不变** ⇒ 也不是释放 cell 的残留体被当指针读。
- 崩溃值的决定性性质：**跨运行稳定**（同一次构建内多次运行、其它指针都随堆 ASLR 变化，而它恒为 `0x0b42_0002102b`），形状为 `高32位(0x0b42, gen/epoch 类) : 低32位(0x0002102b, index 类)` ⇒ **数据派生的 arena 头部/状态字**，不是过期堆指针。
- 崩溃点回源：`lj_BC_FNEW`（解释器）→ `lj_func_newL_gc`（`lj_func.c:292/295`，读 `pt->sizeuv` 时 AV），此时参数 **`pt` 已是该野值** ⇒ **某个写者把“当前函数 proto 的常量 TValue 槽”覆盖成了这个状态字**。（`lj_dispatch.h` 的 `GOTDEF` 列表即解释器调用这些 C helper 的路径。）
- 结构布局快查：`lj_obj.h`/`lj_arena.h` 无 `#pragma pack`、无 bitfield；`LJ_STATIC_ASSERT` 14+2 处；`__attribute__` 只出现在 `lj_gc.h` 的平台分支 ⇒ 未见明显 MSVC/GCC 布局分歧（未穷尽）。

下一步（点状、可直接做）：对“当前函数 proto 的常量槽”下**动态**硬件写断点——地址可由 `KBASE + RD*8` 在 `BC_FNEW` 前算出（KBASE 随 ASLR 变，需在同一 run 内用 `$t0` 用户寄存器动态设置）；命中的写者即根因，候选类为“把 arena 头部/epoch 状态字写进 TValue”的路径（T3b slim-header / P3–P4 状态机）。

### 7. 追记 4（同日）：Windows ASan 车道跑通，崩溃存储点定位到“帧的函数槽”

- **Windows MSVC ASan 车道可用**（工作区 `build-asan.bat`；已实测 `/fsanitize=address` **会定义** `__SANITIZE_ADDRESS__`，故 arena poison 契约生效）：三个 GC 测试 **3/3 确定性**地停在 `lj_err_optype`（lj_err.c:982）**读地址 0x2**，与主复现同链（`lj_err_optype_call` ← `lj_meta_call` ← `->vmeta_call`）。报告性质仍为 access-violation、**无 use-after-poison**。
  - 注意：cdb 跑该 ASan 构建要连续 `g` 两次（ASan 运行时有额外的初始断点），否则会停在 pre-main 并打印无意义的寄存器。
  - 该 ASan 构建里 `rbp = L + 0xB8`（DS 保留槽），取 L 用 `r10`。
- 源码回读：`lj_meta_call(L, func, top)`（lj_meta.c:521-529）里 `func` 就是**帧的函数槽指针（`BASE-16`）**；`lj_err_optype_call`→`lj_err_optype` 先 `lj_typename(o)`，再 `curr_proto(L)`→`lj_debug_slotname`→`debug_framepc`（即既有文档链）⇒ **帧的函数槽（及由它推出的 proto）里是小整数元数据**（依读地址 0x2 推得指针≈2），与本轮“跨运行稳定的 arena 状态字”同族。
- 为抓写者加的 arena 守卫（EXP-ONLY，已存工作区 `guarded/`，源树已还原）：`lj_dbg_arena_struct_ok`（对齐、chunk 槽位 freemap、registry id 一致性）＋“不得销毁 class-current arena”检查＋`lj_dbg_arena_bad`（打印 `a/where/_ReturnAddress` 后 `__debugbreak()`）。带守卫构建中 `white1_free_assert` / `udata_finalize_assert` 曾以 **rc=3** 结束（= 0x80000003 被 bash 截断为 3）：命中的是 VM 侧 `->assert_bad_for_arg_type`（vm_x64.dasc:2830，`FOR_STOP/FOR_STEP` 类型断言，由 4714/4715/4783/4784 跳入）⇒ **循环状态已先被写坏**；守卫本身未打印，故该次不是守卫命中。
- **下一步（点状）**：在 Windows ASan 构建里对“帧的函数槽（`BASE-16`）”下**动态**写断点：先在 `lua_cpcall`（rcx）或 `lj_dbg_arena_bad` 处取 L，再用 `$t0 = L + (slot - L)`（相对偏移在同一次构建内稳定，不受 ASLR 影响）设置 `ba w 8 $t0`，命中者即写者。候选写者仍是“把 arena/GC 状态字写进 TValue 槽”的路径（T3b slim-header、P3–P4 状态机、arena 标记位图）。

### 8. 追记 5（同日）：抓到写者 —— `lj_snap_restore`；状态 = 解释器在 `lua_State` 结构体内“跑帧”

方法：在未插桩的 ASan 构建上，用 cdb 在 `lua_cpcall` 处取 L（`$t0=@rcx`），对 `&L->base`（`$t0+0x18`）下**硬件写断点**，每命中把 `(值, 写者RIP)` 覆盖写入我加的两个**只写不读**全局字（`lj_dbg_wr_val`/`lj_dbg_wr_rip`，EXP-ONLY），随后照常运行到崩溃，直接读全局。命令文件 `cdb/p15.txt`，**嵌套写法必须与 p15 一致**（`ba w 8 $t0+18 \"…; g\"`），且 cdb 跑 ASan 构建要连续两次 `g`。

结果：
- 最后一次写 `L->base` 的值 = `0x44a029c073`（野值），写者 RIP = `lua51DS_arenagc.dll+0x148B35`；反汇编（dumpbin）该处 = `mov rcx,[rsp+2F0h]; mov [rcx],rax; jmp …`，其所在函数 = **`lj_snap_restore`** ⇒ **JIT trace 退出时把快照里的 `base` 恢复成了野值**。
- 同一路径上另一构建/运行的确定性状态（`EXP-META-CALL` 打印，ASan）：`L->base = L+0xA8`、`L->top = L+0xC0`、调用点 `BASE = L+0xB8`，而 `[L+0xA8]/[L+0xB8]/[L+0xC0]` 全为 0。对照 `lj_obj.h` 的 DS 布局锁（`LJ_DS_LUA_STATE_LAYOUT`：`reserved[8] @0xB8`、`userdata @0xC0`，且有 `LJ_STATIC_ASSERT`），B8/C0 正是这两个字段 ⇒ **解释器当时是在 `lua_State` 结构体内的“帧”上运行**；`func = BASE-16` ⇒ 上一层调用也是从结构体内的 base 推出来的（`RA = BASE + A*8 + 16`）。
- 结论（与“崩溃点随布局漂移”“Linux 全绿/Windows 崩”一致）：**错误路径把 `L->base`/`L->top` 留在/写成了 `lua_State` 内部（离栈）地址**，随后凡是**假定 base 必在 Lua 栈上**的消费者就出野指针：JIT 快照（`savestack`/`restorestack` 偏移 → `lj_snap_restore` 写野值）、帧遍历（`curr_proto` → `lj_debug_slotname` → `debug_framepc`）、字节码处理器的 `[BASE+RA*8]` 等。崩溃点因此会随构建/布局换位置（本日先后见 `lj_err_optype`、`lj_BC_TSETV+0x67`、`lj_func_newL_gc`、`->assert_bad_for_arg_type`）。

下一步（修复方向）：
1. 定位**离栈 base 的制造点**：对 `L->base` 的写者做**条件化**记录（同一套全局字技巧，但只在“新值 ∉ `[stack, stack+16*size)`”时记录；cdb 无命令内条件，需在写入后立刻判断——可行做法是把判断搬到 C 侧：在 `lj_err.c`/`lj_api.c`/`lj_meta.c`/`lj_state.c` 的 `L->base =` 站点加“越界即打印”的 EXP 检查，**在 ASan 构建上重跑**，此前同样的检查在非 ASan 构建上 0 命中）。
2. 修复：让 dummy-frame/错误路径之后 `L->base`/`L->top` 必然回到栈上（或让快照捕获不到离栈 base）；涉及 `LJ_DS_EXECERROR` / `LJ_DS_PCALL_ERRSTATUS`（`lj_api.c`）、`lj_debug.c` 的 traceback 补丁与 `lj_err_optype_call` 的 stock dummy-frame hack 的交互。
- 同日的另一族（同一 test 的不同 run）：`lj_BC_TSETV+0x67` = `mov RB,[BASE+RA*8]`，而 WIN 上 RA=rcx 是“指针样”值（DISPATCH 区）而非寄存器序号；同 test 的 617 次 `L->base` 写断点 run 显示**全部在栈内、0 越界** ⇒ 两族是不同的症状面。注意 WIN 专有别名 `RA == CARG1`（源码注释 "Caveat: CARG1 is RA"），以及 arena 独有的 `barrierback` 调用（`lea CARG1,[DISPATCH+GG_DISP2G]` → `lj_gc_barrierback_arena`）是 classic 变体不会走的路径 ⇒ 下一步核对该 callee 的签名/传参约定。
- 追记 5 补充（同日）：给**全树**所有 C 侧 `->base =` 站点（lj_err 6、lj_state 3、lj_meta 1、lj_snap 2、lj_record 3、lj_api 1、lj_ccallback/crecord/gc/…）都加了“越界即打印(file:line)+int3”，在 ASan 构建上重跑：**一次都没命中** ⇒ 离栈 base **不是 C 侧写入**造成的（下一步转汇编侧/快照侧）。
- 传播者两次确证：写 `L->base` 的指令都在 `lj_snap_restore`（RVA 0x149F55 / 0x148B35；指令形如 `mov rcx,[rsp+…]; mov [rcx],rax; jmp …`），写入值分别是 `0x3fd02a4073`、`0x44a029c073`（均为野值）。
- 关键观察：同一 test、同一构建，**写断点的命令开销决定复现族**——较慢的 `.printf` 记录 3/3 次只见 617 次“全在栈内”的写；较快的 `eq` 记录则抓到野值写。（配合 p20 的 ARM 打印：挂断点时 base/top 正常。）⇒ 抓“首个离栈写”需要低开销记录（`eq` 式）或延后挂断点（ignore-count）。
- 追记 5 更正（同日）：用 PDB 实测该构建的真实偏移（`base=0x10`）后重做写断点，发现**此前的“写者”实测监视的是 `L->top`（0x18）而非 `L->base`（0x10）**——即 `lj_snap_restore` 确证的是**它写 `L->top`**（写入值 `0x3fd02a4073`/`0x44a029c073` 野值）；`L->base` 的写者**仍未捕获**。
- 追加的轻量检查（只插在调用入口 `->vmeta_call`（RA/BASE）与 `->vm_call_disatch`（RA），约 40 条指令）：ASan 构建上跑 test_gc_invariants **未命中**（该 run 走 TSETV 族，opcode 字节实测 `0x3C`=BC_TSETV）。
- 两族现状：① “结构体内 base/top”族（`base = L+0xA8`、`top = L+0xC0`、调用点 `BASE = L+0xB8`，C 侧打印实测，native 速度 3/3 复现）；② “TSETV 中 RA=指针值”族（cdb/慢速下复现，opcode 0x3C 确认）。**族随调试器/插桩开销切换** ⇒ 任何带每命中陷阱的写断点都会把 ① 族挤掉；①族的写者需要近原生速度下的判据（C 侧 26 个 `->base =` 站点全部未命中 ⇒ 只剩汇编侧，但 44 站点插桩会杀掉复现）。
### 9. 追记 6（同日）：进程内写断点记录器定位到“首个离栈 base”与 `BC_CALLT`

工具（新增，近原生速度、可条件化；已存入工作区）：
- `src/lj_dbg_rec.c`（EXP-ONLY）：`DR0 = &L->base`（**只监视写、8 字节**）+ `AddVectoredExceptionHandler`；命中时在**进程内**判断新值是否在 `[L->stack, +16*stacksize)`，只有**越界**才打印（值/写者RIP/bytecode PC+opcode/RA/BASE/top/stack/size）并**自动关闭**该断点；经 `ljamalg.c` 末尾 include + `stack_init` 末调用 `lj_dbg_rec_install(L1)` 装配。实测 3/3 输出一致（未像 cdb 那样切换症状族）。

结果（ASan 构建，三个测试一致）：
```
EXP-VEH-BASE-BAD val=L+0xA8 rip=lj_vmeta_call+0x…(RVA 0x3926)
   pc=<bytecode> op=0x43  RA=L+0xB8  BASE=L+0xA8  top=…  stack=…  size=166
```
- `op=0x43` = **`BC_CALLT`**；写者指令 = `mov [r10+0x10],rdx`（= `->vmeta_call` 序言的 `mov L:TMPR->base, BASE`，`r10`=SAVE_L）⇒ `->vmeta_call` 是**传播者**；`BASE` 寄存器**已是** `L+0xA8` ⇒ 制造者在其之前的 **`BASE` 寄存器运算**（候选：`sub BASE,PC`（vararg 重定位）、`sub BASE,RB`（vm_call_tail）、`mov BASE,RB`、ffunc 路径的 `add/sub BASE,imm`），由**过期/垃圾 PC 或 RB（帧链）**驱动。
- 二级验证（把断点重挂到“最后一次合法 base 的 `[base-8]` 帧链槽”）：窗口内**无写入** ⇒ 该帧链槽完好，被破坏的链在别处。
- 负结果：arena 变体的 `lj_mem_realloc`（`lj_gc_arena.c:3702`）走 `g->allocf`（系统分配器）⇒ **Lua 栈不由 arena 管理**，GC 不会经自身分配器改写栈。
- 注意（勿重试）：把 DR1 做成“每次写都 `SetThreadContext` 重挂”的版本会在 VEH 内livELock（实测 387s、无输出）；只在**首次命中**时重挂即可（工作区已恢复该版本）。
- 本轮工作区最终态：`lj_dbg_rec.c`（单 DR0 + 首命中重挂到 `[prev_base-8]`）已存 `patched/lj_dbg_rec.c`；ASan 构建上 1.1s 内给出同一现场（`op=0x43/BC_CALLT`、`val=L+0xA8`、`RA=L+0xB8`、`BASE=L+0xA8`、`prev=stack+0x150`）。
### 10. 追记 7（同日）：记录器升级、两条“假阳性”教训与三个负结果

- 记录器升级（工作区 `lj_dbg_rec.c`，已存 `patched/`）：DR0=`&L->base`（写）+ DR1=**帧链槽 `stack+0x148`**（= 三次运行一致的 `prev-stack = 0x150` 减 8，均为 `stack+0x148`），并在 `resizestack` 后 `lj_dbg_rec_rebase()` 重挂；处理器里加了 `pc==0` 过滤。
- **两条假阳性教训（勿误判）**：① 栈搬迁时**旧缓冲被释放**，CRT/调试堆随后改写其内容，会命中仍指向旧缓冲的 DR1（`ri`/pc 非解释器上下文）；② `resizestack`/栈初始化的 **nil 填充**把 `0xFFFFFFFFFFFFFFFF`（= GC64 的 `LJ_TNIL`）写进栈槽。二者都不是缺陷。过滤办法：只在解释器上下文（PC≠0 且 RA/BASE 合理）下判定，并在栈搬迁前先解除/搬迁后重挂。
- 负结果 A：DS 执行错误入口在 standalone 下**完全惰性**——`lua_setexecutionerror` 在 `extern_error_message_buffer`/`extern_had_execution_error` 为 NULL 时直接 return（`lj_api.c`），`execerror_armed()` 亦为假 ⇒ 不会经引擎槽写内存。
- 负结果 B：**全 fork 无人使用** `LJ_DST_LUA_STATE_RESERVED/USERDATA` 或 `&L->reserved`/`userdata` 作帧（grep 仅见布局定义与 `LJ_STATIC_ASSERT`）⇒ 观测到的 `BASE=L+0xA8`、`RA=L+0xB8`（差值恰为 16=sizeof(TValue)）**不是”刻意用引擎槽当帧“**，而是 `L` 相对值的巧合 ⇒ 靶子应转向 **`BASE` 寄存器运算**（CALLT/vararg/restore 路径中由过期 PC/RB 驱动的 `mov BASE,RB` / `sub BASE,PC` / `sub BASE,RB` / `lea BASE,[…]`）。
- 负结果 C：arena 变体的 `lj_mem_realloc` 走 `g->allocf`（系统分配器）⇒ Lua 栈不由 arena 管理（追记 6 已记）。
- 追记 7 续：把“进入 vmeta 的那条指令及其前两条”都打出来后（记录器加 `prevop/prev2`，纯 C 改动、不切族，3/3 一致）：
  `op=0x43 prevop=0x42 prev2=0x3D`。以**已验证锚点 `BC_TSETV=0x3C`** 校准编号（fork 的 opcode 号整体前移）后：`0x3D = BC_TSETS`、`0x42/0x43 = BC_CALL / BC_CALLM`（二者在 dasc 中**同一个 case**：`case BC_CALL: case BC_CALLM:`，`checkfunc` 失败走 **`->vmeta_call_ra`**）。
  ⇒ 序列 = `TSETS`（写表）→ 一次调用（0x42）→ 紧接着又一次调用（0x43，其 callee 非函数）⇒ **是前一次调用的“返回”把 `BASE` 交回成野值**，随后的调用经 `->vmeta_call_ra` 把它落进 `L->base`。
  ⇒ **下一个靶子：返回路径**（`->fff_res`/`vm_returnp`/`BC_RET*`/`cont_*` 的 base 重建），以及 `vmeta_call_ra` 序言前的 `checkfunc` 分支为何在 `BASE` 已离栈时继续使用它。
- 本轮工具状态：`lj_dbg_rec.c`（DR0=&L->base、首命中重挂 `[prev-8]`、`prevop/prev2` 打印、`pc==0` 过滤、栈搬迁重挂）已存 `patched/lj_dbg_rec.c`；汇编侧任何加码（哪怕 5 条指令）都会切换症状族，**只能走 C/进程内**路线。
### 11. 追记 8（同日）：离栈 base 是“寄存器层面”偏离——历史环与帧入口惯用式

- 记录器再加**最近 4 次 `L->base` 写的历史环**（值/写者 RIP/opcode，纯 C 改动、族不变）：
```
EXP-VEH-BASE-BAD val=L+0xA8 RA=L+0xB8 BASE=L+0xA8 op=0x43 prevop=0x42 prev2=0x3D
EXP-VEH-HIST[0] val=stack+0x150 rip=…(RVA 0x1DC9) op=0x35 prevop=0x34
EXP-VEH-HIST[1] val=stack+0x150 rip=…(RVA 0x1D38) op=0x34 prevop=0x36
EXP-VEH-HIST[2] val=stack+0x130 rip=…(RVA 0x30B0) op=0x66 prevop=0xff
EXP-VEH-HIST[3] val=stack+0x0B0 rip=…(RVA 0x1CE7) op=0x33 prevop=0x42
```
- dumpbin 定位：这些 RIP 分别落在 `lj_BC_TDUP`、`lj_BC_TNEW`、`lj_BC_FUNCC`、`lj_BC_FNEW`、`lj_vmeta_call`（RVA 0x3926）——**全部是同一条“帧入口”惯用式**：
  `mov rbp,[rsp+58h]`（rbp=SAVE_L）→ `mov [rbp+10h],rdx`（**L->base = 该帧的 base**）→ 之后才 `lea` 计算下一帧。
  即：**LuaJIT 里所有 `L->base` 写都发生在“进入新帧”时，写入值 = 该帧 base（由调用方用 `BASE+RA*8+16` 算出的栈内地址）**。
- ⇒ 结论：**族①的 `L+0xA8` 不是被任何 `L->base` 写“写进去”的**（历史环全在栈内），而是**`BASE` 寄存器在两次帧入口之间“自己”偏离**——即污染发生在**寄存器/VM 保存区**层面（`SAVE_*`/`CSAVE_*` 槽或某条 `mov/lea BASE,…`），因此 `->vmeta_call_ra` 只是把它落进 `L->base`。
- 下一步（唯一可行方向，纯 in-process）：在记录器命中处的**上下文**里对 VM 保存区（C 栈上的 `SAVE_*` 槽）做**影子比对**（记录进入解释器时的保存区快照，命中时 diff），或对 `BASE` 的“帧入口值”做链式校验（每次帧入口把 base 存入一个影子数组，偏离时定位相邻两次）。
### 12. 追记 9（同日）：单步跟踪方案被否；三条工具经验

- 尝试：进程内 TF 全程单步，逐步检查 `BASE`(=rdx)/`L->base` 是否离栈，只在“转变的那一步”打印（含上一条指令地址与 opcode）。
  - 第一次实现**无效**（跑得与平时一样快）：`SetThreadContext` 时 `ContextFlags` 只写了 `CONTEXT_DEBUG_REGISTERS`，`EFlags` 不生效 ⇒ 必须 `| CONTEXT_CONTROL`。
  - 生效后立刻抓到一次“转变”，但在 **C 代码上下文**（`rbp != L`、PC 非 bytecode、rdx 与 rsi 同值）⇒ **假阳性** ⇒ 需加 `rbp == L`（解释器里 L 常驻 RB，C 代码里 rbp 是帧指针）判据。
  - 随后处理器在**非映像地址**上读 opcode 自崩 ⇒ 加“代码地址必须落在本 DLL 映像窗口内”（`VirtualQuery(&fn)->AllocationBase`，+32MB 窗口）护栏。
  - **负结果**：TF 全程单步=剧烈扰动，**复现被“治好”**（`exit=1`，两次运行 rc=0，不再崩溃）⇒ 该方案不可行。
- 下一步变体（未做，明确规格）：**DR 执行断点 + 有界 TF 窗口**——记录器在首次离栈命中时已知“最后一次帧入口”的 RIP（`EXP-VEH-HIST[0]`），把 DR0 改成 exec-only 挂在该地址；命中后只单步 N≈2000 步并检查 `BASE`，窗口正好覆盖“帧入口→下一次偏离”，扰动远小于全程单步。
- 工具经验（固化于 `patched/lj_dbg_rec.c`）：VEH 内判定解释器上下文必须**同时**满足 `Rip ∈ 映像窗口` 且 `rbp == L`，否则读 PC/opcode 会误报或自崩。
### 13. 追记 10（同日）：根因结论（寄存器/帧状态被 `lua_State` 相对值污染）

> **已被追记 14 取代**：真正根因是 x64 `barrierback` 宏把保存寄存器 push 进 C 被调方的 Win64 shadow space（见文末 追记 14）。

**根因（证据充分、可复现）**：崩溃点处 **VM 寄存器本身持有 `lua_State` 相对值**，而不是 Lua 栈地址；帧/寄存器状态先被污染，之后所有症状都是传播结果。

决定性实测（无插桩的 ASan 构建 + 记录器构建，互相印证）：
- 族①（`->vmeta_call_ra` 序言写 `L->base` 的那一次）：
  `BASE = L+0xA8`、`RA = L+0xB8`、`RB = L+0xB8`（= `&L->reserved`）、`KBASE = L+0xA8`；`L->base` 写历史 4/4 全在栈内 ⇒ **不是被任何 `L->base` 写写进去的**，而是寄存器在这一段已经离栈。
  原始字节码字（唯一无歧义）：`w0=00000043(A=0) w1=00030242(A=2) w2=0503063d(=TSETS,A=6)` ⇒ 序列 `TSETS → call(A=2) → call(A=0，callee 落在 [L+0xA8]=0)`。
- 族②（`lj_BC_TSETV+0x67`，`mov rbp,[rdx+rcx*8]`）：`rcx`(RA)=`rbp`(RB)=`0x23d8170d440`（`lua_State` 邻域指针）、`rdx`(BASE) 在 *另一个* 堆区 ⇒ 同样是“寄存器被状态相对值污染”。
- 换算校验：`L+0xA8 = (&L->reserved − 16)`、`L+0xB8 = &L->reserved`、`L+0xC0 = &L->userdata`，与 `LJ_DS_LUA_STATE_LAYOUT`（reserved@0xB8 / userdata@0xC0，`LJ_STATIC_ASSERT` 锁定）**逐一对齐** ⇒ 污染值落在 **DS/引擎对齐区**。

已排除（均实测）：C 侧 `L->base` 写（26 站点，0 命中）；读已释放 cell（MSVC ASan poison 生效、无 use-after-poison）；arena 管栈（`lj_gc_arena.c:3702` 走 `g->allocf`）；DS 执行错误槽（standalone 下 NULL⇒惰性）；“刻意用引擎槽当帧”（全 fork 无 `LJ_DST_*` 使用者）。
方法学硬约束（实测）：汇编侧任何加码（5 条指令）或 TF 单步都会**切换症状族/治好复现** ⇒ 只能用纯 C/进程内判据。

修复落点（待实现的一步）：让错误/展开路径（`lj_err.c` 的 unwind、`lj_snap.c` 的 restore、`lj_err_optype_call` 的 dummy frame）在遇到离栈 base/帧时**先恢复一致帧**（而不是沿用），并让 frame-entry 的 base 只在栈内建立；同时把 `RA==CARG1`/`BASE==CARG2/CARG3` 的 X64WIN 别名区逐点核对“C 调用后是否重载”。
### 14. 追记 11（同日）：opcode 编号**订正**与据此收紧的靶子（逐条核对）

以 `lj_bc.h` 的 `BCDEF(_)` 列表为权威（`BCDEF(BCENUM)` 逐项枚举）：

| 编号 | 名称 | | 编号 | 名称 |
|---|---|---|---|---|
| 0x3C | **TSETV** | | 0x41 | **CALLM** |
| 0x3D | **TSETS** | | 0x42 | **CALL** |
| 0x3E | TSETB | | 0x43 | **CALLMT** |
| 0x3F | TSETM | | 0x44 | **CALLT** |

⇒ **订正此前记录**：族①里“正在执行”的那条指令（`op=0x43`）**不是 `BC_CALL`，而是 `BC_CALLMT`（变参尾调用）**；它按 dasc 结构 `ins_AD → add NARGS:RDd, MULTRES → 落入 BC_CALLT`，即**本特性 `LUA_COMPAT_TAILCALL_CFRAME` 修改的那段代码**。上下文（原始字节码字）为
`KNIL(A=6) → TSETS(A=6) → CALL(A=2) → CALLMT(A=0)`，失败点 `RD(=extra_nargs+MULTRES)=0`（本例无 MULTRES 垃圾）。

逐条核对结果：
1. **特性关闭仍复现**：`-DLUA_COMPAT_TAILCALL_CFRAME=0` 下三个测试全部 `exit=3`（= 0x80000003 被 bash 截断，即**命中 `int3` 见证**，如 `->assert_bad_for_arg_type`）⇒ 底层破坏与特性无关，特性只改变“爆点路线”（AV ↔ trap）。此前文档“特性关闭也 AV/同样复现”的描述据此精确化为“同样失败、表现为 trap”。
2. `BC_VARG` 的**栈增长路径已核对无误**：`call lj_state_growstack` 之后确实 `mov BASE, L:RB->base; mov RA, L:RB->top` 重载（mcode 见 dumpbin），不是缺陷点。
3. `BC_CALLMT/CALLT` 的 mcode 已核对（`lea rcx,[rdx+rcx*8+10h]`、`mov rdi,rdx`、`mov rbp,[rcx-10h]`、`checktp→lj_vmeta_call`；特性块 `lj_BC_CALLT_CFRAME`：`mov rdx,rcx`(=BASE=RA) + `mov [rdx-8],rsi` + tailcalls 记账）。
4. ⇒ 靶子收紧为：**变参尾调用链（`CALLMT→CALLT`）之前，寄存器/帧状态已被污染**（`BASE` 在 `CALLMT` 入口就已离栈；`RB`=L+0xB8、`RA`=L+0xB8、`KBASE`=L+0xA8 与 DS 布局锁定值逐一对应）——即污染源自**更早的寄存器写入**，而这里只是呈现点。
### 15. 追记 11 更正 + 追记 12（同日）：进程内逐项复核（布局 / 首个离栈写 / 写者符号 / SAVE_PC 字节码）

**追记 11 更正**：`run-asan.bat`/`run-nocframe.bat`/`run.bat` 原先把测试脚本写死，命令行参数被忽略 ⇒“特性关闭三测试 exit=3（trap）”的结论作废。修正转发后重测（Debug+ASan，`-DLUA_COMPAT_TAILCALL_CFRAME=0`）：
- `test_gc_invariants.lua` → AV `lj_BC_TSETV+0x66`
- `udata_finalize_assert.lua` → AV `lj_BC_TSETS+0x60`
- `white1_free_assert.lua` → AV `lj_BC_TSETV+0x66`
特性打开时同样是这三处随机落点（另见 `lj_BC_GGET+0xe`）⇒ **底层破坏与 `LUA_COMPAT_TAILCALL_CFRAME` 无关**（这次是真正跑的三个不同脚本）。

**工具修正（工作区 `src/lj_dbg_rec.c`，均为教训固化）**：
1. `lj_dbg_rec_rebase()` 改为**空操作**：`resizestack()` 对任意线程都会调用它，此前会把断点重挂到**短命协程**的状态上；协程释放后分配器写其内存 ⇒ 假阳性（实测写者 RIP 在 ASan 运行时里，且 `L->stack=L+0xC8`、`glref=0x8410` 全像回收堆数据）。
2. 只报告**写者 RIP 在 DLL 映像内**的离栈写（映像外的打一行 `EXP-VEH-BASE-BAD-EXT` 后跳过）。
3. 防多线程/继承调试寄存器：Dr0/Dr1 与预期地址不符时清该线程 DR7 后静默返回。
4. 只在 PC 落在映像窗口内才解码字节码（此前直接读 Rsi 会在 VEH 内自崩；ASan 曾报 `lj_dbg_rec_veh` 内 AV）——与追记 9 的教训一致。

**1）布局（进程内实测）**：`sizeof(lua_State)=200`；`base@0x10 top@0x18 maxstack@0x20 stack@0x28 cframe@0x48 stacksize@0x50 glref@0x8 reserved@0xB8 userdata@0xC0`；且 **`G(L)-L = 200 = 0xC8`**。⇒ `L+0xA8=reserved-16`、`L+0xB8=reserved`、`L+0xC0=userdata`、**`L+0xC8=G(L)`**，同一换算体系一次锁定。

**2）首个离栈 `L->base` 写（6+ 次运行、三测试）**：写入值**恰为 `G(L)`**（`dval=200`、`Graw=L+0xC8`、`dbase_field=200`），且状态**存活**（`stacksize=105`、`stack=L+0x8410`、`cframe` 普通 C 栈指针、`reserved0=0`、`userdata=0`、`status=0`）。写者（PDB 符号化）：
- `lj_vmeta_call+0x10` = RVA 0x3926 `mov [r10+10h],rdx`（dasc 1172 `mov L:TMPR->base, BASE`）
- `lj_vmeta_tsetv+0x15` = RVA 0x36B0 `mov [rcx+10h],rdx`（dasc 874 `mov L:CARG1->base, BASE`）
⇒ 都是按设计把 BASE 存进 `L->base` 的 caveat 存，**只是传播者**（逐指令复核）。

**3）事件现场（寄存器按 dasc 定义 BASE=rdx/RA=rcx/RB=rbp/PC=rsi/KBASE=rdi/RD=rax/DISPATCH=rbx/TMPR=r10）**：
`BASE=G(L)`、`RA=G(L)+0x28`、`RB=RA`、`RD=3`（2 参数调用）、`KBASE=合法 proto 常量表`、`DISPATCH=G(L)+GG_G2DISP`、`TMPR=L`、`SAVE_L=L`、`PC=0`。
- `RA = BASE + 3*8 + 16` 正与 `->vmeta_call_ra` 的 `lea RA,[BASE+RA*8+16]`（A=3）吻合 ⇒ 该 CALL(A=3) 的 callee 槽非函数，而 RA 由**已离栈的 BASE** 算出 ⇒ 进调用前 BASE 已坏（追记 8 结论再证，现有 A 值与 RA 增量对证）。

**4）SAVE_PC 处字节码（事件时现场转储；opcode 表取自 `lj_bc.h` 的 BCDEF，权威）**：
`… TDUP → FNEW(A=7) → TSETS(A=7,B=5,C=6) → CALL(A=3,B=3,C=1) → UCLO(A=0) …`
- 与历史环吻合：HIST[0]/[1] 写者 = `lj_BC_FNEW+0xc`、`lj_BC_TDUP+0x1f`（正是这两条指令的“帧入口”存），HIST[2]/[3] = `lj_BC_FUNCC+0x1e`、`lj_vmeta_tsetv+0x19`（同一惯用式，值全在栈内）⇒ **没有任何一步把 `G(L)` 写进 `L->base`**：坏值只存在于**寄存器**。
- 事件时 `PC=0`（rsi=0）而 `SAVE_PC` 合法 ⇒ 从 `TSETS` 的 vmeta 返回到 `CALL(A=3)` 之间 **PC 寄存器被清 0**（新线索：帧链接/PC 传递侧也参与了）。
- 崩溃点符号：`lj_BC_TSETV+0x66`、`lj_BC_TSETS+0x60`、`lj_BC_GGET+0xe`（GGET 用 KBASE/env 取值 ⇒ 与 KBASE 被毁一致）。

**5）下一步（工具已就绪）**：
- 把 DR0 写断点历史环扩到 32 条并逐条打印 SAVE_PC 处 opcode ⇒ 直接定位“最后一个正常 PC”与“PC=0”的转变点（同一工具在追记 12 里已能读 SAVE_PC/全 VM 寄存器）。
- 候选靶子（按证据排序）：`ins_call` 的帧链接存 `mov [BASE-8], PC` 与 `BC_RET/BC_CALLT_Z` 的 `mov PC,[BASE-8]`（PC=0 即链接为 0）；`mov BASE, RA`（CFRAME 路径）与 `mov BASE, RB`（尾部调用）这类寄存器搬运。
### 16. 追记 14（同日，**真正根因**）：`barrierback` 踩 Win64 影子空间 —— cdb 实测链条与修复

**结论（取代追记 10 的“寄存器被 `L` 相对值污染”）**：arena 变体在 **x64** 上新增的 `barrierback` 宏把要保存的寄存器 `push` 进了**被调 C 函数的 Win64 shadow space**，被 `lj_gc_barrierback_arena` 的序言覆盖；弹栈后 `RA`/`BASE`/`TMPR`/`ITYPE` 等 VM 寄存器变成垃圾，之后的 `mov RB,[BASE+RA*8]` 之类读野地址 ⇒ 崩在 `BC_TSETV/BC_TSETS/BC_GGET` 处理内。ASan 构建里“`L->base = G(L)`”（`dval=200`）也是同一根因：带插桩的被调方把入参全部 spill 到 shadow space，其中 `CARG1 = G(L)` 覆盖了宏保存的 `BASE`。

**cdb 实测链条（干净构建：`build/RelWithDebInfo`，无 ASan、无记录器；cdb 直接加载 exe）**：
1. `barrierback` 展开（`lj_BC_TSETV+0xaa..0xd3`）：`push rax/r10/r11/rcx/rdx` → `lea rcx,[rbx-0x1198]`（= `G(L)`）→ `mov rdx,rbp` → `call lj_gc_barrierback_arena` → `pop rdx/rcx/r11/r10/rax` → `jmp lj_BC_TSETV+0x67`。
2. 被调方序言第一条：`lj_gc_barrierback_arena: mov qword ptr [rsp+10h],rbx`。地址核对（R = 宏执行前 rsp）：push 槽 = RC@R-8、TMPR@R-0x10、ITYPE@R-0x18、**RA@R-0x20**、BASE@R-0x28；`call` 后 callee rsp = R-0x30 ⇒ callee 的 `[rsp+0x10]` = **R-0x20** ⇒ **正好覆盖“push RA”槽**。
3. AV 现场：`lj_BC_TSETV+0x67: mov rbp,[rdx+rcx*8]`，`rcx = 0x2350a8c15e0 = rbx`（DISPATCH，= 上一步写进 RA 槽的值）、`rdx = 0x2350a8c8150`；fault 地址 = `rdx + rcx*8` = `0x000013dd5eed3050`，与 cdb 报的 `ds:` 地址一致。
4. 该指令 = dasc `|2: mov RB,[BASE+RA*8]`，正是 `|7: barrierback …; jmp <2` 的**回跳目标** ⇒ 崩点就是 barrier 返回后的第一条指令。
5. 该指令处 `RA` 本该是**操作数索引**（≤255），实测却是指针值 ⇒ 唯一写 `rcx` 的就是宏里的 `lea CARG1,[rbx-0x1198]` 与其后的 `pop` ⇒ 唯一能把 `rbx` 弹进 `rcx` 的就是被叫方写 `[rsp+10h]` 的那条 `mov` ✓。
6. x86 变体的 `barrierback` 是**内联 SSB 实现、没有任何 push**（`or marked,GRAY; mov reg,[…ssbtop]; …; jb >9; call lj_gc_ssb_flush`），且 32 位 ABI 无 shadow space ⇒ **缺陷只存在 x64 宏**，与“Linux 全绿、Windows 崩”一致。

**修复（工作区与主仓库 `src/vm_x64.dasc` 逐字相同）**：先 `sub rsp, 0x50` 预留被调方 shadow space 与保存槽（保持 16 字节对齐），改用 `mov [rsp+0x20..0x40]` 保存/恢复 `RC/TMPR/ITYPE/RA/BASE`，再 `call`。这同时修掉原版 push-only 造成的 8 字节栈错位（call 时 rsp 未 16 对齐）。

**验证**：
- cdb（`build/RelWithDebInfo`）：`test_gc_invariants`、`white1_free_assert`、`udata_finalize_assert`、`c_tailcall_values`、`xpcall_tailcall`、`metamethod_name`、`tailcall_lines` 全部 **0 次 Access violation**（修复前三个 gc 测试 100% AV：`TSETV+0x67` / `TSETS+0x61`）。
- ASan（`build-asan/Debug`）：三测试 **0 sanitizer 报告、exit=0**（修复前 3/3 崩，且给出 `L->base = G(L)` 现场）。
- 主仓库补丁：宏代码与已验证版本逐字一致；用 `buildvm -m peobj` 装配主仓库 `vm_x64.dasc` 成功（产物 29,533 B）。

**gc 全量扫描（仓库 `builds/ninja-multi-vcpkg` 重建 `luajit-arenagc` 后）**：`luajit/test/gc/*.lua` 30 个 → 12 通过；其余 18 个与本次缺陷无关：
- 15 个脚本前提不满足：`cannot resolve symbol 'clock_gettime'`（deep_pause / inc_pause / inc_pause_bench）、`module 'memprof' not found`（memprof_* 共 13 个）——属环境/可选模块；
- 3 个 `openuv_vector_v31_assert` / `thread_openupval_sweep_assert` / `thread_permgray_residual_assert` 在 **`lj_cconv_ct_ct+0x35f`**（`movzx eax,[r14]`，`r14=0x100000000`）AV，**修复前/后构建各 3/3 次同样复现**（把 `barrierback` 临时换回 push 版重编做 A/B）⇒ **既有独立缺陷**，与 shadow space/`barrierback` 无关（用例自述 “FFI white-box (best-effort)”，自行按地址读内部结构）。

**对齐副产物（实测）**：原 push 版在 `lj_BC_TSETV` 的 barrier `call` 处实测 `rsp & 0xf == 8`（不符 Win64「call 时 16 字节对齐」）；本修复后为 `0`，顺带修掉这个既有 ABI 瑕疵。

**仓库内原生验证**：`cmake --build builds/ninja-multi-vcpkg --config RelWithDebInfo --target luajit-arenagc` 重编后 —— `tools/modinfo2cpp.lua`（`CMakeLists.txt:158` 记载“arenagc 会 AV，故 build-time 解释器改用默认变体”）**退出码 0**，且 `src/modinfo.hpp` 产物字节不变；`test/test_gc_invariants.lua`、`test/gc/{white1_free_assert,udata_finalize_assert,huge_swept_tag_assert}.lua` 全 0 ⇒ CMake 里那条 workaround 可复核/解除。

**工具链留档（本次因用户要求改用 cdb）**：`cdb2/cmds_av.txt`（加载 exe→`g`→AV 时 `kb/r/u/dq`）与 `cdb-run2.bat <script.lua>`（真正把脚本参数转发进去）；ASan/记录器路线保留在工作区，但其构建的 dasc 因 `LUA_USE_ASSERT` 与 release 略有差异（`+0x66` vs `+0x67`），**崩溃捕捉以 cdb + release 构建为准**。

# 尾调用层级 parity：C 调用帧被原位复用时的 5.1 `tail` 虚拟层（2026-10-07 修复）

状态：**已修复（2026-10-07，`fce64095`）** —— 缺口在读取侧：`lj_debug_frame()` 只在 PC 型帧槽（`frame_islua`）上读 5.1 尾计数；C 调入的 Lua 帧（主 chunk 等）其帧槽 ftuz 是 C 调用者 delta 标记，顶层尾调用原位复用该槽后计数被整段丢弃，每消一帧少一层。修复 = `lj_debug.c` 新增 `debug_isluaframe()`（`#if LUA_COMPAT_TAILCALL_COUNT` 内）。

## 差异（Lua 5.1 vs 本 VM）

Lua 5.1 `OP_TAILCALL`（PCRLUA 路径）把被调者帧放进调用者帧的位置并 `oci->tailcalls++`（`src/lua51/src/lvm.c:611-630`）；`lua_getstack`/`db_errorfb` 因此对每个被消帧各报一个虚拟层：`what="tail"`、`func=nil`、`source="=(tail call)"`、`namewhat=""`。

本 VM 用 `L->tailcalls` 平行数组 + `LUA_COMPAT_TAILCALL_COUNT` 复刻该语义：

- **写入侧（本来就正确，无需改动）**：`vm_x64.dasc` `BC_CALLT` 对 vararg 帧先把 `BASE` 重定位回真实帧槽再写 `(BASE-1)`，普通帧走 `>8` 旁路写当前帧槽；JIT recorder（`lj_record.c` 的 `((BASE-1)-stack)` 内联）与 `lj_state.c` 的扩容/清零一致。
- **缺口（读取侧）**：**由 C 调入**的 Lua 函数（主 chunk / `kleiloadlua` 载入的 chunk / 任何 C→Lua 调用）其帧槽 ftuz 是 C 调用者 delta 标记（`FRAME_C`/`FRAME_CP`/`FRAME_PCALL`）而不是 PC。顶层 `return f(...)` 原位复用该槽后，计数确实写在 `(BASE-1)`（即该槽），但 `lj_debug_frame()` 只在 `frame_islua(frame)` 时读它 ⇒ **该链上每消一帧少一层**，其后所有**绝对层号**整体前移。

影响面：DST 受保护 mod 的外壳按绝对层号走查自己的加载器帧（`i = debug.getinfo(level); if i.func == load then …`，见 `builds/hang_repro/shell_analysis/host_program_blocks.txt`）⇒ 走查命中错帧 ⇒ 走 `useGlobalEnv → setfenv → shouldExecute` 分支 ⇒ 静默返回空桩（无报错、mod 不加载）。

## 复现

```
src/luajit test/compat51/mainchunk_tailcall.lua    # 修复后 PASS；修复前出现 FAIL + 非 0 退出
lua5.1     test/compat51/mainchunk_tailcall.lua    # PASS（参照形状）
```

Win 离线对拍（任意 Lua 5.1 DLL，无需游戏）：

```
python builds/hang_repro/puc_host.py <lua51.dll> test/compat51/mainchunk_tailcall.lua
python builds/hang_repro/puc_host.py <lua51.dll> builds/hang_repro/fp_tail7.lua <A1|A2|A3|A4>
```

`fp_tail7.lua` 变体：A1 顶层 `return mid(...)` / A2 `loadstring` 后调用 / A3 普通调用对照 / A4 顶层尾调用再尾调用。

## 修复

`lj_debug.c`（`#if LUA_COMPAT_TAILCALL_COUNT` 内）：

```c
static int debug_isluaframe(cTValue *frame)
{
  if (frame_islua(frame)) return 1;
  if (frame_isc(frame) || frame_ispcall(frame)) {   /* C 家族 delta 标记 */
    GCfunc *fn = frame_func(frame);
    return fn->c.gct == ~LJ_TFUNC && isluafunc(fn); /* 槽里是 Lua 函数 ⇒ 原位复用的帧 */
  }
  return 0;
}
```

`lj_debug_frame()` 的 tail 计数读取与 `lj_debug_funcname()` 的"被消帧无名"判断（对齐 5.1 `getfuncname` 的 `CIST_TAIL`）都改用它。varg 伪帧、continuation 帧、dummy（线程）帧被该判据显式排除（函数槽的 gct/ffid 检查）。

## 验证（离线对拍 PUC + 引擎 A/B）

- `test/compat51/mainchunk_tailcall.lua`：PUC ✓ / 修复后 fork ✓（`luajit.exe` 与 `lua51DS.dll` 两条宿主）。
- `builds/hang_repro/fp_tail7.lua` A1/A2/A3/A4 与 PUC **逐项全等**（同宿主下 `end/tails` = 4/2、4/2、4/1、5/3；A3 对照不变）。
- `ctest -C Debug -R "harness_unit|game_mod_throw_abort|luajit_parity"` **7/7**（外层 `tests/CMakeLists.txt` 登记 `luajit_parity_mainchunk_tailcall`，跑 `tests/lua_vm_parity/mainchunk_tailcall.lua` 副本）。
- 引擎（DST 专用服，history：见 `docs/mac-lua51-parity-audit.md` §5.2）：`hide` + `DS_NOJITWINDOW=1` 下受保护 mod 全链 `modmain_ → config → main` ✓（修复前仅 `modmain_.lua`，静默空桩）；日志 `builds/hang_repro/fix_hide_nofix.log`。

## 发布侧注意（实测，未改插件）

受保护壳按 `jit` 可见性选择帧策略：`jit` 隐藏 ⇒ 引擎(5.1)层号；`jit` 可见 ⇒ 上游 LuaJIT（无 `tail` 层）层号。故本 parity VM 只在 **jit 隐藏**时与壳匹配；`jit` 可见的兼容配置（窗口化暴露 / `HideGlobalJIT=false`）与该 VM **互斥**（四臂实测：pre-fix 可见 ✓/隐藏 ✗；parity 隐藏 ✓/可见 ✗）。发布时需保持 `jit` 对受保护 mod 隐藏——`Mod/plugins/jit_runtime.lua` 的窗口化修复因此只适合 pre-parity VM。

## 边界

- `LUA_COMPAT_TAILCALL_COUNT` 仍只在 x64（`vm_x64.dasc` 是唯一实现写入侧的后端）默认开启；x86、x64 非 GC64、arm 等保持 stock。
- 计数上限 `LJ_TAILCALL_COUNT_MAX`（1e6）与数组扩容/清零语义未变；栈重分配时 `lj_state.c` 会迁移/清零新数组。

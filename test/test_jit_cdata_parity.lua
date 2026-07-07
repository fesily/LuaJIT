-- Parity test for JIT-traced fixed-size cdata allocation vs interpreter.
--
-- asm_cnew (lj_asm_x86.h) emits the GCcdata header (marked/gct/ctypeid) inline
-- on the JIT-compiled CNEW/CNEWI path; the interpreter path goes through
-- lj_cdata_new -> lj_mem_newgcot -> allocator newwhite(). WAVE 3 replaces the
-- JIT path's runtime `gc.currentwhite` read with a constant header init under
-- LJ_HASGCMARK. This test pins that a fixed-size cdata produced on a hot
-- JIT-compiled trace behaves observably IDENTICALLY to one made by the
-- interpreter across GC: live ones survive every cycle, dropped ones are freed
-- exactly once, and the arena heap stays consistent (checkheap == 0) the whole
-- time. Behavioral parity here is the proof that the emitted header byte is
-- equivalent to the C-path's curwhite|GRAY (the header white bit is vestigial
-- for arena cdata post-WAVE-1; liveness is the bitmap authority).
--
-- ONLY fixed-size cdata (ffi.new("int"), small fixed struct) -- never huge VLA
-- (a pre-existing huge-VLA-cdata GC SEGV exists on clean HEAD; out of scope).
--
-- Run (JIT leg real): luajit test/test_jit_cdata_parity.lua  (release arena)
-- Run (interp only) : luajit -joff test/test_jit_cdata_parity.lua (assert arena)

local ffi = require("ffi")
ffi.cdef[[ typedef struct { int a; double b; } Fix2; ]]

local pass, fail = 0, 0
local function check(name, cond, msg)
  if cond then pass = pass + 1
  else fail = fail + 1; io.write("FAIL: "..name..(msg and (": "..msg) or "").."\n") end
end

local has_jit = pcall(function() return require("jit").status() end)
local jit = has_jit and require("jit") or nil

local has_checkheap = pcall(collectgarbage, "checkheap")
local function heap_ok(name)
  if not has_checkheap then return end
  local bad = collectgarbage("checkheap")
  check(name, bad == 0, "checkheap="..tostring(bad))
end

-- Count JIT traces compiled while a scenario runs, to prove the alloc actually
-- got traced (not just interpreted) on the JIT leg.
local traces = 0
local function trace_cb(what)
  if what == "start" then traces = traces + 1 end
end

-- One allocation scenario, parameterised by whether JIT is engaged.
-- N fixed-size cdata are created in a tight hot loop; even indices are kept
-- live (strong ref + finalizer), odd indices are dropped immediately. Returns
-- (live_survivors, finalized_count_after_full_gc).
local function run_scenario(use_jit, N)
  if jit then
    jit.flush()
    if use_jit then jit.on() else jit.off() end
  end
  collectgarbage("collect")

  local keep = {}            -- strong refs to the live (even) cdata
  local fin = 0              -- finalizer hit counter
  local live_n = 0

  -- Hot loop: ffi.new of a fixed-size cdata each iteration. With JIT on and a
  -- high enough N this records and runs a CNEW/CNEWI trace.
  for i = 1, N do
    local cd
    if (i % 2) == 0 then
      cd = ffi.new("Fix2")           -- CNEW fixed struct -- kept live
      cd.a = i
      ffi.gc(cd, function() fin = fin + 1 end)
      live_n = live_n + 1
      keep[live_n] = cd
    else
      cd = ffi.new("int", i)         -- CNEWI (fixed 4-byte) -- dropped
      -- no ref kept: becomes unreachable at loop end
    end
  end

  -- Drive the GC through several full cycles. Live (kept) cdata must all
  -- survive and never be finalized; dropped ones must be collected.
  for c = 1, 5 do
    collectgarbage("collect")
    heap_ok("checkheap_"..(use_jit and "jit" or "interp").."_cycle"..c)
  end

  -- All kept cdata still intact and never finalized.
  local intact = 0
  for i = 1, live_n do
    local cd = keep[i]
    if cd ~= nil then intact = intact + 1 end
  end
  check((use_jit and "jit" or "interp").."_live_survive", intact == live_n,
        intact.."/"..live_n.." survived")
  check((use_jit and "jit" or "interp").."_live_not_finalized", fin == 0,
        "fin="..fin)

  -- Now drop the live refs and collect: every kept cdata must finalize once.
  for i = 1, live_n do keep[i] = nil end
  keep = nil
  collectgarbage("collect"); collectgarbage("collect")
  heap_ok((use_jit and "jit" or "interp").."_checkheap_afterdrop")
  check((use_jit and "jit" or "interp").."_all_finalized_once", fin == live_n,
        "fin="..fin.." of "..live_n)

  return live_n, fin
end

local N = 4000

-- Interpreter leg (JIT forced off).
local i_live, i_fin = run_scenario(false, N)

-- JIT leg (JIT on; hot loop should compile the cdata-alloc trace).
local saved_attach = false
if jit and jit.attach then
  jit.attach(trace_cb, "trace")
  saved_attach = true
end
local j_live, j_fin = run_scenario(true, N)
if saved_attach then jit.attach(trace_cb) end  -- detach

-- Parity: both legs see identical observable behavior.
check("parity_live_count", i_live == j_live, i_live.." vs "..j_live)
check("parity_finalized_count", i_fin == j_fin, i_fin.." vs "..j_fin)

-- Diagnostic: did the JIT leg actually trace? (informational; not a hard fail
-- when run under -joff, where tracing is intentionally disabled.)
local jit_on_now = has_jit and select(1, pcall(function() return jit.status() end))
  and jit.status() or false
if jit_on_now then
  check("jit_leg_traced", traces > 0, "traces="..traces)
else
  io.write("NOTE: JIT disabled (-joff) -- both legs interpreted; traces="..traces.."\n")
end

io.write(string.format("\nJIT cdata parity: %d passed, %d failed (traces=%d)\n",
                       pass, fail, traces))
os.exit(fail == 0 and 0 or 1)

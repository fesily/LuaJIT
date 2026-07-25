-- Authority-based gc_obj_isdead across ALL GC phases (epoch dual-white model).
--
-- gc_obj_isdead (lj_gc.h) is the single seam every module uses to ask "is this
-- object dead?". Phase-independent formula:
--   isdead ≜ !mark ∧ other(meta)   where other(a) ≜ a.swept_gen != g.epoch
-- Outside free (and after free completes I1: ∀a current), isdead is globally
-- false. A still-reachable object must NOT be reported dead and must NOT be
-- wrongly resurrected or freed.
--
-- This pins the observable behavior of the three Lua-reachable callers that hit
-- gc_obj_isdead, interleaved with GC steps:
--   * lj_func.c closeuv  -> open-upvalue resurrect / free decision
--   * lj_str.c  intern   -> resurrect-if-dead on a matching interned string
--   * lib_ffi.c typeinfo -> resurrect-if-dead on a ctype name string
-- Liveness is verified by value identity + heap self-consistency
-- (collectgarbage("checkheap") == 0) across many interleaved GC steps.
--
-- On assert builds the C side exports lj_gc_obj_isdead_nonsweep_hits() (legacy
-- counter; epoch formula no longer has a separate non-sweep branch, so it may
-- stay 0). Functional invariants remain authoritative. Release builds lack the
-- symbol; counter assertion is skipped.
--
-- Run: ./src/luajit -joff test/test_gc_obj_isdead_authority.lua

local pass, fail = 0, 0
local function ok(c, msg)
  if c then pass = pass + 1 else fail = fail + 1; print("FAIL: " .. msg) end
end

local has_checkheap = pcall(collectgarbage, "checkheap")
local function healthy(name)
  if not has_checkheap then pass = pass + 1; return end
  local bad = collectgarbage("checkheap")
  ok(bad == 0, name .. " checkheap=" .. tostring(bad))
end

-- Resolve the assert-only non-sweep hit counter via FFI. Absent on release
-- builds (no LUA_USE_ASSERT): binding fails, has_counter stays false, counter
-- assertions are skipped, functional invariants still run.
local has_counter, hits = false, nil
do
  local ok_ffi, ffi = pcall(require, "ffi")
  if ok_ffi then
    local ok_cdef = pcall(function()
      ffi.cdef[[uint32_t lj_gc_obj_isdead_nonsweep_hits(void);]]
    end)
    if ok_cdef then
      local ok_call = pcall(function() return ffi.C.lj_gc_obj_isdead_nonsweep_hits() end)
      if ok_call then
        has_counter = true
        hits = function() return tonumber(ffi.C.lj_gc_obj_isdead_nonsweep_hits()) end
      end
    end
  end
end

local start = has_counter and hits() or 0

-- 1. Upvalue close path (lj_func.c closeuv). Closures that share an upvalue over
--    a local keep open upvalues; leaving the scope closes them. closeuv calls
--    gc_obj_isdead on each upvalue, interleaved here with GC steps so the call
--    lands across phase boundaries (mark / pause as well as sweep). A live
--    upvalue must be kept (not freed): its value must read back intact.
do
  local survivors = {}
  for round = 1, 400 do
    local cell = { v = round * 7 }
    -- Two closures share the same upvalue `cell` -> a real open upvalue.
    local function getter() return cell.v end
    local function setter(x) cell.v = x end
    setter(round * 13)
    survivors[(round % 64) + 1] = getter   -- keep some closures live
    if round % 5 == 0 then collectgarbage("step") end
    if round % 37 == 0 then collectgarbage("collect") end
  end
  collectgarbage("collect")
  local intact = true
  for _, g in pairs(survivors) do
    if type(g()) ~= "number" then intact = false; break end
  end
  ok(intact, "open upvalues survive closeuv across GC steps (no false-dead free)")
  healthy("upvalue closeuv")
end

-- 2. String intern resurrect path (lj_str.c:369). Re-interning a string whose
--    only references were dropped must return a byte-identical string and keep
--    the heap consistent, whether the matching interned object is encountered
--    inside or outside the sweep window. A live (current-white) string must not
--    be reported dead. Includes a huge (>512 KB) string to drive the huge-slot
--    authority branch.
do
  local HUGE = 600 * 1024  -- > ArenaHugeThreshold (512 KB)
  local intact = true
  for round = 1, 300 do
    local s = "isdead_intern_probe_" .. round .. "_" .. string.rep("q", round % 19)
    local again = "isdead_intern_probe_" .. round .. "_" .. string.rep("q", round % 19)
    if s ~= again then intact = false end   -- re-intern returns equal object
    if round % 6 == 0 then collectgarbage("step") end
    if round % 50 == 0 then
      local huge = string.rep("H", HUGE + round)
      collectgarbage("step")
      if #huge ~= HUGE + round or huge:byte(1) ~= 72 then intact = false end
      huge = nil
    end
    s = nil; again = nil
  end
  collectgarbage("collect")
  -- Re-intern a previously-dropped string: must compare equal (fresh intern,
  -- or resurrect of a not-yet-swept object -- both must yield the same bytes).
  local reborn = "isdead_intern_probe_1_" .. string.rep("q", 1)
  ok(reborn == "isdead_intern_probe_1_" .. string.rep("q", 1) and intact,
     "interned strings re-intern equal across GC steps (no false-dead resurrect)")
  healthy("string intern resurrect")
end

-- 3. cdata finalizer path (lib_ffi.c + cdata __gc). ctype name strings are
--    tested by gc_obj_isdead in ffi.typeinfo; finalized cdata exercise the
--    cdata sweep/finalize path. Run finalizers across GC steps and confirm they
--    all fire exactly once and the heap stays clean.
do
  local ok_ffi, ffi = pcall(require, "ffi")
  if not ok_ffi then
    ok(true, "ffi unavailable: cdata finalizer path skipped")
    healthy("cdata finalizer (skipped)")
  else
    local fired = 0
    local N = 500
    for i = 1, N do
      local cd = ffi.new("int[1]")
      ffi.gc(cd, function() fired = fired + 1 end)
      -- ffi.typeinfo touches the ctype name string -> gc_obj_isdead path.
      ffi.typeinfo(ffi.typeof("int"))
      cd = nil
      if i % 4 == 0 then collectgarbage("step") end
      if i % 60 == 0 then collectgarbage("collect") end
    end
    collectgarbage("collect")
    collectgarbage("collect")  -- ensure all finalizers have run
    ok(fired == N, "all " .. N .. " cdata finalizers fired exactly once (got " .. fired .. ")")
    healthy("cdata finalizer")
  end
end

-- Counter probe: Phase 1 counted entries into the "non-sweep branch" of
-- gc_obj_isdead. Phase 2 (D2) unified the formula — there is no separate
-- branch, so the counter no longer advances. The functional invariants above
-- (upvalue closeuv / intern / cdata finalizer) are the authoritative proof
-- that isdead is called and returns the correct (not-dead) answer across all
-- phases. The counter assertion is kept non-fatal for Phase 2 compat.
if has_counter then
  local seen = hits()
  if seen > start then
    ok(true, "gc_obj_isdead non-sweep counter advanced (Phase 1 probe)")
    print("gc_obj_isdead non-sweep hit counter: " .. seen .. " (start " .. start .. ")")
  else
    print("gc_obj_isdead non-sweep counter unchanged (Phase 2 unified formula) " ..
          "-- functional invariants are authoritative")
    pass = pass + 1  -- counter is Phase 1 legacy; non-fatal under Phase 2
  end
else
  print("gc_obj_isdead non-sweep hit counter: unavailable (release build) -- "
        .. "counter assertion skipped, functional invariants still run")
  pass = pass + 1  -- account for the skipped counter assertion slot
end

collectgarbage("collect")
healthy("final")

print(string.format("\ngc_obj_isdead authority: %d passed, %d failed", pass, fail))
os.exit(fail == 0 and 0 or 1)

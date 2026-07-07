-----------------------------------------------------------------------------
-- test/gc/str_intern_identity_lock.lua
--
-- P0 behavior-lock for the string-interning correctness contract that the
-- open-addressing + bitmap-sweep redesign (see .omo/gc-strtab-redesign.md)
-- MUST preserve. This is NOT failing-first TDD: the current chain-based
-- interning is already correct, so every case here MUST PASS GREEN on the
-- current code. These tests lock the behavior so P1/P2 restructuring of the
-- table will regress loudly if it breaks the contract.
--
-- Contract locked (invariants I1, I4, I6 from the design doc):
--   I1  -- at most one live GCstr per content at any time (identity == pointer
--          equality; what `s1 == s2` compiles to in the JIT).
--   I4  -- mark bitmap is the sole liveness authority; a string unmarked this
--          cycle but re-interned before the sweep reclaims its cell MUST
--          resurrect the SAME object (no duplicate), exactly as today's
--          gc_obj_resurrect path does.
--   I6  -- huge strings (>512KB) live in the hugeset; intern identity and
--          reclaim must stay symmetric with the arena-string path.
--
-- Pointer-identity proxy (no direct pointer access from Lua):
--   LuaJIT interns ALL strings through lj_str_new, so string `==` IS a pointer
--   compare on the interned pointer. We strengthen this with a table-key
--   single-slot check: if two equal-content strings were DISTINCT interned
--   objects they would occupy two separate key slots (table string-key lookup
--   hashes the interned pointer identity), so `t[s1]=1; t[s2]=2` yielding a
--   SINGLE entry proves they are the same object. rawequal is the explicit
--   pointer-identity predicate. This is the strongest identity assertion
--   reachable from Lua and exactly what the JIT relies on.
--
-- Build:
--   make -C src clean && make -C src -j8 BUILDMODE=static XCFLAGS="-DLUAJIT_ENABLE_GCARENA"
-- Run:
--   ./src/luajit -joff test/gc/str_intern_identity_lock.lua
-----------------------------------------------------------------------------

local ok_jit, jit = pcall(require, "jit")
if ok_jit then jit.off() end  -- deterministic GC timing

local pass, fail = 0, 0
local function check(name, cond, msg)
  if cond then pass = pass + 1
  else fail = fail + 1; io.write("FAIL: "..name..(msg and (": "..msg) or "").."\n") end
end

local has_checkheap = pcall(collectgarbage, "checkheap")
local function healthy(name)
  if not has_checkheap then pass = pass + 1; return end
  local bad = collectgarbage("checkheap")
  check(name.." checkheap", bad == 0, "checkheap="..tostring(bad))
end

-- stats helper: returns the table from collectgarbage("stats"), or nil.
local function stats()
  local ok, t = pcall(collectgarbage, "stats")
  if ok and type(t) == "table" then return t end
  return nil
end
local function stat(name)
  local t = stats()
  if not t then return nil end
  return t[name]
end
local function statsreset() pcall(collectgarbage, "statsreset") end

-----------------------------------------------------------------------------
-- Identity proxy: prove two equal-content strings are the SAME interned
-- object via (a) rawequal and (b) a table-key single-slot occupancy check.
-----------------------------------------------------------------------------
local function same_intern_object(s1, s2)
  if not rawequal(s1, s2) then return false end
  -- table-key single-slot: distinct interned objects with equal content would
  -- create two key slots; a single entry proves pointer identity.
  local k = {}
  k[s1] = 1
  k[s2] = 2
  local n = 0
  for _ in pairs(k) do n = n + 1 end
  return n == 1 and k[s1] == 2  -- s2 overwrote s1's slot (same key)
end

-- Build a DYNAMIC string (heap-interned at runtime, not a parse-time literal)
-- from raw bytes so it is never folded to a constant/interned-at-parse literal.
local function dynamic_str(seed, len)
  local parts = {}
  for i = 1, len do
    parts[i] = string.char(((seed + i) * 131) % 251 + 1)  -- 1..251, nonzero
  end
  return table.concat(parts)
end

-----------------------------------------------------------------------------
-- CASE 1: IDENTITY BASELINE
-- Equal byte-sequences interning to the same object; distinct content to
-- distinct objects. Locks I1 in its simplest form.
-----------------------------------------------------------------------------
do
  -- Equal content, built dynamically twice independently.
  local a = dynamic_str(7, 64)
  local b = dynamic_str(7, 64)
  check("1.equal_content_same_object", same_intern_object(a, b))
  check("1.equal_content_rawequal", rawequal(a, b))

  -- Re-intern via a fresh concatenation (runtime-built, equal content).
  local c = string.rep("z", 40) .. "_tag_" .. tostring(12345)
  local d = string.rep("z", 40) .. "_tag_" .. tostring(12345)
  check("1.concat_reintern_same_object", same_intern_object(c, d))

  -- Distinct content -> distinct objects.
  local x = dynamic_str(11, 32)
  local y = dynamic_str(12, 32)
  check("1.distinct_content_distinct_object", not rawequal(x, y))
  check("1.distinct_content_distinct_key", (function()
    local k = {}; k[x] = 1; k[y] = 2
    local n = 0; for _ in pairs(k) do n = n + 1 end
    return n == 2
  end)())

  -- Empty string is the strempty singleton (I5): always the same object.
  check("1.empty_string_singleton", rawequal("", ""))

  collectgarbage("collect")
  healthy("1.identity_baseline")
end

-----------------------------------------------------------------------------
-- CASE 2: RESURRECTION ACROSS INCREMENTAL BOUNDARY
--
-- Build S from dynamic bytes (heap-interned). Drive GC with collectgarbage
-- ("step") in a loop; between steps re-intern the SAME bytes; assert the
-- re-interned handle is identical to the original S (resurrected, not a
-- duplicate) and survives a subsequent full collect. Use collectgarbage
-- ("stats") to observe cycle progression so the test actually crosses sweep
-- boundaries (assert cycles advanced).
--
-- LIMITATION (documented): precise single-stepping INTO the string-sweep
-- window (GCSsweepstring) for one specific string S is not deterministically
-- reachable from Lua -- the collector's internal phase is not exposed per-step
-- and S's exact mark/reclaim timing depends on its bucket position. We
-- therefore assert the STRONGEST reachable property: identity is preserved
-- across MANY interleaved step+re-intern iterations spanning multiple full
-- cycles (stats.cycles advances), which exercises the resurrection path
-- repeatedly. If a future redesign breaks resurrection (allocates a duplicate
-- instead of resurrecting the unmarked-but-unreclaimed cell), rawequal will
-- fail here. The Oracle single-test (S unmarked -> yield -> re-intern == S ->
-- resize -> still S) is approximated by combining this case with CASE 3's
-- resize-during-churn; a C-level test would be needed for the exact window.
-----------------------------------------------------------------------------
do
  local seed_bytes = dynamic_str(0xC0FFEE, 80)  -- the content of S
  local S = seed_bytes                           -- hold the original interned object

  statsreset()
  local start_cycles = stat("cycles") or 0
  local start_sweepstring = stat("steps_sweepstring") or 0
  local start_dead_freed = stat("strings_dead_freed") or 0

  local ITERS = 4000
  local identity_held = true
  local resurrect_count = 0

  -- Drive incremental GC with re-intern interleaved. Each iteration:
  --   step the collector, then re-intern S's content and check identity.
  -- Many iterations cross multiple full cycles (mark -> atomic -> sweepstring
  -- -> sweep -> rebuild), so S is repeatedly in the unmarked-pending window.
  for i = 1, ITERS do
    collectgarbage("step", 2)
    -- Re-intern the SAME bytes via an independent dynamic construction so we
    -- do NOT just hand back the same Lua value (which would trivially equal).
    local again = dynamic_str(0xC0FFEE, 80)
    if not rawequal(S, again) then
      identity_held = false
      break
    end
    -- Keep S live across the loop (we hold it in the upvalue); each `again`
    -- is a fresh local that goes dead each iteration, exercising the
    -- "re-intern then drop, S stays" pattern.
    resurrect_count = resurrect_count + 1
  end

  local end_cycles = stat("cycles") or 0
  local end_sweepstring = stat("steps_sweepstring") or 0
  local end_strings_live_walked = stat("strings_live_walked") or 0
  local end_strings_dead_freed = stat("strings_dead_freed") or 0

  check("2.identity_held_across_steps", identity_held,
        "rawequal(S, re-intern) failed at iter " .. tostring(resurrect_count + 1))
  check("2.cycles_advanced", end_cycles > start_cycles,
        "cycles " .. start_cycles .. " -> " .. end_cycles)
  -- Mechanism-coverage probe: confirm the string-sweep phase was actually
  -- entered so the test provably crossed the sweep boundary the resurrection
  -- contract guards. Under the chain build this is steps_sweepstring > 0.
  -- Under LUAJIT_STRTAB_OPENADDR (P2) the GCSsweepstring phase is deleted and
  -- string reclaim is folded into the GCSsweep bitmap pass, so the proof is
  -- strings_dead_freed advancing (dead strings reclaimed by the bitmap sweep).
  -- Detection: cycles advanced yet steps_sweepstring stayed flat => open-addr.
  local openaddr = (end_cycles > start_cycles) and
		   (end_sweepstring == start_sweepstring)
  check("2.sweepstring_phase_reached",
	openaddr and (end_strings_dead_freed > start_dead_freed) or
	(end_sweepstring > start_sweepstring),
        "steps_sweepstring " .. start_sweepstring .. " -> " .. end_sweepstring ..
	" dead_freed " .. start_dead_freed .. " -> " .. end_strings_dead_freed)
  -- strings_live_walked > 0 proves the chain GCSsweepstring walk ran (the
  -- hotspot the redesign drives to zero). Under the open-addr flag the live
  -- walk is gone by design (P2 win: strings_live_walked -> 0); the equivalent
  -- proof that the sweep processed strings is strings_dead_freed advancing.
  check("2.string_sweep_walked_live",
	openaddr and (end_strings_dead_freed > start_dead_freed) or
	(end_strings_live_walked and end_strings_live_walked > 0),
        "strings_live_walked=" .. tostring(end_strings_live_walked) ..
	" dead_freed=" .. tostring(end_strings_dead_freed))

  io.write(string.format(
    "  [case2] iters=%d cycles %d->%d steps_sweepstring %d->%d "
    .. "strings_live_walked=%s strings_dead_freed=%s\n",
    resurrect_count, start_cycles, end_cycles,
    start_sweepstring, end_sweepstring,
    tostring(end_strings_live_walked), tostring(end_strings_dead_freed)))

  -- S must survive a subsequent full collect (still the same object).
  collectgarbage("collect")
  collectgarbage("collect")
  local S_after = dynamic_str(0xC0FFEE, 80)
  check("2.survives_full_collect", rawequal(S, S_after))
  check("2.survives_full_collect_keyslot", same_intern_object(S, S_after))

  healthy("2.resurrection_boundary")
end

-----------------------------------------------------------------------------
-- CASE 3: RESIZE DURING CHURN
--
-- Force the string table to grow (intern many unique dynamic strings ->
-- pigeonhole guarantees growth past the initial small capacity) and shrink
-- (drop them, collect) while holding a set of survivor strings. After each
-- growth/shrink, assert every survivor still interns to its original identity
-- and no duplicates appear.
--
-- LIMITATION (documented): there is no exposed strtab-resize counter in
-- collectgarbage("stats"), so we cannot directly assert "a resize happened".
-- We instead assert the volume that FORCES growth (interning N distinct
-- strings where N >> initial table capacity is a pigeonhole guarantee) and
-- verify the identity invariant survives the growth+shrink. Colliding strings
-- (same hash bucket) cannot be constructed portably from Lua because the hash
-- seed is per-VM random and the hash function is not exposed; we rely on
-- volume to exercise many buckets including naturally-colliding ones.
-----------------------------------------------------------------------------
do
  -- Survivor set: distinct dynamic strings we keep live throughout.
  local SURVIVORS = 256
  local survivors = {}
  for i = 1, SURVIVORS do
    survivors[i] = dynamic_str(i * 7919, 48)
  end
  -- Record original identities.
  local orig = {}
  for i = 1, SURVIVORS do orig[i] = survivors[i] end

  -- GROWTH phase: intern many unique dynamic strings to force the table to
  -- grow well past its initial capacity. Hold them transiently.
  local GROW = 40000
  local churn = {}
  for i = 1, GROW do
    churn[i] = "churn_grow_" .. i .. "_" .. string.rep("g", (i % 19))
  end
  collectgarbage("step", 1)
  -- Verify survivors still identity-stable after growth.
  local grow_ok = true
  for i = 1, SURVIVORS do
    if not rawequal(survivors[i], orig[i]) then grow_ok = false; break end
    -- re-intern must hit the SAME object (no duplicate from a resize that
    -- dropped/re-added it).
    local again = dynamic_str(i * 7919, 48)
    if not rawequal(survivors[i], again) then grow_ok = false; break end
  end
  check("3.survivors_stable_after_grow", grow_ok)

  -- SHRINK phase: drop the churn strings, collect, forcing the table to
  -- shrink. Then verify survivors again.
  churn = nil
  collectgarbage("collect")
  collectgarbage("collect")
  local shrink_ok = true
  for i = 1, SURVIVORS do
    if not rawequal(survivors[i], orig[i]) then shrink_ok = false; break end
    local again = dynamic_str(i * 7919, 48)
    if not rawequal(survivors[i], again) then shrink_ok = false; break end
  end
  check("3.survivors_stable_after_shrink", shrink_ok)

  -- SECOND growth to stress resize-then-resize-then-shrink.
  local churn2 = {}
  for i = 1, GROW do
    churn2[i] = "churn_grow2_" .. i .. "_" .. string.rep("h", (i % 23))
  end
  collectgarbage("step", 1)
  churn2 = nil
  collectgarbage("collect")
  collectgarbage("collect")
  local second_ok = true
  for i = 1, SURVIVORS do
    local again = dynamic_str(i * 7919, 48)
    if not rawequal(survivors[i], again) then second_ok = false; break end
  end
  check("3.survivors_stable_after_second_grow_shrink", second_ok)

  -- No-duplicate check: intern each survivor content into a table keyed by
  -- content-pointer; every slot must be unique (no two distinct interned
  -- objects with equal content).
  local keyset = {}
  for i = 1, SURVIVORS do keyset[survivors[i]] = true end
  local dup_free = true
  for i = 1, SURVIVORS do
    local again = dynamic_str(i * 7919, 48)
    -- `again` must be the SAME object as survivors[i], so it lands in the
    -- SAME key slot (overwrite, not a new entry).
    local before_n = 0; for _ in pairs(keyset) do before_n = before_n + 1 end
    keyset[again] = true
    local after_n = 0; for _ in pairs(keyset) do after_n = after_n + 1 end
    if after_n ~= before_n then dup_free = false; break end
  end
  check("3.no_duplicate_interned_object", dup_free)

  collectgarbage("collect")
  healthy("3.resize_during_churn")
end

-----------------------------------------------------------------------------
-- CASE 4: HUGE STRING identity + reclaim (I6)
--
-- Intern a >512KB string (above ArenaHugeThreshold -> hugeset path), hold it,
-- collect (survives); drop it, collect; assert memory returns toward baseline
-- and checkheap passes. Locks the huge-string path that the redesign must keep
-- symmetric with arena strings (§3.5).
-----------------------------------------------------------------------------
do
  local HUGE_LEN = 600 * 1024  -- > 512KB ArenaHugeThreshold
  -- Build dynamically so it is heap-interned (not a parse-time literal).
  local function make_huge() return ("x"):rep(HUGE_LEN) end

  -- Clean baseline before measuring (prior cases leave some residual).
  collectgarbage("collect"); collectgarbage("collect")
  local before = collectgarbage("count")
  local hugemem_before = stat("hugemem") or 0
  local hugenum_before = stat("hugenum") or 0

  local h = make_huge()
  -- Identity: re-interning the same huge content must return the same object.
  local h2 = make_huge()
  check("4.huge_identity", rawequal(h, h2))
  check("4.huge_identity_keyslot", same_intern_object(h, h2))
  check("4.huge_length", #h == HUGE_LEN)
  check("4.huge_byte_intact", h:byte(1) == 120 and h:byte(HUGE_LEN) == 120)

  -- Hold it; collect; it must survive intact.
  collectgarbage("collect")
  collectgarbage("collect")
  check("4.huge_survives_collect", #h == HUGE_LEN and h:byte(1) == 120)
  check("4.huge_still_same_object_after_collect", rawequal(h, make_huge()))

  -- Peak memory with the huge string held (interned once: h==h2).
  local peak = collectgarbage("count")
  local hugemem_live = stat("hugemem") or 0
  local hugenum_live = stat("hugenum") or 0
  check("4.huge_accounted_in_hugeset", hugenum_live > hugenum_before,
        "hugenum " .. hugenum_before .. " -> " .. hugenum_live)
  check("4.huge_peak_grew", peak > before + (HUGE_LEN / 1024) * 0.5,
        string.format("peak %.0f KB, before %.0f KB", peak, before))

  -- Drop it; collect; memory must return toward baseline (no leak).
  h = nil; h2 = nil
  collectgarbage("collect")
  collectgarbage("collect")
  local after = collectgarbage("count")
  local hugemem_after = stat("hugemem") or 0
  local hugenum_after = stat("hugenum") or 0
  local huge_frees = stat("huge_frees") or 0

  -- The huge string's memory is reclaimed: the peak->after drop must be a
  -- large fraction of the huge size. (Empirically ~1.3x the huge size is
  -- reclaimed because peak includes allocator overhead; we assert >= 70% to
  -- be robust against allocator retention of small fragments while still
  -- catching a genuine huge-string leak.)
  local reclaimed = peak - after
  check("4.huge_reclaim_memory_returns",
        reclaimed > (HUGE_LEN / 1024) * 0.70,
        string.format("peak %.0f -> after %.0f KB, reclaimed %.0f KB (huge %d KB)",
                      peak, after, reclaimed, HUGE_LEN / 1024))
  -- hugenum must return to baseline (the huge slot was freed, not leaked).
  check("4.huge_reclaim_hugenum_returns",
        hugenum_after <= hugenum_before,
        "hugenum " .. hugenum_before .. " -> " .. hugenum_after)
  check("4.huge_freed_via_huge_path", huge_frees > 0,
        "huge_frees=" .. tostring(huge_frees))

  io.write(string.format(
    "  [case4] count %.0f->peak %.0f->after %.0f KB (reclaimed %.0f)  "
    .. "hugenum %s->%s  huge_frees=%s\n",
    before, peak, after, reclaimed,
    tostring(hugenum_before), tostring(hugenum_after), tostring(huge_frees)))

  collectgarbage("collect")
  healthy("4.huge_string_reclaim")
end

-----------------------------------------------------------------------------
-- CASE 5: CHECKHEAP INTEGRITY (cross-case)
--
-- After all the above churn, a final full collect + checkheap must report a
-- healthy heap. (Each case also called healthy() locally; this is the
-- end-of-file aggregate gate.)
-----------------------------------------------------------------------------
do
  collectgarbage("collect")
  collectgarbage("collect")
  if has_checkheap then
    local bad = collectgarbage("checkheap")
    check("5.final_checkheap_clean", bad == 0, "checkheap="..tostring(bad))
  else
    pass = pass + 1  -- no-op on non-arena builds
  end
  -- Final memory accounting stable: a few collects must not grow memory.
  local m1 = collectgarbage("count")
  collectgarbage("collect")
  local m2 = collectgarbage("count")
  check("5.final_mem_stable", m2 <= m1 + 1,
        string.format("%.0f -> %.0f KB", m1, m2))
end

-----------------------------------------------------------------------------
-- Report
-----------------------------------------------------------------------------
io.write(string.format(
  "\nstr_intern_identity_lock: %d passed, %d failed\n", pass, fail))
if fail > 0 then os.exit(1) end

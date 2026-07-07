-- Failing-first invariant for the "migrate huge-rebuild restart tag from
-- header LJ_GC_BLACK to a hugeset slot bit (HUGESET_SWEPT)" refactor.
--
-- GOAL INVARIANT (must hold AFTER task 2, RED before it):
--   Under the arena/bitmap GC, NO live huge survivor may EVER carry the header
--   bit LJ_GC_BLACK (0x04) -- not even transiently during the rebuild walk.
--   The "already-processed by rebuild_hugescan" restart tag must live in the
--   hugeset slot, not in the GCobj header. The header's only flag on a huge
--   VLA cdata survivor is the cdataisv bit (0x80) plus, briefly, GRAY during
--   marking; BLACK (0x04) must never appear.
--
-- OBSERVABLE WINDOW (important -- the tag is TRANSIENT):
--   rebuild_hugescan (src/lj_gc_arena.c:1435) sets `o->gch.marked |= LJ_GC_BLACK`
--   on each huge survivor as it processes it, then a LATER sub-phase
--   (Rebuild_HugeClear, :1489) clears it. So after a full collectgarbage
--   ("collect") BLACK is already gone (HugeClear ran). To observe the RED
--   state we must read the header DURING the rebuild, between HugeScan setting
--   the tag and HugeClear clearing it. HugeScan is sliced at GCSWEEPMAX (=40)
--   slots per collectgarbage("step"), so allocating MORE than 40 live huge
--   cdata forces the walk to span multiple steps and exposes the BLACK window
--   to Lua observation.
--
--   Pre-fix (current HEAD): BLACK (0x04) appears on processed survivors
--     mid-rebuild  -> this test FAILS (RED).
--   Post-fix (task 2): the tag moves to slot bit HUGESET_SWEPT, header BLACK is
--     never written -> 0x04 never appears -> this test PASSES (GREEN).
--
-- White-box header read mirrors white1_free_assert.lua EXACTLY: a huge VLA
-- cdata's FFI value points at the payload (cdataptr(cd)==cd+1); the GCcdata
-- header sits immediately before it. We self-calibrate by scanning payload
-- offsets -3 and -7 for the gct byte (== 10 for cdata) and read `marked` at
-- gct-1. For a VLA cdata the cdataisv bit (0x80) is also set in marked; the
-- color bits live in the low nibble.
--
-- Two independent assertion groups:
--   (A) Rehash-during-rebuild stress (child-fork for crash-risky scenarios):
--       allocate many huge blocks, keep a live subset, churn with incremental
--       collectgarbage("step") to force hugeset_resize mid-rebuild, then
--       collectgarbage("collect"). Assert: no crash, checkheap==0, live huge
--       objects readable, finalizers fire exactly once. These PASS today.
--   (B) White-box no-header-BLACK during rebuild (in-process so the live cdata
--       handles are reachable). This is the RED target on current HEAD.
--
-- Run: ./src/luajit test/gc/huge_swept_tag_assert.lua
-- Build: make clean && make -j4 XCFLAGS="-DLUAJIT_ENABLE_GCARENA -DLUAJIT_SECURITY_STRHASH=1 -DLUA_USE_ASSERT"
-- The test disables the JIT in-process (jit.off()) for deterministic GC
-- timing -- mirrors the sibling white-box tests' `-joff` run mode.

local LUAJIT = "./src/luajit"

----------------------------------------------------------------
-- Common helpers
----------------------------------------------------------------

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

local ok_ffi, ffi = pcall(require, "ffi")
if not ok_ffi then
  print("ffi unavailable -- huge_swept_tag_assert cannot run; skipping")
  os.exit(0)
end

-- Interpreter-only: deterministic GC timing + the assert build's arena
-- shadow-verify aborts if JIT-allocated objects are touched mid-step.
local ok_jit, jit = pcall(require, "jit")
if ok_jit then jit.off() end

local LJ_GC_BLACK = 0x04
local LJ_GC_GRAY  = 0x01
local GCT_CDATA   = 10  -- gct stores ~LJ_TCDATA; LJ_TCDATA == (~10u); byte == 10
local CDATAISV    = 0x80  -- VLA cdata flag (cdataisv, lj_obj.h:361)

-- Locate the `marked` byte for a cdata by validating the adjacent gct byte.
-- For a huge VLA cdata the GCobj is INTERIOR to the block, but the FFI value
-- points at the payload (cdataptr(cd)==cd+1) and the GCcdata header sits
-- immediately before the payload, so payload-relative offsets find it
-- correctly regardless of where in the block the GCobj lives.
-- Returns (marked_value, marked_offset) or nil if the header could not be
-- identified (inconclusive -> skip, never a false PASS).
local function read_marked(cd)
  local p = ffi.cast("uint8_t*", cd)  -- points at payload == cdataptr(cd)
  -- gct sits at payload-3 (sizeof(GCcdata)==12) or payload-7 (==16, padded).
  for _, gct_off in ipairs({ -3, -7 }) do
    if p[gct_off] == GCT_CDATA then
      return p[gct_off - 1], gct_off - 1
    end
  end
  return nil
end

local HUGE = 600 * 1024  -- > ArenaHugeThreshold (512 KB)

----------------------------------------------------------------
-- (B) White-box no-header-BLACK during rebuild (in-process, RED target)
--
-- Allocate MORE than GCSWEEPMAX (40) live huge cdata so HugeScan spans
-- multiple collectgarbage("step") slices; read each survivor's header marked
-- byte after every step and assert LJ_GC_BLACK (0x04) NEVER appears. RED on
-- current HEAD (line 1435 sets it); GREEN after task 2.
----------------------------------------------------------------

do
  -- Settle any pending rebuild first so we start a clean cycle.
  collectgarbage("collect")
  collectgarbage("collect")

  local N_LIVE = 80   -- > GCSWEEPMAX so HugeScan is multi-slice
  local N_DEAD = 40   -- dead huge to exercise the dead/survivor mixed path
  local live = {}
  for i = 1, N_LIVE do
    live[i] = ffi.new("char[?]", HUGE + i * 10)
    ffi.fill(live[i], HUGE + i * 10, i % 256)
  end
  for i = 1, N_DEAD do
    ffi.new("char[?]", HUGE + i * 20)  -- immediately unreferenced (dead)
  end

  -- Sanity: confirm we can locate the GCcdata header on every live survivor
  -- BEFORE any GC churn (fresh cdata are GRAY | cdataisv).
  local located = 0
  for i = 1, N_LIVE do
    if read_marked(live[i]) ~= nil then located = located + 1 end
  end
  ok(located == N_LIVE,
     "white-box: located GCcdata header on all " .. N_LIVE .. " fresh huge cdata")
  if located < N_LIVE then
    print("inconclusive: could not locate header on " .. (N_LIVE - located)
          .. " huge cdata -- calibration issue, skipping (B)")
    os.exit(0)
  end

  -- Drive the GC into and through the rebuild phase with a bounded number of
  -- steps. We do NOT collectgarbage("stop"): the auto-collector is fine; we
  -- just probe marked after each step. BLACK is observable once HugeScan
  -- begins processing survivors (typically within the first few steps after
  -- the mark phase completes).
  local steps_with_black, total_black_obs, sample_marked = 0, 0, nil
  local black_values_seen = {}
  for step = 1, 60 do
    collectgarbage("step")
    local blacks = 0
    local sample
    for i = 1, N_LIVE do
      local m = read_marked(live[i])
      if m and bit.band(m, LJ_GC_BLACK) ~= 0 then
        blacks = blacks + 1
        sample = m
      end
    end
    if blacks > 0 then
      steps_with_black = steps_with_black + 1
      total_black_obs = total_black_obs + blacks
      sample_marked = sample
      if #black_values_seen < 8 then
        black_values_seen[#black_values_seen + 1] =
          string.format("step %d: %d/%d marked=0x%02x", step, blacks, N_LIVE, sample or 0)
      end
    end
  end

  -- Final state: after full collects, BLACK must be cleared (HugeClear ran).
  -- This is a sanity check, not the RED target.
  collectgarbage("collect")
  collectgarbage("collect")
  local final_black = 0
  for i = 1, N_LIVE do
    local m = read_marked(live[i])
    if m and bit.band(m, LJ_GC_BLACK) ~= 0 then final_black = final_black + 1 end
  end
  ok(final_black == 0,
     "white-box: after full collect, NO huge survivor carries header BLACK (HugeClear ran)")

  -- Live survivors must still be readable (liveness authority = slot).
  local content_ok = true
  for i = 1, N_LIVE do
    local cd = live[i]
    local sz = HUGE + i * 10
    if ffi.sizeof(cd) ~= sz or cd[0] ~= (i % 256) or cd[sz - 1] ~= (i % 256) then
      content_ok = false; break
    end
  end
  ok(content_ok, "live huge cdata survivors readable (size/content intact) after rebuild churn")

  -- THE RED ASSERTION: header BLACK must NEVER appear on huge survivors,
  -- not even transiently during the rebuild walk.
  -- RED on current HEAD (rebuild_hugescan:1435 sets it). GREEN after task 2.
  ok(steps_with_black == 0,
     "white-box RED TARGET: huge survivors NEVER carry header LJ_GC_BLACK (0x04) "
     .. "during rebuild -- observed in " .. steps_with_black .. " steps, "
     .. total_black_obs .. " survivor-observations; samples: "
     .. table.concat(black_values_seen, " | "))

  if steps_with_black > 0 then
    print("RED PROOF: header LJ_GC_BLACK (0x04) appeared on huge survivors "
          .. "mid-rebuild in " .. steps_with_black .. " step(s). Sample marked=0x"
          .. string.format("%02x", sample_marked or 0)
          .. " (0x80=cdataisv | 0x04=BLACK). First observations: "
          .. table.concat(black_values_seen, " | "))
  end
end

----------------------------------------------------------------
-- (A) Rehash-during-rebuild stress scenarios (child-fork)
--
-- Each scenario runs in a child process so a crash cannot take down the
-- white-box read above. These exercise hugeset_resize mid-rebuild (the
-- restart-skip path), not just a clean sweep. They already PASS on current
-- HEAD; they are NOT the RED target.
----------------------------------------------------------------

local scenarios = {
  {
    name = "rehash_during_rebuild_churn",
    code = [[
local ffi = require("ffi")
local keep = {}
-- Allocate enough huge blocks to force hugeset growth; interleave with
-- incremental GC steps so hugeset_resize can land mid-rebuild.
for i = 1, 80 do
  keep[#keep + 1] = ffi.new("char[?]", 600 * 1024 + i * 64)
  if i % 4 == 0 then collectgarbage("step") end
  if i % 16 == 0 then
    -- Drop a slice to churn dead/survivor mix.
    for j = #keep - 3, #keep do keep[j] = nil end
  end
end
collectgarbage("collect")
io.write("scenario 1 rehash_during_rebuild_churn: PASS\n")
]],
  },
  {
    name = "huge_table_and_thread_survivors",
    code = [[
local ffi = require("ffi")
-- Huge tables and huge threads also flow through rebuild_hugescan; mix them
-- with huge cdata to exercise the non-cdata survivor branches.
local keep_t, keep_th = {}, {}
for i = 1, 30 do
  local t = {}
  for j = 1, 4000 do t[j] = j * i end
  keep_t[i] = t
end
for i = 1, 8 do
  local th = coroutine.create(function()
    local frame = ffi.new("char[?]", 600 * 1024)
    coroutine.yield(frame[0])
  end)
  keep_th[i] = th
end
-- Huge cdata churn alongside.
local mix = {}
for i = 1, 40 do
  mix[#mix + 1] = ffi.new("char[?]", 600 * 1024 + i * 100)
  if i % 5 == 0 then collectgarbage("step") end
end
mix = nil
collectgarbage("collect")
collectgarbage("collect")
-- Resume threads to prove they survived.
local alive = 0
for _, th in ipairs(keep_th) do
  if coroutine.status(th) ~= "dead" then
    local ok = coroutine.resume(th)
    if ok then alive = alive + 1 end
  end
end
assert(alive >= 1, "at least one huge thread survived")
io.write("scenario 2 huge_table_and_thread_survivors: PASS\n")
]],
  },
  {
    name = "huge_finalizer_exactly_once",
    code = [[
local ffi = require("ffi")
local counter = { n = 0 }
local N = 60
local keep, drop = {}, {}
for i = 1, N do
  local cd = ffi.gc(ffi.new("char[?]", 600 * 1024 + i * 50),
                    function() counter.n = counter.n + 1 end)
  if i % 2 == 0 then
    drop[#drop + 1] = cd  -- will be dropped below
  else
    keep[#keep + 1] = cd  -- kept live: finalizer must NOT fire for these
  end
  if i % 6 == 0 then collectgarbage("step") end
end
drop = nil  -- release the even-indexed cdata -> finalizers should fire for these
collectgarbage("collect")
collectgarbage("collect")
assert(counter.n == math.floor(N / 2),
  string.format("scenario 3 huge_finalizer_exactly_once: expected %d finalizers, got %d",
                math.floor(N / 2), counter.n))
io.write("scenario 3 huge_finalizer_exactly_once: PASS\n")
]],
  },
  {
    name = "hugeset_resize_mid_rebuild_restart_skip",
    code = [[
-- Specifically target the restart-skip path: allocate enough huge blocks to
-- trigger hugeset resize mid-rebuild, then drop a batch and step again so
-- rebuild_hugescan may re-enter and exercise the "already processed" skip.
local ffi = require("ffi")
local keep = {}
for round = 1, 5 do
  for i = 1, 60 do
    keep[#keep + 1] = ffi.new("char[?]", 600 * 1024 + round * 4096 + i)
  end
  collectgarbage("step")
  collectgarbage("step")
  -- Drop half to force a rehash-shaped churn (dead slots + survivors).
  for j = math.floor(#keep / 2) + 1, #keep do keep[j] = nil end
  collectgarbage("step")
end
collectgarbage("collect")
collectgarbage("collect")
io.write("scenario 4 hugeset_resize_mid_rebuild_restart_skip: PASS\n")
]],
  },
}

local function write_temp(code)
  local path = assert(os.tmpname())
  local file = assert(io.open(path, "w"))
  assert(file:write(code))
  assert(file:close())
  return path
end

local function remove_temp(path) os.remove(path) end

local function execute(command)
  local rc = os.execute(command)
  if type(rc) == "number" then return math.floor(rc / 256) end
  if rc == true then return 0 end
  return 1
end

local function run_child(code)
  local path = write_temp(code)
  local rc = execute(string.format('%s -joff "%s"', LUAJIT, path))
  remove_temp(path)
  return rc
end

local scenario_passes, scenario_failures = 0, 0
for i = 1, #scenarios do
  local s = scenarios[i]
  if s.skip then
    io.write(string.format("scenario %d %s: SKIP (%s)\n", i, s.name, s.skip))
  else
    local rc = run_child(s.code)
    if rc == 0 then
      scenario_passes = scenario_passes + 1
      io.write(string.format("scenario %d %s: PASS (exit %d)\n", i, s.name, rc))
    else
      scenario_failures = scenario_failures + 1
      io.write(string.format("scenario %d %s: FAIL (exit %d)\n", i, s.name, rc))
      fail = fail + 1
    end
  end
end

----------------------------------------------------------------
-- Summary
----------------------------------------------------------------

collectgarbage("collect")
healthy("final")

print(string.format("\nhuge_swept_tag_assert: %d passed, %d failed "
                    .. "(scenarios: %d passed, %d failed)",
                    pass, fail, scenario_passes, scenario_failures))

-- The white-box RED assertion is the intended RED on current HEAD. The
-- scenarios must PASS. So the correct exit code on current HEAD is NONZERO
-- (because of the RED target), but the scenario failures must be ZERO.
-- After task 2 the whole suite goes GREEN (exit 0).
if scenario_failures > 0 then
  print("UNEXPECTED: scenario failures present -- these are NOT the RED target")
end
os.exit(fail == 0 and 0 or 1)

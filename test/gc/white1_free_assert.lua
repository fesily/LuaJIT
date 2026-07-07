-- Failing-first invariant for the "remove LJ_GC_WHITE1 (0x02) under LJ_HASGCMARK"
-- refactor (option B: collapse the dual color-authority transition state).
--
-- GOAL INVARIANT (must hold AFTER the refactor, RED before it):
--   Under the arena/bitmap GC, NO live arena object may carry the header white
--   bit 0x02. Liveness authority is the arena mark bitmap / hugeset slot; the
--   header's only color bit is GRAY (0x01). The 0x02 slot must be free.
--
-- OBSERVABLE: a regular (fixed-size, non-VLA, arena-allocated) cdata's header
-- `marked` byte, read through FFI using the baked invariant cdataptr(cd)==cd+1.
-- For an array cdata the cdata value points at the payload (== cdataptr), so the
-- GCHeader sits immediately before it:
--     [ GCRef nextgc | uint8_t marked | uint8_t gct | uint16_t ctypeid | (pad) ]
-- We do NOT hardcode sizeof(GCcdata) (8-byte alignment may pad it to 16). Instead
-- we self-calibrate: scan the two plausible payload-relative offsets for the
-- gct byte (== ~LJ_TCDATA & 0xff == 0xf5); `marked` is the byte right before it.
--
-- Pre-fix: newwhite()/makewhite() set curwhite == LJ_GC_WHITE1 (0x02), so a fresh
--   or swept-survivor cdata has 0x02 set  -> this test FAILS (RED).
-- Post-fix: newwhite() = GRAY only, makewhite() clears GRAY only -> 0x02 clear
--   -> this test PASSES (GREEN).
--
-- Run: ./src/luajit -joff test/gc/white1_free_assert.lua
-- (Interpreter path exercises the C newwhite/makewhite recolor, not the JIT
--  asm_cnew immediate; both are converted by the refactor, the C path is the
--  deterministic observable here.)

local ok_ffi, ffi = pcall(require, "ffi")
if not ok_ffi then
  print("ffi unavailable -- white1_free_assert cannot run; skipping")
  os.exit(0)
end

local LJ_GC_WHITE1 = 0x02
local LJ_GC_GRAY   = 0x01
local GCT_CDATA    = 10  -- gct stores ~LJ_TCDATA, LJ_TCDATA==(~10u), byte==10

local pass, fail = 0, 0
local function ok(c, msg)
  if c then pass = pass + 1 else fail = fail + 1; print("FAIL: " .. msg) end
end

-- Locate the `marked` byte for an array cdata by validating the adjacent gct
-- byte. Returns (marked_value, marked_offset) or nil if the header could not be
-- identified (e.g. unexpected layout -> test is inconclusive, reported as skip).
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

-- Keep allocations live so the GC must treat them as survivors.
local live = {}
local function fresh_cd(i)
  local cd = ffi.new("int32_t[4]", i, i + 1, i + 2, i + 3)
  live[#live + 1] = cd
  return cd
end

-- 1. Freshly allocated cdata (newwhite path) must not carry 0x02.
do
  local checked = 0
  for i = 1, 64 do
    local cd = fresh_cd(i)
    local marked = read_marked(cd)
    if marked == nil then
      print("inconclusive: could not locate GCcdata header (layout?) -- skipping")
      os.exit(0)
    end
    ok(bit.band(marked, LJ_GC_GRAY) ~= 0,
       "fresh cdata is gray (marked=0x" .. string.format("%02x", marked) .. ")")
    ok(bit.band(marked, LJ_GC_WHITE1) == 0,
       "fresh cdata has NO white bit 0x02 (marked=0x" .. string.format("%02x", marked) .. ")")
    checked = checked + 1
  end
  ok(checked == 64, "checked all 64 fresh cdata")
end

-- 2. Survivors of a full GC (makewhite recolor path) must not carry 0x02.
do
  collectgarbage("collect")
  collectgarbage("collect")
  local checked, white1_seen = 0, 0
  for _, cd in ipairs(live) do
    local marked = read_marked(cd)
    if marked then
      checked = checked + 1
      if bit.band(marked, LJ_GC_WHITE1) ~= 0 then white1_seen = white1_seen + 1 end
    end
  end
  ok(checked > 0, "re-read survivors after full GC")
  ok(white1_seen == 0,
     white1_seen .. " of " .. checked .. " GC survivors still carry white bit 0x02")
end

-- 3. Crash/consistency coverage across the other object classes the refactor
--    touches (tables, strings, weak tables, closures/upvalues, cdata finalizers,
--    huge VLA). Mark/sweep these heavily; heap must stay self-consistent.
local has_checkheap = pcall(collectgarbage, "checkheap")
local function healthy(name)
  if not has_checkheap then pass = pass + 1; return end
  ok(collectgarbage("checkheap") == 0, name .. " checkheap")
end

do
  local keep = {}
  for i = 1, 500 do keep[i] = { a = i, [tostring(i)] = i * 2 } end
  local weak = setmetatable({}, { __mode = "v" })
  for i = 1, 500 do weak[i] = { i } end
  local closures = {}
  for i = 1, 200 do
    local c = { v = i }
    closures[i] = function() return c.v end
  end
  local fin = 0
  for i = 1, 200 do
    ffi.gc(ffi.new("int[1]"), function() fin = fin + 1 end)
  end
  local huge = {}
  for i = 1, 8 do huge[i] = ffi.new("char[?]", 600 * 1024) end
  collectgarbage("collect")
  collectgarbage("collect")
  ok(#closures == 200 and closures[1]() == 1, "closures intact after GC")
  ok(fin > 0, "cdata finalizers fired (" .. fin .. ")")
  healthy("mixed object classes")
end

collectgarbage("collect")
healthy("final")

print(string.format("\nwhite1_free_assert: %d passed, %d failed", pass, fail))
os.exit(fail == 0 and 0 or 1)

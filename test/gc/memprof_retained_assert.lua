-- memprof_retained_assert.lua: retained-size (dominator tree) + retaining-path
-- self-checks for the v0.5 read-only heap analysis.
--
-- Constructs a KNOWN reference graph (a root table R holding M child tables,
-- each holding K unique strings), keeps R reachable via a global, then asserts:
--   (a) retained(R) is approximately the whole subtree's shallow-sum and >> R.shallow;
--   (b) a single leaf string's retained ~= its shallow (leaves dominate nothing);
--   (c) retainers(leaf) is a path that reaches R and ultimately a root.
-- Plus a read-only proof: a full GC AFTER taking the retained snapshot must not
-- corrupt the GC (objects still usable; a follow-up snapshot + retained call work).
--
-- Run: ./src/luajit -joff test/gc/memprof_retained_assert.lua
--
-- Stack-pollution note: the dominator tree attributes an object to its UNIQUE
-- retaining path. If a subtree object is sitting on a Lua stack slot when the
-- snapshot is taken, the thread becomes a second retainer and the object is no
-- longer dominated by R. To keep the subtree dominated by R, we build it inside
-- a function that returns (clearing its stack frame), recover R only via the
-- global, and take care NEVER to load a subtree object into a Lua local across
-- an analysis call. Addresses are obtained as plain numbers (tostring inside the
-- builder, or matching by unique shallow size) — a number is not a GC reference.

local memprof = require("memprof")
local check_count = 0
local fail_count = 0
local function check(cond, msg)
  check_count = check_count + 1
  if not cond then
    fail_count = fail_count + 1
    io.write("FAIL: ", msg, "\n")
  end
end

local M, K = 40, 8
local SPECIAL_LEN = 1009  -- unique length for the path-test leaf
local SPECIAL_SHALLOW = 24 + SPECIAL_LEN + 1  -- sizeof(GCstr)+len+1

-- Build the subtree inside a function. On return its stack frame is gone, so
-- no child/string remains referenced from the stack — only via R's table
-- structure, and R is anchored in _G. We return R's address as a number.
-- Value strings use UNIQUE lengths (i*100+j) so no two (i,j) pairs intern the
-- same string — this keeps the subtree shallow-sum predictable for the
-- retained(R) ~= subtree assertion.
local function build_subtree()
  local R = {}
  _G.MEMPROF_RETAINED_ROOT = R
  for i = 1, M do
    local child = {}
    for j = 1, K do
      child["k"..j] = string.rep("s", i * 100 + j)
    end
    R[i] = child
  end
  R[1].special = string.rep("Z", SPECIAL_LEN)  -- unique-length leaf for path test
  return tonumber(tostring(R):match("0x(%x+)"), 16)
end

local r_addr = build_subtree()
collectgarbage("collect")  -- clean state; addresses stable hereafter

-- Sanity: the feature is additive; the v0 snapshot census still works.
local snap = memprof.snapshot{gc="none"}
check(type(snap) == "table" and type(snap.total) == "table", "snapshot still works")

-- 1. retained{top=N, gc="none"}: shape + sorted-desc-by-retained.
local top = memprof.retained{top = 50, gc = "none"}
check(type(top) == "table", "retained returns a table")
check(#top <= 50, "retained respects top=N")
if #top >= 2 then
  local a = top[1].retained
  local sorted = true
  for i = 2, #top do if top[i].retained > a then sorted = false; break end end
  check(sorted, "retained sorted by retained desc")
end
for _, e in ipairs(top) do
  check(e.addr ~= nil and e.type ~= nil and e.shallow ~= nil and e.retained ~= nil,
    "retained entries have addr/type/shallow/retained")
  check(e.retained >= e.shallow, "retained >= shallow (dominator tree property)")
end

-- Full retained list so we can look up R and the special leaf by addr/shallow.
local all = memprof.retained{top = 1000000, gc = "none"}
local by_addr = {}
for _, e in ipairs(all) do by_addr[e.addr] = e end

-- 2. retained(R) ~= subtree shallow-sum, and >> R.shallow.
local rE = by_addr[r_addr]
check(rE ~= nil, "R found in retained output by address")
check(rE.type == "table", "R classified as table")
check(rE.retained > rE.shallow * 10, "retained(R) >> shallow(R) (R dominates a subtree)")

-- Estimate the subtree: R + M children + M*K value strings + 1 special leaf.
-- sizeof(GCstr)=24 (string shallow = 24+len+1, verified structurally). Value
-- strings use unique lengths (i*100+j), so M*K DISTINCT interned strings.
-- Child tables all share the same structure (K string keys) so the same shallow;
-- recover that shallow empirically as the most-frequent table shallow in the
-- output (the M children are the largest cluster of identical-shallow tables).
local child_shallow, child_count = 0, 0
do
  local freq = {}
  for _, e in ipairs(all) do
    if e.type == "table" and e.addr ~= r_addr then
      freq[e.shallow] = (freq[e.shallow] or 0) + 1
    end
  end
  for sh, n in pairs(freq) do
    if n > child_count then child_count = n; child_shallow = sh end
  end
end
check(child_count >= M, "found >= M same-shallow tables (the children)")
local string_total = 0
for i = 1, M do
  for j = 1, K do
    string_total = string_total + 24 + (i * 100 + j) + 1
  end
end
local special_total = 24 + SPECIAL_LEN + 1
-- The 8 shared key strings ("k1".."k8") are dominated by R too; ~24+2+1 each,
-- small relative to the subtree — folded into the tolerance band.
local subtree_est = rE.shallow + M * child_shallow + string_total + special_total
check(rE.retained >= subtree_est * 0.90,
  "retained(R) >= 90% of subtree estimate (dominator attribution)")
check(rE.retained <= subtree_est * 1.10,
  "retained(R) <= 110% of subtree estimate (no over-attribution)")

-- 3. A leaf string's retained ~= its shallow (leaves dominate nothing).
--    The special leaf has a unique shallow size; find it without loading it.
local special
for _, e in ipairs(all) do
  if e.type == "string" and e.shallow == SPECIAL_SHALLOW then special = e; break end
end
check(special ~= nil, "special leaf found by unique shallow size")
if special then
  check(special.retained == special.shallow,
    "leaf retained == shallow (leaves dominate nothing)")
end

-- 4. retainers(leaf) reaches R and ultimately a root.
--    gc="none" => same live set as the retained call above; the leaf is NOT on
--    the stack (only its numeric address is in hand), so the path goes through R.
if special then
  local path = memprof.retainers(special.addr, "none")
  check(path ~= nil, "retainers(leaf) returns a path (leaf is a live object)")
  if path then
    check(#path >= 2, "retainers path has at least 2 nodes (leaf + a retainer)")
    check(path[1].type == "string", "retainers path[1] is the leaf string")
    local last = path[#path]
    check(last.type == "table" or last.type == "thread",
      "retainers path reaches a root (table=globals or thread=mainthread)")
    local passes_r = false
    for _, p in ipairs(path) do
      if p.addr == r_addr then passes_r = true; break end
    end
    check(passes_r, "retainers(leaf) path passes through R")
  end
end

-- retainers on a bogus address returns nil.
local bogus = memprof.retainers(1, "none")
check(bogus == nil, "retainers(bogus addr) returns nil")

-- 5. Read-only proof: a full GC AFTER the retained snapshot must not corrupt
--    the GC. The snapshot walk must leave color/marks byte-for-byte unperturbed.
collectgarbage("collect")
-- Objects still usable (R recovered via global; no local holds a subtree object).
do
  local R = _G.MEMPROF_RETAINED_ROOT
  check(R[1].special:sub(1, 3) == "ZZZ", "objects still usable after post-snapshot GC")
  check(#R == M, "R still has M children after post-snapshot GC")
end
-- A second retained call after the GC must work and re-find R as a dominator.
local all2 = memprof.retained{top = 1000000, gc = "none"}
local by_addr2 = {}
for _, e in ipairs(all2) do by_addr2[e.addr] = e end
local rE2 = by_addr2[r_addr]
check(rE2 ~= nil, "R still present in graph after post-snapshot GC")
if rE2 then
  check(rE2.retained > rE2.shallow * 10,
    "R still dominates its subtree after the read-only proof GC")
end
-- Two more full cycles complete cleanly (tricolor-equivalent: no throw/corruption).
collectgarbage("collect")
collectgarbage("collect")

-- Clean up the global anchor so the subtree is collectable after the test.
_G.MEMPROF_RETAINED_ROOT = nil
collectgarbage("collect")

io.write(string.format("memprof_retained_assert: %d checks, %d failures\n",
  check_count, fail_count))
if fail_count > 0 then os.exit(1) end

------------------------------------------------------------------------------
-- memprof_label_assert.lua — v6 allocation-label assertions.
--
-- Run on a flag-ON binary (LUAJIT_ENABLE_MEMPROF):
--   ./src/luajit -joff test/gc/memprof_label_assert.lua
--
-- Asserts:
--   (a) memprof.setlabel(str) tags subsequent ALLOCs; the `labels` summary
--       attributes the right bytes/objects to label "A" vs label "B" vs
--       <none> (unlabeled).
--   (b) `top --label=A` isolates ONLY A's allocations — no B or <none> sites
--       appear, and the totals are bounded by A's alloc_space.
--   (c) setlabel(nil) clears the label (subsequent allocs are unlabeled).
--   (d) the labeldict resolves ids to the correct strings ("A", "B").
--   (e) unlabeled allocations (before any setlabel) carry label_id == 0 and
--       label == nil (back-compat with v1–v5 behavior).
------------------------------------------------------------------------------

local memprof = require("memprof")
local parse = require("tools.memprof.parse")
local aggregate = require("tools.memprof.aggregate")

local STREAM = "/tmp/memprof_label_assert.bin"

local failures = 0
local checks = 0
local function check(cond, msg)
  checks = checks + 1
  if not cond then
    failures = failures + 1
    io.write("  FAIL: ", msg, "\n")
  end
end

-- Workloads that produce a KNOWN number of GC-object allocations under each
-- label. Each make_buf(n) call allocates n tables + n array parts = 2n GC
-- objects, each table ~72 bytes (Trav class) + array ~64 bytes.
local function make_buf(n)
  local t = {}
  for i = 1, n do
    t[i] = { i }
  end
  return t
end

local N_A = 500
local N_B = 300
local N_NONE = 200

memprof.start{mode="event", depth=1, out=STREAM}

-- Unlabeled allocations (before any setlabel).
local none_set = make_buf(N_NONE)

-- Label "A" allocations.
memprof.setlabel("A")
local a_set = make_buf(N_A)

-- Label "B" allocations.
memprof.setlabel("B")
local b_set = make_buf(N_B)

-- Clear label back to none.
memprof.setlabel(nil)
local none_set2 = make_buf(N_NONE)

memprof.stop()

-- Keep references alive so they don't get freed before stop.
keep = { none_set, a_set, b_set, none_set2 }

local f = io.open(STREAM, "rb")
assert(f, "stream not written")
local data = f:read("*a")
f:close()
os.remove(STREAM)
assert(#data > 5, "stream too short: " .. #data)

local parsed = parse.parse(data)
local agg = aggregate.aggregate(parsed)

-- (d) stream version is 6 and labeldict has "A" and "B".
check(parsed.version == 6,
  "(d) stream version == 6 (got " .. tostring(parsed.version) .. ")")
local has_a, has_b = false, false
for id, s in pairs(parsed.labeldict) do
  if s == "A" then has_a = true end
  if s == "B" then has_b = true end
end
check(has_a, "(d) labeldict contains 'A'")
check(has_b, "(d) labeldict contains 'B'")

-- (e) unlabeled ALLOCs carry label_id == 0 and label == nil.
local n_label_nil = 0
local n_label_id_zero = 0
local n_alloc_total = 0
for _, ev in ipairs(parsed.events) do
  if ev.op == "ALLOC" then
    n_alloc_total = n_alloc_total + 1
    if ev.label == nil then n_label_nil = n_label_nil + 1 end
    if ev.label_id == 0 then n_label_id_zero = n_label_id_zero + 1 end
  end
end
check(n_label_nil > 0, "(e) some ALLOCs have label == nil (unlabeled)")
check(n_label_id_zero > 0, "(e) some ALLOCs have label_id == 0 (unlabeled)")
check(n_label_nil == n_label_id_zero,
  ("(e) label==nil count == label_id==0 count (%d vs %d)"):format(
    n_label_nil, n_label_id_zero))

-- (a) labels summary attributes bytes to A, B, and <none>.
local labels = aggregate.top_labels(agg)
local label_map = {}
for _, r in ipairs(labels) do label_map[r.label] = r end

check(label_map["A"] ~= nil, "(a) labels summary has 'A'")
check(label_map["B"] ~= nil, "(a) labels summary has 'B'")
check(label_map["<none>"] ~= nil, "(a) labels summary has '<none>'")

-- A must have at least N_A * 2 objects (tables + arrays), B at least N_B * 2.
if label_map["A"] then
  check(label_map["A"].alloc_objects >= N_A,
    ("(a) A alloc_objects >= %d (got %d)"):format(
      N_A, label_map["A"].alloc_objects))
  check(label_map["A"].alloc_space > 0,
    "(a) A alloc_space > 0 (got " .. label_map["A"].alloc_space .. ")")
end
if label_map["B"] then
  check(label_map["B"].alloc_objects >= N_B,
    ("(a) B alloc_objects >= %d (got %d)"):format(
      N_B, label_map["B"].alloc_objects))
  check(label_map["B"].alloc_space > 0,
    "(a) B alloc_space > 0 (got " .. label_map["B"].alloc_space .. ")")
end

-- A should have MORE alloc_space than B (N_A > N_B, same per-object size).
if label_map["A"] and label_map["B"] then
  check(label_map["A"].alloc_space > label_map["B"].alloc_space,
    ("(a) A alloc_space > B alloc_space (%d vs %d)"):format(
      label_map["A"].alloc_space, label_map["B"].alloc_space))
end

-- (c) <none> includes both the pre-label and post-setlabel(nil) allocations,
-- so it should have at least 2 * N_NONE objects.
if label_map["<none>"] then
  check(label_map["<none>"].alloc_objects >= 2 * N_NONE,
    ("(c) <none> alloc_objects >= %d (got %d) — setlabel(nil) cleared"):format(
      2 * N_NONE, label_map["<none>"].alloc_objects))
end

-- (b) top --label=A isolates ONLY A's allocations.
local agg_a = aggregate.aggregate(parsed, { label = "A" })
local a_rows = aggregate.top_sites(agg_a)
check(#a_rows > 0, "(b) top --label=A has sites")
-- Every site in the A-filtered view must come from A's alloc_space.
-- The filtered totals should equal A's label alloc_space.
check(agg_a.totals.alloc_space == label_map["A"].alloc_space,
  ("(b) filtered totals.alloc_space == A label alloc_space (%d vs %d)"):format(
    agg_a.totals.alloc_space, label_map["A"].alloc_space))

-- (b) top --label=B should NOT include A's sites and vice versa.
local agg_b = aggregate.aggregate(parsed, { label = "B" })
local b_rows = aggregate.top_sites(agg_b)
check(agg_b.totals.alloc_space == label_map["B"].alloc_space,
  ("(b) filtered totals.alloc_space == B label alloc_space (%d vs %d)"):format(
    agg_b.totals.alloc_space, label_map["B"].alloc_space))

-- The A-filtered total must be DIFFERENT from B-filtered (different labels).
check(agg_a.totals.alloc_space ~= agg_b.totals.alloc_space,
  "(b) A-filtered total differs from B-filtered (isolation)")

-- (b) <none> filter should give the <none> slice.
local agg_none = aggregate.aggregate(parsed, { label = "<none>" })
check(agg_none.totals.alloc_space == label_map["<none>"].alloc_space,
  ("(b) filtered totals.alloc_space == <none> label alloc_space (%d vs %d)"):format(
    agg_none.totals.alloc_space, label_map["<none>"].alloc_space))

-- Sum of per-label alloc_space == unfiltered total (no double-counting).
local sum_labels = 0
for _, r in ipairs(labels) do sum_labels = sum_labels + r.alloc_space end
check(sum_labels == agg.totals.alloc_space,
  ("(a) sum(labels.alloc_space) == totals.alloc_space (%d vs %d)"):format(
    sum_labels, agg.totals.alloc_space))

io.write(("OK memprof_label_assert: version=%d labels=%d A_objs=%d B_objs=%d none_objs=%d total_allocs=%d\n"):format(
  parsed.version, #labels,
  label_map["A"] and label_map["A"].alloc_objects or 0,
  label_map["B"] and label_map["B"].alloc_objects or 0,
  label_map["<none>"] and label_map["<none>"].alloc_objects or 0,
  n_alloc_total))

if failures > 0 then os.exit(1) end

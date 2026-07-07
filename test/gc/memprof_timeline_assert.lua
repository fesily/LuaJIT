------------------------------------------------------------------------------
-- memprof_timeline_assert.lua — v7 timeline (Go-style) assertions.
--
-- Run on a flag-ON binary (LUAJIT_ENABLE_MEMPROF):
--   ./src/luajit -joff test/gc/memprof_timeline_assert.lua
--
-- Asserts:
--   (a) window A->B alloc bytes >> window B->C alloc bytes (heavy vs light).
--   (b) live_bytes at B > live_bytes at A (retained growth between marks).
--   (c) timestamps strictly increasing across A, B, C.
--   (d) the timeline identifies A->B as the hottest window (max alloc bytes).
--   (e) stream version is 7; MARK events parse with kind/name/ts/gc_total.
--   (f) v1-v6 back-compat: existing subcommands still work (no marks = no MARK
--       records, so the 11 existing tests are unaffected).
------------------------------------------------------------------------------

local memprof = require("memprof")
local parse = require("tools.memprof.parse")
local aggregate = require("tools.memprof.aggregate")

local STREAM = "/tmp/memprof_timeline_assert.bin"

local failures = 0
local checks = 0
local function check(cond, msg)
  checks = checks + 1
  if not cond then
    failures = failures + 1
    io.write("  FAIL: ", msg, "\n")
  end
end

-- Allocate a LARGE retained set (N_HEAVY tables + arrays), then a SMALL set.
local N_HEAVY = 2000
local N_LIGHT = 50

local function make_tables(n)
  local t = {}
  for i = 1, n do
    t[i] = { i, i * 2, i * 3 }
  end
  return t
end

memprof.start{mode="event", depth=1, out=STREAM}

memprof.mark("A")
local heavy = make_tables(N_HEAVY)
memprof.mark("B")
local light = make_tables(N_LIGHT)
memprof.mark("C")

memprof.stop()

keep = { heavy, light }

local f = io.open(STREAM, "rb")
assert(f, "stream not written")
local data = f:read("*a")
f:close()
os.remove(STREAM)
assert(#data > 5, "stream too short: " .. #data)

local parsed = parse.parse(data)
local tl = aggregate.timeline_windows(parsed)

-- (e) stream version is 7 and MARK events parsed correctly.
check(parsed.version == 7,
  "(e) stream version == 7 (got " .. tostring(parsed.version) .. ")")

local mark_events = {}
for _, ev in ipairs(parsed.events) do
  if ev.op == "MARK" then
    mark_events[#mark_events+1] = ev
  end
end
check(#mark_events == 3,
  "(e) 3 MARK events (got " .. #mark_events .. ")")
check(#tl.marks == 3,
  "(e) timeline has 3 marks (got " .. #tl.marks .. ")")

if #mark_events == 3 then
  check(mark_events[1].name == "A",
    "(e) mark 1 name == 'A' (got '" .. tostring(mark_events[1].name) .. "')")
  check(mark_events[2].name == "B",
    "(e) mark 2 name == 'B' (got '" .. tostring(mark_events[2].name) .. "')")
  check(mark_events[3].name == "C",
    "(e) mark 3 name == 'C' (got '" .. tostring(mark_events[3].name) .. "')")
  check(mark_events[1].kind == "mark",
    "(e) mark 1 kind == 'mark'")
  check(mark_events[1].ts ~= nil and mark_events[1].ts > 0,
    "(e) mark 1 ts is a positive number")
  check(mark_events[1].gc_total ~= nil and mark_events[1].gc_total > 0,
    "(e) mark 1 gc_total is a positive number")
end

-- (c) timestamps strictly increasing across A, B, C.
if #tl.marks == 3 then
  check(tl.marks[1].ts < tl.marks[2].ts,
    ("(c) ts(A) < ts(B) (%d vs %d)"):format(tl.marks[1].ts, tl.marks[2].ts))
  check(tl.marks[2].ts < tl.marks[3].ts,
    ("(c) ts(B) < ts(C) (%d vs %d)"):format(tl.marks[2].ts, tl.marks[3].ts))
end

-- Find windows. With marks A, B, C we get 4 windows:
--   1: [start] -> A   (name "[start]")
--   2: A -> B         (name "B")
--   3: B -> C         (name "C")
--   4: C -> [end]     (name "[end]")
check(#tl.windows == 4,
  "(d) 4 windows (got " .. #tl.windows .. ")")

-- Find the A->B window (name "B") and B->C window (name "C").
local w_ab, w_bc
for i, w in ipairs(tl.windows) do
  if w.name == "B" then w_ab = w end
  if w.name == "C" then w_bc = w end
end

check(w_ab ~= nil, "(a) window A->B (name='B') found")
check(w_bc ~= nil, "(a) window B->C (name='C') found")

-- (a) window A->B alloc bytes >> window B->C alloc bytes.
if w_ab and w_bc then
  check(w_ab.alloc_bytes > 0,
    "(a) A->B alloc_bytes > 0 (got " .. w_ab.alloc_bytes .. ")")
  check(w_bc.alloc_bytes > 0,
    "(a) B->C alloc_bytes > 0 (got " .. w_bc.alloc_bytes .. ")")
  check(w_ab.alloc_bytes > w_bc.alloc_bytes * 5,
    ("(a) A->B alloc >> B->C alloc (%d vs %d, ratio %.1f)"):format(
      w_ab.alloc_bytes, w_bc.alloc_bytes,
      w_bc.alloc_bytes > 0 and w_ab.alloc_bytes / w_bc.alloc_bytes or 0))
end

-- (b) live_bytes at B > live_bytes at A (retained growth).
if #tl.marks >= 2 then
  check(tl.marks[2].gc_total > tl.marks[1].gc_total,
    ("(b) live_bytes at B > live_bytes at A (%d vs %d)"):format(
      tl.marks[2].gc_total, tl.marks[1].gc_total))
end

-- (d) the timeline identifies A->B as the hottest window.
if w_ab and tl.hottest then
  local hottest_w = tl.windows[tl.hottest]
  check(hottest_w ~= nil, "(d) hottest window exists")
  if hottest_w then
    check(hottest_w.alloc_bytes == w_ab.alloc_bytes,
      ("(d) hottest window is A->B (hottest alloc=%d, A->B alloc=%d, name=%s)"):format(
        hottest_w.alloc_bytes, w_ab.alloc_bytes, hottest_w.name))
  end
end

-- (f) back-compat: existing aggregate functions still work (no MARK events
-- interfere with the site/label/survival aggregations).
local agg = aggregate.aggregate(parsed)
check(agg.totals.alloc_space > 0,
  "(f) aggregate works: totals.alloc_space > 0")
check(agg.totals.event_count > 0,
  "(f) aggregate works: totals.event_count > 0")

-- Print the timeline for visual confirmation.
io.write(("\ntimeline (version=%d, marks=%d, windows=%d):\n"):format(
  parsed.version, #tl.marks, #tl.windows))
for i, w in ipairs(tl.windows) do
  local flag = ""
  if i == tl.hottest and w.alloc_bytes > 0 then flag = " HOTTEST" end
  if w.net_live > 0 then flag = flag .. " LEAK?" end
  io.write(("  %-8s alloc=%d freed=%d net=%d live=%d dur=%dms rate=%.0fB/s%s\n"):format(
    w.name, w.alloc_bytes, w.freed_bytes, w.net_live, w.live_bytes,
    w.duration_ns / 1e6, w.alloc_rate_bps, flag))
end

io.write(("OK memprof_timeline_assert: version=%d marks=%d windows=%d A_alloc=%d B_alloc=%d hottest=%s\n"):format(
  parsed.version, #tl.marks, #tl.windows,
  w_ab and w_ab.alloc_bytes or 0,
  w_bc and w_bc.alloc_bytes or 0,
  tl.windows[tl.hottest] and tl.windows[tl.hottest].name or "?"))

if failures > 0 then os.exit(1) end

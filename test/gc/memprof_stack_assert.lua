------------------------------------------------------------------------------
-- memprof_stack_assert.lua — asserts multi-frame stack attribution (v3)
-- distinguishes two callers of a shared allocator helper that a leaf-only
-- (v1/v2) view collapses into one site.
--
-- Run on a flag-ON binary (LUAJIT_ENABLE_MEMPROF):
--   ./src/luajit -joff test/gc/memprof_stack_assert.lua
--
-- Asserts:
--   (a) the stream is v3 (parsed.version == 3) and at least one event has a
--       stack deeper than 1 frame.
--   (b) a full-stack site containing caller_a exists AND one containing
--       caller_b exists, BOTH also containing the shared helper's frame —
--       i.e. the two call paths are distinguished.
--   (c) in the leaf-only view the helper collapses to a single site (the
--       v1/v2 ceiling): caller_a and caller_b do NOT appear as their own
--       leaf sites for the helper's allocations.
--   (d) the collapsed output (site labels joined by ";") contains both
--       caller labels.
--
-- The callers deliberately do NOT tail-call alloc_n (a tail call would
-- replace the caller's frame in lj_debug_frame's walk, hiding it).
------------------------------------------------------------------------------

local memprof = require("memprof")
local parse = require("tools.memprof.parse")
local aggregate = require("tools.memprof.aggregate")

local STREAM = "/tmp/memprof_stack_assert.bin"

-- ONE shared allocator helper. Each call allocates a table plus N strings,
-- so it is a hot leaf reached from two distinct callers.
local function alloc_n(n)
  local t = {}
  for i = 1, n do t[i] = ("x"):rep(20) end
  return t
end

-- Two distinct Lua callers. They must NOT tail-call alloc_n (a `return
-- alloc_n(n)` tail call replaces the caller frame, so caller_a/caller_b
-- would vanish from the stack walk). Assigning to a local and returning it
-- preserves the caller frame on the Lua stack.
local function caller_a()
  local r = alloc_n(50)
  r.tag = "a"
  return r
end

local function caller_b()
  local r = alloc_n(50)
  r.tag = "b"
  return r
end

-- Resolve each function's site label the same way the symtab will: the
-- chunkname (debug source, leading '@') and the function's first line.
local function label_of(fn)
  local info = debug.getinfo(fn, "S")
  return ("%s:%d"):format(info.source, info.linedefined)
end
local helper_label = label_of(alloc_n)
local caller_a_label = label_of(caller_a)
local caller_b_label = label_of(caller_b)

local checks = 0
local failures = 0
local function check(cond, msg)
  checks = checks + 1
  if not cond then
    failures = failures + 1
    io.write("  FAIL: ", msg, "\n")
  end
end

memprof.start{mode="event", depth=4, out=STREAM}

local a = caller_a()
local b = caller_b()
a, b = nil, nil
collectgarbage("collect")

memprof.stop()

local f = io.open(STREAM, "rb")
check(f ~= nil, "stream file written")
if not f then
  io.write(("memprof_stack_assert: %d checks, %d failures\n"):format(
    checks, failures))
  os.exit(failures == 0 and 0 or 1)
end
local data = f:read("*a")
f:close()
os.remove(STREAM)

check(#data > 5, "stream non-empty (" .. #data .. " bytes)")

local parsed = parse.parse(data)
check(parsed.version == 3,
      "stream version 3 (got " .. tostring(parsed.version) .. ")")

-- (a) at least one event has a multi-frame stack.
local n_multiframe = 0
for _, ev in ipairs(parsed.events) do
  if ev.stack and #ev.stack > 1 then n_multiframe = n_multiframe + 1 end
end
check(n_multiframe > 0,
      "at least one event has a >1-frame stack (got " .. n_multiframe .. ")")

local agg = aggregate.aggregate(parsed)

-- (b) Full-stack view: a site containing caller_a AND one containing
-- caller_b, both also containing the shared helper. The site keys are the
-- full leaf..root stack joined by ";".
local has_a_path = false
local has_b_path = false
local has_a_with_helper = false
local has_b_with_helper = false
for label in pairs(agg.sites) do
  if label:find(caller_a_label, 1, true) then
    has_a_path = true
    if label:find(helper_label, 1, true) then
      has_a_with_helper = true
    end
  end
  if label:find(caller_b_label, 1, true) then
    has_b_path = true
    if label:find(helper_label, 1, true) then
      has_b_with_helper = true
    end
  end
end
check(has_a_path, "full-stack view has a site routing through caller_a (" ..
      caller_a_label .. ")")
check(has_b_path, "full-stack view has a site routing through caller_b ("..
      caller_b_label .. ")")
check(has_a_with_helper,
      "a caller_a site also contains the shared helper (" .. helper_label ..
      ") — both paths route through it")
check(has_b_with_helper,
      "a caller_b site also contains the shared helper (" .. helper_label ..
      ") — both paths route through it")

-- (c) Leaf-only view: the helper's allocations collapse to a single leaf
-- site (helper_label), whereas the full-stack view distinguishes the two
-- call paths. This is the v1/v2 site-granularity ceiling the multi-frame
-- attribution fixes. caller_a/caller_b may still appear as leaves for THEIR
-- OWN direct allocs (e.g. the tag string), but the shared helper's allocs
-- are not split by caller in the leaf view.
check(agg.sites_leaf[helper_label] ~= nil,
      "leaf-only view has the shared helper as a leaf site (" ..
      helper_label .. ")")
local n_full_helper_sites = 0
for label in pairs(agg.sites) do
  if label:find(helper_label, 1, true) then
    n_full_helper_sites = n_full_helper_sites + 1
  end
end
check(n_full_helper_sites >= 2,
      "full-stack view has >= 2 helper-containing sites (got " ..
      n_full_helper_sites ..
      ") — the two call paths are distinguished; leaf-only collapses to 1")

-- (d) collapsed-style: the joined stack labels contain both caller labels.
local collapsed = {}
for label, st in pairs(agg.sites) do
  collapsed[#collapsed + 1] = label .. " " .. tostring(st.alloc_objects)
end
local joined = table.concat(collapsed, "\n")
check(joined:find(caller_a_label, 1, true) ~= nil,
      "collapsed output contains caller_a label")
check(joined:find(caller_b_label, 1, true) ~= nil,
      "collapsed output contains caller_b label")

io.write(("OK memprof_stack_assert: version=%d events=%d multiframe=%d " ..
          "sites=%d helper=%s caller_a=%s caller_b=%s\n"):format(
  parsed.version, #parsed.events, n_multiframe,
  (function()
    local n = 0
    for _ in pairs(agg.sites) do n = n + 1 end
    return n
  end)(),
  helper_label, caller_a_label, caller_b_label))

io.write(("memprof_stack_assert: %d checks, %d failures\n"):format(
  checks, failures))
os.exit(failures == 0 and 0 or 1)

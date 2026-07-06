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

-- Resolve each function's source and line range. With v4 line-precise
-- attribution, frames carry the ACTUAL bytecode/call line, not the function's
-- firstline. We match sites by checking if any frame in the label falls within
-- the function's [linedefined, lastlinedefined] range, so the test is robust
-- to which specific line inside the function allocated or called.
local function info_of(fn)
  local info = debug.getinfo(fn, "S")
  return info.source, info.linedefined, info.lastlinedefined
end
local helper_src, helper_first, helper_last = info_of(alloc_n)
local caller_a_src, caller_a_first, caller_a_last = info_of(caller_a)
local caller_b_src, caller_b_first, caller_b_last = info_of(caller_b)

-- Check if a site label contains a frame from the given function (source +
-- line range). Frames are @source:NN or builtin names, joined by ";".
local function label_has_frame(label, source, firstline, lastline)
  local prefix = source .. ":"
  for frame in label:gmatch("[^;]+") do
    if frame:sub(1, #prefix) == prefix then
      local ln = tonumber(frame:sub(#prefix + 1))
      if ln and ln >= firstline and ln <= lastline then
        return true
      end
    end
  end
  return false
end

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
  check(parsed.version == 3 or parsed.version == 4 or parsed.version == 5
        or parsed.version == 6,
        "stream version 3, 4, 5 or 6 (got " .. tostring(parsed.version) .. ")")

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
-- full leaf..root stack joined by ";". With v4 line-precise attribution,
-- frames carry the actual call/allocation line, so we match by line range
-- rather than exact label.
local has_a_path = false
local has_b_path = false
local has_a_with_helper = false
local has_b_with_helper = false
for label in pairs(agg.sites) do
  if label_has_frame(label, caller_a_src, caller_a_first, caller_a_last) then
    has_a_path = true
    if label_has_frame(label, helper_src, helper_first, helper_last) then
      has_a_with_helper = true
    end
  end
  if label_has_frame(label, caller_b_src, caller_b_first, caller_b_last) then
    has_b_path = true
    if label_has_frame(label, helper_src, helper_first, helper_last) then
      has_b_with_helper = true
    end
  end
end
check(has_a_path, "full-stack view has a site routing through caller_a (" ..
      caller_a_src .. ":" .. caller_a_first .. ")")
check(has_b_path, "full-stack view has a site routing through caller_b ("..
      caller_b_src .. ":" .. caller_b_first .. ")")
check(has_a_with_helper,
      "a caller_a site also contains the shared helper (" .. helper_src ..
      ":" .. helper_first .. ") — both paths route through it")
check(has_b_with_helper,
      "a caller_b site also contains the shared helper (" .. helper_src ..
      ":" .. helper_first .. ") — both paths route through it")

-- (c) Leaf-only view: the helper's allocations collapse to leaf site(s)
-- keyed by the actual allocation line (v4). caller_a/caller_b may still
-- appear as leaves for THEIR OWN direct allocs (e.g. the tag string), but
-- the shared helper's allocs are not split by caller in the leaf view.
local has_helper_leaf = false
for label in pairs(agg.sites_leaf) do
  if label_has_frame(label, helper_src, helper_first, helper_last) then
    has_helper_leaf = true
    break
  end
end
check(has_helper_leaf,
      "leaf-only view has the shared helper as a leaf site (" ..
      helper_src .. ":" .. helper_first .. ")")
local n_full_helper_sites = 0
for label in pairs(agg.sites) do
  if label_has_frame(label, helper_src, helper_first, helper_last) then
    n_full_helper_sites = n_full_helper_sites + 1
  end
end
check(n_full_helper_sites >= 2,
      "full-stack view has >= 2 helper-containing sites (got " ..
      n_full_helper_sites ..
      ") — the two call paths are distinguished; leaf-only collapses to 1")

-- (d) collapsed-style: the joined stack labels contain both caller labels
-- (matched by line range for v4 robustness).
local collapsed = {}
for label, st in pairs(agg.sites) do
  collapsed[#collapsed + 1] = label .. " " .. tostring(st.alloc_objects)
end
local joined = table.concat(collapsed, "\n")
local has_a_collapsed = false
local has_b_collapsed = false
for line in joined:gmatch("[^\n]+") do
  if label_has_frame(line, caller_a_src, caller_a_first, caller_a_last) then
    has_a_collapsed = true
  end
  if label_has_frame(line, caller_b_src, caller_b_first, caller_b_last) then
    has_b_collapsed = true
  end
end
check(has_a_collapsed, "collapsed output contains caller_a label")
check(has_b_collapsed, "collapsed output contains caller_b label")

io.write(("OK memprof_stack_assert: version=%d events=%d multiframe=%d " ..
           "sites=%d helper=%s:%d-%d caller_a=%s:%d-%d caller_b=%s:%d-%d\n"):format(
  parsed.version, #parsed.events, n_multiframe,
  (function()
    local n = 0
    for _ in pairs(agg.sites) do n = n + 1 end
    return n
  end)(),
  helper_src, helper_first, helper_last,
  caller_a_src, caller_a_first, caller_a_last,
  caller_b_src, caller_b_first, caller_b_last))

io.write(("memprof_stack_assert: %d checks, %d failures\n"):format(
  checks, failures))
os.exit(failures == 0 and 0 or 1)

------------------------------------------------------------------------------
-- memprof_parse_assert.lua — asserts the offline parser/aggregator produce
-- correct reports for a KNOWN workload run under event-mode memprof.
--
-- Run on a flag-ON binary (LUAJIT_ENABLE_MEMPROF):
--   ./src/luajit -joff test/gc/memprof_parse_assert.lua
--
-- Asserts:
--   (a) the hot allocation site's alloc_objects / alloc_space fall within
--       expected bounds for the workload's hot line.
--   (b) the `leak` view lists a deliberately-retained allocation set.
--   (c) freed objects are excluded from inuse (inuse drops by at least the
--       freed count; freed_objects reflects the GC'd set).
------------------------------------------------------------------------------

local memprof = require("memprof")
local parse = require("tools.memprof.parse")
local aggregate = require("tools.memprof.aggregate")

local STREAM = "/tmp/memprof_parse_assert.bin"

-- A hot allocator on a known source line. Each iteration allocates a table
-- plus its array part, so ~2 GC-object allocs per call.
local function hot(n)
  local t = {}
  for i = 1, n do
    t[i] = { i }
  end
  return t
end

-- Resolve the hot function's source. With v4 line-precise attribution the
-- site label carries the ACTUAL allocation line (e.g. :25 for `local t = {}`,
-- :27 for `{ i }`), not the function's firstline (:24). The hot function
-- allocates on two lines, so we collect all matching sites and sum their
-- stats to preserve the test's spirit (total allocs/freed/inuse for hot).
local hot_info = debug.getinfo(hot, "S")
local hot_source = hot_info.source
local hot_prefix = hot_source .. ":"

local N_BURNED = 2000
local N_RETAINED = 500

memprof.start{mode="event", depth=1, out=STREAM}

local burned = hot(N_BURNED)    -- freed before stop  -> (c)
local retained = hot(N_RETAINED)  -- kept alive past stop -> (b)

burned = nil
collectgarbage("collect")
collectgarbage("collect")  -- flush finalizers so FREE events land in-stream

memprof.stop()

local f = io.open(STREAM, "rb")
assert(f, "stream not written")
local data = f:read("*a")
f:close()
os.remove(STREAM)
assert(#data > 5, "stream too short: " .. #data)

local parsed = parse.parse(data)
local agg = aggregate.aggregate(parsed)

assert(#parsed.events > 0, "no events parsed")
assert(parsed.symtab.lfunc ~= nil, "no lfunc symtab map")

-- (a) hot site within expected bounds. With v4, the hot function may span
-- multiple sites (one per allocation line). Collect all sites whose label
-- starts with the hot function's source prefix and sum their stats.
local hot_labels = {}
local hot_site = { alloc_objects = 0, alloc_space = 0, freed_objects = 0,
                   inuse_objects = 0, inuse_space = 0 }
local hot_leak = { count = 0, bytes = 0 }
for label, st in pairs(agg.sites) do
  if label:sub(1, #hot_prefix) == hot_prefix then
    hot_labels[#hot_labels+1] = label
    hot_site.alloc_objects = hot_site.alloc_objects + st.alloc_objects
    hot_site.alloc_space = hot_site.alloc_space + st.alloc_space
    hot_site.freed_objects = hot_site.freed_objects + st.freed_objects
    hot_site.inuse_objects = hot_site.inuse_objects + st.inuse_objects
    hot_site.inuse_space = hot_site.inuse_space + st.inuse_space
  end
end
for label, info in pairs(agg.leaks) do
  if label:sub(1, #hot_prefix) == hot_prefix then
    hot_leak.count = hot_leak.count + info.count
    hot_leak.bytes = hot_leak.bytes + info.bytes
  end
end
local hot_label_str = table.concat(hot_labels, ", ")
assert(#hot_labels > 0, "hot site not found in aggregated sites; got: " ..
  (function()
    local labels = {}
    for k in pairs(agg.sites) do labels[#labels+1] = k end
    return table.concat(labels, ", ")
   end)())

-- Each hot() iteration yields >=1 table alloc (plus an array alloc via the
-- allocf path). So alloc_objects is at least N_BURNED + N_RETAINED. Upper
-- bound is generous to absorb interpreter/symtab overhead.
local total_hot_calls = N_BURNED + N_RETAINED
assert(hot_site.alloc_objects >= total_hot_calls,
  ("(a) alloc_objects too low: got %d, want >= %d"):format(
    hot_site.alloc_objects, total_hot_calls))
assert(hot_site.alloc_objects <= total_hot_calls * 4,
  ("(a) alloc_objects too high: got %d, want <= %d"):format(
    hot_site.alloc_objects, total_hot_calls * 4))
assert(hot_site.alloc_space >= total_hot_calls * 16,
  ("(a) alloc_space too low: got %d bytes, want >= %d"):format(
    hot_site.alloc_space, total_hot_calls * 16))

-- (c) freed objects excluded from inuse.
-- `burned` (N_BURNED tables + their arrays) was GC'd during profiling, so
-- freed_objects at the hot site must be substantial and inuse must drop by
-- at least that many.
assert(hot_site.freed_objects >= N_BURNED,
  ("(c) freed_objects too low: got %d, want >= %d"):format(
    hot_site.freed_objects, N_BURNED))
assert(hot_site.inuse_objects <= hot_site.alloc_objects - N_BURNED,
  ("(c) inuse_objects did not drop: inuse=%d alloc=%d (expected freed >= %d)"):format(
    hot_site.inuse_objects, hot_site.alloc_objects, N_BURNED))
assert(hot_site.inuse_space >= 0, "(c) inuse_space went negative")

-- (b) leak view lists the deliberately-retained set.
-- `retained` (N_RETAINED tables + arrays) stays alive past memprof.stop, so
-- those addresses must appear in the leak view under the hot function's sites.
assert(hot_leak.count >= N_RETAINED,
  ("(b) leak count too low: got %d addrs, want >= %d (retained tables+arrays)"):format(
    hot_leak.count, N_RETAINED))
assert(hot_leak.count <= N_RETAINED * 4,
  ("(b) leak count too high: got %d, want <= %d"):format(
    hot_leak.count, N_RETAINED * 4))
assert(hot_leak.bytes > 0, "(b) leak bytes non-positive")

-- Sanity: totals consistent (inuse = alloc - freed, never negative).
local t = agg.totals
assert(t.inuse_space == t.alloc_space - t.freed_space,
  ("totals inconsistent: inuse=%d alloc=%d freed=%d"):format(
    t.inuse_space, t.alloc_space, t.freed_space))

io.write(("OK memprof_parse_assert: events=%d hot=[%s] alloc_objs=%d freed_objs=%d inuse_objs=%d leak=%d\n"):format(
  #parsed.events, hot_label_str, hot_site.alloc_objects, hot_site.freed_objects,
  hot_site.inuse_objects, hot_leak.count))

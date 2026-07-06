------------------------------------------------------------------------------
-- memprof.lua — offline analyzer for the LuaJIT memory-profiler event stream.
--
-- Usage:
--   ./src/luajit tools/memprof.lua <subcmd> <stream.bin> [limit]
--
-- Subcommands:
--   top        Per-site flat allocation stats, sorted by alloc_space desc.
--              Columns: flat_space  objects  site   (Go-pprof flat style)
--   collapsed  Brendan-Gregg flamegraph-input lines:  site1;site2  count
--              (depth-1 today, so single-frame stacks — see note below)
--   summary    Total alloc / freed / inuse bytes + objects, per-type breakdown.
--   leak       Addresses allocated and never freed in the stream, grouped by
--              their allocation site.
--   survival   Per-site survival rate: of the objects each site allocated,
--              the fraction still live at end of stream. Classifies each site
--              as CHURN (survival<0.1, short-lived temporaries), RETAINED
--              (>0.9, long-lived / potential leak), or MIXED. Sorted by
--              alloc volume desc. The metric that distinguishes churn from
--              real leaks in a GC'd VM.
--   pprof      Serialize the aggregated per-site stats into an UNCOMPRESSED
--              google/pprof `Profile` protobuf (raw bytes, no gzip) so the
--              whole `go tool pprof` ecosystem (top/graph/web/flamegraph)
--              works on our data. Writes to the `out` path given after the
--              stream, or stdout if `out` is `-` / omitted.
--              Usage: luajit tools/memprof.lua pprof <stream.bin> [out.pb]
--              View with: go tool pprof [-http] <out.pb>
--
-- The stream is produced by:
--   memprof.start{mode="event", depth=1, out="stream.bin"}
--   ... workload ...
--   memprof.stop()
--
-- Wire format and site-resolution rules: see tools/memprof/parse.lua and the
-- authoritative C emitter src/lj_memprof.c.
--
-- Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h.
------------------------------------------------------------------------------

local parse = require("tools.memprof.parse")
local aggregate = require("tools.memprof.aggregate")
local pprof = require("tools.memprof.pprof")

-- -- helpers ----------------------------------------------------------------

local function fmt_bytes(n)
  -- Human-readable byte count (KiB/MiB) with one decimal, ASCII only.
  local abs = n < 0 and -n or n
  if abs >= 1024 * 1024 then
    return ("%.1fMiB"):format(n / (1024 * 1024))
  elseif abs >= 1024 then
    return ("%.1fKiB"):format(n / 1024)
  else
    return ("%dB"):format(n)
  end
end

local function pad(s, w)
  s = tostring(s)
  if #s >= w then return s end
  return s .. string.rep(" ", w - #s)
end

local function rpad(s, w)
  s = tostring(s)
  if #s >= w then return s end
  return string.rep(" ", w - #s) .. s
end

-- -- subcommands ------------------------------------------------------------

local function cmd_pprof(agg, out_path)
  -- out_path: "-" or nil -> stdout; else file path. Returns byte count.
  local n = pprof.export(agg, out_path)
  if out_path and out_path ~= "-" then
    io.stderr:write(("pprof: wrote %d bytes to %s\n"):format(n, out_path))
  end
  return n
end

local function cmd_top(agg, limit)
  local rows = aggregate.top_sites(agg, limit)
  if #rows == 0 then
    io.write("(no allocation sites recorded)\n")
    return
  end
  io.write(("flat_space   objects   inuse_space  inuse_objs  site\n"))
  for i = 1, #rows do
    local r = rows[i]
    local st = r.stat
    io.write(("%-11s  %-8d  %-11s  %-10d  %s\n"):format(
      fmt_bytes(st.alloc_space), st.alloc_objects,
      fmt_bytes(st.inuse_space), st.inuse_objects,
      r.label))
  end
end

local function cmd_collapsed(agg)
  -- Flamegraph input: `frame;frame;... count`. depth=1 today, so each line is
  -- a single frame (the allocation site) plus its alloc_objects count. Use
  -- alloc_objects (call count) as the sample value, which is the conventional
  -- flamegraph metric for allocation profilers. Sorted by site for stable
  -- output; the flamegraph tool re-sorts anyway.
  --
  -- NOTE: real stack depth requires the v1 emitter to walk N frames; today it
  -- reads only the level-0 frame (memprof_attribution), so every stack is a
  -- single frame. This is the documented v1 limitation.
  local rows = aggregate.top_sites(agg, nil)
  for i = 1, #rows do
    local r = rows[i]
    -- Skip pure-INTERNAL churn buckets that carry no useful site info.
    if r.label ~= "INTERNAL" and r.label ~= "INTERNAL:POD" then
      io.write(("%s %d\n"):format(r.label, r.stat.alloc_objects))
    end
  end
end

local function cmd_summary(agg)
  local t = agg.totals
  io.write(("events:     %d\n"):format(t.event_count))
  io.write(("realloc:    %d\n"):format(t.realloc_count))
  io.write(("free:       %d\n"):format(t.free_count))
  io.write(("podfree:    %d  (%s, %d cells)\n"):format(
    t.podfree_count, fmt_bytes(t.podfree_bytes), t.podfree_objects))
  io.write("\n")
  io.write(("           %12s  %12s  %12s\n"):format("bytes", "objects", "inuse_bytes"))
  io.write(("alloc:     %12s  %12d  %12s\n"):format(
    fmt_bytes(t.alloc_space), t.alloc_objects, fmt_bytes(t.alloc_space)))
  io.write(("freed:     %12s  %12d  %12s\n"):format(
    fmt_bytes(t.freed_space), t.freed_objects, "-"))
  io.write(("inuse:     %12s  %12d  %12s\n"):format(
    fmt_bytes(t.inuse_space), t.inuse_objects, "-"))
  io.write("\nby type:\n")
  io.write(("  %-12s %12s %12s %12s %12s\n"):format(
    "type", "alloc_bytes", "alloc_objs", "freed_bytes", "freed_objs"))
  -- Sort types by alloc_space desc for stable output.
  local trows = {}
  for tag, st in pairs(agg.types) do
    trows[#trows+1] = { tag = tag, st = st }
  end
  table.sort(trows, function(a, b)
    if a.st.alloc_space ~= b.st.alloc_space then
      return a.st.alloc_space > b.st.alloc_space
    end
    return a.tag < b.tag
  end)
  for i = 1, #trows do
    local r = trows[i]
    io.write(("  %-12s %12s %12d %12s %12d\n"):format(
      r.tag,
      fmt_bytes(r.st.alloc_space), r.st.alloc_objects,
      fmt_bytes(r.st.freed_space), r.st.freed_objects))
  end
end

local function cmd_leak(agg)
  -- Group retained addrs by site. Sort sites by retained bytes desc, addrs
  -- within a site by addr (already sorted by the aggregator).
  local srows = {}
  for label, info in pairs(agg.leaks) do
    srows[#srows+1] = { label = label, count = info.count, bytes = info.bytes,
                        addrs = info.addrs }
  end
  table.sort(srows, function(a, b)
    if a.bytes ~= b.bytes then return a.bytes > b.bytes end
    return a.label < b.label
  end)
  if #srows == 0 then
    io.write("(no retained allocations — every alloc was freed in the stream)\n")
    return
  end
  io.write(("retained sites: %d\n"):format(#srows))
  for i = 1, #srows do
    local r = srows[i]
    io.write(("\n%s  %d addrs  %s\n"):format(
      r.label, r.count, fmt_bytes(r.bytes)))
    for _, e in ipairs(r.addrs) do
      io.write(("  0x%012x  %s\n"):format(e.addr, fmt_bytes(e.size)))
    end
  end
end

local function cmd_survival(agg, limit)
  local rows = aggregate.survival_sites(agg, limit)
  if #rows == 0 then
    io.write("(no allocation sites recorded)\n")
    return
  end
  io.write(("allocated  freed  survivors  survival  class     site\n"))
  for i = 1, #rows do
    local r = rows[i]
    io.write(("%-9d  %-5d  %-9d  %-8s  %-8s  %s\n"):format(
      r.allocated, r.freed, r.survivors,
      ("%.3f"):format(r.survival_rate), r.class, r.label))
  end
  -- Per-cycle summary (alloc/freed/survivor counts by birth cycle).
  local surv = agg.survival
  if surv and surv.cycles then
    local cycs = {}
    for c in pairs(surv.cycles) do cycs[#cycs+1] = c end
    if #cycs > 0 then
      table.sort(cycs)
      io.write(("\nby GC cycle (birth cycle -> alloc/freed/survivors):\n"))
      for _, c in ipairs(cycs) do
        local cm = surv.cycles[c]
        io.write(("  cycle %-6d  alloc=%-8d freed=%-8d survivors=%-8d\n"):format(
          c, cm.allocated, cm.freed, cm.survivors))
      end
    end
  end
end

-- -- arg parsing / dispatch -------------------------------------------------

local function usage()
  io.stderr:write([[
usage: luajit tools/memprof.lua <subcmd> <stream.bin> [limit|out]
subcommands: top | collapsed | summary | leak | survival | pprof
  pprof <stream.bin> [out.pb]   write pprof protobuf to out.pb (or stdout)
]])
end

local function main(arg)
  if not arg or #arg < 2 then
    usage()
    return 1
  end
  local subcmd = arg[1]
  local path = arg[2]
  local limit = tonumber(arg[3])
  local out_path = arg[3]  -- for pprof subcommand (may be "-" or a path)

  local subcmds = { top = true, collapsed = true, summary = true,
                    leak = true, survival = true, pprof = true }
  if not subcmds[subcmd] then
    io.stderr:write(("unknown subcommand: %s\n"):format(subcmd))
    usage()
    return 1
  end

  local f, err = io.open(path, "rb")
  if not f then
    io.stderr:write(("cannot open %s: %s\n"):format(path, err or "unknown"))
    return 1
  end
  local data = f:read("*a")
  f:close()
  if not data or #data == 0 then
    io.stderr:write(("empty stream: %s\n"):format(path))
    return 1
  end

  local parsed = parse.parse(data)
  local agg = aggregate.aggregate(parsed)

  if subcmd == "top" then
    cmd_top(agg, limit)
  elseif subcmd == "collapsed" then
    cmd_collapsed(agg)
  elseif subcmd == "summary" then
    cmd_summary(agg)
  elseif subcmd == "leak" then
    cmd_leak(agg)
  elseif subcmd == "survival" then
    cmd_survival(agg, limit)
  elseif subcmd == "pprof" then
    cmd_pprof(agg, out_path)
  end
  return 0
end

-- Run as a script; allow `require("tools.memprof")` to import without
-- side-effects when loaded as a module (e.g. from the test).
if arg and arg[0] and arg[0]:match("memprof%.lua$") then
  local rc = main(arg)
  os.exit(rc or 0)
end

return { main = main, parse = parse, aggregate = aggregate,
          pprof = pprof, fmt_bytes = fmt_bytes }

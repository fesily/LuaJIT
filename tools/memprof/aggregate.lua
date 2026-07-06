------------------------------------------------------------------------------
-- Memory profiler event aggregator.
--
-- Folds the parsed event stream (tools/memprof/parse.lua) into per-site and
-- per-type statistics in the style of Go pprof's flat/cum dimensions:
--   alloc_space    sum of ALLOC sizes (and ALLOC-via-REALLOC nsize)
--   alloc_objects  count of ALLOC events
--   freed_space    sum of FREE osize (matched by addr)
--   freed_objects  count of FREE events
--   inuse_space    alloc_space - freed_space
--   inuse_objects  alloc_objects - freed_objects
--
-- REALLOC (genuine addr-preserving resize): bills osize to freed_* and nsize
-- to alloc_* at the event's site, so a growing buffer shows up as both alloc
-- and freed churn but a net inuse delta of (nsize - osize). REALLOC is
-- emitted by lj_memprof_emit_realloc ONLY for the true-resize case (osz!=0
-- and nsize!=0); pure alloc/osz==0 and pure free/nsize==0 are emitted as
-- ALLOC/FREE opcodes, so the aggregator handles all three opcodes uniformly.
--
-- PODFREE: aggregate bulk free of POD arena (closures/protos). No addr, no
-- src. Billed to an `INTERNAL:POD` bucket on freed_space/freed_objects and a
-- dedicated podfree_bytes counter.
--
-- leak view: addresses allocated (ALLOC or ALLOC-via-REALLOC) and never freed
-- (by FREE or FREE-via-REALLOC of the same addr) within the stream, grouped
-- by their allocation site. retained size = the last known ALLOC size for
-- that addr. This is the event-stream notion of "retained" — it is NOT the
-- pprof live-heap notion (that is P2 snapshot territory) — it only reflects
-- what the stream observed.
--
-- Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h.
------------------------------------------------------------------------------

local parse = require("tools.memprof.parse")

local M = {}

-- Render a stable site key from an event using the symtab. Wraps parse's
-- site_label so callers do not need to import parse directly.
local function site_key(ev, symtab)
  if ev.op == "PODFREE" then
    return "INTERNAL:POD"
  end
  return parse.site_label(ev.src, ev.src_id, symtab)
end

-- Main entry point. `parsed` is the table returned by parse.parse().
-- Returns:
--   sites:   { [site_label] = {alloc_space=, alloc_objects=, freed_space=,
--                              freed_objects=, inuse_space=, inuse_objects=} }
--   types:   { [type_tag]   = {alloc_space=, alloc_objects=, freed_space=,
--                              freed_objects=} }
--   leaks:   { [site_label] = { [addr] = size, ... } }
--   totals:  { alloc_space, alloc_objects, freed_space, freed_objects,
--              inuse_space, inuse_objects, podfree_bytes, podfree_objects,
--              realloc_count, free_count, podfree_count, event_count }
function M.aggregate(parsed)
  local events = parsed.events
  local symtab = parsed.symtab

  local sites = {}
  local types = {}
  local leaks = {}            -- site -> { addr -> lastsize }
  local live = {}             -- addr -> { site=, size= }  (currently allocated)
  local totals = {
    alloc_space = 0, alloc_objects = 0,
    freed_space = 0, freed_objects = 0,
    inuse_space = 0, inuse_objects = 0,
    podfree_bytes = 0, podfree_objects = 0,
    realloc_count = 0, free_count = 0, podfree_count = 0,
    event_count = #events,
  }

  local function site_tab(label)
    local t = sites[label]
    if not t then
      t = { alloc_space = 0, alloc_objects = 0,
            freed_space = 0, freed_objects = 0,
            inuse_space = 0, inuse_objects = 0 }
      sites[label] = t
    end
    return t
  end
  local function type_tab(tag)
    local t = types[tag]
    if not t then
      t = { alloc_space = 0, alloc_objects = 0,
            freed_space = 0, freed_objects = 0 }
      types[tag] = t
    end
    return t
  end

  for i = 1, #events do
    local ev = events[i]
    local op = ev.op

    if op == "ALLOC" then
      local label = site_key(ev, symtab)
      local st = site_tab(label)
      st.alloc_space = st.alloc_space + ev.size
      st.alloc_objects = st.alloc_objects + 1
      st.inuse_space = st.inuse_space + ev.size
      st.inuse_objects = st.inuse_objects + 1
      totals.alloc_space = totals.alloc_space + ev.size
      totals.alloc_objects = totals.alloc_objects + 1
      totals.inuse_space = totals.inuse_space + ev.size
      totals.inuse_objects = totals.inuse_objects + 1
      -- Type axis: ALLOC carries cls (arena-class). gct is 0 at inline time.
      local tag = parse.cls_name(ev.cls)
      local tt = type_tab(tag)
      tt.alloc_space = tt.alloc_space + ev.size
      tt.alloc_objects = tt.alloc_objects + 1
      -- Track live addr for leak view and FREE matching.
      live[ev.addr] = { site = label, size = ev.size }
      local ls = leaks[label]
      if not ls then ls = {}; leaks[label] = ls end
      ls[ev.addr] = ev.size

    elseif op == "REALLOC" then
      -- True resize (osz!=0, nsize!=0). Bill osize as freed, nsize as alloc
      -- at the event's site. Update live[addr] size.
      totals.realloc_count = totals.realloc_count + 1
      local label = site_key(ev, symtab)
      local st = site_tab(label)
      -- freed side
      st.freed_space = st.freed_space + ev.osize
      st.freed_objects = st.freed_objects + 1
      st.inuse_space = st.inuse_space - ev.osize
      st.inuse_objects = st.inuse_objects - 1
      totals.freed_space = totals.freed_space + ev.osize
      totals.freed_objects = totals.freed_objects + 1
      totals.inuse_space = totals.inuse_space - ev.osize
      totals.inuse_objects = totals.inuse_objects - 1
      -- alloc side
      st.alloc_space = st.alloc_space + ev.nsize
      st.alloc_objects = st.alloc_objects + 1
      st.inuse_space = st.inuse_space + ev.nsize
      st.inuse_objects = st.inuse_objects + 1
      totals.alloc_space = totals.alloc_space + ev.nsize
      totals.alloc_objects = totals.alloc_objects + 1
      totals.inuse_space = totals.inuse_space + ev.nsize
      totals.inuse_objects = totals.inuse_objects + 1
      -- REALLOC type axis: use a synthetic "realloc" tag (the C emitter does
      -- not attach cls/gct to REALLOC records). Counted under both alloc and
      -- freed so per-type churn is visible.
      local tt = type_tab("realloc")
      tt.alloc_space = tt.alloc_space + ev.nsize
      tt.alloc_objects = tt.alloc_objects + 1
      tt.freed_space = tt.freed_space + ev.osize
      tt.freed_objects = tt.freed_objects + 1
      -- Update live/leak tracking for this addr.
      local prev = live[ev.addr]
      if prev then
        -- Remove prior leak entry, then re-add with new size.
        local ls = leaks[prev.site]
        if ls then ls[ev.addr] = nil end
      end
      live[ev.addr] = { site = label, size = ev.nsize }
      local ls = leaks[label]
      if not ls then ls = {}; leaks[label] = ls end
      ls[ev.addr] = ev.nsize

    elseif op == "FREE" then
      totals.free_count = totals.free_count + 1
      local osize = ev.osize
      -- FREE records always carry src_kind=INT (the GC frees, not the user),
      -- so the site is INTERNAL from the event's perspective. But for leak
      -- accounting we want to credit the FREE back to the ORIGINAL alloc
      -- site (which we know from live[addr]). This gives accurate per-site
      -- inuse deltas and a correct leak view.
      local prev = live[ev.addr]
      local label
      if prev then
        label = prev.site
        -- Remove from leak set for that site.
        local ls = leaks[label]
        if ls then ls[ev.addr] = nil end
        live[ev.addr] = nil
      else
        -- FREE without a preceding ALLOC in the stream (alloc happened before
        -- memprof.start). Bill to INTERNAL so it is not lost.
        label = "INTERNAL"
      end
      local st = site_tab(label)
      st.freed_space = st.freed_space + osize
      st.freed_objects = st.freed_objects + 1
      st.inuse_space = st.inuse_space - osize
      if st.inuse_objects > 0 then st.inuse_objects = st.inuse_objects - 1 end
      totals.freed_space = totals.freed_space + osize
      totals.freed_objects = totals.freed_objects + 1
      totals.inuse_space = totals.inuse_space - osize
      if totals.inuse_objects > 0 then totals.inuse_objects = totals.inuse_objects - 1 end
      -- Type axis: FREE carries the precise gct. Use gct_name.
      local tag = parse.gct_name(ev.gct)
      local tt = type_tab(tag)
      tt.freed_space = tt.freed_space + osize
      tt.freed_objects = tt.freed_objects + 1

    elseif op == "PODFREE" then
      totals.podfree_count = totals.podfree_count + 1
      totals.podfree_bytes = totals.podfree_bytes + ev.bytes
      totals.podfree_objects = totals.podfree_objects + ev.cellcount
      local label = "INTERNAL:POD"
      local st = site_tab(label)
      st.freed_space = st.freed_space + ev.bytes
      st.freed_objects = st.freed_objects + 1
      st.inuse_space = st.inuse_space - ev.bytes
      totals.freed_space = totals.freed_space + ev.bytes
      totals.freed_objects = totals.freed_objects + 1
      totals.inuse_space = totals.inuse_space - ev.bytes
      local tt = type_tab("pod")
      tt.freed_space = tt.freed_space + ev.bytes
      tt.freed_objects = tt.freed_objects + 1
    end
  end

  -- Prune empty leak buckets (sites where every alloc was freed).
  local pruned_leaks = {}
  for label, addrs in pairs(leaks) do
    local n = 0
    for _ in pairs(addrs) do n = n + 1 end
    if n > 0 then
      -- Materialize as a list of {addr=, size=} for stable output.
      local list = {}
      for addr, size in pairs(addrs) do
        list[#list+1] = { addr = addr, size = size }
      end
      table.sort(list, function(a, b) return a.addr < b.addr end)
      pruned_leaks[label] = { addrs = list, count = n,
                              bytes = (function()
                                local s = 0
                                for _, e in ipairs(list) do s = s + e.size end
                                return s
                              end)() }
    end
  end

  return {
    sites = sites,
    types = types,
    leaks = pruned_leaks,
    totals = totals,
    symtab = symtab,
  }
end

-- Return a list of {label=, stat=} sorted descending by alloc_space (the
-- `top` view). `limit` caps the number of rows; nil = all.
function M.top_sites(agg, limit)
  local rows = {}
  for label, st in pairs(agg.sites) do
    rows[#rows+1] = { label = label, stat = st }
  end
  table.sort(rows, function(a, b)
    if a.stat.alloc_space ~= b.stat.alloc_space then
      return a.stat.alloc_space > b.stat.alloc_space
    end
    return a.label < b.label
  end)
  if limit and #rows > limit then
    for i = #rows, limit + 1, -1 do rows[i] = nil end
  end
  return rows
end

return M

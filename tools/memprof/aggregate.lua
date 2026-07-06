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
--   leak view: addresses allocated (ALLOC or ALLOC-via-REALLOC) and never freed
-- (by FREE or FREE-via-REALLOC of the same addr) within the stream, grouped
-- by their allocation site. retained size = the last known ALLOC size for
-- that addr. This is the event-stream notion of "retained" — it is NOT the
-- pprof live-heap notion (that is P2 snapshot territory) — it only reflects
-- what the stream observed.
--
-- survival view: buckets ALLOC events by their gc_cycle (the completed-GC-cycle
-- counter stamped into each record at emit time). An object is a "survivor"
-- of its birth cycle if it is still live (not freed by FREE or FREE-via-REALLOC
-- of the same addr) at end of stream. per-site survival_rate =
-- survivors / allocated. Churn sites (temporaries that die every iteration)
-- approach 0; retained/leaked sites approach 1. PODFREE has no addr and does
-- not participate. For v1 streams gc_cycle is always 0, so survival collapses
-- to a single-cycle whole-stream view (still meaningful, just not per-cycle).
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
  local live = {}             -- addr -> { site=, size=, cycle= }  (currently allocated)
  -- survival tracking: per-site alloc counts by birth cycle; survivors counted
  -- post-loop from `live`. freed-by-addr removes the live entry, so whatever
  -- remains live at end-of-stream is a survivor of its birth cycle.
  local surv_alloc = {}       -- site -> { [cycle] = alloc_count }
  local surv_freed = {}       -- site -> { [cycle] = freed_count } (matched by addr)
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
      live[ev.addr] = { site = label, size = ev.size, cycle = ev.gc_cycle or 0 }
      local cyc = ev.gc_cycle or 0
      local sa = surv_alloc[label]
      if not sa then sa = {}; surv_alloc[label] = sa end
      sa[cyc] = (sa[cyc] or 0) + 1
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
        -- The old object (osz) is "freed" for survival accounting at its
        -- birth site/cycle; the REALLOC re-bills nsize as a fresh alloc.
        local sf = surv_freed[prev.site]
        if not sf then sf = {}; surv_freed[prev.site] = sf end
        sf[prev.cycle] = (sf[prev.cycle] or 0) + 1
      end
      live[ev.addr] = { site = label, size = ev.nsize, cycle = ev.gc_cycle or 0 }
      local cyc = ev.gc_cycle or 0
      local sa = surv_alloc[label]
      if not sa then sa = {}; surv_alloc[label] = sa end
      sa[cyc] = (sa[cyc] or 0) + 1
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
        -- Record the freed object against its birth cycle for survival.
        local sf = surv_freed[label]
        if not sf then sf = {}; surv_freed[label] = sf end
        sf[prev.cycle] = (sf[prev.cycle] or 0) + 1
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

  -- Survival-rate computation. Whatever remains in `live` at end-of-stream is
  -- a survivor of its birth cycle. Aggregate per-site and per-cycle.
  local surv_sites = {}   -- label -> { allocated=, freed=, survivors=, survival_rate=, by_cycle={...} }
  local surv_cycles = {}  -- cycle -> { allocated=, freed=, survivors= }
  -- Seed per-site and per-cycle alloc/freed from the counters built in-loop.
  for label, cycmap in pairs(surv_alloc) do
    local st = { allocated = 0, freed = 0, survivors = 0,
                 survival_rate = 0, by_cycle = {} }
    for cyc, n in pairs(cycmap) do
      st.allocated = st.allocated + n
      st.by_cycle[cyc] = { allocated = n, freed = 0, survivors = 0 }
      local cm = surv_cycles[cyc]
      if not cm then cm = { allocated = 0, freed = 0, survivors = 0 }
                 surv_cycles[cyc] = cm end
      cm.allocated = cm.allocated + n
    end
    local sf = surv_freed[label]
    if sf then
      for cyc, n in pairs(sf) do
        st.freed = st.freed + n
        local bc = st.by_cycle[cyc]
        if bc then bc.freed = n else st.by_cycle[cyc] = { allocated = 0, freed = n, survivors = 0 } end
        local cm = surv_cycles[cyc]
        if not cm then cm = { allocated = 0, freed = 0, survivors = 0 }
                   surv_cycles[cyc] = cm end
        cm.freed = cm.freed + n
      end
    end
    surv_sites[label] = st
  end
  -- Also seed freed-only sites (FREE without preceding ALLOC in-stream: the
  -- object was allocated before memprof.start). Bill to INTERNAL cycle 0.
  for label, sf in pairs(surv_freed) do
    if not surv_sites[label] then
      local st = { allocated = 0, freed = 0, survivors = 0,
                   survival_rate = 0, by_cycle = {} }
      for cyc, n in pairs(sf) do
        st.freed = st.freed + n
        st.by_cycle[cyc] = { allocated = 0, freed = n, survivors = 0 }
        local cm = surv_cycles[cyc]
        if not cm then cm = { allocated = 0, freed = 0, survivors = 0 }
                   surv_cycles[cyc] = cm end
        cm.freed = cm.freed + n
      end
      surv_sites[label] = st
    end
  end
  -- Count survivors from `live` (addrs still allocated at end of stream).
  for addr, info in pairs(live) do
    local st = surv_sites[info.site]
    if not st then
      st = { allocated = 0, freed = 0, survivors = 0, survival_rate = 0,
             by_cycle = {} }
      surv_sites[info.site] = st
    end
    st.survivors = st.survivors + 1
    local bc = st.by_cycle[info.cycle]
    if not bc then bc = { allocated = 0, freed = 0, survivors = 0 }
                   st.by_cycle[info.cycle] = bc end
    bc.survivors = bc.survivors + 1
    local cm = surv_cycles[info.cycle]
    if not cm then cm = { allocated = 0, freed = 0, survivors = 0 }
               surv_cycles[info.cycle] = cm end
    cm.survivors = cm.survivors + 1
  end
  -- Finalize survival_rate per site (guard against divide-by-zero).
  for label, st in pairs(surv_sites) do
    if st.allocated > 0 then
      st.survival_rate = st.survivors / st.allocated
    else
      st.survival_rate = 0
    end
  end

  return {
    sites = sites,
    types = types,
    leaks = pruned_leaks,
    totals = totals,
    symtab = symtab,
    survival = { sites = surv_sites, cycles = surv_cycles },
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

-- Classify a survival_rate into a churn/retained band.
--   < 0.10 -> CHURN   (short-lived allocation churn)
--   > 0.90 -> RETAINED (long-lived / potential leak)
--   else   -> MIXED
function M.survival_class(rate)
  if rate < 0.10 then return "CHURN"
  elseif rate > 0.90 then return "RETAINED"
  else return "MIXED" end
end

-- Return per-site survival rows sorted by alloc volume desc (the `survival`
-- view). Each row: { label=, allocated=, freed=, survivors=, survival_rate=,
-- class= }. `limit` caps rows; nil = all. Sites with 0 allocated (freed-only,
-- from pre-stream allocations) are included so orphans are visible.
function M.survival_sites(agg, limit)
  local rows = {}
  local surv = agg.survival
  if not surv then return rows end
  for label, st in pairs(surv.sites) do
    rows[#rows+1] = {
      label = label,
      allocated = st.allocated, freed = st.freed, survivors = st.survivors,
      survival_rate = st.survival_rate,
      class = M.survival_class(st.survival_rate),
    }
  end
  table.sort(rows, function(a, b)
    if a.allocated ~= b.allocated then return a.allocated > b.allocated end
    return a.label < b.label
  end)
  if limit and #rows > limit then
    for i = #rows, limit + 1, -1 do rows[i] = nil end
  end
  return rows
end

return M

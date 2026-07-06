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

-- Render a stable FULL-STACK site key from an event using the symtab: the
-- resolved leaf..root frame labels joined by ";" (the flamegraph collapsed
-- convention). For depth=1 streams this collapses to the leaf label, so
-- legacy single-frame streams keep their original site identity.
local function site_key(ev, symtab)
  if ev.op == "PODFREE" then
    return "INTERNAL:POD"
  end
  return parse.stack_label(ev, symtab, ";")
end

-- Leaf-only label (the v1/v2 single-frame identity). Used for the per-leaf
-- view exposed alongside the full-stack view so top/survival/leak can be
-- sliced either way.
local function leaf_key(ev, symtab)
  if ev.op == "PODFREE" then
    return "INTERNAL:POD"
  end
  if ev.stack and ev.stack[1] then
    return parse.frame_label(ev.stack[1], symtab)
  end
  return parse.site_label(ev.src, ev.src_id, symtab)
end

-- Main entry point. `parsed` is the table returned by parse.parse().
-- Returns:
--   sites:       full-stack keyed { [stack_label] = {alloc_space=, ...} }
--   sites_leaf:  leaf-only keyed { [leaf_label]  = {alloc_space=, ...} }
--   types:       { [type_tag]   = {alloc_space=, alloc_objects=, freed_space=,
--                                  freed_objects=} }
--   leaks:       full-stack keyed { [stack_label] = { addrs=, count=, bytes= } }
--   leaks_leaf:  leaf-only keyed { [leaf_label]  = { addrs=, count=, bytes= } }
--   totals:      { alloc_space, alloc_objects, freed_space, freed_objects,
--                  inuse_space, inuse_objects, podfree_bytes, podfree_objects,
--                  realloc_count, free_count, podfree_count, event_count }
--   survival:    { sites = full-stack keyed, sites_leaf = leaf keyed,
--                  cycles = per-cycle }
-- For depth=1 streams the full-stack and leaf labels coincide, so the two
-- views are identical (back-compat with the v1/v2 single-frame aggregator).
function M.aggregate(parsed)
  local events = parsed.events
  local symtab = parsed.symtab

  local sites = {}
  local sites_leaf = {}
  local types = {}
  local leaks = {}             -- full-stack -> { addr -> lastsize }
  local leaks_leaf = {}        -- leaf-only  -> { addr -> lastsize }
  local live = {}              -- addr -> { site=, leaf=, size=, cycle= }
  -- survival tracking: per-site alloc counts by birth cycle; survivors counted
  -- post-loop from `live`. freed-by-addr removes the live entry, so whatever
  -- remains live at end-of-stream is a survivor of its birth cycle.
  local surv_alloc = {}        -- full-stack -> { [cycle] = alloc_count }
  local surv_freed = {}        -- full-stack -> { [cycle] = freed_count }
  local surv_alloc_leaf = {}   -- leaf-only  -> { [cycle] = alloc_count }
  local surv_freed_leaf = {}   -- leaf-only  -> { [cycle] = freed_count }
  local totals = {
    alloc_space = 0, alloc_objects = 0,
    freed_space = 0, freed_objects = 0,
    inuse_space = 0, inuse_objects = 0,
    podfree_bytes = 0, podfree_objects = 0,
    realloc_count = 0, free_count = 0, podfree_count = 0,
    event_count = #events,
  }

  -- Apply a (alloc,freed,inuse) delta tuple to one site map.
  local function apply_stat(map, key, da_s, da_o, df_s, df_o, di_s, di_o)
    local t = map[key]
    if not t then
      t = { alloc_space = 0, alloc_objects = 0,
            freed_space = 0, freed_objects = 0,
            inuse_space = 0, inuse_objects = 0 }
      map[key] = t
    end
    t.alloc_space = t.alloc_space + da_s
    t.alloc_objects = t.alloc_objects + da_o
    t.freed_space = t.freed_space + df_s
    t.freed_objects = t.freed_objects + df_o
    t.inuse_space = t.inuse_space + di_s
    t.inuse_objects = t.inuse_objects + di_o
    return t
  end
  -- Bill the same delta to BOTH the full-stack and leaf-only site maps.
  local function bill(skey, lkey, da_s, da_o, df_s, df_o, di_s, di_o)
    apply_stat(sites, skey, da_s, da_o, df_s, df_o, di_s, di_o)
    apply_stat(sites_leaf, lkey, da_s, da_o, df_s, df_o, di_s, di_o)
  end
  local function leak_add(map, key, addr, size)
    local ls = map[key]
    if not ls then ls = {}; map[key] = ls end
    ls[addr] = size
  end
  local function leak_del(map, key, addr)
    local ls = map[key]
    if ls then ls[addr] = nil end
  end
  -- Bump a per-cycle counter in both the full-stack and leaf survival maps.
  -- `n` is the population-object count this event represents (weight/size);
  -- it defaults to 1 for exact-mode streams where weight == size.
  local function surv_bump(skey, lkey, cyc, map_full, map_leaf, n)
    n = n or 1
    local function bump(map, key)
      local m = map[key]
      if not m then m = {}; map[key] = m end
      m[cyc] = (m[cyc] or 0) + n
    end
    bump(map_full, skey)
    bump(map_leaf, lkey)
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
      local skey = site_key(ev, symtab)
      local lkey = leaf_key(ev, symtab)
      -- v5 sampling: weight = bytes this sample represents; obj = weight/size
      -- = the population-object count. Exact mode: weight == size, obj == 1.
      local w = ev.weight or ev.size
      local obj = w / ev.size
      bill(skey, lkey, w, obj, 0, 0, w, obj)
      totals.alloc_space = totals.alloc_space + w
      totals.alloc_objects = totals.alloc_objects + obj
      totals.inuse_space = totals.inuse_space + w
      totals.inuse_objects = totals.inuse_objects + obj
      -- Type axis: ALLOC carries cls (arena-class). gct is 0 at inline time.
      local tag = parse.cls_name(ev.cls)
      local tt = type_tab(tag)
      tt.alloc_space = tt.alloc_space + w
      tt.alloc_objects = tt.alloc_objects + obj
      -- Track live addr for leak view and FREE matching. Store weight so the
      -- matching FREE bills the same population (keeps inuse non-negative).
      local cyc = ev.gc_cycle or 0
      live[ev.addr] = { site = skey, leaf = lkey, size = ev.size,
			weight = w, cycle = cyc }
      surv_bump(skey, lkey, cyc, surv_alloc, surv_alloc_leaf, obj)
      leak_add(leaks, skey, ev.addr, w)
      leak_add(leaks_leaf, lkey, ev.addr, w)

    elseif op == "REALLOC" then
      -- True resize (osz!=0, nsize!=0). Bill the old object's weight as freed
      -- and the new object (weight=nsize, 1 object) as alloc at the event's
      -- site. REALLOC carries no weight field; the freed side uses the
      -- original ALLOC's weight from live[addr] (exact mode: == osize).
      totals.realloc_count = totals.realloc_count + 1
      local skey = site_key(ev, symtab)
      local lkey = leaf_key(ev, symtab)
      local prev = live[ev.addr]
      local cyc = ev.gc_cycle or 0
      local fw, fobj
      if prev then
        fw = prev.weight
        fobj = prev.weight / prev.size
        leak_del(leaks, prev.site, ev.addr)
        leak_del(leaks_leaf, prev.leaf, ev.addr)
        surv_bump(prev.site, prev.leaf, prev.cycle, surv_freed, surv_freed_leaf, fobj)
      else
        fw = ev.osize
        fobj = 1
      end
      local nw = ev.nsize
      local nobj = 1
      bill(skey, lkey, nw, nobj, fw, fobj, nw - fw, nobj - fobj)
      totals.freed_space = totals.freed_space + fw
      totals.freed_objects = totals.freed_objects + fobj
      totals.inuse_space = totals.inuse_space - fw
      totals.inuse_objects = totals.inuse_objects - fobj
      if totals.inuse_objects < 0 then totals.inuse_objects = 0 end
      totals.alloc_space = totals.alloc_space + nw
      totals.alloc_objects = totals.alloc_objects + nobj
      totals.inuse_space = totals.inuse_space + nw
      totals.inuse_objects = totals.inuse_objects + nobj
      -- REALLOC type axis: synthetic "realloc" tag (C emitter attaches no
      -- cls/gct to REALLOC records). Counted under both alloc and freed so
      -- per-type churn is visible.
      local tt = type_tab("realloc")
      tt.alloc_space = tt.alloc_space + nw
      tt.alloc_objects = tt.alloc_objects + nobj
      tt.freed_space = tt.freed_space + fw
      tt.freed_objects = tt.freed_objects + fobj
      -- Update live/leak tracking for this addr.
      live[ev.addr] = { site = skey, leaf = lkey, size = ev.nsize,
			weight = nw, cycle = cyc }
      surv_bump(skey, lkey, cyc, surv_alloc, surv_alloc_leaf, nobj)
      leak_add(leaks, skey, ev.addr, nw)
      leak_add(leaks_leaf, lkey, ev.addr, nw)

    elseif op == "FREE" then
      totals.free_count = totals.free_count + 1
      local osize = ev.osize
      -- FREE records always carry src_kind=INT (the GC frees, not the user),
      -- so the site is INTERNAL from the event's perspective. But for leak
      -- accounting we want to credit the FREE back to the ORIGINAL alloc
      -- site (which we know from live[addr]). This gives accurate per-site
      -- inuse deltas and a correct leak view.
      -- v5 sampling: bill the original ALLOC's weight/size so the FREE
      -- represents the same population as the ALLOC (inuse stays non-negative
      -- and consistent). The sampled-address set in the C emitter guarantees a
      -- FREE only arrives for a previously-sampled (recorded) addr.
      local prev = live[ev.addr]
      local skey, lkey, fw, fobj
      if prev then
        skey = prev.site
        lkey = prev.leaf
        fw = prev.weight
        fobj = prev.weight / prev.size
        leak_del(leaks, skey, ev.addr)
        leak_del(leaks_leaf, lkey, ev.addr)
        live[ev.addr] = nil
        surv_bump(skey, lkey, prev.cycle, surv_freed, surv_freed_leaf, fobj)
      else
        -- FREE without a preceding ALLOC in the stream (alloc happened before
        -- memprof.start, or an un-sampled object in a corrupted stream). Bill
        -- to INTERNAL with osize/1 so it is not lost. In sampling mode this
        -- branch is unreachable (the C set-gate suppresses un-sampled FREEs).
        skey = "INTERNAL"
        lkey = "INTERNAL"
        fw = osize
        fobj = 1
      end
      local st = apply_stat(sites, skey, 0, 0, fw, fobj, -fw, -fobj)
      if st.inuse_objects < 0 then st.inuse_objects = 0 end
      local stl = apply_stat(sites_leaf, lkey, 0, 0, fw, fobj, -fw, -fobj)
      if stl.inuse_objects < 0 then stl.inuse_objects = 0 end
      totals.freed_space = totals.freed_space + fw
      totals.freed_objects = totals.freed_objects + fobj
      totals.inuse_space = totals.inuse_space - fw
      totals.inuse_objects = totals.inuse_objects - fobj
      if totals.inuse_objects < 0 then totals.inuse_objects = 0 end
      -- Type axis: FREE carries the precise gct. Use gct_name.
      local tag = parse.gct_name(ev.gct)
      local tt = type_tab(tag)
      tt.freed_space = tt.freed_space + fw
      tt.freed_objects = tt.freed_objects + fobj

    elseif op == "PODFREE" then
      totals.podfree_count = totals.podfree_count + 1
      totals.podfree_bytes = totals.podfree_bytes + ev.bytes
      totals.podfree_objects = totals.podfree_objects + ev.cellcount
      local label = "INTERNAL:POD"
      bill(label, label, 0, 0, ev.bytes, 1, -ev.bytes, 0)
      totals.freed_space = totals.freed_space + ev.bytes
      totals.freed_objects = totals.freed_objects + 1
      totals.inuse_space = totals.inuse_space - ev.bytes
      local tt = type_tab("pod")
      tt.freed_space = tt.freed_space + ev.bytes
      tt.freed_objects = tt.freed_objects + 1
    end
  end

  -- Materialize a pruned leak view (drop empty buckets; sorted addr list).
  local function prune_leaks(map)
    local out = {}
    for label, addrs in pairs(map) do
      local n = 0
      for _ in pairs(addrs) do n = n + 1 end
      if n > 0 then
        local list = {}
        for addr, size in pairs(addrs) do
          list[#list+1] = { addr = addr, size = size }
        end
        table.sort(list, function(a, b) return a.addr < b.addr end)
        local bytes = 0
        for _, e in ipairs(list) do bytes = bytes + e.size end
        out[label] = { addrs = list, count = n, bytes = bytes }
      end
    end
    return out
  end
  local pruned_leaks = prune_leaks(leaks)
  local pruned_leaks_leaf = prune_leaks(leaks_leaf)

  -- Survival-rate computation. Whatever remains in `live` at end-of-stream is
  -- a survivor of its birth cycle. Build the per-site/per-cycle tables for a
  -- given (surv_alloc, surv_freed, live-key) axis, returning {sites=,cycles=}.
  -- `key_field` is "site" (full-stack) or "leaf" (leaf-only).
  local function build_survival(sa_map, sf_map, key_field)
    local surv_sites = {}
    local surv_cycles = {}
    for label, cycmap in pairs(sa_map) do
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
      local sf = sf_map[label]
      if sf then
        for cyc, n in pairs(sf) do
          st.freed = st.freed + n
          local bc = st.by_cycle[cyc]
          if bc then bc.freed = n
          else st.by_cycle[cyc] = { allocated = 0, freed = n, survivors = 0 } end
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
    for label, sf in pairs(sf_map) do
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
    -- v5 sampling: each surviving sampled object represents weight/size
    -- population objects, so survivors/allocated stay on the same scale.
    for addr, info in pairs(live) do
      local label = info[key_field]
      local st = surv_sites[label]
      if not st then
        st = { allocated = 0, freed = 0, survivors = 0, survival_rate = 0,
               by_cycle = {} }
        surv_sites[label] = st
      end
      local sobj = info.weight / info.size
      st.survivors = st.survivors + sobj
      local bc = st.by_cycle[info.cycle]
      if not bc then bc = { allocated = 0, freed = 0, survivors = 0 }
                     st.by_cycle[info.cycle] = bc end
      bc.survivors = bc.survivors + sobj
      local cm = surv_cycles[info.cycle]
      if not cm then cm = { allocated = 0, freed = 0, survivors = 0 }
                 surv_cycles[info.cycle] = cm end
      cm.survivors = cm.survivors + sobj
    end
    -- Finalize survival_rate per site (guard against divide-by-zero).
    for _, st in pairs(surv_sites) do
      if st.allocated > 0 then
        st.survival_rate = st.survivors / st.allocated
      else
        st.survival_rate = 0
      end
    end
    return { sites = surv_sites, cycles = surv_cycles }
  end
  local surv_full = build_survival(surv_alloc, surv_freed, "site")
  local surv_leaf = build_survival(surv_alloc_leaf, surv_freed_leaf, "leaf")

  return {
    sites = sites,
    sites_leaf = sites_leaf,
    types = types,
    leaks = pruned_leaks,
    leaks_leaf = pruned_leaks_leaf,
    totals = totals,
    symtab = symtab,
    survival = { sites = surv_full.sites, sites_leaf = surv_leaf.sites,
                 cycles = surv_full.cycles },
  }
end

-- Return a list of {label=, stat=} sorted descending by alloc_space (the
-- `top` view). `limit` caps the number of rows; nil = all.
-- `opts.leaf == true` selects the leaf-only view (agg.sites_leaf); the
-- default is the full-stack view (agg.sites).
function M.top_sites(agg, limit, opts)
  opts = opts or {}
  local map = opts.leaf and agg.sites_leaf or agg.sites
  local rows = {}
  for label, st in pairs(map) do
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
-- `opts.leaf == true` selects the leaf-only survival view; default is full-stack.
function M.survival_sites(agg, limit, opts)
  opts = opts or {}
  local rows = {}
  local surv = agg.survival
  if not surv then return rows end
  local map = opts.leaf and surv.sites_leaf or surv.sites
  for label, st in pairs(map) do
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

------------------------------------------------------------------------------
-- memprof_sample_assert.lua — v5 sampling-mode assertions.
--
-- Asserts the byte-accumulator sampling gate (memprof.start{mode="sample",
-- interval=N}) produces a FAR smaller event stream than exact mode for the
-- same workload, while the WEIGHTED alloc totals approximate the true
-- population, and no inuse bucket goes negative (the sampled-address set
-- keeps FREE/REALLOC consistent with sampled ALLOCs).
--
-- Run on a flag-ON binary (LUAJIT_ENABLE_MEMPROF):
--   ./src/luajit -joff test/gc/memprof_sample_assert.lua
--
-- Asserts:
--   (a) sampled ALLOC event count < 25% of exact ALLOC event count.
--   (b) sampled weighted alloc_space (sum of weights) approximates the true
--       GC-object alloc bytes (derived from the exact run, excluding rawbuf
--       cls=0xff) within +/- 25%. The accumulator makes bytes near-exact
--       (only the final < interval carry is lost), so this is generous.
--   (c) NO negative inuse buckets: totals.inuse_space >= 0 and every per-site
--       inuse_space/inuse_objects >= 0 (the set-gate guarantees a FREE only
--       follows a sampled ALLOC, so inuse = weighted_alloc - weighted_freed
--       stays non-negative).
------------------------------------------------------------------------------

local memprof = require("memprof")
local parse = require("tools.memprof.parse")
local aggregate = require("tools.memprof.aggregate")

local EXACT_STREAM   = "/tmp/memprof_sample_assert_exact.bin"
local SAMPLE_STREAM  = "/tmp/memprof_sample_assert_sample.bin"
local INTERVAL       = 64 * 1024   -- 64 KB sample period

-- A large, deterministic GC-object workload. Allocates N strings (~160 B each
-- after the GCstr header + cell rounding) and M small tables, keeping a
-- fraction retained and letting the rest be collectable. Total GC-object
-- allocation is several MB, so the 64 KB interval is crossed many times
-- (~tens of samples) and the final carry is a tiny fraction of the total.
local N_STRINGS = 20000
local M_TABLES  = 4000
local KEEP_EVERY = 50   -- retain 1 string per KEEP_EVERY (long-lived set)

local failures = 0
local checks = 0
local function check(cond, msg)
  checks = checks + 1
  if not cond then
    failures = failures + 1
    io.write("  FAIL: ", msg, "\n")
  end
end

-- The workload, parameterized only so both runs execute identical code. The
-- `keep` table holds retained references so a fraction of objects survive the
-- final full GC (exercising the FREE path + inuse accounting).
local function workload()
  local keep = {}
  for i = 1, N_STRINGS do
    local s = string.rep("x", 128) .. tostring(i)
    if i % KEEP_EVERY == 0 then keep[#keep+1] = s end
  end
  for i = 1, M_TABLES do
    local t = {}
    t[1] = i; t[2] = i + 1; t[3] = i + 2
  end
  collectgarbage("collect")   -- free the un-retained objects (emits FREEs)
  collectgarbage("collect")   -- second pass to settle sweep
  return keep
end

-- Profile one run and return (parsed, agg, alloc_event_count).
local function profile_run(stream, mode, interval)
  collectgarbage("collect")
  collectgarbage("collect")
  local ok, err
  if mode == "sample" then
    ok, err = pcall(memprof.start, { mode = "sample", interval = interval,
                                     out = stream, depth = 1 })
  else
    ok, err = pcall(memprof.start, { mode = "exact", out = stream, depth = 1 })
  end
  check(ok, "memprof.start " .. mode .. " succeeded (" .. tostring(err) .. ")")
  if not ok then return nil, nil, 0 end
  workload()
  memprof.stop()
  local f = io.open(stream, "rb")
  check(f ~= nil, mode .. " stream file opened")
  if not f then return nil, nil, 0 end
  local data = f:read("*a")
  f:close()
  check(#data > 5, mode .. " stream non-empty (" .. #data .. " bytes)")
  local parsed = parse.parse(data)
  local agg = aggregate.aggregate(parsed)
  local n_alloc = 0
  for _, ev in ipairs(parsed.events) do
    if ev.op == "ALLOC" then n_alloc = n_alloc + 1 end
  end
  return parsed, agg, n_alloc
end

-- -- Run both modes --------------------------------------------------------

local e_parsed, e_agg, e_n_alloc = profile_run(EXACT_STREAM, "exact", 0)
local s_parsed, s_agg, s_n_alloc = profile_run(SAMPLE_STREAM, "sample", INTERVAL)

if e_agg and s_agg then
  -- (a) sampled ALLOC events << exact ALLOC events (< 25%).
  local ratio = s_n_alloc / math.max(e_n_alloc, 1)
  check(ratio < 0.25,
        "sampled ALLOC events < 25% of exact (sampled=" .. s_n_alloc ..
        " exact=" .. e_n_alloc .. " ratio=" .. ("%.3f"):format(ratio) .. ")")

  -- True GC-object alloc bytes: exact-mode alloc_space minus the rawbuf
  -- (cls=0xff) contribution, since sampling suppresses raw-buffer events.
  -- The exact totals.alloc_space sums every ALLOC's size including rawbuf;
  -- subtract the rawbuf type total to get the GC-object true total.
  local e_rawbuf = (e_agg.types["rawbuf"] and e_agg.types["rawbuf"].alloc_space) or 0
  local e_gc_alloc_bytes = e_agg.totals.alloc_space - e_rawbuf
  local s_weighted_bytes = s_agg.totals.alloc_space   -- sum of weights

  -- (b) weighted alloc bytes approximate the true GC-object total (+/- 25%).
  -- The accumulator carries < interval bytes at stop (lost), so the error is
  -- < interval / total, a few percent for a multi-MB workload.
  local lo = e_gc_alloc_bytes * 0.75
  local hi = e_gc_alloc_bytes * 1.25
  check(s_weighted_bytes >= lo and s_weighted_bytes <= hi,
        "sampled weighted alloc_bytes within +/-25%% of true GC total " ..
        "(weighted=" .. s_weighted_bytes .. " true=" .. e_gc_alloc_bytes ..
        " bounds=[" .. math.floor(lo) .. "," .. math.floor(hi) .. "])")

  -- (c) no negative inuse: totals and every per-site bucket.
  check(s_agg.totals.inuse_space >= 0,
        "sampled totals.inuse_space >= 0 (got " .. s_agg.totals.inuse_space .. ")")
  check(s_agg.totals.inuse_objects >= -0.5,
        "sampled totals.inuse_objects >= 0 (got " ..
        ("%.3f"):format(s_agg.totals.inuse_objects) .. ")")
  local neg_sites = 0
  for label, st in pairs(s_agg.sites) do
    if st.inuse_space < -0.5 or st.inuse_objects < -0.5 then
      neg_sites = neg_sites + 1
    end
  end
  check(neg_sites == 0,
        "no per-site negative inuse in sampled stream (" .. neg_sites .. " bad)")

  -- Also verify inuse == weighted_alloc - weighted_freed for totals
  -- (the core consistency invariant, within float rounding).
  local inuse_computed = s_agg.totals.alloc_space - s_agg.totals.freed_space
  check(math.abs(inuse_computed - s_agg.totals.inuse_space) < 1.0,
        "sampled inuse_space == alloc_space - freed_space (computed=" ..
        ("%.1f"):format(inuse_computed) .. " stored=" ..
        ("%.1f"):format(s_agg.totals.inuse_space) .. ")")

  -- Sanity: the sampled stream version is 5 and weights are present on ALLOCs.
  check(s_parsed.version == 5 or s_parsed.version == 6 or s_parsed.version == 7, "sampled stream version 5, 6 or 7 (got " ..
        tostring(s_parsed.version) .. ")")
  local n_weighted = 0
  for _, ev in ipairs(s_parsed.events) do
    if ev.op == "ALLOC" and ev.weight then n_weighted = n_weighted + 1 end
  end
  check(n_weighted == s_n_alloc,
        "every sampled ALLOC carries a weight field (" .. n_weighted ..
        " of " .. s_n_alloc .. ")")

  io.write(("  exact:   alloc_events=%d gc_alloc_bytes=%d total_events=%d\n"):
           format(e_n_alloc, e_gc_alloc_bytes, #e_parsed.events))
  io.write(("  sampled: alloc_events=%d weighted_bytes=%d total_events=%d (interval=%d)\n"):
           format(s_n_alloc, s_weighted_bytes, #s_parsed.events, INTERVAL))
end

io.write("memprof_sample_assert: ", checks, " checks, ", failures, " failures\n")
if failures > 0 then os.exit(1) end

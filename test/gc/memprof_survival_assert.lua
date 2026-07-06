-- memprof_survival_assert.lua — survival-rate analysis assertions.
--
-- The survival metric distinguishes short-lived allocation churn from real
-- leaks: of the objects a site allocated, what fraction is still live at end
-- of stream? Churn sites (temporaries that die every iteration) approach 0;
-- retained/leaked sites approach 1.
--
-- Workload: two distinct LFUNC sites.
--   (i)  churn_site  — allocates unique strings into a local, drops them on
--                      return; collectgarbage("collect") between batches frees
--                      them across GC cycles -> survival ~0.
--   (ii) retain_site — allocates unique strings into a pre-sized global table
--                      that persists across all cycles -> survival ~1.
--
-- Assert: churn survival < 0.2, retain survival > 0.8.
--
-- Run: ./src/luajit -joff test/gc/memprof_survival_assert.lua

local memprof = require("memprof")
local parse = require("tools.memprof.parse")
local aggregate = require("tools.memprof.aggregate")

local STREAM = "/tmp/memprof_survival_assert.bin"

local failures = 0
local checks = 0
local function check(cond, msg)
  checks = checks + 1
  if not cond then
    failures = failures + 1
    io.write("  FAIL: ", msg, "\n")
  end
end

-- Pre-size the retained table BEFORE profiling so retain_site never triggers
-- a table realloc (a realloc would bill freed+alloc at the retain site and
-- distort the survival ratio). Filling with false reserves the array part.
local RETAIN_TOTAL = 400
local retained = {}
for i = 1, RETAIN_TOTAL do retained[i] = false end

local BATCHES = 4
local PER_BATCH = 100

-- CHURN site: allocate unique strings, drop them on return. Allocated at the
-- line below; attribution -> this function's proto (LFUNC).
local function churn_site(batch)
  local drop
  for i = 1, PER_BATCH do
    drop = "churn_" .. batch .. "_" .. i .. "_" .. string.rep("x", i % 31)
  end
  return drop  -- last one escapes the loop but dies when caller drops it
end

-- RETAIN site: allocate unique strings, store into the pre-sized global.
local function retain_site(batch, store)
  for i = 1, PER_BATCH do
    local idx = (batch - 1) * PER_BATCH + i
    store[idx] = "retain_" .. batch .. "_" .. i .. "_" .. string.rep("y", i % 29)
  end
end

-- Resolve the site label the aggregator will assign to each function: the
-- symtab maps proto_ptr -> {chunkname=, firstline=}, and site_label renders
-- "<chunkname>:<firstline>". debug.getinfo "S" gives source + linedefined,
-- which match chunkname + firstline.
local function site_label_of(fn)
  local info = debug.getinfo(fn, "S")
  return info.source .. ":" .. info.linedefined
end
local churn_label = site_label_of(churn_site)
local retain_label = site_label_of(retain_site)

-- -- Run the workload under the profiler -----------------------------------

local ok, err = pcall(function()
  memprof.start{ mode = "event", depth = 1, out = STREAM }
end)
check(ok, "memprof.start succeeded (" .. tostring(err) .. ")")
if not ok then
  io.write("memprof_survival_assert: ", checks, " checks, ", failures, " failures\n")
  os.exit(failures == 0 and 0 or 1)
end

for batch = 1, BATCHES do
  churn_site(batch)            -- churn: alloc + drop
  retain_site(batch, retained) -- retain: alloc + keep
  -- Full GC between batches: advances g->gc.stats.cycles and frees the churn
  -- objects from this and earlier batches (they are unreferenced).
  collectgarbage("collect")
end
-- Final collects to ensure the last batch's churn objects are swept before
-- stop (otherwise they'd still be live and inflate churn survival).
collectgarbage("collect")
collectgarbage("collect")

memprof.stop()

-- -- Parse, aggregate, assert ----------------------------------------------

local f = io.open(STREAM, "rb")
check(f ~= nil, "stream file opened")
if not f then
  io.write("memprof_survival_assert: ", checks, " checks, ", failures, " failures\n")
  os.exit(failures == 0 and 0 or 1)
end
local data = f:read("*a")
f:close()
check(#data > 5, "stream non-empty (" .. #data .. " bytes)")

local parsed = parse.parse(data)
check(parsed.version == 2 or parsed.version == 3,
      "stream version 2 or 3 (got " .. tostring(parsed.version) .. ")")
local agg = aggregate.aggregate(parsed)
local surv = agg.survival
check(surv ~= nil, "aggregate produced survival table")

local churn_info = surv and surv.sites[churn_label]
local retain_info = surv and surv.sites[retain_label]

-- Diagnostic: if a site is missing, dump what we have so the failure is
-- debuggable rather than a bare "nil".
if not churn_info or not retain_info then
  io.write("  site labels expected:\n    churn  = ", churn_label, "\n",
           "    retain = ", retain_label, "\n")
  if surv then
    io.write("  survival sites present:\n")
    for label, st in pairs(surv.sites) do
      io.write("    ", label, "  alloc=", st.allocated,
               " freed=", st.freed, " surv=", st.survivors,
               " rate=", ("%.3f"):format(st.survival_rate), "\n")
    end
  end
end

check(churn_info ~= nil, "churn site found in survival (" .. churn_label .. ")")
check(retain_info ~= nil, "retain site found in survival (" .. retain_label .. ")")

if churn_info then
  check(churn_info.allocated >= BATCHES * PER_BATCH,
        "churn allocated >= " .. (BATCHES*PER_BATCH) ..
        " (got " .. churn_info.allocated .. ")")
  local crate = churn_info.survival_rate
  check(crate < 0.20,
        "churn survival < 0.20 (got " .. ("%.3f"):format(crate) ..
        "; alloc=" .. churn_info.allocated .. " surv=" .. churn_info.survivors .. ")")
end

if retain_info then
  check(retain_info.allocated >= BATCHES * PER_BATCH,
        "retain allocated >= " .. (BATCHES*PER_BATCH) ..
        " (got " .. retain_info.allocated .. ")")
  local rrate = retain_info.survival_rate
  check(rrate > 0.80,
        "retain survival > 0.80 (got " .. ("%.3f"):format(rrate) ..
        "; alloc=" .. retain_info.allocated .. " surv=" .. retain_info.survivors .. ")")
end

-- Sanity: multiple GC cycles were observed (at least 2 distinct birth cycles).
if surv and surv.cycles then
  local ncyc = 0
  for _ in pairs(surv.cycles) do ncyc = ncyc + 1 end
  check(ncyc >= 2, "at least 2 distinct GC cycles observed (got " .. ncyc .. ")")
end

io.write(("OK memprof_survival_assert: churn=%s surv=%.3f | retain=%s surv=%.3f\n"):format(
  churn_label, churn_info and churn_info.survival_rate or -1,
  retain_label, retain_info and retain_info.survival_rate or -1))
io.write("memprof_survival_assert: ", checks, " checks, ", failures, " failures\n")
os.exit(failures == 0 and 0 or 1)

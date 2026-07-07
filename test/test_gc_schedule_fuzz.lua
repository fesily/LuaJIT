-- P1-5: GC schedule fuzzing.
--
-- Borrows from SpiderMonkey gczeal(mode, frequency) and V8 --gc-interval=N:
-- instead of a fixed allocate-then-collect rhythm, drive a normal workload
-- while injecting GC actions (single steps of varying size, full collects,
-- and checkheap probes) at pseudo-random allocation intervals. Time-window
-- bugs -- a write barrier that is correct at step boundary N but not N+1, or
-- an allocation that races the sweep cursor -- only show up under irregular
-- schedules, which a fixed cadence never reaches.
--
-- Deterministic: a fixed seed makes every run identical and reproducible; on
-- a checkheap failure the seed + iteration is printed so the exact schedule
-- can be replayed. Pass a seed as arg[1] to explore other schedules.
--
-- Run: luajit -joff test/test_gc_schedule_fuzz.lua [seed]

local pass, fail = 0, 0
local function check(name, cond, msg)
  if cond then pass = pass + 1
  else fail = fail + 1; io.write("FAIL: "..name..(msg and (": "..msg) or "").."\n") end
end
local has_checkheap = pcall(collectgarbage, "checkheap")

-- Small deterministic LCG so the schedule does not depend on math.random's
-- implementation and replays identically given a seed.
local seed = tonumber(arg and arg[1]) or 0x9e3779b9
local function rnd(n)
  seed = (seed * 1103515245 + 12345) % 2147483648
  return seed % n
end

-- A live working set we mutate throughout, plus a shadow of expected values
-- so any lost/corrupted object is caught independently of checkheap.
local SLOTS = 1500
local live = {}
local shadow = {}
local function fill(i)
  local tag = "slot"..i.."_"..rnd(1000000)
  live[i] = { id = i, tag = tag, kids = { rnd(100), rnd(100), rnd(100) } }
  shadow[i] = tag
end
for i = 1, SLOTS do fill(i) end

collectgarbage("stop")            -- we drive the GC by hand from here
local ITERS = tonumber(arg and arg[2]) or 20000
local worst = 0
for it = 1, ITERS do
  -- Mutator action: replace, drop, or link a random slot.
  local act = rnd(3)
  local i = rnd(SLOTS) + 1
  if act == 0 then
    fill(i)                       -- allocate fresh (replaces old -> garbage)
  elseif act == 1 then
    if live[i] then live[i].kids[(rnd(3)) + 1] = { rnd(100) } end  -- mutate
  else
    local j = rnd(SLOTS) + 1
    if live[i] and live[j] then live[i].link = live[j] end          -- cross-link
  end

  -- GC action at a pseudo-random cadence.
  local g = rnd(10)
  if g < 5 then
    collectgarbage("step", rnd(8) + 1)        -- small step, varied size
  elseif g < 7 then
    collectgarbage("step", 1)                 -- minimal step (tightest window)
  elseif g == 7 then
    collectgarbage("collect")                 -- occasional full cycle
  end

  -- checkheap probe at irregular points; fail loud with the seed to replay.
  if has_checkheap and rnd(7) == 0 then
    local bad = collectgarbage("checkheap")
    if bad ~= 0 then
      io.write(string.format("FAIL: schedule_fuzz checkheap=%d at iter=%d seed=%d\n",
                             bad, it, tonumber(arg and arg[1]) or 0x9e3779b9))
      fail = fail + 1
      break
    end
  end
end
collectgarbage("restart")
collectgarbage("collect")

if fail == 0 then check("schedule_fuzz_clean", true) end

-- After the storm, every slot that still holds a value must be uncorrupted
-- (its tag matches the shadow recorded at fill time). Slots are only ever
-- replaced wholesale, so a mismatch means a live object was corrupted.
local intact = 0
for i = 1, SLOTS do
  if live[i] then
    if live[i].id == i and live[i].tag == shadow[i] then intact = intact + 1
    else check("slot_integrity_"..i, false, "tag mismatch") end
  end
end
check("survivors_intact", intact > 0, "intact="..intact)

io.write(string.format("\nSchedule fuzz (%d iters): %d passed, %d failed\n",
                       ITERS, pass, fail))
if fail > 0 then os.exit(1) end

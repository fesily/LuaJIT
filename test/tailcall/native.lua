local probe = require("tailcallprobe")
local util = require("jit.util")
local function ctail(n) return probe.values(n) end
local function ccall(n) local a = probe.values(n); return a end
local function fftail(n) return math.abs(n) end
local callable = setmetatable({}, {__call = probe.meta_values})
local function metatail(n) return callable(n) end
local function args(n) return n, false, nil end
local function expanded(n) return probe.values(args(n)) end
local function drive(fn, n, iterations)
  local sum = 0
  for i = 1, iterations do
    local a = fn(n)
    if a then sum = sum+a end
  end
  return sum
end

local function check(fn, label, stitching)
  jit.flush()
  local recorded, exits = {}, {}
  local function record(tr, func)
    if func == fn then recorded[tr] = true end
  end
  local function texit(tr) exits[tr] = (exits[tr] or 0)+1 end
  jit.attach(record, "record")
  jit.attach(texit, "texit")
  local before = probe.count()
  assert(drive(fn, 1, 3000) == (stitching and 3003000 or 3000))
  local linked, owners = {}, {}
  for tr in pairs(recorded) do
    local info = util.traceinfo(tr)
    if info and info.linktype == (stitching and "stitch" or "loop") then
      assert(#util.tracemc(tr) > 0, label .. " has no machine code")
      owners[#owners+1] = tr
      if stitching and info.link ~= 0 and util.traceinfo(info.link) then
        linked[info.link] = true
      elseif not stitching then
        linked[tr] = true
      end
    end
  end
  assert(next(linked), label .. " did not compile the exercised call site")
  exits = {}
  assert(drive(fn, stitching and 0 or "-2", 100) == (stitching and 0 or 200))
  local executed = false
  for tr in pairs(linked) do
    if exits[tr] then executed = true end
  end
  jit.attach(record)
  jit.attach(texit)
  assert(executed, label .. " compiled successor did not execute")
  assert(probe.count()-before == (stitching and 3100 or 0),
    label .. " replayed or skipped a C call")
  print("ok native " .. label .. " traces=" .. table.concat(owners, ","))
end
jit.off(check)
jit.opt.start("hotloop=3", "hotexit=2", "minstitch=0")
check(ctail, "C tail", true)
check(ccall, "ordinary C", true)
check(metatail, "C __call tail", true)
check(expanded, "C CALLMT", true)
check(fftail, "fast tail", false)
jit.off()

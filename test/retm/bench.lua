local probe = require("retmprobe")
local mode, n = arg[1], tonumber(arg[2]) or 5000000
local warmcount = tonumber(arg[3]) or 1000000
jit.opt.start("hotloop=10", "minstitch=0")
local function fixed(x) return probe.values(x) end
local function vararg(...) return probe.values(...) end
local function ordinary(count)
  local sum = 0
  for i = 1, count do
    local a = probe.values(1)
    sum = sum+a
  end
  return sum
end
local function tailfixed(count)
  local sum = 0
  for i = 1, count do
    local a = fixed(1)
    sum = sum+a
  end
  return sum
end
local function tailvararg(count)
  local sum = 0
  for i = 1, count do
    local a = vararg(1)
    sum = sum+a
  end
  return sum
end
-- Same tailc workload as test/hotpath/bench2.lua in the review worktree.
local function bench2(count)
  local gi = gcinfo
  local function hastag(tag) return gi() end
  local sum, t = 0, {1, 2, 3, 4, 5, 6, 7, 8}
  for i = 1, count do
    if hastag("x") then
      local a = i*0.5
      local b = a*a-i
      for j = 1, #t do b = b+t[j]*j end
      sum = sum+(b%7)+(i%3)*2
    end
  end
  return sum
end
local fn = mode == "ordinary" and ordinary or mode == "fixed" and tailfixed or
           mode == "bench2" and bench2 or tailvararg
local warm = fn(warmcount)
assert(mode == "bench2" or warm == warmcount*1001)
collectgarbage()
if arg[4] == "worker" then
  io.stdout:setvbuf("no")
  print("ready")
  for command in io.lines() do
    if command == "quit" then break end
    local count = assert(tonumber(command))
    collectgarbage()
    local start = os.clock()
    local result = fn(count)
    local elapsed = os.clock()-start
    assert(mode == "bench2" or result == count*1001)
    assert(mode ~= "bench2" or count ~= 20000000 or result == 92499994)
    print("r="..result)
    print(string.format("%.9f", elapsed))
  end
  return
end
local start = os.clock()
local result = fn(n)
local elapsed = os.clock()-start
assert(mode == "bench2" or result == n*1001)
assert(mode ~= "bench2" or n ~= 20000000 or result == 92499994)
print("r="..result)
print(string.format("%.9f", elapsed))

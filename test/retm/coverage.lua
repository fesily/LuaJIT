local probe = require("retmprobe")
local mode = arg[1] or "fixed"
local util = jit and require("jit.util")
local vmdef = jit and require("jit.vmdef")
local target, targetpc, stitched, linked, retf, retfv = nil, nil, 0, 0, 0, 0
if jit then
  jit.opt.start("hotloop=3", "hotexit=2", "minstitch=0")
  jit.attach(function(what, tr, fn, pc, parent, exit)
    if what == "start" and parent and exit == -1 then
      target, targetpc = fn, pc
    end
  end, "trace")
end
local function fixed(n) return probe.values(n) end
local function vararg(...) return probe.values(...) end
local function args(n) return n, false, nil end
local function expanded(n) return probe.values(args(n)) end
local wrapper = mode == "vararg" and vararg or mode == "expanded" and expanded or fixed
local sum = 0
for i = 1, 5000 do
  local n = i%2 == 0 and 300 or 600
  local a = wrapper(n)
  assert(a == n*1000+1)
  sum = sum+a
end
assert(sum == 2250005000)
if jit then
  jit.off()
  for tr = 1, 1000 do
    local info = util.traceinfo(tr)
    if info then
      if info.linktype == "stitch" then stitched = stitched+1 end
      if info.link and info.link ~= 0 and info.linktype ~= "stitch" then
        linked = linked+1
      end
      for ins = 1, info.nins do
        local _, ot = util.traceir(tr, ins)
        if ot then
          local op = vmdef.irnames:sub(6*math.floor(ot/256)+1,6*math.floor(ot/256)+6)
          if op == "RETF  " then retf = retf+1 end
          if op == "RETFV " then retfv = retfv+1 end
        end
      end
    end
  end
  assert(stitched > 0 and linked > 0, "no stitch/caller link")
  local ins = target and util.funcbc(target, targetpc)
  local op = ins and vmdef.bcnames:sub(6*(ins%256)+1,6*(ins%256)+6) or "unknown"
  print("ok "..mode.." target="..op.." stitches="..stitched.." linked="..linked..
        " RETF="..retf.." RETFV="..retfv.." sum="..sum)
else print("ok "..mode.." PUC sum="..sum) end

local ffi = require("ffi")
ffi.cdef[[ typedef struct { int id; } R; ]]
local errors = 0
local function check(n,c) if not c then errors=errors+1; print("FAIL: "..n) end end

-- A. Live cdata never finalized across many cycles.
do
  local fin = 0
  local keep = {}
  for i=1,80 do local cd=ffi.new("R",{id=i}); ffi.gc(cd,function() fin=fin+1 end); keep[i]=cd end
  for c=1,8 do collectgarbage("collect") end
  check("A_live_not_finalized", fin==0)
  check("A_still_usable", keep[40].id==40)
end

-- B. Resurrection: finalizer stashes the cdata in a global table. It must
--    survive the cycle it was finalized in, and NOT be re-finalized later
--    (ffi.gc clears the finalizer after first run unless re-armed).
do
  local fin = 0
  local stash = {}
  for i=1,60 do
    local cd=ffi.new("R",{id=i})
    ffi.gc(cd,function(self) fin=fin+1; stash[#stash+1]=self end)
  end
  collectgarbage("collect"); collectgarbage("collect")
  check("B_all_finalized_once", fin==60)
  check("B_resurrected_count", #stash==60)
  local intact=0
  for _,cd in ipairs(stash) do if cd.id>=1 and cd.id<=60 then intact=intact+1 end end
  check("B_resurrected_intact", intact==60)
  -- Drop strong refs, collect again: must not double-finalize (finalizer gone).
  stash=nil
  collectgarbage("collect"); collectgarbage("collect")
  check("B_no_double_finalize", fin==60)
end

-- C. Incremental: live cdata across stepped GC.
do
  collectgarbage("stop")
  local fin=0; local keep={}
  for i=1,200 do
    local cd=ffi.new("R",{id=i}); ffi.gc(cd,function() fin=fin+1 end); keep[i]=cd
    if i%10==0 then collectgarbage("step",3) end
  end
  collectgarbage("restart"); collectgarbage("collect")
  check("C_incremental_live_not_finalized", fin==0)
end

print(errors==0 and "OK" or ("FAILED: "..errors))
os.exit(errors==0 and 0 or 1)

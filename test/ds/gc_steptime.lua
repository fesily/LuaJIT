-- DS engine API: time-sliced incremental GC (LJ_DS_ENABLE_GC_STEP_TIME).
-- lua_gc(L, LUA_GCSTEPTIME, ns) sets the budget and returns the previous one;
-- lua_gc(L, LUA_GCSTEP2, 0) runs GC steps until the budget is used up and
-- returns 1 once a cycle completed. Runs on a separate state through the FFI.
local ffi = require("ffi")
ffi.cdef[[
typedef struct lua_State lua_State;
lua_State *luaL_newstate(void);
void luaL_openlibs(lua_State *L);
int luaL_loadstring(lua_State *L, const char *s);
int lua_pcall(lua_State *L, int nargs, int nresults, int errfunc);
int lua_gc(lua_State *L, int what, int data);
void lua_close(lua_State *L);
]]
local C = ffi.C
local LUA_GCCYCLE, LUA_GCSTOP, LUA_GCRESTART, LUA_GCCOLLECT, LUA_GCCOUNT = -1, 0, 1, 2, 3
local LUA_GCSTEPTIME, LUA_GCSTEP2 = 10, 11

local L2 = C.luaL_newstate()
C.luaL_openlibs(L2)

if C.lua_gc(L2, LUA_GCSTEPTIME, 777) == -1 then
  print("skip: LUA_GCSTEPTIME not supported")
  C.lua_close(L2)
  return
end
assert(C.lua_gc(L2, LUA_GCSTEPTIME, 0) == 777, "GCSTEPTIME returns the old budget")

local function run(src)
  assert(C.luaL_loadstring(L2, src) == 0)
  assert(C.lua_pcall(L2, 0, 0, 0) == 0)
end

C.lua_gc(L2, LUA_GCSTOP, 0)
run[[
  local live = {}
  for i = 1, 200000 do live[i] = { i, tostring(i) } end
  _G.live = live
  finalized = 0
]]

-- Leaves garbage (incl. userdata with __gc) and runs one time-sliced cycle
-- from GCSpause. Returns the number of LUA_GCSTEP2 calls it took.
local function sliced_cycle(budget_ns)
  C.lua_gc(L2, LUA_GCCOLLECT, 0)
  C.lua_gc(L2, LUA_GCSTOP, 0)
  run[[
    local g = {}
    for i = 1, 200000 do g[i % 1000 + 1] = { i, {} } end
    for i = 1, 100 do
      local p = newproxy(true)
      getmetatable(p).__gc = function() finalized = finalized + 1 end
    end
  ]]
  C.lua_gc(L2, LUA_GCRESTART, 0)
  local before = C.lua_gc(L2, LUA_GCCOUNT, 0)
  local cyc = C.lua_gc(L2, LUA_GCCYCLE, 0)
  local calls = 0
  repeat
    C.lua_gc(L2, LUA_GCSTEPTIME, budget_ns)
    local r = C.lua_gc(L2, LUA_GCSTEP2, 0)
    assert(r == 0 or r == 1, "GCSTEP2 returns 0 or 1")
    calls = calls + 1
    assert(calls < 1000000, "time-sliced cycle never completes")
  until r == 1
  assert((C.lua_gc(L2, LUA_GCCYCLE, 0) - cyc) % 256 >= 1, "cycle counter")
  assert(C.lua_gc(L2, LUA_GCCOUNT, 0) < before, "cycle collected nothing")
  return calls
end

-- A small budget slices the cycle into several calls; it still finishes,
-- frees the garbage and runs the finalizers.
assert(sliced_cycle(200000) > 1, "200us budget did not slice the cycle")
run[[ assert(finalized == 100, "finalizers ran: " .. finalized) ]]

-- A budget far above the cycle cost finishes it in one call.
assert(sliced_cycle(2000000000) == 1, "2s budget did not finish in one call")
run[[ assert(finalized == 200, "finalizers ran: " .. finalized) ]]

C.lua_close(L2)

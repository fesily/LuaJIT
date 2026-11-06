-- Shared benchmark cases for bench.lua / workload.lua.
-- Integer cases stay inside int32 for every iteration; see README.md.
return {
  int1 = {"local s = 0 for i = 1, n do s = s + 1 end return s", 1000000, 100},
  int2 = {"local s = 0 for i = 1, n do s = s + i end return s", 40000, 2500},
  fp1 = {"local x = 0.5 for i = 1, n do x = x * 0.999999 + 0.5 end return x",
	 1000000, 100},
}

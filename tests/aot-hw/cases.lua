-- Shared benchmark cases for bench.lua / workload.lua.
--
-- Integer cases stay inside int32 for every iteration ([-2^31, 2^31-1]):
-- the interpreter and the AOT both switch to a double result on int32
-- overflow, which would silently turn the case into a float benchmark.
return {
  -- ADD only, one op per iteration.
  int1 = {"local s = 0 for i = 1, n do s = s + 1 end return s", 1000000, 100},
  -- ADD, s stays <= 800020000 (< 2^31).
  int2 = {"local s = 0 for i = 1, n do s = s + i end return s", 40000, 2500},
  -- Pure double chain: mul + add.
  fp1 = {"local x = 0.5 for i = 1, n do x = x * 0.999999 + 0.5 end return x",
	 1000000, 100},
  -- MOD int fast path: s stays in [0, 6], s + i <= 1000006.
  modi = {"local s = 0 for i = 1, n do s = (s + i) % 7 end return s",
	  1000000, 100},
  -- MOD float path: all doubles, values stay small.
  modf = {"local x = 0.5 for i = 1, n do x = (x + 0.25) % 1.5 end return x",
	  1000000, 100},
  -- POW: always a double through pow(); general operands, finite result.
  powr = {"local x, y = 1.234, 0.789 local a = 0.0 "..
	  "for i = 1, n do a = x ^ y end return a", 1000000, 5},
  -- BAND + BXOR + BSAR + BSHL, all int results.
  bits = {"local h = 1 for i = 1, n do h = (h + i) & 0xffff "..
	  "h = h ~ (h << 3) h = h ~>> 1 end return h", 1000000, 100},
  -- Six dependent int ops per iteration: not bit specific, this is the
  -- straight-line multi-op chain reference case.
  chain = {"local h = 0 for i = 1, n do h = h + 1 h = h + 2 h = h + 3 "..
	   "h = h * 1 h = h - 1 h = h + 4 end return h", 1000000, 100},
  -- ISEQS + JMP per iteration (the KSTR runs once).
  streq = {"local s = 'needle' local c = 0 "..
	   "for i = 1, n do if s == 'needle' then c = c + 1 end end return c",
	   1000000, 100},
}

------------------------------------------------------------------------------
-- memprof_cfunc_assert.lua — asserts C/builtin allocation sites resolve to
-- readable builtin names via ffid -> jit.vmdef.ffnames, NOT raw C:0x<addr> or
-- the unresolvable C:ff<id> fallback.
--
-- Run on a flag-ON binary (LUAJIT_ENABLE_MEMPROF):
--   ./src/luajit -joff test/gc/memprof_cfunc_assert.lua
--
-- This test deliberately does NOT pre-seed package.path. The resolution must
-- work the SAME way the real CLI (tools/memprof.lua) works: parse.lua locates
-- jit.vmdef on its own. If the CLI can't resolve vmdef without caller help,
-- this test FAILS.
--
-- Asserts:
--   (a) the top site is EXACTLY "string.format" — a fully resolved builtin
--       name, not "C:ff89" (fallback) and not "C:0x<addr>" (old pointer path).
--   (b) NO site renders as "C:0x<hex>" or "C:ff<digits>" — every CFUNC site
--       either resolves to a real name or is the generic "C" (ffid==1).
------------------------------------------------------------------------------

local memprof = require("memprof")
local parse = require("tools.memprof.parse")
local aggregate = require("tools.memprof.aggregate")

local STREAM = "/tmp/memprof_cfunc_assert.bin"

local N = 4000

memprof.start{mode="event", depth=1, out=STREAM}

-- string.format allocates a result string inside the fast function; the
-- level-0 frame is the CFUNC (ffid = string.format's fast-function id), so
-- the result-string ALLOC attributes to CFUNC with that ffid.
local t = {}
for i = 1, N do
  t[i] = string.format("item_%d_%s", i, "x")
end

memprof.stop()

local f = io.open(STREAM, "rb")
assert(f, "stream not written")
local data = f:read("*a")
f:close()
os.remove(STREAM)
assert(#data > 5, "stream too short: " .. #data)

local parsed = parse.parse(data)
local agg = aggregate.aggregate(parsed)

local top = aggregate.top_sites(agg)
assert(#top > 0, "no sites aggregated")

-- (a) top site is the EXACT resolved builtin name.
local top_label = top[1].label
assert(top_label == "string.format",
  ("(a) top site is not 'string.format' (got %q) — vmdef did not resolve"):format(
    top_label))

-- (b) no unresolvable CFUNC renderings anywhere.
local all_labels = {}
for _, row in ipairs(top) do
  all_labels[#all_labels+1] = row.label
  assert(not row.label:match("^C:0x"),
    "(b) found C:0x<addr> site: " .. row.label)
  assert(not row.label:match("^C:ff%d"),
    "(b) found unresolvable C:ff<id> site (vmdef fallback): " .. row.label)
end

local labels_str = table.concat(all_labels, ", ")
io.write(("OK memprof_cfunc_assert: sites=%d top=%s labels=[%s]\n"):format(
  #top, top_label, labels_str))

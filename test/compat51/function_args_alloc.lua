-- The implicit 5.1 'arg' table of a vararg function is built only when the
-- function (or a closure nested in it) names 'arg'. Everything else skips the
-- per-call allocation; see FuncState.need_vararg in luajit/src/lj_parse.c.
local has_ffi, ffi = pcall(require, "ffi")
local compat_vararg_n = has_ffi and ffi.abi("compat_vararg_n")

-- KB allocated by `calls` invocations of fn, with the GC stopped and the JIT
-- off (a trace could sink the table and hide the allocation).
local function alloc_kb(fn, calls)
    jit.off()
    collectgarbage()
    collectgarbage("stop")
    local before = collectgarbage("count")
    for i = 1, calls do
        fn(i, 2, 3)
    end
    local grown = collectgarbage("count") - before
    collectgarbage("restart")
    jit.on()
    return grown
end

local CALLS = 20000

-- Class.__call shape (scripts/class.lua): forwards '...', never names 'arg'.
local function ctor(self, a, b, c) return a end
local function class_call(class_tbl, ...)
    return ctor(class_tbl, ...)
end

local function count_only(...)
    return select("#", ...)
end

local function uses_arg(...)
    return arg[1]
end

local unref = alloc_kb(class_call, CALLS)
local unref2 = alloc_kb(count_only, CALLS)
local ref = alloc_kb(uses_arg, CALLS)
assert(ref > 512, "control: referenced 'arg' must allocate, got " .. ref .. " KB")
assert(unref < 64, "unreferenced 'arg' allocated " .. unref .. " KB")
assert(unref2 < 64, "unreferenced 'arg' allocated " .. unref2 .. " KB")

-- Captured two closures deep: the owning function must still build it.
local function deep(...)
    return function()
        return function()
            return arg[2], rawget(arg, "n")
        end
    end
end
do
    local second, n = deep("a", "b", "c")()()
    assert(second == "b")
    if compat_vararg_n then assert(n == 3) else assert(n == nil) end
end

-- Captured from inside nested blocks only.
local function in_block(flag, ...)
    if flag then
        for _ = 1, 1 do
            local get = function() return arg end
            return get()
        end
    end
end
assert(in_block(true, "x")[1] == "x")

-- A nested vararg function naming 'arg' gets its own; the outer one doesn't.
local function outer(...)
    local function inner(...)
        return arg[1]
    end
    return inner("inner"), select("#", ...)
end
do
    local v, count = outer("outer", nil)
    assert(v == "inner" and count == 2)
end

-- Assignment counts as naming it.
local function assign(...)
    arg = { "replaced" }
    return arg[1]
end
assert(assign("orig") == "replaced")

-- Short functions with an expression body.
local short_arg = |...| -> arg
local short_count = |...| -> select("#", ...)
do
    local t = short_arg(1, nil, 3)
    assert(type(t) == "table" and t[1] == 1 and t[3] == 3)
    assert(short_count(1, nil, 3) == 3)
    assert(alloc_kb(short_count, CALLS) < 64)
end

-- Same functions hot under the JIT (the skipped prologue is KPRI + JMP).
do
    local s1, s2, s3 = 0, 0, 0
    for i = 1, 2000 do
        s1 = s1 + class_call(nil, i, 2, 3)
        s2 = s2 + count_only(i, nil, i)
        s3 = s3 + uses_arg(i, 2)
    end
    assert(s1 == 2001000 and s2 == 6000 and s3 == 2001000)
end

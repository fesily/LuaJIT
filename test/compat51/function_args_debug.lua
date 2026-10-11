local has_ffi, ffi = pcall(require, "ffi")
local compat_vararg_n = has_ffi and ffi.abi("compat_vararg_n")

local function assert_compat_argn(value, expected)
    if compat_vararg_n then
        assert(value == expected)
    else
        assert(value == nil)
    end
end

local function get_local(level, target)
    local index = 1
    local match = nil
    while true do
        local name, value = debug.getlocal(level, index)
        if not name then
            return match
        end
        if name == target then
            match = value
        end
        index = index + 1
    end
end

-- All locals named `target` in declaration order: { n = count, [i] = value }.
local function get_locals(level, target)
    local index = 1
    local found = { n = 0 }
    while true do
        local name, value = debug.getlocal(level, index)
        if not name then
            return found
        end
        if name == target then
            found.n = found.n + 1
            found[found.n] = value
        end
        index = index + 1
    end
end

local function inspect_locals(...)
    local packed = { ... }
    local first = arg[1]  -- Naming 'arg' makes the function build the table.
    local function probe()
        local compat_arg = get_local(3, "arg")
        local packed_local = get_local(3, "packed")
        assert(type(compat_arg) == "table")
        assert(compat_arg[1] == "alpha")
        assert(compat_arg[2] == nil)
        assert_compat_argn(rawget(compat_arg, "n"), 3)
        assert(type(packed_local) == "table")
        assert(packed_local[1] == "alpha")
        assert(packed_local[2] == nil)
        assert(select("#", unpack(packed_local, 1, 3)) == 3)
    end
    probe()
    assert(first == "alpha")
end

inspect_locals("alpha", nil, "omega")

-- Nothing names 'arg': the local still exists (same slot and debug name), but
-- its table is never built, so it reads as nil.
local function inspect_unreferenced(...)
    local packed = { ... }
    local function probe()
        local args = get_locals(3, "arg")
        assert(args.n == 1)
        assert(args[1] == nil)
        assert(get_local(3, "packed")[1] == "alpha")
    end
    probe()
end

inspect_unreferenced("alpha", nil, "omega")

local function inspect_shadow(...)
    local arg = { "shadowed" }
    local function probe()
        local shadow = get_local(3, "arg")
        assert(type(shadow) == "table")
        assert(shadow[1] == "shadowed")
        assert(shadow.n == nil)
        -- Only the shadowing local is named, so the implicit 'arg' stays nil.
        local args = get_locals(3, "arg")
        assert(args.n == 2)
        assert(args[1] == nil)
        assert(args[2] == shadow)
    end
    probe()
end

inspect_shadow("unused", nil)
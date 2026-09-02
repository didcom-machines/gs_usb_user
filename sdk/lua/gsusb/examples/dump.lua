#!/usr/bin/env luajit
--[[ Minimal candump-equivalent built on the Lua gsusb SDK.

Structural difference from the Python version: Python's Bus runs a
background listener thread, so its main loop just polls a queue that
fills itself. LuaJIT here has no threads at all (see bus.lua's module
doc), so this loop's own bus:recv() call *is* what reads the device --
there's no thread filling anything in the background.

Also unlike the Python version: Lua's standard library has no SIGINT
handling (no FFI here to reach one, see lua_gsusb.c's module doc), so
there's no graceful "Ctrl-C to stop" cleanup -- the process just
terminates like any other Lua script. bus:close() below only runs on a
normal exit path, not on SIGINT.

usage: luajit dump.lua [bitrate]
]]

-- Makes this script runnable standalone (luajit examples/dump.lua)
-- without needing LUA_PATH set -- Lua only searches package.path as
-- already configured, not the script's own directory, so the sibling
-- ../bus.lua module needs this (same reason sys.path.insert(0, ...)
-- exists in the Python examples).
local script_dir = (arg[0] or ""):match("^(.*/)") or "./"
package.path = script_dir .. "../?.lua;" .. package.path

local Bus = require("bus")

local function format_frame(f)
    local tags = {}
    if f.extended then table.insert(tags, "EFF") end
    if f.rtr then table.insert(tags, "RTR") end
    if f.err then table.insert(tags, "ERR") end
    local tag = #tags > 0 and (" [" .. table.concat(tags, ",") .. "]") or ""

    local hex = {}
    for i = 1, #f.data do
        table.insert(hex, string.format("%02X", f.data:byte(i)))
    end

    return string.format("Frame(id=0x%X%s, len=%d, data=%s)", f.id, tag, #f.data, table.concat(hex, " "))
end

local bitrate = tonumber(arg[1]) or 500000

local bus = Bus.new()
bus:open()
bus:configure(bitrate)
bus:start()
io.stderr:write(string.format("listening at %d bps\n", bitrate))

while true do
    local frame, err = bus:recv(1000)
    if frame then
        print(format_frame(frame))
    elseif err then
        io.stderr:write("recv failed: " .. tostring(err) .. "\n")
        break
    end
    -- err == nil, frame == nil: plain timeout, loop again
end

bus:close()

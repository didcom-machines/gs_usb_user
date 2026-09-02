#!/usr/bin/env luajit
--[[ candump-style event capture

Structural differences from the Python version:

- No PredicateDetector -- Bus:add_detector() already takes a plain
  fn(frame, bus) callback directly (see bus.lua's module doc), so a
  catch-all "match everything" callback needs no wrapper at all, just a
  function that never returns early.

- No argparse in Lua's standard library, so CLI parsing below is a small
  hand-rolled loop instead -- this SDK avoids adding dependencies for
  something this size (same reasoning as bus.lua having no Detector
  hierarchy: keep only the structure actually needed).

- frame.flags is a raw integer here (GSUSB_FRAME_FD/BRS/ESI bits, see
  ../../../../driver/include/gsusb.h) -- the Lua binding doesn't decode
  it into named booleans the way the Python SDK's Frame.is_fd/is_brs/
  is_esi properties do, and doesn't export those bit values as named
  constants either (only the MODE_* ones, see lua_gsusb.c), so this
  checks the bits directly against their gsusb.h values.

- --usb-bus/--usb-addr require bus.lua's Bus.new()/Bus:open() to forward
  them through to ctx:open() -- added there alongside this port, since
  the Python version's Bus already supported the equivalent.

usage: luajit capture.lua [bitrate] [--vid=0xHEX] [--pid=0xHEX]
                           [--usb-bus=N] [--usb-addr=N] [--listen-only]
                           [--channel=NAME]
]]

local script_dir = (arg[0] or ""):match("^(.*/)") or "./"
package.path = script_dir .. "../?.lua;" .. package.path

local bit = require("bit")
local Bus = require("bus")

-- gsusb_frame::flags bits (driver/include/gsusb.h) -- not exported as
-- named constants by lua_gsusb.c, see module doc above.
local GSUSB_FRAME_FD = 0x01
local GSUSB_FRAME_BRS = 0x02

local function parse_args()
    local opts = {
        bitrate = 500000,
        channel = "can0",
        listen_only = false,
    }
    for _, a in ipairs(arg) do
        local key, val = a:match("^%-%-([%w-]+)=(.*)$")
        if key == "vid" then
            opts.vid = tonumber(val)
        elseif key == "pid" then
            opts.pid = tonumber(val)
        elseif key == "usb-bus" then
            opts.usb_bus = tonumber(val)
        elseif key == "usb-addr" then
            opts.usb_addr = tonumber(val)
        elseif key == "channel" then
            opts.channel = val
        elseif a == "--listen-only" then
            opts.listen_only = true
        elseif a:match("^%d+$") then
            opts.bitrate = tonumber(a)
        else
            io.stderr:write("unknown argument: " .. a .. "\n")
            os.exit(1)
        end
    end
    return opts
end

local function format_frame(f, iface)
    local tags = {}
    if f.extended then table.insert(tags, "EFF") end
    if f.rtr then table.insert(tags, "RTR") end
    if f.err then table.insert(tags, "ERR") end
    if bit.band(f.flags, GSUSB_FRAME_FD) ~= 0 then table.insert(tags, "FD") end
    if bit.band(f.flags, GSUSB_FRAME_BRS) ~= 0 then table.insert(tags, "BRS") end
    local tag = #tags > 0 and (" " .. table.concat(tags, ",")) or ""

    local hex = {}
    for i = 1, #f.data do
        table.insert(hex, string.format("%02X", f.data:byte(i)))
    end
    local hexstr = table.concat(hex, " ")

    local ascii = f.data:gsub(".", function(c)
        local b = c:byte()
        return (b >= 32 and b < 127) and c or "."
    end)

    return string.format("(%16.6f) %-6s %08X%s  [%d] %-23s  '%s'",
        f.timestamp_us / 1000000.0, iface, f.id, tag, #f.data, hexstr, ascii)
end

local function main()
    local opts = parse_args()

    local bus = Bus.new({
        vid = opts.vid, pid = opts.pid,
        usb_bus = opts.usb_bus, usb_addr = opts.usb_addr,
    })
    bus:open()
    bus:configure(opts.bitrate)
    bus:add_detector(function(frame, _)
        print(format_frame(frame, opts.channel))
    end)
    bus:start(opts.listen_only)

    io.stderr:write(string.format("listening at %d bps%s\n",
        opts.bitrate, opts.listen_only and " (listen-only)" or ""))

    while true do
        bus:sleep(1)
    end
end

local ok, err = pcall(main)
if not ok then
    io.stderr:write("error: " .. tostring(err) .. "\n")
    os.exit(1)
end

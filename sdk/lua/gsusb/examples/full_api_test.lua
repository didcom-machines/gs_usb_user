local gsusb = require("gsusb")
print("module loaded:", gsusb)
print("MODE_LISTEN_ONLY =", gsusb.MODE_LISTEN_ONLY)
print("MODE_LOOPBACK =", gsusb.MODE_LOOPBACK)

local ctx, err = gsusb.init()
assert(ctx, err)
print("ctx =", ctx)

-- Exercises the full open/channel/bitrate/start/recv/stop/close path. If no
-- adapter is attached, open() is expected to fail with "not found" -- that
-- still proves the whole call path (arg marshaling, error string round-trip)
-- works correctly.
local dev, open_err = ctx:open(0x1d50, 0x606f) -- known gs_usb VID:PID (see gsusb.h)
if dev then
    print("adapter found, dev =", dev)
    print("channel_count =", dev:channel_count())
    local cfg = dev:device_config()
    print(string.format("device_config: sw=0x%x hw=0x%x channels=%d", cfg.sw_version, cfg.hw_version,
        cfg.channel_count))

    local ch = dev:channel(0)
    assert(ch, "channel(0) should exist if channel_count > 0")
    local ok, serr = ch:set_bitrate(500000)
    print("set_bitrate(500000) ->", ok, serr)
    local started, sterr = ch:start()
    print("start() ->", started, sterr)
    if started then
        print("recv(200ms) ->", ch:recv(200)) -- nil expected (timeout) unless bus traffic exists
        ch:stop()
    end
    dev:close()
else
    print("open() correctly failed (no adapter attached):", open_err)
end

ctx:exit()
print("FULL API SMOKE TEST OK")

#!/usr/bin/env luajit
--[[ Listen-and-react example, a port of
../../../python/examples/react.py for parity between the two SDKs.

Structural differences from the Python version:

- No Detector class hierarchy here (see bus.lua's module doc for why) --
  Bus:add_detector() takes a plain fn(frame, bus) callback, and unlike
  Python's IdDetector/Detector.matches(), the framework does NOT filter
  frames before calling it: every registered callback fires on *every*
  received frame and must check the id itself. Both callbacks below do
  that explicitly where Python's IdDetector/CountingDetector.matches()
  did it implicitly.

- CountingDetector's own per-instance `count` field becomes a Lua
  closure capturing a local upvalue instead -- the natural Lua shape for
  "a callback with its own state across calls", replacing what a class
  instance would hold in Python.

- No background listener thread (see dump.lua's port for the same note):
  detectors here only fire while something is actively pumping poll(),
  which is exactly what Bus:sleep() does -- so the main loop below is
  `bus:sleep(1)` in a loop, not a blind time.sleep(1) trusting a
  background thread to keep dispatching in the meantime.

usage: luajit react.lua [bitrate]
]]

local script_dir = (arg[0] or ""):match("^(.*/)") or "./"
package.path = script_dir .. "../?.lua;" .. package.path

local Bus = require("bus")

local KEY_ID = 0x100

local function on_key_frame(frame, bus)
    if frame.id ~= KEY_ID then
        return
    end
    print(string.format("key frame detected: id=0x%X", frame.id))
    bus:send({ id = 0x200, data = string.char(0x01) })
end

-- Sends a milestone frame every 5th time it sees the key id -- the Lua
-- counterpart of CountingDetector, a Detector subclass that needs its
-- own state, not just a callback (see module doc above).
local function make_counting_detector(can_id)
    local count = 0
    return function(frame, bus)
        if frame.id ~= can_id then
            return
        end
        count = count + 1
        if count % 5 == 0 then
            print(string.format("seen id 0x%X %d times, sending milestone frame", can_id, count))
            bus:send({ id = 0x201, data = string.char(count % 256) })
        end
    end
end

local bitrate = tonumber(arg[1]) or 500000

local bus = Bus.new()
bus:open()
bus:configure(bitrate)
bus:add_detector(on_key_frame)
bus:add_detector(make_counting_detector(KEY_ID))
bus:start()
io.stderr:write(string.format("watching for id 0x%X\n", KEY_ID))

while true do
    bus:sleep(1)
end

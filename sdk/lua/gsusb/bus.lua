--[[ Bus: a small convenience wrapper over the raw gsusb Lua/C binding
(lua_gsusb.c) -- the Lua counterpart of this repo's own Python gsusb.bus
module (Bus/Frame/Detector), kept at the same generic, protocol-agnostic
layer for the same reason: this driver knows nothing about IOX/FMX, only
"open an adapter, send/receive raw CAN frames". fmx-can-control's Lua
IOXDevice/IOXBus port is what builds the IOX/FMX object model on top of
this, exactly as its Python counterpart builds on gsusb.Bus/
PredicateDetector.

Frame shape matches the C binding's (and the Python SDK's) field names
directly: {id, extended, rtr, err, data, flags, timestamp_us} -- no
separate Frame class here, a plain table is enough (Lua has no
dataclass-style value-object convention worth adding just for this).

No listener thread: the Python Bus runs a background thread that is the
sole reader of the C library's receive queue, feeding both registered
Detectors and a recv() queue. LuaJIT here has no threads at all, and the
C binding's recv() is a plain blocking call -- so this Bus is pumped
explicitly instead. poll(timeout_ms) is the primitive: one recv() call,
dispatched to every registered callback if a frame arrived. Application
code (or iox_bus.lua/iox_device.lua above this) is responsible for
calling poll() in a loop; nothing happens on its own between calls --
see those modules for how request/response waiting is built out of that
(a plain poll loop with a deadline, the same shape fmx_send.py's
wait_for_response() already used on the Python side, generalized instead
of being reinvented per module).

No Detector object hierarchy either: the Python SDK's IdDetector/
MaskDetector/PredicateDetector exist for matching a subset of traffic at
the Bus level, but every caller in this whole SDK (IOXDevice.open(),
IOXBus.open()) only ever registers a PredicateDetector(lambda f: True,
...) -- "give me everything, I'll filter by node in my own decode()".
So add_detector()/remove_detector() here just take a plain callback
fn(frame, bus) directly; there was no real use of the matches() layer to
port.
]]
local gsusb = require("gsusb")

local Bus = {}
Bus.__index = Bus

-- vid/pid default to the known gs_usb/CANable id (see gsusb.h) -- the
-- only adapter this SDK has ever been tested against; pass your own if
-- you have a different gs_usb-compatible device.
local DEFAULT_VID = 0x1d50
local DEFAULT_PID = 0x606f

function Bus.new(opts)
    opts = opts or {}
    local self = setmetatable({}, Bus)
    self._vid = opts.vid or DEFAULT_VID
    self._pid = opts.pid or DEFAULT_PID
    self._usb_bus = opts.usb_bus  -- nil = "match any", same as ctx:open()'s own default
    self._usb_addr = opts.usb_addr
    self._channel_index = opts.channel or 0
    self._detectors = {}
    self._ctx = nil
    self._dev = nil
    self._ch = nil
    return self
end

-- --- lifecycle ---

function Bus:open()
    if self._ch ~= nil then
        return self
    end
    local ctx, ctx_err = gsusb.init()
    if not ctx then
        error("gsusb.init() failed: " .. tostring(ctx_err))
    end
    -- self._usb_bus/_usb_addr pass through as nil when unset, which
    -- ctx:open()'s underlying luaL_optinteger() treats the same as
    -- omitting the argument (defaults to -1, "match any" -- see
    -- lua_gsusb.c's l_ctx_open()).
    local dev, dev_err = ctx:open(self._vid, self._pid, self._usb_bus, self._usb_addr)
    if not dev then
        ctx:exit()
        error(string.format("no adapter found (vid=0x%04x pid=0x%04x): %s",
            self._vid, self._pid, tostring(dev_err)))
    end
    local ch, ch_err = dev:channel(self._channel_index)
    if not ch then
        dev:close()
        ctx:exit()
        error("channel " .. self._channel_index .. " does not exist on this device: " .. tostring(ch_err))
    end
    self._ctx, self._dev, self._ch = ctx, dev, ch
    return self
end

function Bus:close()
    if self._ch ~= nil then
        self._ch:stop()
        self._ch = nil
    end
    if self._dev ~= nil then
        self._dev:close()
        self._dev = nil
    end
    if self._ctx ~= nil then
        self._ctx:exit()
        self._ctx = nil
    end
end

function Bus:_require_open()
    if self._ch == nil then
        error("Bus is not open -- call open() first")
    end
end

-- --- configuration / start ---

function Bus:configure(bitrate, sample_point)
    self:_require_open()
    assert(self._ch:set_bitrate(bitrate, sample_point or 0))
end

function Bus:start(listen_only)
    self:_require_open()
    assert(self._ch:start(listen_only and gsusb.MODE_LISTEN_ONLY or 0))
end

-- --- send / recv ---

function Bus:send(frame, timeout_ms)
    self:_require_open()
    return assert(self._ch:send(frame, timeout_ms))
end

-- One blocking recv, up to timeout_ms (default 1000). Returns the frame
-- table, or nil on a plain timeout, or nil+errmsg on a real error --
-- same three-way shape as the C binding's ch:recv() (see lua_gsusb.c).
function Bus:recv(timeout_ms)
    self:_require_open()
    return self._ch:recv(timeout_ms or 1000)
end

-- --- detectors: plain fn(frame, bus) callbacks (see module docstring) ---

function Bus:add_detector(fn)
    table.insert(self._detectors, fn)
end

function Bus:remove_detector(fn)
    for i, d in ipairs(self._detectors) do
        if d == fn then
            table.remove(self._detectors, i)
            return
        end
    end
end

-- One recv() call, dispatched to every registered detector if a frame
-- arrived. Returns true if a frame was processed, false on a plain
-- timeout. Raises on a real recv error (mirrors send()/configure()'s
-- assert-on-failure style -- a real I/O error isn't something a caller
-- should have to remember to check for separately).
function Bus:poll(timeout_ms)
    local frame, err = self:recv(timeout_ms)
    if frame == nil then
        if err ~= nil then
            error("gsusb recv failed: " .. tostring(err))
        end
        return false
    end
    for _, fn in ipairs(self._detectors) do
        fn(frame, self)
    end
    return true
end

-- Waits `seconds`, pumping poll() the whole time instead of blocking
-- blindly -- Lua's standard library has no sleep() at all (no FFI here to
-- reach a libc usleep/nanosleep either, see lua_gsusb.c's module
-- docstring on why). Used by IOXDevice:sleep()/IOXBus:sleep()
-- (fmx-can-control) so on_event()/on_change() keep firing during a
-- "sleep" instead of it stopping dispatch for its whole duration.
--
-- CORRECTNESS NOTE (a real bug this shipped with initially): recv()'s
-- timeout_ms is only a real wait when NO frame arrives -- it returns the
-- instant one IS available, same as any select()/poll()-style timeout.
-- On a bus with any regular traffic (heartbeats, another node's status
-- bursts, ...) poll() can return almost immediately, over and over, so
-- chunking fixed-size timeouts and just subtracting them from a
-- countdown (the original implementation here) silently races through
-- the whole requested duration in a fraction of the real time whenever
-- the bus is busy -- exactly backwards from what a "sleep" should
-- guarantee. There is no sub-second wall clock available without FFI on
-- this target (os.clock() measures CPU time, not wall time -- it does
-- NOT advance while blocked in recv(); confirmed empirically: ~0.001s of
-- os.clock() elapsed across a real 2-second wait), so this uses
-- os.time()'s 1-second resolution as the actual authority on elapsed
-- time instead of trusting poll()'s return timing -- meaning a sub-
-- second request rounds up to whichever whole-second boundary comes
-- next (a 0.5s ask can take anywhere from ~0-1s), and any request is
-- accurate to within about a second either way. If sub-second accuracy
-- ever matters, add a real monotonic-clock binding to lua_gsusb.c
-- (clock_gettime(CLOCK_MONOTONIC, ...)) rather than trying to fake one
-- here.
function Bus:sleep(seconds)
    local deadline = os.time() + seconds
    while os.time() < deadline do
        self:poll(200)
    end
end

return Bus

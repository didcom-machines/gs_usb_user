#!/usr/bin/env luajit
--[[ J1939 engine RPM (EEC1 / SPN 190) console monitor.

EEC1 message, PGN 0xF004 (61444), SPN 190 (Engine Speed):
signal starts at bit 24, length 16, little-endian ("Intel"), scale 0.125,
offset 0. High byte of the raw value > 0xFA means "error/not available"
per J1939 convention.

Uses LuaJIT's `bit` library for the bitfield math (bit.band/bor/rshift/
lshift) -- this SDK targets LuaJIT without FFI (see lua_gsusb.c's module
doc), and `bit` is a separate, always-available LuaJIT extension, not
part of FFI, so it works under the same constraint. Lua 5.1's native
operators (this codebase's baseline, see lua_gsusb.c) have no bitwise
operators at all -- those were only added in Lua 5.3.

usage: luajit rpm_monitor.lua [bitrate]
]]

local script_dir = (arg[0] or ""):match("^(.*/)") or "./"
package.path = script_dir .. "../?.lua;" .. package.path

local bit = require("bit")
local Bus = require("bus")

local EEC1_PGN = 0xF004 -- PGN 61444
local SPN190_START_BIT = 24 -- byte-aligned here: byte 3, bit 0
local SPN190_LENGTH = 16
local SPN190_SCALE = 0.125 -- rpm per bit
local SPN190_NOT_AVAILABLE_HIGH_BYTE = 0xFA -- raw high byte above this = error/NA

-- Extracts the PGN from a 29-bit J1939 arbitration id. EEC1 is a
-- broadcast PDU2-format message, so the low byte (source address)
-- doesn't factor into the PGN and is dropped here.
local function pgn_of(can_id)
    return bit.band(bit.rshift(can_id, 8), 0xFFFF)
end

-- Generic little-endian ("Intel") bit-field extraction -- works for any
-- SPN, not just byte-aligned ones like SPN 190 happens to be. `data` is
-- a Lua string (binary-safe, see lua_gsusb.c's frame marshaling note);
-- Lua strings are 1-indexed, hence the +1 on the byte index.
local function extract_intel(data, start_bit, length)
    local value = 0
    for i = 0, length - 1 do
        local pos = start_bit + i
        local byte_idx = math.floor(pos / 8)
        local bit_idx = pos % 8
        if byte_idx < #data then
            local byte = data:byte(byte_idx + 1)
            if bit.band(bit.rshift(byte, bit_idx), 1) == 1 then
                value = bit.bor(value, bit.lshift(1, i))
            end
        end
    end
    return value
end

-- Returns engine RPM if `frame` is an EEC1 message carrying a valid
-- (available) SPN 190 reading, else nil.
local function try_get_engine_speed(frame)
    if not frame.extended or pgn_of(frame.id) ~= EEC1_PGN then
        return nil
    end
    if #frame.data < 6 then
        return nil
    end

    local raw = extract_intel(frame.data, SPN190_START_BIT, SPN190_LENGTH)
    if bit.rshift(raw, 8) > SPN190_NOT_AVAILABLE_HIGH_BYTE then
        return nil -- J1939 error/not-available indicator
    end

    return raw * SPN190_SCALE
end

local bitrate = tonumber(arg[1]) or 250000 -- J1939 standard rate

local bus = Bus.new()
bus:open()
bus:configure(bitrate)
bus:start(true) -- listen_only: pure observer, never ACKs or transmits onto the bus
io.stderr:write(string.format("watching for EEC1 (PGN 0x%04X) engine speed frames at %d bps\n", EEC1_PGN, bitrate))

while true do
    local frame, err = bus:recv(1000)
    if frame then
        local rpm = try_get_engine_speed(frame)
        if rpm then
            print(string.format("RPM: %.1f", rpm))
        end
    elseif err then
        io.stderr:write("recv failed: " .. tostring(err) .. "\n")
        break
    end
end

bus:close()

#!/usr/bin/env python3
"""J1939 engine RPM (EEC1 / SPN 190) console monitor, built on candetect --
works with whichever supported adapter is attached.

EEC1 message, PGN 0xF004 (61444), SPN 190 (Engine Speed):
signal starts at bit 24, length 16, little-endian ("Intel"), scale 0.125,
offset 0. High byte of the raw value > 0xFA means "error/not available"
per J1939 convention.

Note: real J1939 buses run at 250 kbit/s (the default here), but if
candetect ends up opening the gcan backend, its only confirmed-working
rate so far is 500 kbit/s (see driver/gcan_native/README.md) -- configure()
will raise in that case. Pass 500000 explicitly if you're testing against
a 500 kbit/s bench bus with J1939-shaped traffic rather than the real
thing. Also note neither backend's listen-only mode is guaranteed here --
this process ACKs frames on the bus like any other CAN node either way,
but never transmits anything itself since it never calls bus.send().

usage: rpm_monitor.py [bitrate]
"""
import os
import sys
from typing import Optional

# Makes this script runnable standalone (python3 examples/rpm_monitor.py)
# without needing PYTHONPATH set -- Python only puts the script's own
# directory on sys.path, not its parent, so the sibling ../candetect
# package needs this.
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

from candetect import Frame, NoSupportedAdapterError, open_bus

EEC1_PGN = 0xF004  # PGN 61444
SPN190_START_BIT = 24  # byte-aligned here: byte 3, bit 0
SPN190_LENGTH = 16
SPN190_SCALE = 0.125  # rpm per bit
SPN190_NOT_AVAILABLE_HIGH_BYTE = 0xFA  # raw high byte above this = error/NA


def pgn_of(can_id: int) -> int:
    """Extracts the PGN from a 29-bit J1939 arbitration id. EEC1 is a
    broadcast PDU2-format message, so the low byte (source address)
    doesn't factor into the PGN and is dropped here."""
    return (can_id >> 8) & 0xFFFF


def extract_intel(data: bytes, start_bit: int, length: int) -> int:
    """Generic little-endian ("Intel") bit-field extraction -- works for
    any SPN, not just byte-aligned ones like SPN 190 happens to be."""
    value = 0
    for i in range(length):
        bit = start_bit + i
        byte_idx, bit_idx = divmod(bit, 8)
        if byte_idx < len(data) and (data[byte_idx] >> bit_idx) & 1:
            value |= 1 << i
    return value


def try_get_engine_speed(frame: Frame) -> Optional[float]:
    """Returns engine RPM if `frame` is an EEC1 message carrying a valid
    (available) SPN 190 reading, else None."""
    if not frame.is_extended or pgn_of(frame.id) != EEC1_PGN:
        return None
    if len(frame.data) < 6:
        return None

    raw = extract_intel(frame.data, SPN190_START_BIT, SPN190_LENGTH)
    if (raw >> 8) > SPN190_NOT_AVAILABLE_HIGH_BYTE:
        return None  # J1939 error/not-available indicator

    return raw * SPN190_SCALE


def main():
    bitrate = int(sys.argv[1]) if len(sys.argv) > 1 else 250000  # J1939 standard rate

    try:
        bus = open_bus()
    except NoSupportedAdapterError as e:
        print(f"error: {e}", file=sys.stderr)
        sys.exit(1)

    print(f"opened {type(bus).__module__}.{type(bus).__qualname__}", file=sys.stderr)
    with bus:
        bus.configure(bitrate=bitrate)
        bus.start()
        print(f"watching for EEC1 (PGN 0x{EEC1_PGN:04X}) engine speed frames "
              f"at {bitrate} bps, Ctrl-C to stop", file=sys.stderr)
        try:
            while True:
                frame = bus.recv(timeout_ms=1000)
                if frame is None:
                    continue
                rpm = try_get_engine_speed(frame)
                if rpm is not None:
                    print(f"RPM: {rpm:.1f}")
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()

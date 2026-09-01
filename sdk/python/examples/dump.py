#!/usr/bin/env python3
"""Minimal candump-equivalent, built on candetect -- works with whichever
supported adapter is attached (gs_usb-protocol, or the GCAN/ECAN/Rexon
USBCANI-V503), no code change needed either way.

usage: dump.py [bitrate]

bitrate defaults to 500000, which both backends can do. Passing anything
else only works if a gsusb-protocol adapter is what gets opened -- the
gcan backend only has a confirmed-working init sequence for 500000 (see
driver/gcan_native/README.md) and raises for anything else.
"""
import os
import sys

# Makes this script runnable standalone (python3 examples/dump.py) without
# needing PYTHONPATH set -- Python only puts the script's own directory on
# sys.path, not its parent, so the sibling ../candetect package needs this.
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

from candetect import NoSupportedAdapterError, open_bus


def main():
    bitrate = int(sys.argv[1]) if len(sys.argv) > 1 else 500000

    try:
        bus = open_bus()
    except NoSupportedAdapterError as e:
        print(f"error: {e}", file=sys.stderr)
        sys.exit(1)

    print(f"opened {type(bus).__module__}.{type(bus).__qualname__}", file=sys.stderr)
    with bus:
        bus.configure(bitrate=bitrate)
        bus.start()
        print(f"listening at {bitrate} bps, Ctrl-C to stop", file=sys.stderr)
        try:
            while True:
                frame = bus.recv(timeout_ms=1000)
                if frame is not None:
                    print(frame)
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()

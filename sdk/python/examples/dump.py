#!/usr/bin/env python3
"""Minimal candump-equivalent built on the gsusb Python SDK.

usage: dump.py [bitrate]
"""
import sys

from gsusb import Bus


def main():
    bitrate = int(sys.argv[1]) if len(sys.argv) > 1 else 500000
    with Bus() as bus:
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

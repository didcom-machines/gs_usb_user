#!/usr/bin/env python3
"""candump-style event capture built on the gsusb Python SDK.

Uses the Detector object pattern (a catch-all PredicateDetector) rather
than the polling recv() loop shown in dump.py -- the Bus's listener thread
calls our callback for every frame as it arrives, and we just format and
echo it, so this is a true event listener rather than a poll loop.

usage: capture.py [bitrate] [--vid=0xHEX --pid=0xHEX] [--listen-only]
"""
import argparse
import os
import sys
import time

# Makes this script runnable standalone (python3 examples/capture.py)
# without needing PYTHONPATH set -- Python only puts the script's own
# directory on sys.path, not its parent, so the sibling ../gsusb package
# needs this.
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

from gsusb import Bus, Frame, GsusbError, PredicateDetector


def auto_int(text: str) -> int:
    return int(text, 0)  # accepts "0x1D50" as well as plain decimal


def parse_args():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("bitrate", type=int, nargs="?", default=500000)
    p.add_argument("--vid", type=auto_int, default=None, help="USB vendor id, e.g. 0x1D50")
    p.add_argument("--pid", type=auto_int, default=None, help="USB product id, e.g. 0x606F")
    p.add_argument("--usb-bus", type=int, default=None)
    p.add_argument("--usb-addr", type=int, default=None)
    p.add_argument("--listen-only", action="store_true",
                    help="don't ACK frames on the bus (safe default for pure monitoring)")
    p.add_argument("--channel", "-i", default="can0", help="label shown in the printed lines")
    return p.parse_args()


def format_frame(frame: Frame, iface: str) -> str:
    tags = []
    if frame.is_extended: tags.append("EFF")
    if frame.is_rtr: tags.append("RTR")
    if frame.is_error: tags.append("ERR")
    if frame.is_fd: tags.append("FD")
    if frame.is_brs: tags.append("BRS")
    tag = f" {','.join(tags)}" if tags else ""

    hexbytes = " ".join(f"{b:02X}" for b in frame.data)
    ascii_col = "".join(chr(b) if 32 <= b < 127 else "." for b in frame.data)

    ts = frame.timestamp_us / 1_000_000.0
    return (f"({ts:>16.6f}) {iface:<6} {frame.id:08X}{tag}  "
            f"[{frame.len}] {hexbytes:<23}  '{ascii_col}'")


def main():
    args = parse_args()

    def on_any_frame(frame, bus):
        print(format_frame(frame, args.channel))

    try:
        with Bus(vid=args.vid, pid=args.pid,
                  usb_bus=args.usb_bus, usb_addr=args.usb_addr) as bus:
            bus.configure(bitrate=args.bitrate)
            bus.add_detector(PredicateDetector(lambda f: True, callback=on_any_frame))
            bus.start(listen_only=args.listen_only)

            print(f"listening at {args.bitrate} bps"
                  f"{' (listen-only)' if args.listen_only else ''}, Ctrl-C to stop",
                  file=sys.stderr)
            try:
                while True:
                    time.sleep(1)
            except KeyboardInterrupt:
                pass
    except GsusbError as e:
        print(f"error: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()

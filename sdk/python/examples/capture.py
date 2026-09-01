#!/usr/bin/env python3
"""candump-style event capture built on candetect -- works with whichever
supported adapter is attached.

Uses the Detector object pattern (a catch-all PredicateDetector) rather
than the polling recv() loop shown in dump.py -- the Bus's listener thread
calls our callback for every frame as it arrives, and we just format and
echo it, so this is a true event listener rather than a poll loop.

usage: capture.py [bitrate] [--prefer=gsusb|gcan] [--listen-only]
"""
import argparse
import os
import sys
import time

# Makes this script runnable standalone (python3 examples/capture.py)
# without needing PYTHONPATH set -- Python only puts the script's own
# directory on sys.path, not its parent, so the sibling ../candetect
# package needs this.
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

from candetect import Frame, NoSupportedAdapterError, PredicateDetector, open_bus


def parse_args():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("bitrate", type=int, nargs="?", default=500000,
                    help="500000 works on either backend; other rates need a gsusb-protocol adapter")
    p.add_argument("--prefer", choices=["gsusb", "gcan"], default=None,
                    help="try this backend first (still falls back to the other)")
    p.add_argument("--listen-only", action="store_true",
                    help="don't ACK frames on the bus (gsusb only -- gcan.Bus.start() raises for this)")
    p.add_argument("--channel", "-i", default="can0", help="label shown in the printed lines")
    return p.parse_args()


def format_frame(frame: Frame, iface: str) -> str:
    tags = []
    if frame.is_extended: tags.append("EFF")
    if frame.is_rtr: tags.append("RTR")
    if frame.is_error: tags.append("ERR")
    if frame.is_fd: tags.append("FD")
    if frame.is_brs: tags.append("BRS")
    if frame.is_esi: tags.append("ESI")
    tag = f" {','.join(tags)}" if tags else ""

    hexbytes = " ".join(f"{b:02X}" for b in frame.data)
    ascii_col = "".join(chr(b) if 32 <= b < 127 else "." for b in frame.data)

    # timestamp_us is a real hardware tick counter on gsusb, always 0 on
    # gcan (not decoded yet -- see driver/gcan_native/README.md).
    ts = frame.timestamp_us / 1_000_000.0
    return (f"({ts:>16.6f}) {iface:<6} {frame.id:08X}{tag}  "
            f"[{frame.len}] {hexbytes:<23}  '{ascii_col}'")


def main():
    args = parse_args()

    def on_any_frame(frame, bus):
        print(format_frame(frame, args.channel))

    try:
        bus = open_bus(prefer=args.prefer)
    except NoSupportedAdapterError as e:
        print(f"error: {e}", file=sys.stderr)
        sys.exit(1)

    print(f"opened {type(bus).__module__}.{type(bus).__qualname__}", file=sys.stderr)
    try:
        with bus:
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
    except Exception as e:
        print(f"error: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()

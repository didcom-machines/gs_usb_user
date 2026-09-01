#!/usr/bin/env python3
"""Listen-and-react example demonstrating the object pattern for detection,
built on candetect -- works with whichever supported adapter is attached.

Two ways of reacting to the same key frame are shown side by side: a plain
IdDetector with a callback function (the common case), and a Detector
subclass that carries its own state across detections (a counter here) --
use that shape when a one-shot callback isn't enough.

usage: react.py [bitrate]
"""
import os
import sys
import time

# Makes this script runnable standalone (python3 examples/react.py) without
# needing PYTHONPATH set -- Python only puts the script's own directory on
# sys.path, not its parent, so the sibling ../candetect package needs this.
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

from candetect import Detector, Frame, IdDetector, NoSupportedAdapterError, open_bus

KEY_ID = 0x100


def on_key_frame(frame, bus):
    print(f"key frame detected: {frame}")
    bus.send(Frame.standard(0x200, bytes([0x01])))


class CountingDetector(Detector):
    """Sends a milestone frame every 5th time it sees the key id -- an
    example of a Detector that needs its own state, not just a callback."""

    def __init__(self, can_id):
        self.can_id = can_id
        self.count = 0

    def matches(self, frame):
        return frame.id == self.can_id

    def on_detect(self, frame, bus):
        self.count += 1
        if self.count % 5 == 0:
            print(f"seen id 0x{self.can_id:X} {self.count} times, sending milestone frame")
            bus.send(Frame.standard(0x201, bytes([self.count & 0xFF])))


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
        bus.add_detector(IdDetector(KEY_ID, callback=on_key_frame))
        bus.add_detector(CountingDetector(KEY_ID))
        bus.start()
        print(f"watching for id 0x{KEY_ID:X}, Ctrl-C to stop", file=sys.stderr)
        try:
            while True:
                time.sleep(1)
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()

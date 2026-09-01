"""Object-oriented event detection for CAN traffic.

A Bus's background listener thread calls matches(frame) for every received
frame, against every registered Detector; when one returns True,
on_detect(frame, bus) runs -- still on that thread, so keep it fast, and
call bus.send(...) from inside it to react.

Use the built-ins (IdDetector, MaskDetector, PredicateDetector) with a plain
callback for the common case, or subclass Detector directly when you need
state (counters, debouncing, a multi-frame sequence) across detections.

Hardware-agnostic, like Frame -- works the same regardless of which
backend's Bus is dispatching to it.
"""
import logging

from .frame import Frame

logger = logging.getLogger("cancore.detectors")


class Detector:
    """Base class for a detection rule registered with a Bus."""

    def matches(self, frame: Frame) -> bool:
        raise NotImplementedError

    def on_detect(self, frame: Frame, bus) -> None:
        """Override to react. `bus` is the Bus that detected the frame --
        call bus.send(...) here to write a response. Default: no-op."""


class IdDetector(Detector):
    """Matches an exact CAN arbitration id (Frame.id -- EFF/RTR/ERR flag
    bits already masked off, so 0x100 matches both standard and extended
    frames carrying that id)."""

    def __init__(self, can_id: int, callback=None):
        self.can_id = can_id
        self._callback = callback

    def matches(self, frame: Frame) -> bool:
        return frame.id == self.can_id

    def on_detect(self, frame: Frame, bus) -> None:
        if self._callback is not None:
            self._callback(frame, bus)

    def __repr__(self):
        return f"IdDetector(0x{self.can_id:X})"


class MaskDetector(Detector):
    """Matches frame.id & mask == pattern & mask -- for watching a group or
    range of related ids (e.g. all ids sharing a device/node field)."""

    def __init__(self, pattern: int, mask: int, callback=None):
        self.pattern = pattern
        self.mask = mask
        self._callback = callback

    def matches(self, frame: Frame) -> bool:
        return (frame.id & self.mask) == (self.pattern & self.mask)

    def on_detect(self, frame: Frame, bus) -> None:
        if self._callback is not None:
            self._callback(frame, bus)

    def __repr__(self):
        return f"MaskDetector(pattern=0x{self.pattern:X}, mask=0x{self.mask:X})"


class PredicateDetector(Detector):
    """Matches via an arbitrary predicate(frame) -> bool, for conditions
    that don't fit an id/mask -- payload contents, combinations of fields,
    anything else."""

    def __init__(self, predicate, callback=None):
        self._predicate = predicate
        self._callback = callback

    def matches(self, frame: Frame) -> bool:
        return bool(self._predicate(frame))

    def on_detect(self, frame: Frame, bus) -> None:
        if self._callback is not None:
            self._callback(frame, bus)

    def __repr__(self):
        return f"PredicateDetector({self._predicate!r})"

"""cancore: the hardware-agnostic types shared by both driver backends in
this SDK (gsusb and gcan) -- Frame and the Detector family. Neither has a
ctypes dependency or any notion that gsusb/gcan exist; each backend's own
_ffi.py owns translating a Frame to/from its C library's wire struct.

There's exactly one Frame class and one Detector family, not one per
backend, precisely so gsusb.Frame is gcan.Frame (both packages re-export
these, they don't define their own) -- code written against one backend's
Bus doesn't need a different Frame type to build messages for the other,
and isinstance checks/collections mixing frames from both backends work
without special-casing.
"""
from .detectors import Detector, IdDetector, MaskDetector, PredicateDetector
from .frame import Frame

__all__ = [
    "Frame",
    "Detector", "IdDetector", "MaskDetector", "PredicateDetector",
]

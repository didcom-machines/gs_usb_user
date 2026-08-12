"""Python SDK for the gs_usb userspace CAN driver.

    from gsusb import Bus, Frame, IdDetector

    with Bus() as bus:
        bus.configure(bitrate=500000)
        bus.add_detector(IdDetector(0x100, callback=lambda f, b: b.send(Frame.standard(0x200, b"\\x01"))))
        bus.start()
        ...

See sdk/python/README.md for setup and sdk/python/examples/ for full
programs. Requires libgsusb.so (built from ../../driver) to be reachable --
see gsusb._ffi.load_library() for the search order, or set
GSUSB_LIBRARY_PATH.
"""
from .bus import Bus
from .detectors import Detector, IdDetector, MaskDetector, PredicateDetector
from .devices import KNOWN_DEVICES, KnownDevice
from .exceptions import GsusbError
from .frame import Frame

__version__ = "0.1.0"

__all__ = [
    "Bus", "Frame",
    "Detector", "IdDetector", "MaskDetector", "PredicateDetector",
    "GsusbError", "KNOWN_DEVICES", "KnownDevice",
]

"""Python SDK for the GCAN/ECAN/Rexon USBCANI-V503 CAN adapter, via
libgcan_native.so (driver/gcan_native/) -- a from-scratch, libusb-1.0-only
driver that (unlike driver/gcan/'s vendor libECanVci.so.1) builds and runs
natively on ARM. Its Bus/Frame API is deliberately shaped to match
gsusb.Bus/gsusb.Frame wherever this hardware's confirmed capabilities
allow -- see gcan.bus.Bus's module doc for what's real vs. a
shape-parity stub that raises GcanError.

    from gcan import Bus, Frame, IdDetector

    with Bus() as bus:
        bus.configure(bitrate=500000)
        bus.add_detector(IdDetector(0x100, callback=lambda f, b: b.send(Frame.standard(0x200, b"\\x01"))))
        bus.start()
        ...

See sdk/python/README.md for setup and sdk/python/examples/ for full
programs, and driver/gcan_native/README.md for the reverse-engineered
protocol behind this and its current limitations (CAN1, 500 kbit/s,
classic CAN only -- and unverified against real hardware so far). Requires
libgcan_native.so to be reachable -- see gcan._ffi.load_library() for the
search order, or set GCAN_NATIVE_LIBRARY_PATH.
"""
from cancore import Detector, Frame, IdDetector, MaskDetector, PredicateDetector

from .bus import Bus
from .exceptions import GcanError

__version__ = "0.1.0"

__all__ = [
    "Bus", "Frame",
    "Detector", "IdDetector", "MaskDetector", "PredicateDetector",
    "GcanError",
]

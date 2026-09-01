"""candetect: auto-detects which supported CAN adapter is attached and
returns the matching backend's Bus, so a caller doesn't have to import
gsusb or gcan directly, or know in advance which one is plugged in.

    from candetect import open_bus

    with open_bus() as bus:
        bus.configure(bitrate=500000)
        bus.start()
        bus.send(bus.Frame.standard(0x123, b"\\xDE\\xAD\\xBE\\xEF"))

This works because gsusb.Bus and gcan.Bus were deliberately built to the
same shape (see gcan.bus.Bus's module doc for the alignment rationale) --
same method names/signatures (open/close, configure, start/stop, send,
recv, device_config/channel_count/features, get_state/set_identify/
get_termination/set_termination) and numerically-matching error codes.
Frame goes further than "same shape": gsusb.Frame and gcan.Frame are
*the same class* (cancore.Frame -- see cancore's module doc), not two
lookalike ones, so there's nothing to reconcile there at all. Code that
sticks to the common Bus surface runs unchanged against whichever backend
this returns.

What this does NOT paper over: actual hardware capability. gs_usb-protocol
adapters really do support CAN-FD, multiple channels, listen-only/
loopback/one-shot modes, and a real bit-timing calculator; the GCAN
adapter's gcan_native backend accepts the same method calls but raises for
every one of those (see driver/gcan_native/README.md) -- autodetection
picks the right driver for the hardware in front of you, it doesn't make
every feature portable. Code that needs a gsusb-only or gcan-only feature
should still import that package directly instead of going through here.
"""
from typing import Optional

from cancore import Detector, Frame, IdDetector, MaskDetector, PredicateDetector

_BACKENDS = ("gsusb", "gcan")


class NoSupportedAdapterError(Exception):
    """Raised by open_bus() when no backend found a supported adapter.

    str(err) lists what was tried and why each attempt failed (e.g. no
    matching USB device, or that backend's shared library not being
    built/reachable) -- see the individual GsusbError/GcanError/OSError
    causes via err.attempts.
    """

    def __init__(self, attempts: dict):
        self.attempts = attempts
        detail = ", ".join(f"{name}: {exc}" for name, exc in attempts.items())
        super().__init__(f"no supported CAN adapter found -- tried: {detail}")


def _open_backend(name: str, **kwargs):
    if name == "gsusb":
        from gsusb import Bus
    elif name == "gcan":
        from gcan import Bus
    else:
        raise ValueError(f"unknown backend {name!r} -- must be one of {_BACKENDS}")
    return Bus(**kwargs).open()


def open_bus(prefer: Optional[str] = None, **kwargs):
    """Tries each backend in turn (gsusb first, then gcan, unless
    `prefer` names one to try first) and returns the first one that
    successfully opens a device -- already open()'d, ready to
    configure()/start(). Use it as a context manager (`with open_bus() as
    bus:`) or call bus.close() yourself when done.

    kwargs are forwarded to whichever backend's Bus(...) constructor ends
    up being used (e.g. library_path=, channel=). Only pass kwargs valid
    for *every* backend you want to be triable -- a kwarg that's only
    valid for one backend (like gsusb's vid=/pid=) raises TypeError
    immediately from that backend's constructor rather than being treated
    as "device not found" and falling through to the next one. If you
    need a backend-specific argument, import that backend directly
    instead of going through open_bus().

    :param prefer: "gsusb" or "gcan" to try that backend first (still
        falls back to the other if it isn't found)
    :raises NoSupportedAdapterError: if no backend found a device
    :raises ValueError: if `prefer` isn't a known backend name
    :raises TypeError: if a kwarg isn't valid for a backend being tried
    """
    if prefer is not None and prefer not in _BACKENDS:
        raise ValueError(f"prefer must be one of {_BACKENDS}, got {prefer!r}")

    order = list(_BACKENDS)
    if prefer is not None:
        order.remove(prefer)
        order.insert(0, prefer)

    attempts = {}
    for name in order:
        try:
            return _open_backend(name, **kwargs)
        except TypeError:
            raise  # a caller bug (bad kwarg), not "device not found" -- don't hide it
        except Exception as exc:
            # GsusbError/GcanError (no matching device, or the C library
            # rejected the request) or OSError (that backend's shared
            # library isn't built/reachable) -- both mean "try the next
            # backend", not "give up immediately".
            attempts[name] = exc

    raise NoSupportedAdapterError(attempts)


__all__ = [
    "open_bus", "NoSupportedAdapterError",
    # Re-exported from cancore so backend-agnostic code has one import to
    # reach for -- these are the exact same classes gsusb/gcan re-export,
    # not copies.
    "Frame", "Detector", "IdDetector", "MaskDetector", "PredicateDetector",
]

"""Low-level ctypes bindings to libgcan_native.so.

This module is a 1:1 mirror of driver/gcan_native/include/gcan_native.h --
struct layout and function signatures here must match that header exactly,
since ctypes does no verification of its own. Nothing in here is meant to
be used directly; see gcan.bus.Bus and cancore.Frame for the public API.

Constant names and values here mirror gsusb's own _ffi.py (which mirrors
driver/include/gsusb.h) wherever the concept exists, by design -- see
gcan_native.h's module doc for what's genuinely supported vs. a
shape-parity stub that always errors.

frame_to_ctypes()/frame_from_ctypes() are this module's translation
between cancore.Frame (hardware-agnostic) and GcanNativeFrame (this
library's wire struct) -- the only place that translation happens, so
Frame itself doesn't need to know this backend exists. Unlike gsusb's
GsusbFrame, GcanNativeFrame has no flags field at all (this hardware
confirmed has no CAN-FD, see gcan_native.h's module doc) -- so unlike
RTR/ERR (which the C library itself rejects), frame_to_ctypes() has to
reject an FD/BRS/ESI-flagged Frame itself, in Python, since there's no
struct field to even carry that bit into.
"""
import ctypes as ct
import ctypes.util
import os

from cancore import Frame
from cancore.frame import EFF_FLAG, RTR_FLAG, ERR_FLAG, EFF_MASK, SFF_MASK, FRAME_FD, FRAME_BRS, FRAME_ESI

GCAN_NATIVE_MAX_DLEN = 8  # classic CAN only -- this hardware has no CAN-FD

# gcan_native_feature bits -- all currently unimplemented (always 0 from
# gcan_native_channel_features()); named to match gsusb's FEATURE_* set
# wherever the concept exists.
FEATURE_LISTEN_ONLY = 1 << 0
FEATURE_LOOP_BACK = 1 << 1
FEATURE_ONE_SHOT = 1 << 2
FEATURE_HW_TIMESTAMP = 1 << 3
FEATURE_IDENTIFY = 1 << 4
FEATURE_TERMINATION = 1 << 5
FEATURE_GET_STATE = 1 << 6
FEATURE_SECOND_CHANNEL = 1 << 7

# gcan_native_mode_flags (gcan_native_channel_start()) -- mode_flags must
# be 0 right now; anything else is rejected. Named for parity with gsusb's
# MODE_* set.
MODE_LISTEN_ONLY = 1 << 0
MODE_LOOPBACK = 1 << 1
MODE_ONE_SHOT = 1 << 2

# gcan_native_can_state
STATE_ERROR_ACTIVE = 0
STATE_ERROR_WARNING = 1
STATE_ERROR_PASSIVE = 2
STATE_BUS_OFF = 3
STATE_STOPPED = 4

# gcan_native_error -- values and meanings match gsusb_error 1:1.
OK = 0
ERR_IO = -1
ERR_NOMEM = -2
ERR_NOT_FOUND = -3
ERR_ACCESS = -4
ERR_TIMEOUT = -5
ERR_INVALID = -6
ERR_BUSY = -7
ERR_NO_BITTIMING_SOLUTION = -8


class GcanNativeFrame(ct.Structure):
    _fields_ = [
        ("can_id", ct.c_uint32),
        ("len", ct.c_uint8),
        ("data", ct.c_uint8 * GCAN_NATIVE_MAX_DLEN),
        ("timestamp", ct.c_uint32),
    ]


class GcanNativeDeviceConfig(ct.Structure):
    _fields_ = [
        ("channel_count", ct.c_uint8),
    ]


def frame_to_ctypes(frame: Frame) -> GcanNativeFrame:
    """Converts a cancore.Frame to this library's wire struct. Raises
    ValueError for an FD/BRS/ESI-flagged frame -- there's no field in
    GcanNativeFrame to carry that bit into, since this hardware confirmed
    has no CAN-FD (see this module's doc). RTR/ERR aren't checked here;
    the C library itself rejects those (GCAN_NATIVE_ERR_INVALID), since
    GcanNativeFrame's can_id field can carry the bit even though the wire
    protocol doesn't confirm support for it.
    """
    if frame.flags & (FRAME_FD | FRAME_BRS | FRAME_ESI):
        raise ValueError(
            "this hardware (driver/gcan_native/) has no CAN-FD support -- "
            "cannot send a frame built with Frame.canfd()"
        )
    data = bytes(frame.data)
    if len(data) > GCAN_NATIVE_MAX_DLEN:
        raise ValueError(
            f"frame data length {len(data)} exceeds {GCAN_NATIVE_MAX_DLEN} "
            f"bytes -- this hardware is classic CAN only, no CAN-FD"
        )
    c = GcanNativeFrame()
    c.can_id = frame.can_id
    c.len = len(data)
    for i, byte in enumerate(data):
        c.data[i] = byte
    c.timestamp = frame.timestamp_us
    return c


def frame_from_ctypes(c: GcanNativeFrame) -> Frame:
    """Converts this library's wire struct to a cancore.Frame. flags is
    always 0 (no CAN-FD detection possible), and timestamp_us is always 0
    (the RX record bytes that presumably carry it aren't decoded yet --
    see driver/gcan_native/README.md)."""
    return Frame(
        can_id=c.can_id,
        data=bytes(c.data[: c.len]),
        timestamp_us=c.timestamp,
    )


def _candidate_paths(explicit_path):
    if explicit_path:
        yield explicit_path
    env_path = os.environ.get("GCAN_NATIVE_LIBRARY_PATH")
    if env_path:
        yield env_path
    # bundled next to this package, e.g. sdk/python/gcan/libgcan_native.so
    yield os.path.join(os.path.dirname(os.path.abspath(__file__)), "libgcan_native.so")
    # this repo's own driver/gcan_native/bin/, for running against a source
    # checkout without a separate deploy step
    yield os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "..", "..", "..", "driver", "gcan_native", "bin", "libgcan_native.so")
    yield "/usr/local/lib/libgcan_native.so"
    yield "/usr/lib/libgcan_native.so"
    found = ctypes.util.find_library("gcan_native")
    if found:
        yield found


def load_library(explicit_path=None):
    """Loads libgcan_native.so, trying (in order): explicit_path, the
    GCAN_NATIVE_LIBRARY_PATH env var, a copy bundled next to this package,
    this repo's driver/gcan_native/bin/, /usr/local/lib, /usr/lib, then the
    standard ldconfig search path. Raises OSError with every path tried if
    none work.
    """
    tried = []
    for path in _candidate_paths(explicit_path):
        tried.append(path)
        try:
            lib = ct.CDLL(path)
        except OSError:
            continue
        _configure(lib)
        return lib
    raise OSError(f"could not locate libgcan_native.so (tried: {tried})")


def _configure(lib):
    lib.gcan_native_init.argtypes = [ct.POINTER(ct.c_void_p)]
    lib.gcan_native_init.restype = ct.c_int

    lib.gcan_native_exit.argtypes = [ct.c_void_p]
    lib.gcan_native_exit.restype = None

    lib.gcan_native_open.argtypes = [ct.c_void_p, ct.c_char_p, ct.c_size_t]
    lib.gcan_native_open.restype = ct.c_void_p

    lib.gcan_native_close.argtypes = [ct.c_void_p]
    lib.gcan_native_close.restype = None

    lib.gcan_native_channel_count.argtypes = [ct.c_void_p]
    lib.gcan_native_channel_count.restype = ct.c_uint

    lib.gcan_native_get_device_config.argtypes = [ct.c_void_p]
    lib.gcan_native_get_device_config.restype = ct.POINTER(GcanNativeDeviceConfig)

    lib.gcan_native_channel_get.argtypes = [ct.c_void_p, ct.c_uint]
    lib.gcan_native_channel_get.restype = ct.c_void_p

    lib.gcan_native_channel_features.argtypes = [ct.c_void_p]
    lib.gcan_native_channel_features.restype = ct.c_uint32

    lib.gcan_native_channel_set_bitrate.argtypes = [ct.c_void_p, ct.c_uint32, ct.c_double]
    lib.gcan_native_channel_set_bitrate.restype = ct.c_int

    lib.gcan_native_channel_set_bittiming_raw.argtypes = [
        ct.c_void_p, ct.c_uint32, ct.c_uint32, ct.c_uint32, ct.c_uint32, ct.c_uint32,
    ]
    lib.gcan_native_channel_set_bittiming_raw.restype = ct.c_int

    lib.gcan_native_channel_start.argtypes = [ct.c_void_p, ct.c_uint32]
    lib.gcan_native_channel_start.restype = ct.c_int

    lib.gcan_native_channel_stop.argtypes = [ct.c_void_p]
    lib.gcan_native_channel_stop.restype = ct.c_int

    lib.gcan_native_channel_get_state.argtypes = [
        ct.c_void_p, ct.POINTER(ct.c_uint32), ct.POINTER(ct.c_uint32), ct.POINTER(ct.c_uint32),
    ]
    lib.gcan_native_channel_get_state.restype = ct.c_int

    lib.gcan_native_channel_set_identify.argtypes = [ct.c_void_p, ct.c_int]
    lib.gcan_native_channel_set_identify.restype = ct.c_int

    lib.gcan_native_channel_get_termination.argtypes = [ct.c_void_p, ct.POINTER(ct.c_int)]
    lib.gcan_native_channel_get_termination.restype = ct.c_int

    lib.gcan_native_channel_set_termination.argtypes = [ct.c_void_p, ct.c_int]
    lib.gcan_native_channel_set_termination.restype = ct.c_int

    lib.gcan_native_channel_send.argtypes = [ct.c_void_p, ct.POINTER(GcanNativeFrame), ct.c_uint]
    lib.gcan_native_channel_send.restype = ct.c_int

    lib.gcan_native_channel_recv.argtypes = [ct.c_void_p, ct.POINTER(GcanNativeFrame), ct.c_uint]
    lib.gcan_native_channel_recv.restype = ct.c_int

    lib.gcan_native_strerror.argtypes = [ct.c_int]
    lib.gcan_native_strerror.restype = ct.c_char_p

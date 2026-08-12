"""Low-level ctypes bindings to libgsusb.so.

This module is a 1:1 mirror of driver/include/gsusb.h -- struct layouts and
function signatures here must match that header exactly, since ctypes does
no verification of its own. Nothing in here is meant to be used directly;
see gsusb.bus.Bus and gsusb.frame.Frame for the public API.
"""
import ctypes as ct
import ctypes.util
import os

GSUSB_MAX_DLEN = 64

# CAN ID flags/masks -- identical bit layout to Linux <linux/can.h>.
EFF_FLAG = 0x80000000
RTR_FLAG = 0x40000000
ERR_FLAG = 0x20000000
EFF_MASK = 0x1FFFFFFF
SFF_MASK = 0x000007FF

# gsusb_frame::flags
FRAME_FD = 0x01
FRAME_BRS = 0x02
FRAME_ESI = 0x04

# gsusb_feature bits
FEATURE_LISTEN_ONLY = 1 << 0
FEATURE_LOOP_BACK = 1 << 1
FEATURE_TRIPLE_SAMPLE = 1 << 2
FEATURE_ONE_SHOT = 1 << 3
FEATURE_HW_TIMESTAMP = 1 << 4
FEATURE_IDENTIFY = 1 << 5
FEATURE_USER_ID = 1 << 6
FEATURE_PAD_PKTS_TO_MAX = 1 << 7
FEATURE_FD = 1 << 8
FEATURE_REQ_QUIRK_LPC546XX = 1 << 9
FEATURE_BT_CONST_EXT = 1 << 10
FEATURE_TERMINATION = 1 << 11
FEATURE_BERR_REPORTING = 1 << 12
FEATURE_GET_STATE = 1 << 13

# gsusb_mode_flags (gsusb_channel_start())
MODE_LISTEN_ONLY = 1 << 0
MODE_LOOPBACK = 1 << 1
MODE_TRIPLE_SAMPLE = 1 << 2
MODE_ONE_SHOT = 1 << 3
MODE_FD = 1 << 4
MODE_BERR_REPORTING = 1 << 5

# gsusb_can_state
STATE_ERROR_ACTIVE = 0
STATE_ERROR_WARNING = 1
STATE_ERROR_PASSIVE = 2
STATE_BUS_OFF = 3
STATE_STOPPED = 4
STATE_SLEEPING = 5

# gsusb_error
OK = 0
ERR_IO = -1
ERR_NOMEM = -2
ERR_NOT_FOUND = -3
ERR_ACCESS = -4
ERR_TIMEOUT = -5
ERR_INVALID = -6
ERR_BUSY = -7
ERR_NO_BITTIMING_SOLUTION = -8


class GsusbFrame(ct.Structure):
    _fields_ = [
        ("can_id", ct.c_uint32),
        ("len", ct.c_uint8),
        ("flags", ct.c_uint8),
        ("data", ct.c_uint8 * GSUSB_MAX_DLEN),
        ("timestamp_us", ct.c_uint32),
    ]


class GsusbBittimingConst(ct.Structure):
    _fields_ = [
        ("feature", ct.c_uint32),
        ("fclk_can", ct.c_uint32),
        ("tseg1_min", ct.c_uint32),
        ("tseg1_max", ct.c_uint32),
        ("tseg2_min", ct.c_uint32),
        ("tseg2_max", ct.c_uint32),
        ("sjw_max", ct.c_uint32),
        ("brp_min", ct.c_uint32),
        ("brp_max", ct.c_uint32),
        ("brp_inc", ct.c_uint32),
    ]


class GsusbDeviceConfig(ct.Structure):
    _fields_ = [
        ("sw_version", ct.c_uint32),
        ("hw_version", ct.c_uint32),
        ("channel_count", ct.c_uint8),
    ]


def _candidate_paths(explicit_path):
    if explicit_path:
        yield explicit_path
    env_path = os.environ.get("GSUSB_LIBRARY_PATH")
    if env_path:
        yield env_path
    # bundled next to this package, e.g. sdk/python/gsusb/libgsusb.so
    yield os.path.join(os.path.dirname(os.path.abspath(__file__)), "libgsusb.so")
    yield "/usr/local/lib/libgsusb.so"
    yield "/usr/lib/libgsusb.so"
    found = ctypes.util.find_library("gsusb")
    if found:
        yield found


def load_library(explicit_path=None):
    """Loads libgsusb.so, trying (in order): explicit_path, the
    GSUSB_LIBRARY_PATH env var, a copy bundled next to this package,
    /usr/local/lib, /usr/lib, then the standard ldconfig search path.
    Raises OSError with every path tried if none work.
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
    raise OSError(f"could not locate libgsusb.so (tried: {tried})")


def _configure(lib):
    lib.gsusb_init.argtypes = [ct.POINTER(ct.c_void_p)]
    lib.gsusb_init.restype = ct.c_int

    lib.gsusb_exit.argtypes = [ct.c_void_p]
    lib.gsusb_exit.restype = None

    lib.gsusb_open.argtypes = [
        ct.c_void_p, ct.c_uint16, ct.c_uint16, ct.c_int, ct.c_int,
        ct.c_char_p, ct.c_size_t,
    ]
    lib.gsusb_open.restype = ct.c_void_p

    lib.gsusb_close.argtypes = [ct.c_void_p]
    lib.gsusb_close.restype = None

    lib.gsusb_channel_count.argtypes = [ct.c_void_p]
    lib.gsusb_channel_count.restype = ct.c_uint

    lib.gsusb_get_device_config.argtypes = [ct.c_void_p]
    lib.gsusb_get_device_config.restype = ct.POINTER(GsusbDeviceConfig)

    lib.gsusb_channel_get.argtypes = [ct.c_void_p, ct.c_uint]
    lib.gsusb_channel_get.restype = ct.c_void_p

    lib.gsusb_channel_bt_const.argtypes = [ct.c_void_p]
    lib.gsusb_channel_bt_const.restype = ct.POINTER(GsusbBittimingConst)

    lib.gsusb_channel_data_bt_const.argtypes = [ct.c_void_p]
    lib.gsusb_channel_data_bt_const.restype = ct.POINTER(GsusbBittimingConst)

    lib.gsusb_channel_features.argtypes = [ct.c_void_p]
    lib.gsusb_channel_features.restype = ct.c_uint32

    lib.gsusb_channel_set_bitrate.argtypes = [ct.c_void_p, ct.c_uint32, ct.c_double]
    lib.gsusb_channel_set_bitrate.restype = ct.c_int

    lib.gsusb_channel_set_data_bitrate.argtypes = [ct.c_void_p, ct.c_uint32, ct.c_double]
    lib.gsusb_channel_set_data_bitrate.restype = ct.c_int

    lib.gsusb_channel_set_bittiming_raw.argtypes = [
        ct.c_void_p, ct.c_uint32, ct.c_uint32, ct.c_uint32, ct.c_uint32, ct.c_uint32,
    ]
    lib.gsusb_channel_set_bittiming_raw.restype = ct.c_int

    lib.gsusb_channel_set_data_bittiming_raw.argtypes = [
        ct.c_void_p, ct.c_uint32, ct.c_uint32, ct.c_uint32, ct.c_uint32, ct.c_uint32,
    ]
    lib.gsusb_channel_set_data_bittiming_raw.restype = ct.c_int

    lib.gsusb_channel_start.argtypes = [ct.c_void_p, ct.c_uint32]
    lib.gsusb_channel_start.restype = ct.c_int

    lib.gsusb_channel_stop.argtypes = [ct.c_void_p]
    lib.gsusb_channel_stop.restype = ct.c_int

    lib.gsusb_channel_get_state.argtypes = [
        ct.c_void_p, ct.POINTER(ct.c_uint32), ct.POINTER(ct.c_uint32), ct.POINTER(ct.c_uint32),
    ]
    lib.gsusb_channel_get_state.restype = ct.c_int

    lib.gsusb_channel_set_identify.argtypes = [ct.c_void_p, ct.c_int]
    lib.gsusb_channel_set_identify.restype = ct.c_int

    lib.gsusb_channel_get_termination.argtypes = [ct.c_void_p, ct.POINTER(ct.c_int)]
    lib.gsusb_channel_get_termination.restype = ct.c_int

    lib.gsusb_channel_set_termination.argtypes = [ct.c_void_p, ct.c_int]
    lib.gsusb_channel_set_termination.restype = ct.c_int

    lib.gsusb_channel_send.argtypes = [ct.c_void_p, ct.POINTER(GsusbFrame), ct.c_uint]
    lib.gsusb_channel_send.restype = ct.c_int

    lib.gsusb_channel_recv.argtypes = [ct.c_void_p, ct.POINTER(GsusbFrame), ct.c_uint]
    lib.gsusb_channel_recv.restype = ct.c_int

    lib.gsusb_strerror.argtypes = [ct.c_int]
    lib.gsusb_strerror.restype = ct.c_char_p

    lib.gsusb_len2dlc_cc.argtypes = [ct.c_uint8]
    lib.gsusb_len2dlc_cc.restype = ct.c_uint8
    lib.gsusb_dlc2len_cc.argtypes = [ct.c_uint8]
    lib.gsusb_dlc2len_cc.restype = ct.c_uint8
    lib.gsusb_len2dlc_fd.argtypes = [ct.c_uint8]
    lib.gsusb_len2dlc_fd.restype = ct.c_uint8
    lib.gsusb_dlc2len_fd.argtypes = [ct.c_uint8]
    lib.gsusb_dlc2len_fd.restype = ct.c_uint8

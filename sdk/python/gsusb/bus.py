"""Bus: opens one gs_usb channel and, once started, runs one background
listener thread that is the sole consumer of the C library's receive
queue. Every frame it reads is both dispatched to registered Detector
objects and pushed onto an internal queue that recv() drains -- so
detectors and polling-style recv() calls can be used together (or alone)
without racing each other for the same frames. Your own code (or a
Detector's on_detect()) can call send() at any time; it's an independent
USB OUT transfer that never blocks on, or is blocked by, the listener
thread. See driver/include/gsusb.h's module docstring for why a single
process can do this without extra locking of its own.
"""
import ctypes as ct
import logging
import queue
import threading
from typing import Optional

from cancore import Frame

from . import _ffi
from .devices import KNOWN_DEVICES
from .exceptions import GsusbError

logger = logging.getLogger("gsusb.bus")

# How often the listener thread wakes up when the bus is idle, purely to
# notice stop() -- it adds no latency to detecting a real frame, since
# gsusb_channel_recv() wakes up immediately when one arrives.
_RECV_POLL_MS = 200


class Bus:
    """One CAN channel on one gs_usb-compatible adapter.

    with Bus(bitrate=500000) as bus:
        bus.add_detector(IdDetector(0x100, callback=my_reaction))
        bus.start()
        ...
        bus.send(Frame.standard(0x200, b"\\x01"))

    vid/pid/bus/addr identify which USB device to open (see gsusb_info
    --scan on the target); if vid/pid are omitted, every id in
    gsusb.devices.KNOWN_DEVICES is tried in turn, same as the CLI tools.
    """

    #: cancore.Frame (the same class gcan.Bus.Frame points to -- there's
    #: only one Frame class, see cancore's module doc), exposed here so
    #: code that obtained a Bus without importing gsusb or cancore
    #: directly (e.g. via candetect.open_bus()) can still do
    #: bus.Frame.standard(...).
    Frame = Frame

    def __init__(self, vid: Optional[int] = None, pid: Optional[int] = None,
                 usb_bus: Optional[int] = None, usb_addr: Optional[int] = None,
                 channel: int = 0, library_path: Optional[str] = None):
        self._lib = _ffi.load_library(library_path)
        self._vid = vid
        self._pid = pid
        self._usb_bus = usb_bus
        self._usb_addr = usb_addr
        self._channel_index = channel

        self._ctx = None
        self._dev = None
        self._ch = None

        self._detectors = []
        self._detectors_lock = threading.Lock()
        self._listener_thread: Optional[threading.Thread] = None
        self._stop_event = threading.Event()
        self._started = False

        # The listener thread is the *only* caller of gsusb_channel_recv()
        # once started -- it's a single-consumer queue at the C level, so a
        # second, independent consumer (e.g. recv() calling it directly)
        # would race the listener thread for the same frames and silently
        # lose almost every time. recv() instead drains this queue, which
        # the listener thread feeds alongside dispatching to detectors, so
        # both can be used together without competing.
        self._recv_queue: "queue.Queue[Frame]" = queue.Queue(maxsize=512)

    # --- lifecycle ---

    def open(self) -> "Bus":
        if self._dev is not None:
            return self

        ctx_out = ct.c_void_p()
        rc = self._lib.gsusb_init(ct.byref(ctx_out))
        if rc != 0:
            raise GsusbError(rc, f"gsusb_init: {self._errmsg(rc)}")
        self._ctx = ctx_out

        candidates = (
            [(self._vid, self._pid)] if (self._vid is not None and self._pid is not None)
            else [(d.vid, d.pid) for d in KNOWN_DEVICES]
        )

        errbuf = ct.create_string_buffer(256)
        dev = None
        last_err = "no candidate ids tried"
        for vid, pid in candidates:
            errbuf.value = b""
            h = self._lib.gsusb_open(
                self._ctx, vid, pid,
                self._usb_bus if self._usb_bus is not None else -1,
                self._usb_addr if self._usb_addr is not None else -1,
                errbuf, len(errbuf),
            )
            if h:
                dev = h
                break
            last_err = errbuf.value.decode(errors="replace")

        if dev is None:
            self._lib.gsusb_exit(self._ctx)
            self._ctx = None
            raise GsusbError(_ffi.ERR_NOT_FOUND, last_err)

        self._dev = dev
        ch = self._lib.gsusb_channel_get(self._dev, self._channel_index)
        if not ch:
            self.close()
            raise GsusbError(_ffi.ERR_INVALID,
                             f"channel {self._channel_index} does not exist on this device")
        self._ch = ch
        return self

    def close(self) -> None:
        self.stop()
        if self._dev is not None:
            self._lib.gsusb_close(self._dev)
            self._dev = None
            self._ch = None
        if self._ctx is not None:
            self._lib.gsusb_exit(self._ctx)
            self._ctx = None

    def __enter__(self) -> "Bus":
        return self.open()

    def __exit__(self, exc_type, exc, tb) -> None:
        self.close()

    def _require_open(self) -> None:
        if self._dev is None:
            raise RuntimeError("Bus is not open -- call open() or use it as a context manager")

    # --- device / channel info ---

    @property
    def device_config(self) -> dict:
        self._require_open()
        c = self._lib.gsusb_get_device_config(self._dev).contents
        return {"sw_version": c.sw_version, "hw_version": c.hw_version,
                "channel_count": c.channel_count}

    @property
    def channel_count(self) -> int:
        self._require_open()
        return self._lib.gsusb_channel_count(self._dev)

    @property
    def features(self) -> int:
        self._require_open()
        return self._lib.gsusb_channel_features(self._ch)

    @property
    def bt_const(self) -> Optional[dict]:
        self._require_open()
        p = self._lib.gsusb_channel_bt_const(self._ch)
        return self._btc_to_dict(p.contents) if p else None

    @property
    def data_bt_const(self) -> Optional[dict]:
        self._require_open()
        p = self._lib.gsusb_channel_data_bt_const(self._ch)
        return self._btc_to_dict(p.contents) if p else None

    @staticmethod
    def _btc_to_dict(c: "_ffi.GsusbBittimingConst") -> dict:
        return {
            "feature": c.feature, "fclk_can": c.fclk_can,
            "tseg1_min": c.tseg1_min, "tseg1_max": c.tseg1_max,
            "tseg2_min": c.tseg2_min, "tseg2_max": c.tseg2_max,
            "sjw_max": c.sjw_max, "brp_min": c.brp_min,
            "brp_max": c.brp_max, "brp_inc": c.brp_inc,
        }

    # --- configuration ---

    def configure(self, bitrate: int, sample_point: float = 0.0, fd: bool = False,
                  data_bitrate: Optional[int] = None, data_sample_point: float = 0.0) -> None:
        """Sets nominal (and, if fd=True, data-phase) bit timing. Must be
        called before start(). sample_point=0 picks a CiA-recommended
        default; pass fd=True and data_bitrate for CAN-FD."""
        self._require_open()
        rc = self._lib.gsusb_channel_set_bitrate(self._ch, bitrate, sample_point)
        self._check(rc, "set_bitrate")
        if fd:
            if data_bitrate is None:
                raise ValueError("fd=True requires data_bitrate")
            rc = self._lib.gsusb_channel_set_data_bitrate(self._ch, data_bitrate, data_sample_point)
            self._check(rc, "set_data_bitrate")

    def configure_raw(self, prop_seg: int, phase_seg1: int, phase_seg2: int,
                       sjw: int, brp: int, data: bool = False) -> None:
        """Sends exact raw timing register values, bypassing the bit-timing
        calculator. Pass data=True to set the CAN-FD data-phase timing."""
        self._require_open()
        fn = (self._lib.gsusb_channel_set_data_bittiming_raw if data
              else self._lib.gsusb_channel_set_bittiming_raw)
        rc = fn(self._ch, prop_seg, phase_seg1, phase_seg2, sjw, brp)
        self._check(rc, "set_bittiming_raw")

    # --- start/stop ---

    def start(self, listen_only: bool = False, loopback: bool = False, fd: bool = False,
              one_shot: bool = False, triple_sample: bool = False,
              berr_reporting: bool = False) -> None:
        self._require_open()
        if self._started:
            return

        mode = 0
        if listen_only: mode |= _ffi.MODE_LISTEN_ONLY
        if loopback: mode |= _ffi.MODE_LOOPBACK
        if fd: mode |= _ffi.MODE_FD
        if one_shot: mode |= _ffi.MODE_ONE_SHOT
        if triple_sample: mode |= _ffi.MODE_TRIPLE_SAMPLE
        if berr_reporting: mode |= _ffi.MODE_BERR_REPORTING

        rc = self._lib.gsusb_channel_start(self._ch, mode)
        self._check(rc, "start")

        self._started = True
        self._stop_event.clear()
        self._listener_thread = threading.Thread(
            target=self._listen_loop, name="gsusb-bus-listener", daemon=True)
        self._listener_thread.start()

    def stop(self) -> None:
        if not self._started:
            return
        self._stop_event.set()
        if self._listener_thread is not None:
            self._listener_thread.join(timeout=2.0)
            self._listener_thread = None
        if self._ch is not None:
            self._lib.gsusb_channel_stop(self._ch)
        self._started = False

    # --- detectors (the object pattern for event detection) ---

    def add_detector(self, detector) -> None:
        with self._detectors_lock:
            self._detectors.append(detector)

    def remove_detector(self, detector) -> None:
        with self._detectors_lock:
            self._detectors.remove(detector)

    def _listen_loop(self) -> None:
        frame_c = _ffi.GsusbFrame()
        while not self._stop_event.is_set():
            rc = self._lib.gsusb_channel_recv(self._ch, ct.byref(frame_c), _RECV_POLL_MS)
            if rc == 0:
                continue  # idle timeout, loop to re-check _stop_event
            if rc < 0:
                logger.warning("gsusb_channel_recv: %s", self._errmsg(rc))
                break

            frame = _ffi.frame_from_ctypes(frame_c)

            # Feed recv()'s queue (drop-oldest if a caller never drains it,
            # same overflow policy as the C library's own ring buffer).
            try:
                self._recv_queue.put_nowait(frame)
            except queue.Full:
                try:
                    self._recv_queue.get_nowait()
                except queue.Empty:
                    pass
                try:
                    self._recv_queue.put_nowait(frame)
                except queue.Full:
                    pass

            with self._detectors_lock:
                detectors = list(self._detectors)
            for detector in detectors:
                try:
                    if detector.matches(frame):
                        detector.on_detect(frame, self)
                except Exception:
                    logger.exception("detector %r raised while handling %r", detector, frame)

    # --- send / poll-style recv ---

    def send(self, frame: Frame, timeout_ms: int = 1000) -> None:
        self._require_open()
        c = _ffi.frame_to_ctypes(frame)
        rc = self._lib.gsusb_channel_send(self._ch, ct.byref(c), timeout_ms)
        self._check(rc, "send")

    def recv(self, timeout_ms: int = 1000) -> Optional[Frame]:
        """Blocking single-frame receive, for polling-style code instead of
        (or alongside) detectors -- drains the same frames the listener
        thread feeds to detectors (see _listen_loop), not a second
        independent read of the device. Returns None on timeout."""
        self._require_open()
        try:
            return self._recv_queue.get(timeout=timeout_ms / 1000.0)
        except queue.Empty:
            return None

    # --- state / identify / termination ---

    def get_state(self) -> dict:
        self._require_open()
        state, rxerr, txerr = ct.c_uint32(), ct.c_uint32(), ct.c_uint32()
        rc = self._lib.gsusb_channel_get_state(
            self._ch, ct.byref(state), ct.byref(rxerr), ct.byref(txerr))
        self._check(rc, "get_state")
        return {"state": state.value, "rxerr": rxerr.value, "txerr": txerr.value}

    def set_identify(self, on: bool) -> None:
        self._require_open()
        rc = self._lib.gsusb_channel_set_identify(self._ch, 1 if on else 0)
        self._check(rc, "set_identify")

    def get_termination(self) -> bool:
        self._require_open()
        val = ct.c_int()
        rc = self._lib.gsusb_channel_get_termination(self._ch, ct.byref(val))
        self._check(rc, "get_termination")
        return bool(val.value)

    def set_termination(self, enable: bool) -> None:
        self._require_open()
        rc = self._lib.gsusb_channel_set_termination(self._ch, 1 if enable else 0)
        self._check(rc, "set_termination")

    # --- internal ---

    def _errmsg(self, rc: int) -> str:
        msg = self._lib.gsusb_strerror(rc)
        return msg.decode(errors="replace") if msg else "unknown error"

    def _check(self, rc: int, context: str) -> int:
        if rc < 0:
            raise GsusbError(rc, f"{context}: {self._errmsg(rc)}")
        return rc

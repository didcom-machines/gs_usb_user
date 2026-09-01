"""Bus: opens one channel on the GCAN/ECAN/Rexon USBCANI-V503 adapter and,
once started, runs one background listener thread that is the sole
consumer of the C library's receive queue. Every frame it reads is both
dispatched to registered Detector objects and pushed onto an internal
queue that recv() drains -- so detectors and polling-style recv() calls
can be used together (or alone) without racing each other for the same
frames. Your own code (or a Detector's on_detect()) can call send() at any
time; it's an independent USB OUT transfer that never blocks on, or is
blocked by, the listener thread.

This mirrors gsusb.bus.Bus's shape closely by design -- same method
names/signatures wherever the concept applies -- so porting code between
the two is close to a search-and-replace. Capabilities gsusb.Bus has that
this protocol hasn't confirmed yet (get_state, set_identify,
get_termination/set_termination, configure_raw, a second channel) keep
their method names here but raise GcanError rather than not existing; see
driver/gcan_native/README.md for exactly what's confirmed vs. stubbed.
bt_const/data_bt_const have no equivalent at all here, deliberately: they
describe a hardware bit-timing *calculator*'s register ranges, and this
protocol's init sequence is one opaque captured byte string, not
decomposed registers -- there's no calculator for them to describe.
"""
import ctypes as ct
import logging
import queue
import threading
from typing import Optional

from cancore import Frame

from . import _ffi
from .exceptions import GcanError

logger = logging.getLogger("gcan.bus")

# How often the listener thread wakes up when the bus is idle, purely to
# notice stop() -- it adds no latency to detecting a real frame, since
# gcan_native_channel_recv() wakes up immediately when one arrives.
_RECV_POLL_MS = 200


class Bus:
    """One CAN channel on the GCAN/ECAN/Rexon USBCANI-V503 adapter.

    with Bus() as bus:
        bus.configure(bitrate=500000)
        bus.add_detector(IdDetector(0x100, callback=my_reaction))
        bus.start()
        ...
        bus.send(Frame.standard(0x200, b"\\x01"))

    Unlike gsusb.Bus, there's no vid/pid/usb_bus/usb_addr to pass -- this
    driver only knows one adapter model (a fixed USB id) and doesn't yet
    support selecting among several identical ones if more than one is
    attached.
    """

    #: cancore.Frame (the same class gsusb.Bus.Frame points to -- there's
    #: only one Frame class, see cancore's module doc), exposed here so
    #: code that obtained a Bus without importing gcan or cancore
    #: directly (e.g. via candetect.open_bus()) can still do
    #: bus.Frame.standard(...).
    Frame = Frame

    def __init__(self, channel: int = 0, library_path: Optional[str] = None):
        self._lib = _ffi.load_library(library_path)
        self._channel_index = channel

        self._ctx = None
        self._dev = None
        self._ch = None

        self._detectors = []
        self._detectors_lock = threading.Lock()
        self._listener_thread: Optional[threading.Thread] = None
        self._stop_event = threading.Event()
        self._started = False

        # The listener thread is the *only* caller of
        # gcan_native_channel_recv() once started -- a second, independent
        # consumer (e.g. recv() calling it directly) would race it for the
        # same frames. recv() instead drains this queue, which the
        # listener thread feeds alongside dispatching to detectors.
        self._recv_queue: "queue.Queue[Frame]" = queue.Queue(maxsize=512)

    # --- lifecycle ---

    def open(self) -> "Bus":
        if self._dev is not None:
            return self

        ctx_out = ct.c_void_p()
        rc = self._lib.gcan_native_init(ct.byref(ctx_out))
        if rc != 0:
            raise GcanError(rc, f"gcan_native_init: {self._errmsg(rc)}")
        self._ctx = ctx_out

        errbuf = ct.create_string_buffer(256)
        dev = self._lib.gcan_native_open(self._ctx, errbuf, len(errbuf))
        if not dev:
            self._lib.gcan_native_exit(self._ctx)
            self._ctx = None
            raise GcanError(_ffi.ERR_NOT_FOUND, errbuf.value.decode(errors="replace"))
        self._dev = dev

        ch = self._lib.gcan_native_channel_get(self._dev, self._channel_index)
        if not ch:
            self.close()
            raise GcanError(_ffi.ERR_INVALID,
                             f"channel {self._channel_index} does not exist on this device")
        self._ch = ch
        return self

    def close(self) -> None:
        self.stop()
        if self._dev is not None:
            self._lib.gcan_native_close(self._dev)
            self._dev = None
            self._ch = None
        if self._ctx is not None:
            self._lib.gcan_native_exit(self._ctx)
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
        """Unlike gsusb.Bus.device_config, there's no sw_version/hw_version
        here -- no board/firmware-version readout has been reverse
        engineered for this protocol (see driver/gcan_native/README.md)."""
        self._require_open()
        c = self._lib.gcan_native_get_device_config(self._dev).contents
        return {"channel_count": c.channel_count}

    @property
    def channel_count(self) -> int:
        self._require_open()
        return self._lib.gcan_native_channel_count(self._dev)

    @property
    def features(self) -> int:
        """Always 0 right now -- see gcan_native_feature's doc."""
        self._require_open()
        return self._lib.gcan_native_channel_features(self._ch)

    # --- configuration ---

    def configure(self, bitrate: int = 500000, sample_point: float = 0.0) -> None:
        """Runs the init handshake. Must be called before start().
        500000 (the default) is the only confirmed-working bitrate; any
        other value raises GcanError. sample_point is accepted for
        signature parity with gsusb.Bus.configure() but has no effect --
        there's no bit-timing calculator behind this driver, see this
        module's doc."""
        self._require_open()
        rc = self._lib.gcan_native_channel_set_bitrate(self._ch, bitrate, sample_point)
        self._check(rc, "set_bitrate")

    def configure_raw(self, prop_seg: int, phase_seg1: int, phase_seg2: int,
                       sjw: int, brp: int) -> None:
        """Not supported -- always raises GcanError. No raw register-level
        timing has been reverse engineered for this protocol, unlike
        gsusb.Bus.configure_raw()."""
        self._require_open()
        rc = self._lib.gcan_native_channel_set_bittiming_raw(
            self._ch, prop_seg, phase_seg1, phase_seg2, sjw, brp)
        self._check(rc, "set_bittiming_raw")

    # --- start/stop ---

    def start(self, listen_only: bool = False, loopback: bool = False,
              one_shot: bool = False) -> None:
        """Every flag here still requires mode_flags=0 at the C layer
        right now (none is confirmed supported) -- passing any of them
        True raises GcanError rather than silently starting in normal
        mode anyway."""
        self._require_open()
        if self._started:
            return

        mode = 0
        if listen_only: mode |= _ffi.MODE_LISTEN_ONLY
        if loopback: mode |= _ffi.MODE_LOOPBACK
        if one_shot: mode |= _ffi.MODE_ONE_SHOT

        rc = self._lib.gcan_native_channel_start(self._ch, mode)
        self._check(rc, "start")

        self._started = True
        self._stop_event.clear()
        self._listener_thread = threading.Thread(
            target=self._listen_loop, name="gcan-bus-listener", daemon=True)
        self._listener_thread.start()

    def stop(self) -> None:
        if not self._started:
            return
        self._stop_event.set()
        if self._listener_thread is not None:
            self._listener_thread.join(timeout=2.0)
            self._listener_thread = None
        if self._ch is not None:
            self._lib.gcan_native_channel_stop(self._ch)
        self._started = False

    # --- detectors (the object pattern for event detection) ---

    def add_detector(self, detector) -> None:
        with self._detectors_lock:
            self._detectors.append(detector)

    def remove_detector(self, detector) -> None:
        with self._detectors_lock:
            self._detectors.remove(detector)

    def _listen_loop(self) -> None:
        frame_c = _ffi.GcanNativeFrame()
        while not self._stop_event.is_set():
            rc = self._lib.gcan_native_channel_recv(self._ch, ct.byref(frame_c), _RECV_POLL_MS)
            if rc == 0:
                continue  # idle timeout (or an unrecognized record), loop to re-check _stop_event
            if rc < 0:
                logger.warning("gcan_native_channel_recv: %s", self._errmsg(rc))
                break

            frame = _ffi.frame_from_ctypes(frame_c)

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
        """Raises GcanError if frame.is_rtr -- not confirmed supported by
        this protocol, see Frame.is_rtr's docstring. Raises ValueError if
        frame.is_fd -- this hardware confirmed has no CAN-FD support."""
        self._require_open()
        c = _ffi.frame_to_ctypes(frame)
        rc = self._lib.gcan_native_channel_send(self._ch, ct.byref(c), timeout_ms)
        self._check(rc, "send")

    def recv(self, timeout_ms: int = 1000) -> Optional[Frame]:
        """Blocking single-frame receive, for polling-style code instead of
        (or alongside) detectors -- drains the same frames the listener
        thread feeds to detectors (see _listen_loop). Returns None on
        timeout."""
        self._require_open()
        try:
            return self._recv_queue.get(timeout=timeout_ms / 1000.0)
        except queue.Empty:
            return None

    # --- state / identify / termination (all currently unsupported stubs) ---

    def get_state(self) -> dict:
        """Not supported -- always raises GcanError. No status/error
        register readout has been reverse engineered for this protocol,
        unlike gsusb.Bus.get_state()."""
        self._require_open()
        state, rxerr, txerr = ct.c_uint32(), ct.c_uint32(), ct.c_uint32()
        rc = self._lib.gcan_native_channel_get_state(
            self._ch, ct.byref(state), ct.byref(rxerr), ct.byref(txerr))
        self._check(rc, "get_state")
        return {"state": state.value, "rxerr": rxerr.value, "txerr": txerr.value}

    def set_identify(self, on: bool) -> None:
        """Not supported -- always raises GcanError. Unknown whether this
        hardware has an identify/blink command."""
        self._require_open()
        rc = self._lib.gcan_native_channel_set_identify(self._ch, 1 if on else 0)
        self._check(rc, "set_identify")

    def get_termination(self) -> bool:
        """Not supported -- always raises GcanError. Unknown whether this
        hardware has switchable termination."""
        self._require_open()
        val = ct.c_int()
        rc = self._lib.gcan_native_channel_get_termination(self._ch, ct.byref(val))
        self._check(rc, "get_termination")
        return bool(val.value)

    def set_termination(self, enable: bool) -> None:
        """Not supported -- always raises GcanError."""
        self._require_open()
        rc = self._lib.gcan_native_channel_set_termination(self._ch, 1 if enable else 0)
        self._check(rc, "set_termination")

    # --- internal ---

    def _errmsg(self, rc: int) -> str:
        msg = self._lib.gcan_native_strerror(rc)
        return msg.decode(errors="replace") if msg else "unknown error"

    def _check(self, rc: int, context: str) -> int:
        if rc < 0:
            raise GcanError(rc, f"{context}: {self._errmsg(rc)}")
        return rc

"""Optional python-can BusABC adapter, registered as interface="gsusb".

Not imported by gsusb/__init__.py, so the core SDK has no python-can
dependency -- only import this module (or use `can.Bus(interface="gsusb")`)
if you have python-can installed.

Note this is a *different* interface name than python-can's own built-in
"gs_usb" (with underscore), which talks to the same protocol via pyusb and
a separate `gs_usb` PyPI package. That built-in interface doesn't support
CAN-FD, hardware timestamps, listen-only/termination/get-state, or
multi-channel devices; this adapter, backed by ../../driver's libgsusb.so,
does.

Field mapping note: gsusb.Frame bakes EFF/RTR/ERR into can_id (same layout
as Linux <linux/can.h>); can.Message uses separate is_extended_id/
is_remote_frame/is_error_frame booleans and a bare arbitration_id. This
adapter is exactly the translation between the two.

Timestamp note: gsusb.Frame.timestamp_us is the device's free-running
hardware tick counter (not correlated to wall-clock time -- see
driver/README.md), so can.Message.timestamp here is the host's
time.time() at the moment the frame was retrieved from the driver, not a
device-derived value. Good enough for logging/ordering; not a substitute
for hardware-timestamp-based latency analysis.
"""
import time
from typing import Optional

try:
    import can
except ImportError as exc:  # pragma: no cover
    raise ImportError(
        "gsusb.python_can requires the 'python-can' package: pip install python-can"
    ) from exc

from cancore import Frame

from . import _ffi
from .bus import Bus


class GsUsbNativeBus(can.BusABC):
    """python-can interface="gsusb", backed by this project's own gsusb.Bus."""

    def __init__(
        self,
        channel=None,
        vid: Optional[int] = None,
        pid: Optional[int] = None,
        usb_bus: Optional[int] = None,
        usb_addr: Optional[int] = None,
        gs_channel: int = 0,
        bitrate: int = 500_000,
        fd: bool = False,
        data_bitrate: Optional[int] = None,
        sample_point: float = 0.0,
        data_sample_point: float = 0.0,
        listen_only: bool = False,
        loopback: bool = False,
        one_shot: bool = False,
        library_path: Optional[str] = None,
        can_filters=None,
        **kwargs,
    ):
        """
        :param channel: purely descriptive label for channel_info; does not
            select the device (use vid/pid/usb_bus/usb_addr for that, same
            as gsusb.Bus -- see gsusb_info --scan on the target)
        :param gs_channel: which CAN channel on the adapter (0 unless it's
            a multi-channel device)
        :param fd: enable CAN-FD; requires data_bitrate
        :param sample_point: 0 picks a CiA-recommended default (see
            driver/src/bittiming.h)
        """
        self._bus = Bus(vid=vid, pid=pid, usb_bus=usb_bus, usb_addr=usb_addr,
                        channel=gs_channel, library_path=library_path)
        self._bus.open()
        self._bus.configure(bitrate=bitrate, sample_point=sample_point, fd=fd,
                            data_bitrate=data_bitrate, data_sample_point=data_sample_point)
        self._bus.start(listen_only=listen_only, loopback=loopback, fd=fd, one_shot=one_shot)

        self.channel_info = channel or f"gsusb channel {gs_channel}"
        self._can_protocol = can.CanProtocol.CAN_FD if fd else can.CanProtocol.CAN_20

        super().__init__(channel=channel, can_filters=can_filters, **kwargs)

    def send(self, msg: "can.Message", timeout: Optional[float] = None) -> None:
        flags = 0
        if msg.is_fd:
            flags |= _ffi.FRAME_FD
        if msg.bitrate_switch:
            flags |= _ffi.FRAME_BRS
        if msg.error_state_indicator:
            flags |= _ffi.FRAME_ESI

        can_id = msg.arbitration_id
        if msg.is_extended_id:
            can_id |= _ffi.EFF_FLAG
        if msg.is_remote_frame:
            can_id |= _ffi.RTR_FLAG
        if msg.is_error_frame:
            can_id |= _ffi.ERR_FLAG

        frame = Frame(can_id=can_id, data=bytes(msg.data), flags=flags)
        timeout_ms = round(timeout * 1000) if timeout else 1000
        try:
            self._bus.send(frame, timeout_ms=timeout_ms)
        except Exception as exc:
            raise can.exceptions.CanOperationError(str(exc)) from exc

    def _recv_internal(self, timeout: Optional[float]):
        # Poll in reasonably sized chunks rather than 1ms slices; BusABC's
        # own recv() loop re-calls us as needed for a real indefinite block.
        timeout_ms = round(timeout * 1000) if timeout else 200
        frame = self._bus.recv(timeout_ms=timeout_ms)
        if frame is None:
            return None, False

        msg = can.Message(
            timestamp=time.time(),
            arbitration_id=frame.id,
            is_extended_id=frame.is_extended,
            is_remote_frame=frame.is_rtr,
            is_error_frame=frame.is_error,
            channel=self.channel_info,
            data=bytes(frame.data),
            is_fd=frame.is_fd,
            bitrate_switch=frame.is_brs,
            error_state_indicator=frame.is_esi,
        )
        return msg, False

    def shutdown(self) -> None:
        super().shutdown()
        self._bus.close()

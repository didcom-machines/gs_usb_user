"""Optional python-can BusABC adapter, registered as interface="gcan".

Not imported by gcan/__init__.py, so the core SDK has no python-can
dependency -- only import this module (or use `can.Bus(interface="gcan")`)
if you have python-can installed.

Field mapping note: gcan.Frame bakes EFF/RTR/ERR into can_id (same layout
as gsusb.Frame and Linux <linux/can.h>); can.Message uses separate
is_extended_id/is_remote_frame/is_error_frame booleans and a bare
arbitration_id. This hardware has no CAN-FD (a confirmed hardware fact,
not a gap -- see gcan_native.h), so is_fd is always False and sending an
FD message raises. Remote frames are accepted here for interface parity
with gsusb's adapter, but Bus.send() will raise -- see Frame.is_rtr's
docstring.
"""
import time
from typing import Optional

try:
    import can
except ImportError as exc:  # pragma: no cover
    raise ImportError(
        "gcan.python_can requires the 'python-can' package: pip install python-can"
    ) from exc

from cancore import Frame

from . import _ffi
from .bus import Bus


class GcanNativeBus(can.BusABC):
    """python-can interface="gcan", backed by this project's own gcan.Bus."""

    def __init__(
        self,
        channel=None,
        can_index: int = 0,
        bitrate: int = 500_000,
        listen_only: bool = False,
        loopback: bool = False,
        one_shot: bool = False,
        library_path: Optional[str] = None,
        can_filters=None,
        **kwargs,
    ):
        """
        :param channel: purely descriptive label for channel_info; does not
            select the device (there's only one supported adapter model,
            opened by USB id -- see gcan.Bus's docstring)
        :param bitrate: 500000 (the default) is the only confirmed-working
            rate -- see driver/gcan_native/README.md
        """
        self._bus = Bus(channel=can_index, library_path=library_path)
        self._bus.open()
        self._bus.configure(bitrate=bitrate)
        self._bus.start(listen_only=listen_only, loopback=loopback, one_shot=one_shot)

        self.channel_info = channel or f"gcan channel {can_index}"
        self._can_protocol = can.CanProtocol.CAN_20

        super().__init__(channel=channel, can_filters=can_filters, **kwargs)

    def send(self, msg: "can.Message", timeout: Optional[float] = None) -> None:
        if msg.is_fd:
            raise can.exceptions.CanOperationError(
                "this hardware (driver/gcan_native/) has no CAN-FD support")

        can_id = msg.arbitration_id
        if msg.is_extended_id:
            can_id |= _ffi.EFF_FLAG
        if msg.is_remote_frame:
            can_id |= _ffi.RTR_FLAG
        if msg.is_error_frame:
            can_id |= _ffi.ERR_FLAG

        frame = Frame(can_id=can_id, data=bytes(msg.data))
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
        )
        return msg, False

    def shutdown(self) -> None:
        super().shutdown()
        self._bus.close()

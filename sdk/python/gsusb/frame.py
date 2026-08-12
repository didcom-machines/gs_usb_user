"""Frame: a CAN frame, with the same id-flag-bit layout as Linux <linux/can.h>
(and the C driver's gsusb_frame), plus convenience constructors and
properties so application code rarely has to touch raw flag bits directly.
"""
from dataclasses import dataclass

from . import _ffi


@dataclass
class Frame:
    can_id: int
    data: bytes = b""
    flags: int = 0
    timestamp_us: int = 0

    @property
    def is_extended(self) -> bool:
        return bool(self.can_id & _ffi.EFF_FLAG)

    @property
    def is_rtr(self) -> bool:
        return bool(self.can_id & _ffi.RTR_FLAG)

    @property
    def is_error(self) -> bool:
        return bool(self.can_id & _ffi.ERR_FLAG)

    @property
    def id(self) -> int:
        """The bare arbitration id, with the EFF/RTR/ERR flag bits masked
        off -- this is almost always what you want to match against."""
        mask = _ffi.EFF_MASK if self.is_extended else _ffi.SFF_MASK
        return self.can_id & mask

    @property
    def is_fd(self) -> bool:
        return bool(self.flags & _ffi.FRAME_FD)

    @property
    def is_brs(self) -> bool:
        return bool(self.flags & _ffi.FRAME_BRS)

    @property
    def is_esi(self) -> bool:
        return bool(self.flags & _ffi.FRAME_ESI)

    @property
    def len(self) -> int:
        return len(self.data)

    # --- convenience constructors ---

    @classmethod
    def standard(cls, can_id: int, data: bytes = b"") -> "Frame":
        return cls(can_id=can_id & _ffi.SFF_MASK, data=bytes(data))

    @classmethod
    def extended(cls, can_id: int, data: bytes = b"") -> "Frame":
        return cls(can_id=(can_id & _ffi.EFF_MASK) | _ffi.EFF_FLAG, data=bytes(data))

    @classmethod
    def remote(cls, can_id: int, extended: bool = False) -> "Frame":
        eff = _ffi.EFF_FLAG if extended else 0
        mask = _ffi.EFF_MASK if extended else _ffi.SFF_MASK
        return cls(can_id=(can_id & mask) | eff | _ffi.RTR_FLAG, data=b"")

    @classmethod
    def canfd(cls, can_id: int, data: bytes = b"", brs: bool = True,
              esi: bool = False, extended: bool = False) -> "Frame":
        eff = _ffi.EFF_FLAG if extended else 0
        mask = _ffi.EFF_MASK if extended else _ffi.SFF_MASK
        flags = _ffi.FRAME_FD
        if brs:
            flags |= _ffi.FRAME_BRS
        if esi:
            flags |= _ffi.FRAME_ESI
        return cls(can_id=(can_id & mask) | eff, data=bytes(data), flags=flags)

    def __repr__(self) -> str:
        tags = []
        if self.is_extended: tags.append("EFF")
        if self.is_rtr: tags.append("RTR")
        if self.is_error: tags.append("ERR")
        if self.is_fd: tags.append("FD")
        if self.is_brs: tags.append("BRS")
        if self.is_esi: tags.append("ESI")
        tag = f" [{','.join(tags)}]" if tags else ""
        return (f"Frame(id=0x{self.id:X}{tag}, len={self.len}, "
                f"data={self.data.hex(' ').upper()})")

    # --- ctypes interop, internal use by Bus ---

    @classmethod
    def _from_ctypes(cls, c: "_ffi.GsusbFrame") -> "Frame":
        return cls(
            can_id=c.can_id,
            data=bytes(c.data[: c.len]),
            flags=c.flags,
            timestamp_us=c.timestamp_us,
        )

    def _to_ctypes(self) -> "_ffi.GsusbFrame":
        data = bytes(self.data)
        max_len = _ffi.GSUSB_MAX_DLEN if (self.flags & _ffi.FRAME_FD) else 8
        if len(data) > max_len:
            raise ValueError(
                f"frame data length {len(data)} exceeds max {max_len} bytes "
                f"for this frame type (set flags=gsusb.FRAME_FD for CAN-FD)"
            )
        c = _ffi.GsusbFrame()
        c.can_id = self.can_id
        c.len = len(data)
        c.flags = self.flags
        for i, byte in enumerate(data):
            c.data[i] = byte
        c.timestamp_us = self.timestamp_us
        return c

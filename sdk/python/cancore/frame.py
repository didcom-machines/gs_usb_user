"""Frame: a CAN frame, with the same id-flag-bit layout as Linux
<linux/can.h>, plus convenience constructors and properties so
application code rarely has to touch raw flag bits directly.

This class is deliberately hardware-agnostic -- it has no ctypes
dependency and doesn't know gsusb or gcan exist. Each backend's own
_ffi.py owns the translation to/from its C library's wire struct
(frame_to_ctypes()/frame_from_ctypes()), including deciding what to do
with a capability its hardware doesn't have (e.g. gcan's backend rejects
an FD frame or one with RTR/ERR set -- see gcan/_ffi.py). Frame itself
just carries the data; it's the one thing both backends agree on, so
there's exactly one Frame class, not one per backend.
"""
from dataclasses import dataclass

# CAN ID flags/masks -- identical bit layout to Linux <linux/can.h>.
EFF_FLAG = 0x80000000
RTR_FLAG = 0x40000000
ERR_FLAG = 0x20000000
EFF_MASK = 0x1FFFFFFF
SFF_MASK = 0x000007FF

# Frame.flags bits (CAN-FD) -- meaningful only on backends whose hardware
# actually supports CAN-FD (gsusb's gs_usb-protocol adapters can; gcan's
# GCAN adapter confirmed can't, see driver/gcan_native/README.md).
FRAME_FD = 0x01
FRAME_BRS = 0x02
FRAME_ESI = 0x04


@dataclass
class Frame:
    can_id: int
    data: bytes = b""
    flags: int = 0
    #: Device hw timestamp. A real microsecond tick counter on backends
    #: with hardware timestamp support; always 0 on backends without one
    #: (e.g. gcan's -- its RX record's trailing bytes that presumably
    #: carry this aren't decoded yet, see driver/gcan_native/README.md,
    #: so despite the field name no unit is actually confirmed there).
    timestamp_us: int = 0

    @property
    def is_extended(self) -> bool:
        return bool(self.can_id & EFF_FLAG)

    @property
    def is_rtr(self) -> bool:
        return bool(self.can_id & RTR_FLAG)

    @property
    def is_error(self) -> bool:
        return bool(self.can_id & ERR_FLAG)

    @property
    def id(self) -> int:
        """The bare arbitration id, with the EFF/RTR/ERR flag bits masked
        off -- this is almost always what you want to match against."""
        mask = EFF_MASK if self.is_extended else SFF_MASK
        return self.can_id & mask

    @property
    def is_fd(self) -> bool:
        return bool(self.flags & FRAME_FD)

    @property
    def is_brs(self) -> bool:
        return bool(self.flags & FRAME_BRS)

    @property
    def is_esi(self) -> bool:
        return bool(self.flags & FRAME_ESI)

    @property
    def len(self) -> int:
        return len(self.data)

    # --- convenience constructors ---

    @classmethod
    def standard(cls, can_id: int, data: bytes = b"") -> "Frame":
        return cls(can_id=can_id & SFF_MASK, data=bytes(data))

    @classmethod
    def extended(cls, can_id: int, data: bytes = b"") -> "Frame":
        return cls(can_id=(can_id & EFF_MASK) | EFF_FLAG, data=bytes(data))

    @classmethod
    def remote(cls, can_id: int, extended: bool = False) -> "Frame":
        """Builds an RTR frame. Whether sending one actually works depends
        on the backend -- gsusb's protocol supports it; gcan's doesn't
        confirm it does, and Bus.send() raises there (see
        driver/gcan_native/README.md)."""
        eff = EFF_FLAG if extended else 0
        mask = EFF_MASK if extended else SFF_MASK
        return cls(can_id=(can_id & mask) | eff | RTR_FLAG, data=b"")

    @classmethod
    def canfd(cls, can_id: int, data: bytes = b"", brs: bool = True,
              esi: bool = False, extended: bool = False) -> "Frame":
        """Builds a CAN-FD frame. Only meaningful on a backend whose
        hardware supports CAN-FD (gsusb's does; gcan's confirmed doesn't
        -- Bus.send() raises there rather than silently sending a classic
        frame)."""
        eff = EFF_FLAG if extended else 0
        mask = EFF_MASK if extended else SFF_MASK
        flags = FRAME_FD
        if brs:
            flags |= FRAME_BRS
        if esi:
            flags |= FRAME_ESI
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

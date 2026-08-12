"""Known gs_usb-compatible USB vendor:product ids -- mirrors the table in
driver/tools/common.h. Bus() tries these in order when vid/pid aren't given.
"""
from collections import namedtuple

KnownDevice = namedtuple("KnownDevice", "vid pid name")

KNOWN_DEVICES = [
    KnownDevice(0x1D50, 0x606F, "gs_usb (original GS USB firmware)"),
    KnownDevice(0x1209, 0x2323, "candleLight"),
    KnownDevice(0x1CD2, 0x606F, "CES CANext FD"),
    KnownDevice(0x16D0, 0x10B8, "ABE canDebugger FD"),
    KnownDevice(0x16D0, 0x0F30, "Xylanta Saint3"),
    KnownDevice(0x1209, 0xCA01, "CANnectivity"),
]

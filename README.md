# gs_usb userspace: driver + SDK

**Purpose:** make USB-to-CAN adapters (candleLight, CANable, the original
GS USB2CAN, CES CANext FD, and others -- see `driver/README.md` for the
full list) work on Linux machines where you can't build or load a kernel
module -- embedded/vendor Linux images (OpenWrt/RutOS routers, Yocto
builds, locked-down appliances) that ship without `gs_usb.ko` and with no
way to add one. It talks to the adapter's existing USB vendor protocol
directly via `libusb-1.0`, replicating the kernel driver's wire format in
userspace -- no SocketCAN, no kernel module. Includes a Python SDK for
writing applications on top of it (originally built for deployment on
devices like the Teltonika RUTX11, but usable anywhere).

## Requirements

- **A working generic USB host-controller stack in the kernel** -- i.e.
  the system can already enumerate USB devices at all (`usbcore` plus
  whatever host-controller driver the hardware needs: xHCI/EHCI/OHCI).
  This project sits *on top of* that generic USB support via
  `libusb-1.0`/`usbfs`; it replaces only the CAN-adapter-specific kernel
  driver (`gs_usb.ko`), not USB support in general. Every mainstream Linux
  kernel -- including RutOS/OpenWrt on the RUTX11 -- ships the generic USB
  stack even when it omits `gs_usb.ko`, so this requirement is almost
  never actually missing in practice, but it is a real one: without a USB
  base driver already in place for `libusb` to sit on, there's no device
  node for either the kernel driver or this one to talk to.
- `libusb-1.0` -- build-time: `libusb-1.0-0-dev` + a C compiler; runtime:
  just the shared library, already present on virtually any desktop/server
  Linux and straightforward to have on embedded targets too.
- Root, or the provided udev rule (`driver/udev/99-gsusb.rules`) so a
  regular user in the `plugdev` group can open the adapter -- no kernel
  module and no `python-can`/pip required for the Python SDK (see
  `sdk/python/README.md`).

## Layout

- **[`driver/`](driver/)** -- the C library (`libusb-1.0`-based, replicates
  the kernel driver's wire protocol) and CLI tools (`gsusb_info`,
  `gsusb_dump`, `gsusb_send`, `gsusb_react`). Builds `libgsusb.so` as well
  as the standalone binaries. Start here to build/deploy on a new target,
  or to use it directly from C.

- **[`driver/gcan/`](driver/gcan/)** -- the vendor's own `libECanVci.so.1`
  for the GCAN/ECAN/Rexon USBCANI-V503 adapter (x86/x86-64 prebuilt
  binaries only -- no ARM build) alongside a third-party reverse-engineered
  `libusb-1.0` CLI for the same hardware that the from-scratch driver below
  is built from.

- **[`driver/gcan_native/`](driver/gcan_native/)** -- a from-scratch,
  `libusb-1.0`-only userspace driver for the same GCAN/ECAN/Rexon
  USBCANI-V503 adapter, built from a reverse-engineered protocol rather
  than the vendor blob above -- so it builds and runs natively on ARM
  (the RUTX11 included). Currently CAN1/500 kbit/s/classic-CAN only, and
  unverified against real hardware (see its README). This is what the
  Python SDK now binds to.

- **[`sdk/python/`](sdk/python/)** -- Python bindings, via `ctypes`, with
  an object-oriented event-detection API (`Bus`, `Frame`, `Detector`) for
  applications that need to watch the bus and react by transmitting,
  concurrently, from a single process. `Frame`/`Detector` (`cancore/`) are
  hardware-agnostic and shared as-is by two backends -- `gsusb` (backed by
  `driver/`'s `libgsusb.so`) and `gcan` (backed by `driver/gcan_native/`'s
  `libgcan_native.so`) -- whose `Bus` classes are deliberately built to
  the same shape, plus `candetect` to auto-pick whichever adapter is
  attached. See `sdk/python/README.md` for the API alignment and gcan's
  current limitations. Both backends include an optional
  `python-can`-compatible `BusABC` adapter (interface names
  `"gsusb"`/`"gcan"`) for environments
  that have `python-can` installed.

- **[`deploy/rutx11.sh`](deploy/rutx11.sh)** -- one-shot script that
  cross-builds the gsusb driver, the gcan_native driver, and the Python SDK
  for the RUTX11 (armv7/musl) and deploys all of it over SSH, with a real
  post-deploy verification. `./deploy/rutx11.sh --host <ip>`.

## Quick start

```sh
cd driver && make          # builds the gsusb CLI tools + libgsusb.so
cd driver/gcan_native && make   # builds libgcan_native.so + gcan_native_tool
```
Then either use the CLI tools directly (see `driver/README.md` and
`driver/gcan_native/README.md`), or build a Python application against
`sdk/python/candetect` (auto-picks whichever adapter is attached) or a
specific backend, `sdk/python/gsusb`/`sdk/python/gcan` (see
`sdk/python/README.md`). Deploying to a RUTX11 specifically:
`deploy/rutx11.sh --host <ip>`.

## Status

Verified end-to-end against real hardware (a CANable 2.5 running
candleLight firmware) on two different targets: an NVIDIA Jetson (aarch64,
Ubuntu, glibc) and a Teltonika RUTX11 (armv7, RutOS/OpenWrt, musl) -- see
each subproject's README for target-specific build/deploy notes.

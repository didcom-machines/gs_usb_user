# gs_usb userspace: driver + SDK

A userspace port of the Linux kernel's `gs_usb` CAN driver, for systems
where you can't build or load a kernel module -- plus a Python SDK for
writing applications on top of it (built for deployment on devices like the
Teltonika RUTX11).

## Layout

- **[`driver/`](driver/)** -- the C library (`libusb-1.0`-based, replicates
  the kernel driver's wire protocol) and CLI tools (`gsusb_info`,
  `gsusb_dump`, `gsusb_send`, `gsusb_react`). Builds `libgsusb.so` as well
  as the standalone binaries. Start here to build/deploy on a new target,
  or to use it directly from C.

- **[`sdk/python/`](sdk/python/)** -- Python bindings over `libgsusb.so`
  (via `ctypes`, no compiler needed on the target), with an object-oriented
  event-detection API (`Bus`, `Frame`, `Detector`) for applications that
  need to watch the bus and react by transmitting, concurrently, from a
  single process. Also includes an optional `python-can`-compatible
  `BusABC` adapter (`gsusb.python_can`, interface name `"gsusb"`) for
  environments that have `python-can` installed.

- **[`deploy/rutx11.sh`](deploy/rutx11.sh)** -- one-shot script that
  cross-builds the driver + Python SDK for the RUTX11 (armv7/musl) and
  deploys both over SSH, with a real post-deploy verification (device scan
  + shared-library load check). `./deploy/rutx11.sh --host <ip>`.

## Quick start

```sh
cd driver && make          # builds the CLI tools + libgsusb.so
```
Then either use the CLI tools directly (see `driver/README.md`), or build a
Python application against `sdk/python/gsusb` (see `sdk/python/README.md`).
Deploying to a RUTX11 specifically: `deploy/rutx11.sh --host <ip>`.

## Status

Verified end-to-end against real hardware (a CANable 2.5 running
candleLight firmware) on two different targets: an NVIDIA Jetson (aarch64,
Ubuntu, glibc) and a Teltonika RUTX11 (armv7, RutOS/OpenWrt, musl) -- see
each subproject's README for target-specific build/deploy notes.

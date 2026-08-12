# Android / Termux (aarch64) — secondary target

Cross-build of the gs_usb driver's CLI tools for aarch64 Android, for use
inside [Termux](https://termux.dev/). This lives on the `android-termux`
branch, separate from the main project, and is not part of the primary
Jetson/RUTX11 deliverable.

**Status: first real on-device attempt found a real bug, now fixed (still
not fully re-verified).** A first run on actual hardware hit
`gsusb_info: I/O error` on startup -- a genuine issue (see "A second,
less obvious fix" below), not a fluke, since it comes from
`libusb_init()`'s own device scan failing under SELinux. Fixed by using
`libusb_init_context()` with `LIBUSB_OPTION_NO_DEVICE_DISCOVERY` whenever
`--usb-fd` is given. Confirmed at the API level (a standalone test
program calling `gsusb_init_no_discovery()` succeeds) and the fix
compiles/cross-compiles cleanly, but hasn't yet been re-run on the actual
device that hit the original error -- that's the next thing to confirm.

## Why this needed real driver changes, not just a cross-compile

Unrooted Android's SELinux policy blocks a normal process from opening
`/dev/bus/usb/*` directly, so `gsusb_open()`'s usual enumerate-and-open flow
can't work in Termux without root. The standard workaround is
[Termux:API](https://wiki.termux.com/wiki/Termux:API)'s `termux-usb`
command: it requests permission through Android's own `UsbManager`
permission dialog and, once granted, runs your program with the
already-open file descriptor appended as its last argument.

To support that, `driver/include/gsusb.h` gained `gsusb_open_fd()`
(wraps an already-open fd via `libusb_wrap_sys_device()`, needs
libusb >= 1.0.23), and all four CLI tools gained a `--usb-fd N` flag as an
alternative to `--vid`/`--pid`. See the commit that introduced this for
the full design rationale.

**A second, less obvious fix was needed too**: plain `libusb_init()` scans
for/enumerates all attached USB devices at startup, regardless of whether
you'll actually use that scan -- and that scan itself fails under SELinux
on unrooted Android (surfaces as a generic `gsusb_info: I/O error`, since
it doesn't map to any more specific libusb error code), even though
`gsusb_open_fd()`'s `libusb_wrap_sys_device()` path never needed the scan
in the first place. libusb has a dedicated option for exactly this --
`LIBUSB_OPTION_NO_DEVICE_DISCOVERY`, whose own doc comment literally says
"typically needed on Android" -- set via `libusb_init_context()` instead
of plain `libusb_init()`. `gsusb_init_no_discovery()` wraps this; the CLI
tools call it automatically whenever `--usb-fd` is given.

## Build

```sh
deploy/android/build.sh
```

Requires Docker locally. Downloads the Android NDK (~660MB, cached in
`deploy/android/cache/` after the first run) and cross-builds libusb-1.0
from source, **statically linked** into the CLI tools -- so nothing needs
installing in Termux at runtime beyond the binaries themselves (no
`pkg install libusb` version-matching risk). Output: `dist-android/`.

Two real build issues came up and are documented in `build.sh` itself:
- `libusb`'s `autogen.sh` runs its own internal bare `./configure` (no
  flags) as a side effect, which always fails when cross-compiling --
  that failure is expected and ignored; the actual cross-compile happens
  in a separate, correctly-flagged `./configure` call right after.
- Android's bionic libc provides `pthread_*` directly (no separate
  `-lpthread` to link against, unlike glibc/musl) -- same underlying
  reason as musl on the RUTX11, but bionic doesn't even provide a
  compatibility stub, so `-lpthread` is a hard link error here.

## Deploying to a device

No SSH deploy script here (unlike `deploy/rutx11.sh`) since there's no
device to test against yet. Get the binaries onto the phone with `adb
push dist-android/* /data/local/tmp/` then `run-as`/copy into Termux's
home, or just download them from within Termux (e.g. host them somewhere
and `curl`/`wget`), then `chmod +x`.

## Running via termux-usb (unrooted)

```sh
pkg install termux-api      # once
termux-usb -l                # find the device path, e.g. /dev/bus/usb/001/002
```

`termux-usb -r -e COMMAND DEVICE` requests permission and runs COMMAND
with the granted fd appended as its last argument -- since our tools want
that fd via an explicit `--usb-fd N` flag rather than a bare trailing
number, point termux-usb at `run-with-usb.sh` (in this directory) instead
of directly at a tool:

```sh
TOOL=./gsusb_info TOOL_ARGS=--scan \
  termux-usb -r -e ./run-with-usb.sh /dev/bus/usb/001/002
```

**This wrapper is unverified** -- double-check `termux-usb --help` on the
actual device for its exact argument-passing behavior before relying on
it; the general shape (fd appended as the last arg) is documented Termux:
API behavior, but the precise quoting/argv details haven't been confirmed
against a real `termux-usb` invocation here.

## If the device is rooted instead

Skip `termux-usb` entirely -- run Termux as root (or `su -c`) and use the
binaries exactly like on the Jetson/RUTX11: `--vid`/`--pid` (or no flags,
to scan known ids) work normally once the process has direct permission to
open the USB device node.

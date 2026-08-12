# gs_usb_user

A userspace port of the Linux kernel's `gs_usb` CAN driver
(`drivers/net/can/usb/gs_usb.c`), for systems where you can't build/load a
kernel module. It talks directly to the same USB vendor requests and bulk
endpoints the kernel driver uses, via `libusb-1.0`, with no SocketCAN and no
kernel module involved.

This works *because* you can't load the module: with `gs_usb.ko` absent,
these adapters (candleLight, CANable, the original GS USB2CAN, CES CANext
FD, etc.) are vendor-specific USB devices that no other kernel driver binds
to, so the device sits unclaimed and `libusb` can open/claim it directly
from userspace (as an unprivileged user, given the udev rule below, or as
root).

## What's here

- `include/gsusb.h` - public C API of `libgsusb`.
- `src/gsusb.c` - the library: device discovery, the exact control-transfer
  protocol (`GS_USB_BREQ_*`), and bulk IN/OUT frame I/O.
- `src/gsusb_proto.h` - wire structs/constants transcribed from the kernel
  driver (control request numbers, frame layout, feature/mode flag bits).
- `src/bittiming.c` - computes CAN bit-timing register values (prop_seg,
  phase_seg1/2, sjw, brp) for a target bitrate + sample point.
- `tools/gsusb_info.c` - lists/inspects adapters (channels, bit-timing
  constants, supported features, termination state).
- `tools/gsusb_dump.c` - `candump`-style live frame dump.
- `tools/gsusb_send.c` - `cansend`-style frame transmit.
- `udev/99-gsusb.rules` - lets a non-root user (in the `plugdev` group)
  open the adapter.

## Build

```sh
sudo apt install build-essential libusb-1.0-0-dev pkg-config   # Debian/Ubuntu (arm64 or otherwise)
make
```

Binaries land in `bin/`. Cross-compiling for arm64 from another host: install
an aarch64 toolchain and a matching `libusb-1.0-dev` (or sysroot), then
`make CC=aarch64-linux-gnu-gcc`.

## Permissions

Either run the tools as root, or install the udev rule so your normal user
(in the `plugdev` group) can open the adapter:

```sh
sudo make install      # installs binaries to /usr/local/bin and the udev rule
sudo usermod -aG plugdev $USER   # if not already in that group; log out/in after
```

## Usage

```sh
# see what's plugged in without touching it
bin/gsusb_info --scan

# open the (first) adapter and print its capabilities
bin/gsusb_info

# live dump on channel 0 at 500 kbit/s until Ctrl-C
bin/gsusb_dump --bitrate 500000

# CAN-FD, 500k nominal / 2M data phase
bin/gsusb_dump --fd --bitrate 500000 --data-bitrate 2000000

# send a classic frame
bin/gsusb_send 123#DEADBEEF

# send a remote frame
bin/gsusb_send 123#R

# send a CAN-FD frame with BRS set (flags nibble bit0)
bin/gsusb_send --fd '1FFFFFFF##1' 0011223344556677

# target a specific adapter when more than one is plugged in
bin/gsusb_info --scan
bin/gsusb_dump --bus 3 --addr 7 --bitrate 250000
```

Run any tool with `--help` for the full flag list.

## Using it as a library

```c
#include "gsusb.h"

void *ctx;
gsusb_init(&ctx);
gsusb_dev *dev = gsusb_open(ctx, GSUSB_VID_CANDLELIGHT, GSUSB_PID_CANDLELIGHT,
                            -1, -1, NULL, 0);
gsusb_channel *ch = gsusb_channel_get(dev, 0);
gsusb_channel_set_bitrate(ch, 500000, 0 /* auto sample point */);
gsusb_channel_start(ch, 0 /* mode flags */);

gsusb_frame f = { .can_id = 0x123, .len = 4, .data = {0xDE,0xAD,0xBE,0xEF} };
gsusb_channel_send(ch, &f, 1000);

gsusb_frame rx;
if (gsusb_channel_recv(ch, &rx, 1000) > 0) { /* ... */ }

gsusb_channel_stop(ch);
gsusb_close(dev);
gsusb_exit(ctx);
```

## Known limitations vs. the kernel driver

- **One process at a time.** `libusb` claims the USB interface exclusively;
  unlike SocketCAN, this can't be shared across processes/interfaces. Only
  one of `gsusb_dump`/`gsusb_send`/your own program can hold a given adapter
  open at once.
- **Throughput.** The kernel driver keeps ~30 RX and 10 TX URBs in flight
  concurrently. This port uses one background reader thread doing
  synchronous bulk reads plus synchronous bulk writes for TX - plenty for
  diagnostics, scripting, and moderate bus load, but not tuned for
  saturating a bus at maximum frame rate. If you need that, look at
  `libusb`'s async API in `src/gsusb.c`'s reader thread as the extension
  point.
- **No tx-echo tracking.** `gsusb_channel_send()` returns once the USB OUT
  transfer completes, not once the device's tx-ack frame comes back (the
  reader thread still correctly discards those ack frames rather than
  misreporting them as received traffic).
- **Bit-timing calculator is our own**, not the kernel's
  `can_calc_bittiming()` — see `src/bittiming.h`. It searches all `brp`
  values the adapter's reported constants allow and picks the one closest
  to the requested bitrate, tie-broken by sample point. Use
  `gsusb_channel_set_bittiming_raw()` if you need to match an exact known-
  good set of register values instead.
- **Not implemented:** the CANtact Pro/LPC546XX firmware quirk, and
  `CAN_CTRLMODE_CC_LEN8_DLC` (encoding classic-CAN DLC 9-15 while still
  capping the payload at 8 bytes). Every other feature bit the protocol
  defines - CAN-FD, BRS/ESI, hardware timestamps, listen-only, loopback,
  triple-sample, one-shot, bus-error reporting, get-state, identify/blink,
  and termination control - is implemented.

## Supported adapters

Same USB IDs the kernel driver matches, all on USB interface 0:

| VID:PID | Device |
|---|---|
| 1d50:606f | Original GS USB2CAN |
| 1209:2323 | candleLight |
| 1cd2:606f | CES CANext FD |
| 16d0:10b8 | ABE canDebugger FD |
| 16d0:0f30 | Xylanta Saint3 |
| 1209:ca01 | CANnectivity |

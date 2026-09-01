# gcan_native

A from-scratch, `libusb-1.0`-only userspace driver for the **GCAN / ECAN /
Rexon USBCANI-V503** USB-CAN adapter (USB id `0c66:000c`) -- built to run
natively on ARM (the original goal was the Teltonika RUTX11, armv7/musl),
which [`driver/gcan/`](../gcan/)'s vendor `libECanVci.so.1` can't do since
it only ships x86/x86-64 prebuilt binaries.

## Origin

The wire protocol here isn't reverse-engineered by this project from
scratch -- it's carried over from
[rezaistoni-cloud/Gcan-USB-PRO-II-V503-for-RASPI-ARM64](https://github.com/rezaistoni-cloud/Gcan-USB-PRO-II-V503-for-RASPI-ARM64)
(MIT-licensed), a small CLI proof-of-concept built from real USB captures
of the vendor's `libECanVci.so` talking to this hardware. This project
refactors that captured protocol into a proper library, ctypes-bindable
from Python instead of shelled out to as a one-shot CLI tool, with an API
shaped to match `driver/`'s own `gsusb.h`/`libgsusb.so` as closely as
this hardware's confirmed capabilities allow -- same
init/open/channel-handle model, same function and error-code naming, so
porting code between the two drivers is close to a search-and-replace.
Where a gsusb capability has no reverse-engineered equivalent here yet,
the matching entry point still exists but returns
`GCAN_NATIVE_ERR_INVALID` rather than being silently absent -- see
"Confirmed protocol / current limitations" below for exactly which.

**This has not been verified against real hardware by this project** --
the upstream capture work was; this refactor has only been compile-checked
(no CAN adapter was available while writing it). Treat it as a promising,
unverified starting point, not a validated driver, until you've run it
against your actual adapter.

## Confirmed protocol / current limitations

Real, working capability:

- **One channel (index 0), 500 kbit/s only, classic CAN only.** The init
  handshake is a captured fixed byte sequence, not a computed bit-timing
  calculator like `driver/`'s gsusb port has -- other channels/bitrates
  aren't a missing *feature*, they're unknown protocol surface.
  `gcan_native_channel_set_bitrate()` returns `GCAN_NATIVE_ERR_INVALID`
  for anything but 500000 rather than guessing.
- TX/RX frame formats, endpoint numbers, and the exact init/close byte
  sequences are in [`src/gcan_native_proto.h`](src/gcan_native_proto.h).

Shape-parity stubs -- these entry points exist (matching `gsusb.h`'s
naming) so calling code doesn't need per-driver special-casing, but they
always return `GCAN_NATIVE_ERR_INVALID` today:

- `gcan_native_channel_set_bittiming_raw()` -- no raw register-level
  timing has been reverse engineered (unlike the vendor SDK's
  `Timing0`/`Timing1`, this capture's init sequence is opaque bytes, not
  decomposed registers).
- `gcan_native_channel_start()` with any `mode_flags` set -- no
  listen-only/loopback/one-shot mode has been captured.
- `gcan_native_channel_get_state()` -- no status/error register readout.
- `gcan_native_channel_set_identify()` -- unknown whether this hardware
  has an identify/blink command.
- `gcan_native_channel_get_termination()`/`set_termination()` -- unknown
  whether this hardware has switchable termination.
- A second channel (`gcan_native_channel_get(dev, 1)` returns `NULL`) --
  "USBCANI" vs. "PRO II" naming suggests a second physical channel may
  exist, but it has no captured protocol at all.
- Sending or receiving an RTR frame (`GCAN_NATIVE_RTR_FLAG`) -- not
  confirmed how/whether the wire protocol encodes it.
- Error-frame detection (`GCAN_NATIVE_ERR_FLAG`) -- not implemented; no
  frame this library produces ever has it set.

Deliberately *not* stubbed, because the concept doesn't apply to this
hardware at all (not a "not yet" gap): CAN-FD, and
`gcan_native_bittiming_const`-style register-range introspection (there's
no bit-timing calculator here for such a struct to describe).

Also unresolved: the 16-byte RX record's last 3 bytes (timestamp/
status-like) aren't decoded -- unknown meaning, so `gcan_native_frame`'s
`timestamp` field is always 0.

Extending any of the above needs a new USB capture (Wireshark/`usbmon`)
against real hardware exercising that specific feature via the vendor's
own Linux/Windows SDK, then adding the resulting bytes to
`gcan_native_proto.h`.

## Build

```sh
sudo apt install build-essential libusb-1.0-0-dev pkg-config   # Debian/Ubuntu
make
```

Binaries land in `bin/`: `gcan_native_tool` (a `tx`/`rx` CLI, mostly for
manual hardware verification) and `libgcan_native.so` (what the Python SDK
binds to). Cross-compiling for armv7: install an `arm-linux-gnueabihf`
toolchain and a matching `libusb-1.0-dev` (or sysroot), then
`make CC=arm-linux-gnueabihf-gcc` -- or reuse the Docker+QEMU Alpine flow
[`deploy/rutx11.sh`](../../deploy/rutx11.sh) already sets up for `driver/`.

## Usage

```sh
# listen on CAN1 @ 500 kbit/s until Ctrl-C
bin/gcan_native_tool rx

# send a classic frame
bin/gcan_native_tool tx --id 123 --data DEADBEEF

# send an extended-id frame
bin/gcan_native_tool tx --id 18DAF110 --data 11223344 --ext
```

## Permissions

Either run as root, or install the udev rule so your normal user (in the
`plugdev` group) can open the adapter:

```sh
sudo make install
sudo usermod -aG plugdev $USER   # if not already in that group; log out/in after
```

## Using it as a library

```c
#include "gcan_native.h"

void *ctx;
gcan_native_init(&ctx);

char err[256];
gcan_native_dev *dev = gcan_native_open(ctx, err, sizeof(err));
gcan_native_channel *ch = gcan_native_channel_get(dev, 0);
gcan_native_channel_set_bitrate(ch, 500000, 0 /* sample_point: no effect yet, see above */);
gcan_native_channel_start(ch, 0 /* mode flags -- must be 0 right now */);

gcan_native_frame f = { .can_id = 0x123, .len = 4, .data = {0xDE,0xAD,0xBE,0xEF} };
gcan_native_channel_send(ch, &f, 1000);

gcan_native_frame rx;
if (gcan_native_channel_recv(ch, &rx, 1000) > 0) { /* ... */ }

gcan_native_channel_stop(ch);
gcan_native_close(dev);
gcan_native_exit(ctx);
```

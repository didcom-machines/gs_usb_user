# Python SDK

This directory hosts four packages:

- **[`cancore/`](cancore/)** -- the hardware-agnostic types shared by both
  backends: `Frame` and the `Detector` family. Pure Python, no ctypes, no
  notion that gsusb/gcan exist. There's exactly one `Frame` class here,
  not one per backend -- `gsusb.Frame` and `gcan.Frame` both just
  re-export it, so they're literally the same class
  (`gsusb.Frame is gcan.Frame`), not two lookalikes.
- **[`gsusb/`](gsusb/)** -- bindings for [`driver/`](../../driver)'s
  userspace port of the Linux `gs_usb` driver (`libgsusb.so`), for
  gs_usb-protocol adapters (candleLight, CANable, the original GS
  USB2CAN, CES CANext FD, and others -- see `driver/README.md`). The
  fuller-featured backend: CAN-FD, multi-channel, a real bit-timing
  calculator, listen-only/loopback/one-shot/termination/identify/get-state.
- **[`gcan/`](gcan/)** -- bindings for
  [`driver/gcan_native/`](../../driver/gcan_native)'s from-scratch driver
  for the GCAN/ECAN/Rexon USBCANI-V503 adapter (`libgcan_native.so`).
  Builds and runs natively on ARM, unlike that adapter's vendor SDK -- but
  is currently one channel/500 kbit/s/classic-CAN only, and unverified
  against real hardware. Its `Bus` API is deliberately shaped to match
  `gsusb`'s wherever this hardware's confirmed capabilities allow (see
  "API shape" below).
- **[`candetect/`](candetect/)** -- a small dispatcher that probes for
  whichever of the two adapters above is attached and returns the
  matching backend's `Bus`, already opened, so calling code doesn't have
  to import `gsusb` or `gcan` directly or know in advance what's plugged
  in.

## Which one do I want?

- **Not sure, or writing something that should just work either way:**
  `candetect.open_bus()`.
- **You know you have a gs_usb-protocol adapter, or need CAN-FD/multi-
  channel/termination/etc:** `gsusb` directly.
- **You know you have the GCAN adapter and only need channel 0 @ 500
  kbit/s classic CAN:** `gcan` directly.

## API shape: gsusb and gcan are aligned by design

`Frame` and `Detector` aren't just shape-compatible between backends --
they're the *same classes* (`cancore/`), since neither has any real
dependency on which C library is behind the `Bus` using them. `Bus`
itself can't be unified that way (each backend's `Bus` talks to a
different C ABI), so instead `gcan.Bus` is deliberately shaped to match
`gsusb.Bus` wherever this hardware's confirmed capabilities allow -- same
method names, same signatures, same `*Error.code` values. Porting code
between the two is close to a search-and-replace, and it's what makes
`candetect` possible without a translation layer.

Where `gsusb` has a capability this protocol hasn't confirmed yet
(`get_state()`, `set_identify()`, `get_termination()`/`set_termination()`,
`configure_raw()`, a second channel, listen-only/loopback/one-shot modes),
the same method still exists on `gcan.Bus` -- it just always raises
`GcanError` instead of not existing, so code written against one SDK gets
a clear runtime error naming exactly what's unsupported rather than an
`AttributeError`. `Frame.canfd()`/`Frame.remote()` similarly always exist
(there's only one `Frame` class), but `gcan.Bus.send()` raises for either
-- `ValueError` for an FD frame (this hardware confirmed can't carry one;
gcan's wire struct has no field for it at all), `GcanError` for RTR (the
C library rejects it; not confirmed whether the wire protocol even has an
RTR bit). See `gcan/bus.py`'s and `gcan/_ffi.py`'s module docs, and
`driver/gcan_native/README.md`, for exactly what's real vs. a stub.

**This alignment does not make every feature portable.** `candetect`
picks the right driver for the hardware in front of you; it doesn't make
gs_usb's CAN-FD or bit-timing calculator work on the GCAN adapter, or vice
versa. Code that relies on a capability only one backend has should still
import that backend directly rather than going through `candetect`.

## Quick start

```python
from candetect import open_bus

with open_bus() as bus:                 # tries gsusb, then gcan
    bus.configure(bitrate=500000)
    bus.start()
    bus.send(bus.Frame.standard(0x321, b"\xDE\xAD\xBE\xEF"))

    import time
    time.sleep(60)                      # your app's main loop goes here
```

Or import a specific backend directly:

```python
from gsusb import Bus, Frame, IdDetector

with Bus() as bus:                      # tries every known gs_usb adapter id
    bus.configure(bitrate=500000)
    bus.add_detector(IdDetector(0x100, callback=lambda f, b: b.send(Frame.standard(0x200, b"\x01"))))
    bus.start()
```

```python
from gcan import Bus, Frame

with Bus() as bus:                      # opens the (one) supported GCAN adapter
    bus.configure(bitrate=500000)       # the only confirmed-working rate
    bus.start()
```

All four examples (`dump.py`, `react.py`, `rpm_monitor.py`, `capture.py`)
are built on `candetect` and work with whichever adapter is attached, no
flag needed:
```sh
python3 examples/dump.py 500000
python3 examples/react.py 500000
python3 examples/capture.py 500000 --prefer gcan   # force-try gcan first
```

## Setup: shared libraries

`cancore` needs no shared library at all (it's pure Python -- `Frame`/
`Detector` have no ctypes dependency). Each *backend* needs its own C
library built and reachable:

- **gsusb:** `cd driver && make` produces `bin/libgsusb.so`. Cross-compile
  for a different architecture per `driver/README.md`. Found automatically
  if bundled inside `gsusb/` (next to `__init__.py`), otherwise set
  `GSUSB_LIBRARY_PATH` (see `gsusb/_ffi.py:load_library()`).
- **gcan:** `cd driver/gcan_native && make` produces
  `bin/libgcan_native.so` -- needs only `libusb-1.0-0-dev`, no vendor
  blob. For the RUTX11 (armv7, musl), `deploy/rutx11.sh` cross-builds both
  drivers and stages both Python packages for you. Found automatically if
  bundled inside `gcan/`, or if running against a source checkout of this
  repo (it also checks `driver/gcan_native/bin/`), otherwise set
  `GCAN_NATIVE_LIBRARY_PATH` (see `gcan/_ffi.py:load_library()`).

`candetect.open_bus()` only needs *one* of the two to actually be present
and find a device -- the other backend's `OSError` (library not found) or
`GsusbError`/`GcanError` (no matching device) is caught and reported as
part of `NoSupportedAdapterError` if both fail.

Installing this SDK (`pip install .` from this directory, or
`pip install .[python-can]` for the `python-can` adapters) pulls in all
four packages -- `cancore`, `gsusb`, `gcan`, and `candetect` -- since none
of them have Python dependencies of their own (`gsusb`/`gcan` each depend
on `cancore` being importable alongside them, which a normal install
already guarantees).

## API overview

- **`Bus`** (`gsusb.Bus` / `gcan.Bus`) -- one CAN channel on an adapter.
  `open()`/`close()` (or use as a context manager), `configure(bitrate=...)`,
  `start()`/`stop()`, `send(frame)`, `recv(timeout_ms=...)` for
  polling-style code, plus `device_config`/`channel_count`/`features` for
  introspection and `get_state()`/`set_identify()`/`get_termination()`/
  `set_termination()`. `gsusb.Bus` additionally takes `vid=`/`pid=`/
  `usb_bus=`/`usb_addr=` to target a specific adapter (there's no
  equivalent on `gcan.Bus` -- only one adapter model is supported, opened
  by a fixed USB id) and `configure(fd=..., data_bitrate=...)` for CAN-FD.

- **`Frame`** (`cancore.Frame`, re-exported as `gsusb.Frame`/`gcan.Frame`
  -- one class, not two) -- `can_id` (flags baked in, same bit layout as
  Linux `<linux/can.h>`), `data`, `flags` (FD/BRS/ESI bits), `timestamp_us`,
  plus `.id` (bare arbitration id), `.is_extended`/`.is_rtr`/`.is_error`/
  `.is_fd`/`.is_brs`/`.is_esi`, and constructors `Frame.standard()`,
  `Frame.extended()`, `Frame.remote()`, `Frame.canfd()`. Whether a given
  `Bus.send()` actually accepts an FD or RTR frame depends on the backend
  it's attached to -- see "API shape" above. `timestamp_us` is a real
  hardware timestamp on `gsusb`; always 0 on `gcan` (its meaning/unit
  isn't decoded yet).

- **`Detector`** and friends (`cancore`, same story -- one class each,
  re-exported by both backends) -- the object pattern for event
  detection. `Bus`'s listener thread calls
  `detector.matches(frame)` for every registered detector on every
  received frame, and `on_detect(frame, bus)` when it matches (call
  `bus.send(...)` here to react). Built-ins: `IdDetector(can_id,
  callback=...)`, `MaskDetector(pattern, mask, callback=...)`,
  `PredicateDetector(fn, callback=...)`. Subclass `Detector` directly when
  a detection needs its own state across events -- see `examples/react.py`.

- **`open_bus(prefer=None, **kwargs)`** (`candetect`) -- tries `gsusb`
  then `gcan` (or the other order if `prefer="gcan"`), returns the first
  one that opens successfully. Raises `candetect.NoSupportedAdapterError`
  if neither does.

## python-can interoperability

Both `gsusb/python_can.py` and `gcan/python_can.py` are optional
`can.BusABC` adapters, registered as `interface="gsusb"` / `"gcan"` via
`can.interface` entry points (neither is imported by its package's
`__init__.py`, so the core SDKs have no `python-can` dependency). Requires
`python-can` installed (`pip install .[python-can]`, or plain
`pip install python-can` alongside this package):

```python
import can
with can.Bus(interface="gsusb", bitrate=500000) as bus:
    bus.send(can.Message(arbitration_id=0x123, data=b"\xDE\xAD\xBE\xEF"))
```

`candetect` has no `python-can` adapter of its own -- pick whichever
backend's you need, or call `candetect.open_bus()` yourself and hand the
result to your own code instead of `can.Bus(...)`.

## Why this can listen and write at the same time

`Bus.start()` spawns one Python thread that drains the C library's
receive queue and dispatches to detectors; `Bus.send()` is an independent
call that never blocks on, or is blocked by, that thread. One `Bus` in one
process can watch traffic and transmit reactions concurrently with no
extra locking on your part.

## Errors

Every SDK call that can fail raises `gsusb.GsusbError` or `gcan.GcanError`
(`.code` is the raw driver error code -- the two share the same numeric
meanings -- `str(err)` explains what failed). `candetect.open_bus()`
wraps both into `NoSupportedAdapterError` only when *every* backend fails;
a successful open returns a plain backend `Bus`, and errors from using it
afterwards are that backend's own exception type. A `Bus` instance is
single-owner over a claimed USB interface: only one `Bus` (in one
process) can hold a given adapter open at a time.

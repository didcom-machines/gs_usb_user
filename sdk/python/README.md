# gsusb Python SDK

Python bindings for the [`driver/`](../../driver) userspace gs_usb CAN
library, via `ctypes` over `libgsusb.so` -- no C extension to build, no
compiler needed on the target device. Built for programming applications on
devices like the RUTX11 where you can't load a kernel module but do have
Python.

## What you need on the target

1. **`libgsusb.so`**, built from `driver/` for the target's architecture:
   ```sh
   cd driver
   make          # produces bin/libgsusb.so alongside the CLI tools
   ```
   For a different architecture than your build machine, cross-build it
   (see `driver/README.md`) -- e.g. for the RUTX11 (armv7, musl) this repo
   was built and verified using an Alpine Linux container under QEMU
   emulation (`docker run --platform linux/arm/v7 alpine:latest ...`).

2. **This `gsusb/` package** (pure Python, no build step) -- either:
   - `pip install .` from this directory (needs pip on the target), or
   - just copy the `gsusb/` directory onto the device and make sure it's
     importable (same directory as your script, or on `PYTHONPATH`) --
     this is the simpler option for a minimal embedded Python install.

3. Point the SDK at `libgsusb.so`. It's found automatically if you drop a
   copy of it *inside* the `gsusb/` package directory (next to
   `__init__.py`); otherwise set the environment variable:
   ```sh
   export GSUSB_LIBRARY_PATH=/usr/local/lib/libgsusb.so
   ```
   (See `gsusb/_ffi.py:load_library()` for the full search order.)

## Quick start

```python
from gsusb import Bus, Frame, IdDetector

def on_key_frame(frame, bus):
    print("reacting to", frame)
    bus.send(Frame.standard(0x200, bytes([0x01])))

with Bus() as bus:                      # tries every known adapter id;
                                         # pass vid=/pid=/usb_bus=/usb_addr=
                                         # to target one specifically
    bus.configure(bitrate=500000)
    bus.add_detector(IdDetector(0x100, callback=on_key_frame))
    bus.start()                         # spawns a background listener thread

    bus.send(Frame.standard(0x321, b"\xDE\xAD\xBE\xEF"))  # write any time

    import time
    time.sleep(60)                      # your app's main loop goes here
```

Run the included examples directly on the target:
```sh
python3 examples/dump.py 500000
python3 examples/react.py 500000
```

## API overview

- **`Bus`** -- one CAN channel on one adapter. `open()`/`close()` (or use
  as a context manager), `configure(bitrate=..., fd=..., data_bitrate=...)`,
  `start(loopback=..., listen_only=..., ...)`/`stop()`, `send(frame)`,
  `recv(timeout_ms=...)` for polling-style code, plus `get_state()`,
  `set_identify()`, `get_termination()`/`set_termination()`,
  `device_config`/`channel_count`/`features`/`bt_const` for introspection.

- **`Frame`** -- `can_id` (flags baked in, same bit layout as Linux
  `<linux/can.h>`), `data`, `flags`, `timestamp_us`, plus `.id` (bare
  arbitration id), `.is_extended`/`.is_rtr`/`.is_error`/`.is_fd`/`.is_brs`,
  and constructors `Frame.standard()`, `Frame.extended()`, `Frame.remote()`,
  `Frame.canfd()`.

- **`Detector`** and friends -- the object pattern for event detection.
  `Bus`'s listener thread calls `detector.matches(frame)` for every
  registered detector on every received frame, and `on_detect(frame, bus)`
  when it matches (call `bus.send(...)` here to react). Built-ins:
  `IdDetector(can_id, callback=...)`, `MaskDetector(pattern, mask,
  callback=...)`, `PredicateDetector(fn, callback=...)`. Subclass
  `Detector` directly when a detection needs its own state across events
  (counters, debouncing, sequences) -- see `examples/react.py`.

## Why this can listen and write at the same time

`Bus.start()` spawns one Python thread that drains the C library's receive
queue and dispatches to detectors; `Bus.send()` is an independent call that
never blocks on, or is blocked by, that thread. One `Bus` in one process
can watch traffic and transmit reactions concurrently with no extra
locking on your part -- see `driver/README.md`'s note on why a single
libusb-owned USB interface can't be shared across *processes*, which is a
different constraint from this.

## Errors

Every SDK call that can fail raises `gsusb.GsusbError` (`.code` is the raw
driver error code, `str(err)` explains what failed). A `Bus` instance is
single-owner over a claimed USB interface: only one `Bus` (in one process)
can hold the same adapter open at a time.

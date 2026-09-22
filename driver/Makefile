# Userspace port of the Linux gs_usb CAN driver (libusb-1.0 based).
#
# Requires libusb-1.0 development headers, e.g.:
#   Debian/Ubuntu: sudo apt install build-essential libusb-1.0-0-dev pkg-config
#   Fedora:        sudo dnf install gcc make libusb1-devel pkgconf-pkg-config
#
# Cross-compiling for arm64 from another arch: install an aarch64 toolchain
# and the matching libusb-1.0-dev for that arch (or a full sysroot), then:
#   make CC=aarch64-linux-gnu-gcc

CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra
CSTD := -std=c11 -D_POSIX_C_SOURCE=200809L

PKGCONFIG := $(shell command -v pkg-config 2>/dev/null)
ifneq ($(PKGCONFIG),)
LIBUSB_CFLAGS := $(shell pkg-config --cflags libusb-1.0)
LIBUSB_LIBS   := $(shell pkg-config --libs libusb-1.0)
else
LIBUSB_CFLAGS := -I/usr/include/libusb-1.0
LIBUSB_LIBS   := -lusb-1.0
endif

INCLUDES := -Iinclude $(LIBUSB_CFLAGS)
LDLIBS   := $(LIBUSB_LIBS) -lpthread -lm

BINDIR := bin
OBJDIR := build

LIB_SRCS := src/gsusb.c src/bittiming.c
LIB_OBJS := $(LIB_SRCS:src/%.c=$(OBJDIR)/%.o)
LIB_PIC_OBJS := $(LIB_SRCS:src/%.c=$(OBJDIR)/%.pic.o)
LIB_HDRS := include/gsusb.h src/gsusb_proto.h src/bittiming.h

# slcan (drivers/net/can/slcan/slcan-core.c ASCII protocol over a tty, e.g.
# CANable boards running SLCAN firmware) needs only plain POSIX termios --
# no libusb, so its objects/library are built without $(LIBUSB_CFLAGS)/LIBS.
SLCAN_SRCS := src/slcan.c
SLCAN_OBJS := $(SLCAN_SRCS:src/%.c=$(OBJDIR)/%.o)
SLCAN_PIC_OBJS := $(SLCAN_SRCS:src/%.c=$(OBJDIR)/%.pic.o)
SLCAN_HDRS := include/slcan.h

# candev (include/candev.h) is the transparent auto-detecting layer on top
# of both backends: it links gsusb's + slcan's objects together, so it
# needs libusb like gsusb does.
CANDEV_SRCS := src/candev.c
CANDEV_OBJS := $(CANDEV_SRCS:src/%.c=$(OBJDIR)/%.o)
CANDEV_PIC_OBJS := $(CANDEV_SRCS:src/%.c=$(OBJDIR)/%.pic.o)
CANDEV_HDRS := include/candev.h

TOOLS := gsusb_info gsusb_dump gsusb_send gsusb_react
SLCAN_TOOLS := slcan_dump slcan_send
CANDEV_TOOLS := candev_dump candev_send
TOOL_BINS := $(addprefix $(BINDIR)/,$(TOOLS) $(SLCAN_TOOLS) $(CANDEV_TOOLS))

# libgsusb.so/libslcan.so/libcandev.so are the shared libraries the Python
# SDK (sdk/python) dlopens via ctypes. Each exposes the API of its matching
# public header with default ELF visibility -- no extra glue needed.
SONAME := libgsusb.so
SLCAN_SONAME := libslcan.so
CANDEV_SONAME := libcandev.so

.PHONY: all clean install lib
all: $(TOOL_BINS) lib
lib: $(BINDIR)/$(SONAME) $(BINDIR)/$(SLCAN_SONAME) $(BINDIR)/$(CANDEV_SONAME)

$(OBJDIR)/%.o: src/%.c $(LIB_HDRS) $(SLCAN_HDRS) $(CANDEV_HDRS) | $(OBJDIR)
	$(CC) $(CSTD) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJDIR)/%.pic.o: src/%.c $(LIB_HDRS) $(SLCAN_HDRS) $(CANDEV_HDRS) | $(OBJDIR)
	$(CC) $(CSTD) $(CFLAGS) -fPIC $(INCLUDES) -c $< -o $@

$(BINDIR)/$(SONAME): $(LIB_PIC_OBJS) | $(BINDIR)
	$(CC) -shared -Wl,-soname,$(SONAME) $(LIB_PIC_OBJS) -o $@ $(LIBUSB_LIBS) -lpthread -lm

$(BINDIR)/$(SLCAN_SONAME): $(SLCAN_PIC_OBJS) | $(BINDIR)
	$(CC) -shared -Wl,-soname,$(SLCAN_SONAME) $(SLCAN_PIC_OBJS) -o $@ -lpthread

$(BINDIR)/$(CANDEV_SONAME): $(CANDEV_PIC_OBJS) $(LIB_PIC_OBJS) $(SLCAN_PIC_OBJS) | $(BINDIR)
	$(CC) -shared -Wl,-soname,$(CANDEV_SONAME) $(CANDEV_PIC_OBJS) $(LIB_PIC_OBJS) $(SLCAN_PIC_OBJS) -o $@ $(LIBUSB_LIBS) -lpthread -lm

$(BINDIR)/gsusb_info: tools/gsusb_info.c tools/common.h $(LIB_OBJS) | $(BINDIR)
	$(CC) $(CSTD) $(CFLAGS) $(INCLUDES) $< $(LIB_OBJS) -o $@ $(LDLIBS)

$(BINDIR)/gsusb_dump: tools/gsusb_dump.c tools/common.h $(LIB_OBJS) | $(BINDIR)
	$(CC) $(CSTD) $(CFLAGS) $(INCLUDES) $< $(LIB_OBJS) -o $@ $(LDLIBS)

$(BINDIR)/gsusb_send: tools/gsusb_send.c tools/common.h $(LIB_OBJS) | $(BINDIR)
	$(CC) $(CSTD) $(CFLAGS) $(INCLUDES) $< $(LIB_OBJS) -o $@ $(LDLIBS)

$(BINDIR)/gsusb_react: tools/gsusb_react.c tools/common.h $(LIB_OBJS) | $(BINDIR)
	$(CC) $(CSTD) $(CFLAGS) $(INCLUDES) $< $(LIB_OBJS) -o $@ $(LDLIBS)

$(BINDIR)/slcan_dump: tools/slcan_dump.c $(SLCAN_OBJS) | $(BINDIR)
	$(CC) $(CSTD) $(CFLAGS) -Iinclude $< $(SLCAN_OBJS) -o $@ -lpthread

$(BINDIR)/slcan_send: tools/slcan_send.c $(SLCAN_OBJS) | $(BINDIR)
	$(CC) $(CSTD) $(CFLAGS) -Iinclude $< $(SLCAN_OBJS) -o $@ -lpthread

$(BINDIR)/candev_dump: tools/candev_dump.c $(CANDEV_OBJS) $(LIB_OBJS) $(SLCAN_OBJS) | $(BINDIR)
	$(CC) $(CSTD) $(CFLAGS) $(INCLUDES) $< $(CANDEV_OBJS) $(LIB_OBJS) $(SLCAN_OBJS) -o $@ $(LDLIBS)

$(BINDIR)/candev_send: tools/candev_send.c $(CANDEV_OBJS) $(LIB_OBJS) $(SLCAN_OBJS) | $(BINDIR)
	$(CC) $(CSTD) $(CFLAGS) $(INCLUDES) $< $(CANDEV_OBJS) $(LIB_OBJS) $(SLCAN_OBJS) -o $@ $(LDLIBS)

$(BINDIR) $(OBJDIR):
	mkdir -p $@

clean:
	rm -rf $(BINDIR) $(OBJDIR)

# DESTDIR defaults to nothing (install to the real root); pass
# DESTDIR=/some/staging/dir for packaging.
install: all
	install -Dm755 $(BINDIR)/gsusb_info $(DESTDIR)/usr/local/bin/gsusb_info
	install -Dm755 $(BINDIR)/gsusb_dump $(DESTDIR)/usr/local/bin/gsusb_dump
	install -Dm755 $(BINDIR)/gsusb_send $(DESTDIR)/usr/local/bin/gsusb_send
	install -Dm755 $(BINDIR)/gsusb_react $(DESTDIR)/usr/local/bin/gsusb_react
	install -Dm755 $(BINDIR)/slcan_dump $(DESTDIR)/usr/local/bin/slcan_dump
	install -Dm755 $(BINDIR)/slcan_send $(DESTDIR)/usr/local/bin/slcan_send
	install -Dm755 $(BINDIR)/candev_dump $(DESTDIR)/usr/local/bin/candev_dump
	install -Dm755 $(BINDIR)/candev_send $(DESTDIR)/usr/local/bin/candev_send
	install -Dm755 $(BINDIR)/$(SONAME) $(DESTDIR)/usr/local/lib/$(SONAME)
	install -Dm755 $(BINDIR)/$(SLCAN_SONAME) $(DESTDIR)/usr/local/lib/$(SLCAN_SONAME)
	install -Dm755 $(BINDIR)/$(CANDEV_SONAME) $(DESTDIR)/usr/local/lib/$(CANDEV_SONAME)
	install -Dm644 udev/99-gsusb.rules $(DESTDIR)/etc/udev/rules.d/99-gsusb.rules
	@echo "Installed. Run 'udevadm control --reload && udevadm trigger' (or replug the adapter) to apply the udev rule."

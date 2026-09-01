# From-scratch, libusb-1.0-only userspace driver for the GCAN/ECAN/Rexon
# USBCANI-V503 (see README.md) -- unlike driver/gcan/'s vendor
# libECanVci.so.1, this builds natively for any arch libusb-1.0 supports
# (armv7 included), no vendor blob.
#
# Requires libusb-1.0 development headers, e.g.:
#   Debian/Ubuntu: sudo apt install build-essential libusb-1.0-0-dev pkg-config
#   Fedora:        sudo dnf install gcc make libusb1-devel pkgconf-pkg-config
#
# Cross-compiling for armv7 from another arch: install an arm-linux-
# gnueabihf toolchain and the matching libusb-1.0-dev (or a sysroot), then:
#   make CC=arm-linux-gnueabihf-gcc

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
LDLIBS   := $(LIBUSB_LIBS)

BINDIR := bin
OBJDIR := build

LIB_SRCS := src/gcan_native.c
LIB_OBJS := $(LIB_SRCS:src/%.c=$(OBJDIR)/%.o)
LIB_PIC_OBJS := $(LIB_SRCS:src/%.c=$(OBJDIR)/%.pic.o)
LIB_HDRS := include/gcan_native.h src/gcan_native_proto.h

# libgcan_native.so is the shared library the Python SDK (sdk/python)
# dlopens via ctypes -- every gcan_native_* function has default ELF
# visibility, no extra glue needed.
SONAME := libgcan_native.so

.PHONY: all clean install lib
all: $(BINDIR)/gcan_native_tool lib
lib: $(BINDIR)/$(SONAME)

$(OBJDIR)/%.o: src/%.c $(LIB_HDRS) | $(OBJDIR)
	$(CC) $(CSTD) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJDIR)/%.pic.o: src/%.c $(LIB_HDRS) | $(OBJDIR)
	$(CC) $(CSTD) $(CFLAGS) -fPIC $(INCLUDES) -c $< -o $@

$(BINDIR)/$(SONAME): $(LIB_PIC_OBJS) | $(BINDIR)
	$(CC) -shared -Wl,-soname,$(SONAME) $(LIB_PIC_OBJS) -o $@ $(LIBUSB_LIBS)

$(BINDIR)/gcan_native_tool: tools/gcan_native_tool.c $(LIB_OBJS) | $(BINDIR)
	$(CC) $(CSTD) $(CFLAGS) $(INCLUDES) $< $(LIB_OBJS) -o $@ $(LDLIBS)

$(BINDIR) $(OBJDIR):
	mkdir -p $@

clean:
	rm -rf $(BINDIR) $(OBJDIR)

# DESTDIR defaults to nothing (install to the real root); pass
# DESTDIR=/some/staging/dir for packaging.
install: all
	install -Dm755 $(BINDIR)/gcan_native_tool $(DESTDIR)/usr/local/bin/gcan_native_tool
	install -Dm755 $(BINDIR)/$(SONAME) $(DESTDIR)/usr/local/lib/$(SONAME)
	install -Dm644 udev/99-gcan-native.rules $(DESTDIR)/etc/udev/rules.d/99-gcan-native.rules
	@echo "Installed. Run 'udevadm control --reload && udevadm trigger' (or replug the adapter) to apply the udev rule."

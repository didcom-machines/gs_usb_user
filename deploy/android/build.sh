#!/usr/bin/env bash
# Cross-builds the gs_usb driver CLI tools for aarch64 Android/Termux using
# the Android NDK, statically linking libusb-1.0 (cross-built from source)
# so nothing extra needs installing in Termux at runtime beyond the binary
# itself.
#
# NOT verified against a real Android device -- there was none available to
# test on when this was built. Compiles cleanly and produces correctly
# formed ELF64 AArch64 PIE binaries linked only against bionic's libc/libm/
# libdl, but the actual runtime behavior on-device (especially the
# termux-usb fd-handoff path -- see ../../driver/include/gsusb.h's
# gsusb_open_fd()) has not been confirmed on hardware.
#
# Usage: deploy/android/build.sh
# Requires locally: docker. Downloads the Android NDK (~660MB, cached in
# this directory after the first run) and builds libusb from source
# (also cached) -- only the gsusb driver itself rebuilds on repeat runs.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ANDROID_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DIST_DIR="$REPO_ROOT/dist-android"
CACHE_DIR="$ANDROID_DIR/cache"
NDK_VERSION="r27c"
API_LEVEL="24"

command -v docker >/dev/null 2>&1 || {
	echo "error: docker is required (used to run the Android NDK cross-build)." >&2
	exit 1
}

mkdir -p "$CACHE_DIR" "$DIST_DIR"

if [ ! -f "$CACHE_DIR/ndk.zip" ]; then
	echo "==> Downloading Android NDK $NDK_VERSION (~660MB, cached after this)"
	curl -sL -o "$CACHE_DIR/ndk.zip" \
		"https://dl.google.com/android/repository/android-ndk-${NDK_VERSION}-linux.zip"
fi

BUILD_SCRIPT="$(mktemp)"
cat >"$BUILD_SCRIPT" <<'INNER'
set -e
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null
apt-get install -y -qq unzip git autoconf automake libtool pkg-config build-essential >/dev/null

cd /tmp && unzip -q /cache/ndk.zip
NDK="/tmp/android-ndk-${NDK_VERSION}"
TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64"
CC="$TOOLCHAIN/bin/aarch64-linux-android${API_LEVEL}-clang"
AR=$TOOLCHAIN/bin/llvm-ar
RANLIB=$TOOLCHAIN/bin/llvm-ranlib
STRIP=$TOOLCHAIN/bin/llvm-strip
READELF=$TOOLCHAIN/bin/llvm-readelf

# libusb: cross-built from source and statically linked, so Termux doesn't
# need `pkg install libusb` at runtime and there's no risk of an ABI
# mismatch against whatever version Termux's own package happens to ship.
if [ -f /cache/libusb-install/lib/libusb-1.0.a ]; then
	echo "==> Using cached libusb-install"
	cp -r /cache/libusb-install /tmp/libusb-install
else
	echo "==> Cross-building libusb-1.0.27 for aarch64-linux-android"
	git clone --quiet --depth 1 --branch v1.0.27 https://github.com/libusb/libusb.git /tmp/libusb-src
	cd /tmp/libusb-src
	# autogen.sh's own internal bare `./configure` (no args) always fails
	# here -- it can't cross-compile with zero flags -- but the autoreconf
	# step it runs first (regenerating configure/Makefile.in) succeeds
	# fine, which is all that's actually needed before our own properly
	# flagged ./configure call below. Safe to ignore its exit code.
	./autogen.sh >/tmp/autogen.log 2>&1 || true
	CC=$CC AR=$AR RANLIB=$RANLIB STRIP=$STRIP \
		./configure --host=aarch64-linux-android --prefix=/tmp/libusb-install \
		--enable-static --disable-shared --disable-udev --disable-dependency-tracking \
		>/tmp/configure.log 2>&1
	make -j"$(nproc)" >/tmp/make.log 2>&1
	make install >/dev/null 2>&1
	mkdir -p /cache
	cp -r /tmp/libusb-install /cache/libusb-install
fi

echo "==> Cross-compiling gsusb driver + CLI tools"
cp -r /src /tmp/gsusb-build
cd /tmp/gsusb-build/driver
mkdir -p bin build
# -fPIE/-pie: required for executables on modern Android.
# No -lpthread: bionic libc provides pthread_* directly, unlike glibc/musl
# which need a separate (real or stub) library.
CFLAGS_COMMON="-std=c11 -D_POSIX_C_SOURCE=200809L -O2 -Wall -Wextra -fPIE -Iinclude -I/tmp/libusb-install/include/libusb-1.0"
LDFLAGS_COMMON="-pie -L/tmp/libusb-install/lib -lusb-1.0 -lm"

$CC $CFLAGS_COMMON -c src/gsusb.c -o build/gsusb.o
$CC $CFLAGS_COMMON -c src/bittiming.c -o build/bittiming.o

for tool in gsusb_info gsusb_dump gsusb_send gsusb_react; do
	$CC $CFLAGS_COMMON tools/$tool.c build/gsusb.o build/bittiming.o -o bin/$tool $LDFLAGS_COMMON
done

echo "==> Verifying ELF headers"
for tool in gsusb_info gsusb_dump gsusb_send gsusb_react; do
	echo "-- $tool --"
	$READELF -h bin/$tool | grep -E 'Class|Machine|Type'
	$READELF -d bin/$tool | grep NEEDED
done

mkdir -p /src/dist-android
cp bin/gsusb_info bin/gsusb_dump bin/gsusb_send bin/gsusb_react /src/dist-android/
chown "$HOST_UID:$HOST_GID" /src/dist-android/*
echo "==> Done: dist-android/{gsusb_info,gsusb_dump,gsusb_send,gsusb_react}"
INNER

docker run --rm \
	-v "$REPO_ROOT":/src \
	-v "$CACHE_DIR":/cache \
	-e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
	-e API_LEVEL="$API_LEVEL" -e NDK_VERSION="$NDK_VERSION" \
	-v "$BUILD_SCRIPT":/build.sh \
	ubuntu:24.04 bash /build.sh

rm -f "$BUILD_SCRIPT"

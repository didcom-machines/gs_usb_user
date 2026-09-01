#!/usr/bin/env bash
# Cross-builds both C drivers (the gs_usb port and the from-scratch
# gcan_native driver) and the Python SDK for the RUTX11 (armv7 Cortex-A7,
# musl libc, RutOS/OpenWrt) and deploys all of it over SSH. Safe to re-run.
#
# Usage: deploy/rutx11.sh [--host HOST] [--user USER] [--remote-dir DIR] [--skip-build]
#
# Requires locally: docker (cross-builds via QEMU emulation of Alpine Linux,
# which ships a musl+libusb-dev toolchain matching the router's environment
# closely), ssh, scp, tar.
#
# Requires on the RUTX11: nothing beyond what stock RutOS already has --
# root SSH access and Python 3 with ctypes (both present by default). No
# compiler, pip, or kernel module needed on the router itself.
set -euo pipefail

HOST="${GSUSB_RUTX11_HOST:-RUTX11}"
SSH_USER="${GSUSB_RUTX11_USER:-root}"
REMOTE_DIR="${GSUSB_RUTX11_DIR:-/usr/local/gsusb}"
SKIP_BUILD=0

usage() {
	cat <<EOF
usage: $0 [--host HOST] [--user USER] [--remote-dir DIR] [--skip-build]

  --host HOST        RUTX11 hostname/IP (default: RUTX11, or \$GSUSB_RUTX11_HOST)
  --user USER        SSH user (default: root, or \$GSUSB_RUTX11_USER)
  --remote-dir DIR   deployment directory on the router (default: /usr/local/gsusb)
  --skip-build       reuse the previous build in ./dist-rutx11 instead of rebuilding
  -h, --help         show this help

You'll be prompted for the SSH password once -- the connection is
multiplexed for the rest of the script's ssh/scp calls. Passwordless key
auth works too, if you've already run e.g. ssh-copy-id root@RUTX11.
EOF
}

while [ $# -gt 0 ]; do
	case "$1" in
	--host) HOST="$2"; shift 2 ;;
	--user) SSH_USER="$2"; shift 2 ;;
	--remote-dir) REMOTE_DIR="$2"; shift 2 ;;
	--skip-build) SKIP_BUILD=1; shift ;;
	-h | --help) usage; exit 0 ;;
	*) echo "unknown argument: $1" >&2; usage; exit 1 ;;
	esac
done

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIST_DIR="$REPO_ROOT/dist-rutx11"

log() { printf '\n\033[1;34m==>\033[0m %s\n' "$1"; }

# --- 1. cross-build for armv7/musl via Docker + QEMU (Alpine Linux) ---

if [ "$SKIP_BUILD" -eq 0 ]; then
	command -v docker >/dev/null 2>&1 || {
		echo "error: docker is required to cross-build for the RUTX11 (armv7/musl)." >&2
		echo "       (if Docker Desktop's WSL integration has dropped, restart Docker Desktop and re-run)" >&2
		exit 1
	}

	log "Cross-building both drivers (gsusb + gcan_native) for armv7/musl"
	rm -rf "$DIST_DIR"
	mkdir -p "$DIST_DIR/driver" "$DIST_DIR/gcan_native"

	docker run --rm -v "$REPO_ROOT":/src --platform linux/arm/v7 \
		-e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
		alpine:latest sh -c '
			set -e
			apk add --no-cache build-base libusb-dev pkgconfig >/dev/null
			cp -r /src /tmp/build

			cd /tmp/build/driver
			make clean >/dev/null 2>&1 || true
			make
			mkdir -p /src/dist-rutx11/driver
			cp bin/gsusb_info bin/gsusb_dump bin/gsusb_send bin/gsusb_react bin/libgsusb.so /src/dist-rutx11/driver/

			cd /tmp/build/driver/gcan_native
			make clean >/dev/null 2>&1 || true
			make
			mkdir -p /src/dist-rutx11/gcan_native
			cp bin/gcan_native_tool bin/libgcan_native.so /src/dist-rutx11/gcan_native/

			chown -R "$HOST_UID:$HOST_GID" /src/dist-rutx11/driver /src/dist-rutx11/gcan_native
		'
else
	log "Skipping the (slow) C cross-build, reusing $DIST_DIR"
	[ -d "$DIST_DIR/driver" ] && [ -d "$DIST_DIR/gcan_native" ] || {
		echo "error: $DIST_DIR/{driver,gcan_native} not found; run once without --skip-build first" >&2
		exit 1
	}
fi

# Python SDK is pure Python -- always re-staged (cheap, no Docker/build
# needed), even with --skip-build, so iterating on Python-only changes
# doesn't require a full C cross-build to actually take effect.
log "Staging Python SDK (cancore + gsusb + gcan + candetect)"
mkdir -p "$DIST_DIR/python"
rm -rf "$DIST_DIR/python/cancore" "$DIST_DIR/python/gsusb" "$DIST_DIR/python/gcan" \
       "$DIST_DIR/python/candetect" "$DIST_DIR/python/examples"
cp -r "$REPO_ROOT/sdk/python/cancore" "$REPO_ROOT/sdk/python/gsusb" "$REPO_ROOT/sdk/python/gcan" \
      "$REPO_ROOT/sdk/python/candetect" "$REPO_ROOT/sdk/python/examples" "$DIST_DIR/python/"
rm -rf "$DIST_DIR/python/cancore/__pycache__" "$DIST_DIR/python/gsusb/__pycache__" "$DIST_DIR/python/gcan/__pycache__"
cp "$DIST_DIR/driver/libgsusb.so" "$DIST_DIR/python/gsusb/libgsusb.so"
cp "$DIST_DIR/gcan_native/libgcan_native.so" "$DIST_DIR/python/gcan/libgcan_native.so"

# --- 2. open a multiplexed SSH connection (prompts for the password once) ---

CTRL_DIR="$(mktemp -d)"
CTRL_PATH="$CTRL_DIR/ctrl-%r@%h:%p"
SSH_OPTS=(-o "ControlMaster=auto" -o "ControlPath=$CTRL_PATH" -o "ControlPersist=120" -o "StrictHostKeyChecking=accept-new")

cleanup() {
	ssh "${SSH_OPTS[@]}" -O exit "$SSH_USER@$HOST" >/dev/null 2>&1 || true
	rm -rf "$CTRL_DIR"
}
trap cleanup EXIT

log "Connecting to $SSH_USER@$HOST"
ssh "${SSH_OPTS[@]}" -fN "$SSH_USER@$HOST"

# --- 3. deploy ---

log "Checking whether a gsusb/gcan tool is already holding an adapter open"
BUSY="$(ssh "${SSH_OPTS[@]}" "$SSH_USER@$HOST" "ps w 2>/dev/null | grep -E 'gsusb_(dump|send|react)|gcan_native_tool' | grep -v grep || true")"
if [ -n "$BUSY" ]; then
	echo "warning: found a running tool that may be holding a USB interface open:" >&2
	echo "$BUSY" >&2
	echo "(only one process can claim a given adapter at a time -- stop it manually if verification below reports the device busy)" >&2
fi

log "Creating $REMOTE_DIR on the router"
ssh "${SSH_OPTS[@]}" "$SSH_USER@$HOST" "mkdir -p '$REMOTE_DIR/python'"

log "Copying gsusb CLI tools + libgsusb.so"
scp "${SSH_OPTS[@]}" "$DIST_DIR"/driver/* "$SSH_USER@$HOST:$REMOTE_DIR/"

log "Copying gcan_native_tool + libgcan_native.so"
scp "${SSH_OPTS[@]}" "$DIST_DIR"/gcan_native/* "$SSH_USER@$HOST:$REMOTE_DIR/"

log "Copying Python SDK (cancore + gsusb + gcan + candetect)"
tar czf "$CTRL_DIR/python-sdk.tgz" -C "$DIST_DIR/python" cancore gsusb gcan candetect examples
scp "${SSH_OPTS[@]}" "$CTRL_DIR/python-sdk.tgz" "$SSH_USER@$HOST:$REMOTE_DIR/python/"

read -r -d '' UNPACK_SCRIPT <<EOF || true
set -e
cd '$REMOTE_DIR/python'
tar xzf python-sdk.tgz
rm python-sdk.tgz
chmod +x '$REMOTE_DIR'/gsusb_info '$REMOTE_DIR'/gsusb_dump '$REMOTE_DIR'/gsusb_send '$REMOTE_DIR'/gsusb_react '$REMOTE_DIR'/gcan_native_tool
EOF
ssh "${SSH_OPTS[@]}" "$SSH_USER@$HOST" "$UNPACK_SCRIPT"

log "Installing udev rules (harmless as root; kept for consistency with other targets)"
scp "${SSH_OPTS[@]}" "$REPO_ROOT/driver/udev/99-gsusb.rules" \
	"$REPO_ROOT/driver/gcan_native/udev/99-gcan-native.rules" \
	"$SSH_USER@$HOST:$REMOTE_DIR/"

# --- 4. verify ---

log "Verifying deployment"
read -r -d '' VERIFY_SCRIPT <<EOF || true
echo "--- gsusb_info --scan (does not claim the device, safe even if something else holds it) ---"
'$REMOTE_DIR/gsusb_info' --scan || true
echo "--- gcan_native_tool (prints usage, does not touch the device) ---"
'$REMOTE_DIR/gcan_native_tool' || true
echo "--- python3 import + shared-library load check (both backends + candetect) ---"
cd '$REMOTE_DIR/python'
python3 -c "
import cancore, gsusb, gcan, candetect
print('gsusb SDK import OK, version', gsusb.__version__)
print('gcan SDK import OK, version', gcan.__version__)
print('gsusb.Frame is gcan.Frame (shared cancore.Frame):', gsusb.Frame is gcan.Frame)
try:
    print('libgsusb.so loaded OK:', gsusb._ffi.load_library())
except OSError as e:
    print('libgsusb.so NOT loaded:', e)
try:
    print('libgcan_native.so loaded OK:', gcan._ffi.load_library())
except OSError as e:
    print('libgcan_native.so NOT loaded:', e)
print('candetect import OK:', candetect.open_bus)
"
EOF
ssh "${SSH_OPTS[@]}" "$SSH_USER@$HOST" "$VERIFY_SCRIPT"

log "Done. Deployed to $SSH_USER@$HOST:$REMOTE_DIR"
cat <<EOF

  gsusb CLI tools: $REMOTE_DIR/{gsusb_info,gsusb_dump,gsusb_send,gsusb_react}
  gcan CLI tool:   $REMOTE_DIR/gcan_native_tool
  Python SDK:      $REMOTE_DIR/python/{cancore,gsusb,gcan,candetect}   (run scripts from $REMOTE_DIR/python, or add it to PYTHONPATH)
  Examples:        $REMOTE_DIR/python/examples/{dump,react,rpm_monitor,capture}.py

  Note: candetect.open_bus() picks whichever adapter is attached. The gcan
  backend (gcan_native) is one channel/500kbit/classic-CAN only and
  unverified against real hardware -- see driver/gcan_native/README.md
  before relying on it.

EOF

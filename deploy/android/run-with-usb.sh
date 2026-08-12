#!/data/data/com.termux/files/usr/bin/bash
# Bridges termux-usb's fd-appending convention to this driver's --usb-fd
# flag. termux-usb's `-e COMMAND` runs COMMAND with the granted file
# descriptor appended as its LAST argument -- so point termux-usb at this
# wrapper (not directly at gsusb_info/etc.), and set TOOL/TOOL_ARGS to say
# what to actually run.
#
# NOT verified against a real device -- there was none available to test
# this on. Double check termux-usb's exact argv-passing behavior on your
# device (`termux-usb --help`) before relying on this.
#
# Example:
#   pkg install termux-api          # once, if not already installed
#   termux-usb -l                   # find the device path
#   TOOL=./gsusb_info TOOL_ARGS=--scan \
#     termux-usb -r -e ./run-with-usb.sh /dev/bus/usb/001/002
set -eu

TOOL="${TOOL:-./gsusb_info}"
TOOL_ARGS="${TOOL_ARGS:-}"

# Last positional argument is the fd termux-usb granted us.
FD="${!#}"

# shellcheck disable=SC2086  # TOOL_ARGS is intentionally word-split
exec "$TOOL" $TOOL_ARGS --usb-fd "$FD"

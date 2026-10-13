#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Install the Pixel 6 Android-side pieces into Waydroid's vendor overlay
# (/var/lib/waydroid/overlay/vendor). Run as root on the phone:
#   sh install-overlay.sh [<dir>...]
# Each <dir> is the output of mesa/build.sh or gnss-hal/build-hal.sh (a
# vendor/ tree); their files are copied as they are. Always (also with no
# <dir>): patch this phone's own hwcomposer.waydroid.so from Waydroid's
# vendor image with hwcomposer/patch-hwcomposer.py (it refuses any other
# build). Restart Waydroid afterwards: systemctl restart waydroid-container
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
O=/var/lib/waydroid/overlay/vendor
IMG=/var/lib/waydroid/images/vendor.img

[ "$(id -u)" = 0 ] || { echo "run as root" >&2; exit 1; }
command -v debugfs >/dev/null || { echo "needs debugfs (e2fsprogs-extra)" >&2; exit 1; }
[ -f "$IMG" ] || { echo "no $IMG: run 'waydroid init' first" >&2; exit 1; }

for d in "$@"; do
	[ -d "$d/vendor" ] || { echo "$d: no vendor/ tree" >&2; exit 1; }
	(cd "$d/vendor" && find . -type f) | while read -r f; do
		install -Dm644 "$d/vendor/$f" "$O/$f"
		case "$f" in ./bin/*) chmod 755 "$O/$f" ;; esac
		echo "installed $O/${f#./}"
	done
done

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
debugfs -R "dump /lib64/hw/hwcomposer.waydroid.so $T/hwc.so" "$IMG" 2>/dev/null
python3 "$HERE/hwcomposer/patch-hwcomposer.py" "$T/hwc.so" "$T/hwc-patched.so"
install -Dm644 "$T/hwc-patched.so" "$O/lib64/hw/hwcomposer.waydroid.so"
echo "installed $O/lib64/hw/hwcomposer.waydroid.so"

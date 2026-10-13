#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Build packages from the patched pmaports checkout and sideload them.
#
#   PHONE=user@host scripts/build.sh [--no-sideload] <package>...
#
# <package> is an origin name (APKBUILD dir). Sideloads the origin plus every
# subpackage of it already installed on the phone, then checks apk really has
# those versions. PMB_CONFIG selects a pmbootstrap config (default: its own
# default). Run it in a real terminal: pmbootstrap asks for sudo.
#
# Kernel (linux-postmarketos-gs101): every pkgrel shares /lib/modules/<rel>,
# so never leave a sideloaded kernel unflashed. Phone to fastboot, then
# `pmbootstrap flasher boot` (RAM test) and `pmbootstrap flasher flash_kernel`.
set -e
PMB="pmbootstrap${PMB_CONFIG:+ -c $PMB_CONFIG}"
W=$($PMB config work)
P=$($PMB config aports)
A=$W/packages/edge/aarch64
dev=$($PMB config device)

sideload=1
[ "$1" = --no-sideload ] && { sideload=; shift; }
[ $# -gt 0 ] || { sed -n '3,14s/^# \{0,1\}//p' "$0"; exit 1; }
[ -z "$sideload" ] || [ -n "$PHONE" ] || {
	echo "set PHONE=user@host (ssh) or pass --no-sideload" >&2; exit 1; }

apkbuild_dir() {
	d=$(find "$P" -path "$P/.git" -prune -o -path "*/$1/APKBUILD" -print | head -1)
	[ -n "$d" ] || { echo "no APKBUILD for $1" >&2; exit 1; }
	dirname "$d"
}
version() {
	(cd "$(apkbuild_dir "$1")" && . ./APKBUILD >/dev/null 2>&1 && echo "$pkgver-r$pkgrel")
}
# every package an APKBUILD produces (pkgname + subpackages, :func stripped)
all_packages() {
	(cd "$(apkbuild_dir "$1")" && . ./APKBUILD >/dev/null 2>&1 &&
		for s in $pkgname $subpackages; do echo "${s%%:*}"; done)
}
for pkg; do apkbuild_dir "$pkg" >/dev/null; done

# stale chroot mounts from earlier runs make the build fail
findmnt -rn -o TARGET | grep "^$W/" | sort -r | xargs -r -n1 sudo umount -l 2>/dev/null || true
# mkinitfs (kernel trigger, flasher) needs it; pmbootstrap reinstalls can drop it
r=$W/chroot_rootfs_$dev
[ ! -d "$r" ] || [ -e "$r/etc/os-release" ] || \
	sudo ln -s ../usr/lib/os-release "$r/etc/os-release"

$PMB checksum "$@"
$PMB build --arch aarch64 "$@"
[ -n "$sideload" ] || { echo "BUILT: $*"; exit 0; }

# sideload by path: pmbootstrap's name lookup trips over some subpackages
inst=$(ssh "$PHONE" "apk list -I 2>/dev/null")
files= want=
for pkg; do
	v=$(version "$pkg")
	subs=$(echo "$inst" | awk -v o="{$pkg}" '$3 == o { print $1 }' |
		sed 's/-[^-]*-r[0-9]*$//')
	# nothing of it on the phone yet: everything it builds
	[ -n "$subs" ] || subs=$(all_packages "$pkg" | while read -r n; do
		[ -f "$A/$n-$v.apk" ] && echo "$n"; done)
	for n in $(printf '%s\n' "$pkg" $subs | sort -u); do
		[ -f "$A/$n-$v.apk" ] || { echo "not built: $A/$n-$v.apk"; exit 1; }
		files="$files $A/$n-$v.apk" want="$want $n-$v"
	done
done
$PMB sideload --host "${PHONE#*@}" --user "${PHONE%@*}" $files

# a sideload has failed silently before
inst=$(ssh "$PHONE" "apk list -I 2>/dev/null")
for w in $want; do
	echo "$inst" | grep -q "^$w " || { echo "SIDELOAD DID NOT LAND: $w"; exit 1; }
done
echo
echo "BUILT + SIDELOADED:$want"
case " $* " in *" linux-postmarketos-gs101 "*)
	echo "Kernel: phone to fastboot, then: $PMB flasher boot   # RAM test";;
esac

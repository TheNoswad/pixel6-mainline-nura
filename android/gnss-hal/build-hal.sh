#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Build the Waydroid GNSS HAL (android.hardware.gnss-service.waydroid) with the
# Android NDK (bionic, so not in pmbootstrap). Output in <outdir>/vendor/...,
# laid out for /var/lib/waydroid/overlay (install with ../install-overlay.sh).
#   PHONE=user@phone sh build-hal.sh <outdir>
#
# Inputs (fetched once into $SRC, default ~/.cache/waydroid-gnss-hal):
# - AOSP android-13 hardware/interfaces: frozen android.hardware.gnss V2 AIDL
# - AOSP android-13 frameworks/native: libbinder_ndk C++/platform headers
# - libbinder_ndk.so + liblog.so of the Waydroid system image, to link
#   against (the NDK stubs lack the platform symbols: service manager,
#   thread pool, VINTF stability). Copied over ssh from the phone's running
#   container (PHONE=user@host), or put them in $DEVLIBS yourself.
set -eu
OUT=${1:?usage: build-hal.sh <outdir>}
HERE=$(cd "$(dirname "$0")" && pwd)
NDK=${NDK:-$HOME/Android/Sdk/ndk/29.0.14206865}
SRC=${SRC:-$HOME/.cache/waydroid-gnss-hal}
# Android 13's aidl: newer ones generate code for newer libbinder_ndk headers
AIDL=${AIDL:-$SRC/bt33/android-13/aidl}
TAG=android-13.0.0_r75
SDK=33
API=$SRC/hw/gnss/aidl/aidl_api/android.hardware.gnss/2
HASH=fc957f1d3d261d065ff5e5415f2d21caa79c310f

mkdir -p "$SRC"
sparse() {	# repo dir paths...
	repo=$1 dir=$2; shift 2
	[ -d "$SRC/$dir" ] && return
	git clone -q --depth 1 --filter=blob:none --sparse -b $TAG \
		https://android.googlesource.com/platform/$repo "$SRC/$dir"
	git -C "$SRC/$dir" sparse-checkout set "$@"
}
if [ ! -x "$AIDL" ]; then
	mkdir -p "$SRC/bt33"
	curl -sfL -o "$SRC/bt33/bt.zip" \
		https://dl.google.com/android/repository/build-tools_r33.0.2-linux.zip
	(cd "$SRC/bt33" && unzip -q bt.zip android-13/aidl android-13/lib64/'*')
fi
sparse hardware/interfaces hw gnss/aidl
sparse frameworks/native fn libs/binder/ndk/include_cpp \
	libs/binder/ndk/include_ndk libs/binder/ndk/include_platform
[ "$(cat "$API/.hash")" = $HASH ] || { echo "unexpected gnss V2 hash" >&2; exit 1; }
DEVLIBS=${DEVLIBS:-$SRC/devlibs}
if [ ! -f "$DEVLIBS/libbinder_ndk.so" ]; then
	[ -n "${PHONE:-}" ] || { echo "set PHONE=user@phone or DEVLIBS" >&2; exit 1; }
	mkdir -p "$DEVLIBS"
	for l in libbinder_ndk.so liblog.so; do
		scp -q "$PHONE:/var/lib/waydroid/rootfs/system/lib64/$l" "$DEVLIBS/"
	done
fi

W=$SRC/work; rm -rf "$W"; mkdir -p "$W/gen"
(cd "$API" && "$AIDL" --lang=ndk --structured --stability=vintf --version=2 \
	--hash=$HASH --min_sdk_version=$SDK -I . -o "$W/gen/src" -h "$W/gen/include" \
	$(find android -name '*.aidl'))

TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
B=$SRC/fn/libs/binder/ndk
CXX="$TC/aarch64-linux-android$SDK-clang++"
CXXFLAGS="-std=c++17 -O2 -fPIE -ffunction-sections -fdata-sections -Wall -Wno-deprecated-declarations
	-I$W/gen/include -I$B/include_cpp -I$B/include_ndk -I$B/include_platform"
objs=
for f in $(find "$W/gen/src" -name '*.cpp') "$HERE/gnss-waydroid.cpp"; do
	o=$W/$(basename "$f" .cpp).o
	$CXX $CXXFLAGS -c "$f" -o "$o"
	objs="$objs $o"
done
BIN=android.hardware.gnss-service.waydroid
$CXX -pie -static-libstdc++ -Wl,--gc-sections -Wl,--as-needed -o "$W/$BIN" $objs \
	-L"$DEVLIBS" -lbinder_ndk -llog
"$TC/llvm-strip" "$W/$BIN"

install -Dm755 "$W/$BIN" "$OUT/vendor/bin/hw/$BIN"
install -Dm644 "$HERE/$BIN.rc" "$OUT/vendor/etc/init/$BIN.rc"
install -Dm644 "$HERE/$BIN.xml" "$OUT/vendor/etc/vintf/manifest/$BIN.xml"
(cd "$OUT" && sha256sum vendor/bin/hw/$BIN)

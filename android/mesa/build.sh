#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Rebuild Waydroid's vendor Mesa with the gs101 Mali-G78 model (runs on bazzite).
#   sh build.sh <outdir>
# Mesa version must match the Waydroid vendor image (26.0.8 in the
# 20260927 MAINLINE vendor). Builds panfrost GL/EGL/GBM with the Android NDK
# inside a fedora:44 container; output = <outdir>/vendor/lib64/..., laid out
# for /var/lib/waydroid/overlay (install with ../install-overlay.sh).
set -eu
OUT=${1:?usage: build.sh <outdir>}
HERE=$(cd "$(dirname "$0")" && pwd)
MESA_VER=${MESA_VER:-26.0.8}
NDK=${NDK:-$HOME/Android/Sdk/ndk/29.0.14206865}
SDK=33
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
W=$OUT/.work; mkdir -p "$W"
[ -f "$W/mesa-$MESA_VER.tar.xz" ] || curl -sfL -o "$W/mesa-$MESA_VER.tar.xz" \
	https://archive.mesa3d.org/mesa-$MESA_VER.tar.xz
# earlier runs leave files owned by container uids: remove inside the userns
podman unshare rm -rf "$W/src"; mkdir "$W/src"
tar -xf "$W/mesa-$MESA_VER.tar.xz" -C "$W/src" --strip-components=1
for p in "$HERE"/*.patch; do (cd "$W/src" && git apply "$p"); done
cat > "$W/cross.ini" <<INI
[constants]
ndk_path = '/ndk'
[binaries]
ar = ndk_path / 'toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-ar'
c = [ndk_path / 'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android$SDK-clang']
cpp = [ndk_path / 'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android$SDK-clang++', '-fno-exceptions', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '--start-no-unused-arguments', '-static-libstdc++', '--end-no-unused-arguments']
c_ld = 'lld'
cpp_ld = 'lld'
strip = ndk_path / 'toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip'
pkg-config = 'false'
[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'armv8'
endian = 'little'
INI
podman run --rm -v "$W:/w:Z" -v "$NDK:/ndk:ro,Z" registry.fedoraproject.org/fedora:44 sh -euc "
dnf -qy install meson ninja-build gcc gcc-c++ python3-mako python3-pyyaml \
	python3-packaging python3-ply glslang bison flex git-core pkgconf \
	llvm-devel clang-devel libclc libclc-devel spirv-llvm-translator-devel \
	spirv-tools-devel libdrm-devel zlib-ng-compat-devel expat-devel >/dev/null
cd /w/src
# 1. host tools: mesa_clc, vtn_bindgen2, panfrost_compile
if [ ! -x /w/host/bin/panfrost_compile ]; then
rm -rf /w/build-host
meson setup /w/build-host -Dprefix=/w/host -Dbuildtype=release \
	-Dplatforms= -Dgallium-drivers= -Dvulkan-drivers= -Degl=disabled \
	-Dglx=disabled -Dgbm=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled \
	-Dmesa-clc=enabled -Dinstall-mesa-clc=true \
	-Dtools=panfrost -Dprecomp-compiler=enabled -Dinstall-precomp-compiler=true
ninja -C /w/build-host install >/dev/null
fi
export PATH=/w/host/bin:\$PATH
# 2. Android cross build
rm -rf /w/build
meson setup /w/build --cross-file /w/cross.ini \
	-Dbuildtype=release -Db_ndebug=true -Dstrip=true \
	-Dplatforms=android -Dplatform-sdk-version=$SDK \
	-Dandroid-stub=true -Dandroid-libbacktrace=disabled \
	-Dallow-fallback-for=libdrm \
	-Dmesa-clc=system -Dprecomp-compiler=system \
	-Dgallium-drivers=panfrost -Dvulkan-drivers= \
	-Degl=enabled -Dgbm=enabled -Dgles1=enabled -Dgles2=enabled \
	-Degl-lib-suffix=_mesa -Dgles-lib-suffix=_mesa \
	-Dllvm=disabled -Dzstd=disabled -Dexpat=disabled \
	-Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled \
	-Dgallium-va=disabled -Dgallium-extra-hud=false \
	-Dgallium-rusticl=false -Dtools=[] -Dbuild-tests=false
ninja -C /w/build
"
# overlay layout
V=$OUT/vendor/lib64
mkdir -p "$V/egl"
for f in libgallium_dri.so dri_gbm.so; do
	install -m644 "$(find "$W/build" -name $f -type f | head -1)" "$V/$f"
done
for f in libEGL_mesa.so libGLESv1_CM_mesa.so libGLESv2_mesa.so; do
	install -m644 "$(find "$W/build" -name $f -type f | head -1)" "$V/egl/$f"
done
(cd "$OUT" && find vendor -type f | sort | xargs sha256sum)

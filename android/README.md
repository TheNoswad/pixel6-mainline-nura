# Android-side pieces for Waydroid on the Pixel 6

Waydroid runs a LineageOS 20 (Android 13) container. Three things inside it
need Pixel 6 specific files in its vendor overlay
(`/var/lib/waydroid/overlay/vendor`, `mount_overlays` must be on). They are
Android (bionic) binaries, built with Google's Android NDK on a PC, not by
pmbootstrap, so they are not packages: build them, copy the outputs to the
phone and run `install-overlay.sh` there.

| Directory | What | Why |
|---|---|---|
| `mesa/` | Mesa (panfrost) with the gs101 Mali-G78 model, `build.sh <outdir>` | Waydroid's Mesa does not know the G78 in gs101: no hardware GL without it |
| `gnss-hal/` | AIDL GNSS HAL reading the host's NMEA socket, `PHONE=user@phone build-hal.sh <outdir>` | GPS in Android apps (host side: bcm4776-gnssd-waydroid) |
| `hwcomposer/` | Binary patch for Waydroid's `hwcomposer.waydroid.so` | Plasma Mobile's configure events re-hotplug the display every 1-3 s, SurfaceFlinger crashes and apps stop opening |

The hwcomposer is not built: `install-overlay.sh` takes it from the phone's
own Waydroid vendor image and patches it there. The patcher checks the exact
input file (vendor image 20260927 MAINLINE, BuildId
8937ba8f2556cc3e0050372676e63c9d) and refuses anything else.

## Build (on a PC)

Needs podman (Mesa builds in a Fedora container), the Android NDK r29
(`NDK=` path, default `~/Android/Sdk/ndk/29.0.14206865`) and, for the HAL,
ssh access to the phone with Waydroid initialised (it copies
libbinder_ndk.so/liblog.so from the container to link against).

    sh mesa/build.sh ~/oriole-android/mesa
    PHONE=user@phone sh gnss-hal/build-hal.sh ~/oriole-android/gnss-hal

The Mesa version must match the Waydroid vendor image (26.0.8 in
20260927 MAINLINE).

## Install (on the phone)

    scp -r android ~/oriole-android user@phone:
    ssh -t user@phone 'sudo sh android/install-overlay.sh oriole-android/mesa oriole-android/gnss-hal'
    ssh -t user@phone 'sudo systemctl restart waydroid-container'

Kernel side (in this repository's packages): gs101 network options for
Waydroid (linux-postmarketos-gs101), the iptables modules
(device-google-oriole), the GNSS socket bind mount (temp/waydroid) and the
host sensors (waydroid-sensors-usf).

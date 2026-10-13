# GNSS HAL for Waydroid

Waydroid's vendor image has no GNSS HAL, so Android has no `gps` location
provider. This adds one, `android.hardware.gnss-service.waydroid`.
It is a minimal AIDL `IGnss` (V2) vendor HAL that reads NMEA from the host.

- **Socket:** `/run/waydroid-gnss/nmea.sock` on the host. The patched
  waydroid (temp/waydroid, 0001) binds the directory to
  `/dev/waydroid-gnss` in the container.
  - The socket has group 1021 (`waydroid-gps` on the host, from
    sysusers.d in bcm4776-gnssd-waydroid), which is Android's `gps` group. The HAL runs
    as `gps`, so it can read the socket.
  - Apps cannot read the socket. They get locations only through
    LocationManager and its permissions.
- **Provider:** on the Pixel 6 that is `bcm4776-gnssd-waydroid`, a second
  listening socket of bcm4776-gnssd. Any NMEA stream source works.
- **Power:** the HAL connects only between Android's `start()` and
  `stop()`. The host GNSS runs only while an Android app wants GPS.
- **Sentences:**
  - RMC gives position, speed, track and date.
  - GGA gives altitude, converted to the WGS84 ellipsoid with the geoid
    separation, and HDOP.
  - GST gives the horizontal error estimate. When GST is missing, the
    accuracy is HDOP x 5 m.
  - GSV and GSA give the satellite status, once per read. NMEA 4.11
    signal IDs become carrier frequencies, so a GPS L5 signal is its own
    entry. GSA marks the satellites used in the fix.
- **Extensions:** none (measurements, PSDS, AGNSS, ...).
- **Install:** files go to `/var/lib/waydroid/overlay/vendor/`
  (`bin/hw`, `etc/init`, `etc/vintf/manifest`), see ../README.md.

## Build

The binary links Android's bionic and `libbinder_ndk`, so it is built with
the Android NDK outside pmbootstrap: `PHONE=user@phone sh build-hal.sh
<outdir>`. The script fetches:

- the frozen `android.hardware.gnss` V2 AIDL from AOSP android-13
  (hash-checked),
- Android 13's `aidl` (build-tools 33.0.2),
- the libbinder_ndk headers.

It links against `libbinder_ndk.so`/`liblog.so` copied from the Waydroid
system image. The NDK stubs lack the platform service manager symbols.
Android 13 (LineageOS 20) container; no SELinux on the host kernel, so no
sepolicy is shipped.

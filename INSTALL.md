# Installing

This replaces Android on the phone. It has been done on one phone; expect
rough edges, and read all of this before you start.

## Rules that protect your phone

- **Never relock the bootloader** while anything but a complete stock
  image is installed. A relocked phone that fails verification may not
  boot to anything.
- **Never flash an older bootloader** than the phone already has. Pixel 6
  bootloaders have anti-rollback protection; going back can brick it.
- **Never write to the bootloader, modem, efs or persist partitions.** efs
  and persist hold data unique to your phone (IMEI, radio calibration,
  sensor and speaker calibration) that no factory image can restore. The
  device package marks these partitions read-only at boot (`61-oriole-stock-ro.rules`)
  and only ever mounts them read-only, but nothing can protect them from
  your own `fastboot flash` or `dd`.
- **Back up efs, efs_backup, modem_userdata and persist** (see below) and
  keep the copies off the phone.

## What you need

- A Pixel 6 (oriole) that can be OEM-unlocked. Carrier-locked phones
  usually cannot.
- Stock Android build **CP1A.260405.005**. The packages were built and
  tested against it: the firmware package extracts this build's vendor
  image and the modem runs the modem image already on the phone. If your
  phone is on a newer build, do not downgrade the bootloader (anti-rollback);
  a newer build is untested.
- A Linux PC with [pmbootstrap](https://docs.postmarketos.org/pmbootstrap/),
  git, `fastboot`, about 30 GB free (the factory image download is large),
  and a USB cable.

## 1. Unlock the bootloader

In Android: Settings > About phone, tap "Build number" seven times;
Settings > System > Developer options, enable "OEM unlocking". Then:

    adb reboot bootloader
    fastboot flashing unlock    # confirm on the phone; this wipes it

## 2. Build

    git clone https://github.com/TheNoswad/pixel6-mainline-nura.git pixel6
    git clone https://gitlab.postmarketos.org/postmarketOS/pmaports.git
    git -C pmaports checkout "$(cat pixel6/UPSTREAM)"
    pixel6/scripts/apply.sh pmaports

    pmbootstrap init
        # Work path: anything; pmaports location: the pmaports checkout above
        # Channel: edge, vendor: google, device: oriole
        # User interface: phosh or plasma-mobile (both tested)

Build the device packages once so you see build errors before installing
(firmware-google-oriole downloads Google's factory image here):

    pmbootstrap build --arch aarch64 linux-postmarketos-gs101 \
        firmware-google-oriole device-google-oriole
    pmbootstrap install

## 3. Flash

Phone in fastboot mode (power + volume down), connected by USB:

    pmbootstrap flasher flash_vbmeta
    pmbootstrap flasher flash_rootfs     # writes userdata
    pmbootstrap flasher flash_kernel     # writes boot
    fastboot reboot

To try a kernel without flashing it: `pmbootstrap flasher boot`.

The first boot takes a while: it resizes the root filesystem and the modem
needs about half a minute after the UI is up. Plugging a charger into the
switched-off phone boots it.

## 4. Back up the per-phone partitions

Do this on the first boot, before anything else. Over USB the phone is at
172.16.42.1 (`ssh user@172.16.42.1`, the user from `pmbootstrap init`):

    for p in efs efs_backup modem_userdata persist; do
        sudo dd if=/dev/disk/by-partlabel/$p of=$p.img bs=1M
    done
    sha256sum *.img > SHA256SUMS

Copy the images and SHA256SUMS to your PC and somewhere else safe. They
contain your IMEI: don't publish them.

## 5. Set up the modem

The SIM's carrier settings are not detected yet. The packaged
`/etc/ModemManager/shannon.conf` is for **T-Mobile US and its MVNOs**
(attach APN `Wholesale`). For another carrier, set its attach APN there and
in the NetworkManager mobile connection; the carrier manifest in that file
is T-Mobile's and other carriers are untested.

**Emergency calls are untested. Keep another phone for them.**

## Waydroid

Install Waydroid as usual (`waydroid init`), then build and install the
Android-side parts as described in [android/README.md](android/README.md)
for hardware GL, GPS and sensors.

## Updating

Packages you built yourself are installed from your local pmbootstrap
repository. To move to a newer pmaports commit: update `UPSTREAM` in your
clone, check out that commit, run `scripts/apply.sh` again and rebuild
anything whose upstream APKBUILD changed. `scripts/build.sh` builds and
sideloads packages to a running phone (`PHONE=user@host`).

Kernel packages of the same version share `/lib/modules`: after installing
a new kernel package, always flash it (`pmbootstrap flasher flash_kernel`)
before rebooting.

## Going back to Android

Download the factory image for your build from
<https://developers.google.com/android/images>, put the phone in fastboot
mode and run its `flash-all.sh` (it wipes userdata). Leave the bootloader
unlocked unless the phone runs a complete, unmodified factory image.

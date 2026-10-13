# pixel6-mainline-nura: Pixel 6 (oriole) mainline port

Packages that run [postmarketOS](https://postmarketos.org) on the Google
Pixel 6 (`google-oriole`, Tensor gs101) as a daily-usable phone: calls, SMS,
MMS, mobile data, GPS, Bluetooth audio, sensors, haptics, the front camera
and Waydroid with hardware GL.

**Built on M8's work.** The kernel is
[M8's gs101-mainline tree](https://github.com/m8l8th814n-eng/gs101-mainline),
which did the mainline gs101 bring-up, and the modem boots with M8's
reverse-engineered [tinycbd](https://github.com/m8l8th814n-eng/tinycbd).
This repository is packaging and fixes on top of that. M8 also has
[Mesa with Vulkan](https://github.com/m8l8th814n-eng/mesa-gs101) and
[U-Boot](https://github.com/m8l8th814n-eng/u-boot-gs101) for gs101.

> **Unofficial.** This is a personal project. It is not affiliated with or
> endorsed by postmarketOS, Alpine Linux, Google or the authors of the
> kernel tree it builds on. **Do not report problems with it to
> postmarketOS** or any upstream project: open an issue here instead.
>
> **AI-assisted.** Much of the code, the patches and the reverse engineering
> was done with an AI assistant (Claude). That is why none of it is sent
> upstream; most upstream projects don't accept such contributions. Review
> it as you would any unknown code before you flash it.

It can still break your phone in ways that are hard to undo. Read
[INSTALL.md](INSTALL.md) completely before you start, especially the parts
about backups and the bootloader.

## Status

Tested on one Pixel 6 (stock build CP1A.260405.005) with both Phosh and
Plasma Mobile, on a T-Mobile US network (Mint Mobile SIM).

| Works | Notes |
|---|---|
| Display, touch, GPU (OpenGL ES, panfrost) | GPU frequency scaling through ACPM; Flatpak apps need a G78-aware GL extension (see HARDWARE.md) |
| Wi-Fi | Firmware hangs now restart the interface (fix still in long-term testing) |
| Bluetooth | A2DP and HFP (headset calls) |
| Speakers, earpiece, microphones | With the stock speaker protection and this phone's calibration |
| Mobile data, SMS, MMS, VoLTE calls | **T-Mobile US and its MVNOs only** for now, see below |
| Charging | USB-PD up to 9 V. Plugging in a powered-off phone boots it (no off-mode charging screen) |
| GPS | Own driver, no Broadcom userspace; every fix is a cold start (~40 s outdoors) |
| Sensors | Orientation, ambient light (auto-brightness), compass |
| Haptics | Calibrated per phone from the persist partition |
| Front camera | Works in apps; preview is choppy, colours need tuning |
| Waydroid | Hardware GL, GPS, sensors (Android-side parts are manual builds: [android/](android/README.md)) |

**Known problems and missing features**

- **Carrier:** the modem is configured for T-Mobile US
  (`/etc/ModemManager/shannon.conf`, attach APN "Wholesale"). Other carriers
  need their own values and are untested; choosing the configuration from
  the SIM is not implemented.
- **Emergency calls are untested. Do not rely on this phone for them.**
- Calls only work about 2.5-3 minutes after boot (the first IMS
  registration always fails and retries).
- Suspend saves no power: s2idle works (Wi-Fi is unloaded around it) but
  draws about as much as awake idle, and deep suspend never resumes.
  Battery life is short (about 600 mA idle drain measured).
- No double-tap to wake.
- Vulkan (PanVK) is new and opt-in: set `PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1`.
- Rear cameras (main GN1, ultra-wide) don't work.
- GPS has no warm start yet.
- The fuel gauge reports a fixed battery temperature.

[HARDWARE.md](HARDWARE.md) has the per-component details.

## Repository layout

The layout mirrors [pmaports](https://gitlab.postmarketos.org/postmarketOS/pmaports);
it is an overlay applied onto one pinned pmaports commit (`UPSTREAM`).

| Path | What |
|---|---|
| `device/testing/device-google-oriole` | Device package: udev, systemd units, audio (UCM), stock partitions read-only |
| `device/testing/linux-postmarketos-gs101` | Kernel: [gs101-mainline](https://github.com/m8l8th814n-eng/gs101-mainline) @ 850061c7 + patches 0005-0029 |
| `device/testing/firmware-google-oriole` | Firmware, **extracted at build time from Google's factory image**; nothing proprietary is in this repository |
| `main/oriole-modem` | Modem bring-up (Shannon 5123): CP boot, RFS server, carrier config |
| `main/oriole-sensors` | Sensors through the AoC sensor hub (USF), iio-sensor-proxy replacement, auto-brightness |
| `main/bcm4776-gnssd` | GNSS daemon for the BCM4776 (NMEA to geoclue and Waydroid) |
| `main/aoc-voiced` | Keeps the AoC voice call path open during calls |
| `main/waydroid-sensors-usf` | Sensors HAL for Waydroid |
| `main/bootmac`, `main/postmarketos-base-ui` | Small changes to the upstream packages (Bluetooth address, audio follows headsets, HFP) |
| `temp/` | Forks of Alpine packages with patches: modemmanager (Shannon plugin), mesa (G78, PanVK v9), libcamera (gs101 CSIS), callaudiod, mmsd-tng, mobile-broadband-provider-info, waydroid |
| `android/` | Android-side Waydroid parts, built by hand with the NDK |
| `scripts/apply.sh` | Copy the packages onto a pmaports checkout |
| `scripts/build.sh` | Build packages and sideload them onto the phone |

## Credits

The kernel is [M8's gs101-mainline tree](https://github.com/m8l8th814n-eng/gs101-mainline),
which did the hard part of mainline gs101 support. The modem boots with
M8's [tinycbd](https://github.com/m8l8th814n-eng/tinycbd), and the audio
bring-up used M8's [gs101-miscscripts](https://github.com/m8l8th814n-eng/gs101-miscscripts).
Thanks to the
postmarketOS, Alpine, Mesa, libcamera, ModemManager and Waydroid projects
whose work all of this sits on.

## Licence

See [NOTICE.md](NOTICE.md). In short: GPL-3.0-or-later for the repository,
kernel patches GPL-2.0-only, patches to other projects under that project's
licence. Firmware is never part of the repository, and the built firmware
and oriole-modem packages must not be redistributed.

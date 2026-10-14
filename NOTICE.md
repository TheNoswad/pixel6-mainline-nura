# Licensing and third-party notices

## Licences in this repository

| What | Licence |
|---|---|
| Repository as a whole, packaging (APKBUILDs, install scripts), `scripts/` | GPL-3.0-or-later (see [LICENSE](LICENSE); the layout and many files derive from [pmaports](https://gitlab.postmarketos.org/postmarketOS/pmaports), which is GPL-3.0) |
| Our own programs: `main/aoc-voiced`, `main/bcm4776-gnssd`, `main/oriole-modem` (oriole-rfsd, units), `main/oriole-sensors`, `main/waydroid-sensors-usf`, `android/` (except the Mesa patch in `android/mesa`, which is MIT like Mesa) | GPL-3.0-or-later |
| `device/testing/device-google-oriole` (files it installs) | MIT |
| Kernel patches and config, `device/testing/linux-postmarketos-gs101` | GPL-2.0-only, like the kernel |
| Patches in `temp/` and in `main/bootmac`, `main/postmarketos-base-ui` | The licence of the project they patch (see each APKBUILD's `license=`) |

Files carry SPDX headers where the format allows.

## Third-party sources used at build time

These are downloaded by the APKBUILDs, not stored here.

- **Kernel:** [github.com/m8l8th814n-eng/gs101-mainline](https://github.com/m8l8th814n-eng/gs101-mainline)
  @ 850061c79264d018317e9f849701f165e138e20c, GPL-2.0. Credit for the
  gs101 mainline work goes to its authors.
- **Haptics driver** (patch 0028): Cirrus Logic's cs40l2x driver as
  shipped in Google's [amplifiers kernel modules](https://android.googlesource.com/kernel/google-modules/amplifiers)
  @ 33976d763bc0, GPL-2.0, with our port on top. The original copyright
  notices are kept in the patch.
- **U-Boot:** [github.com/m8l8th814n-eng/u-boot-gs101](https://github.com/m8l8th814n-eng/u-boot-gs101)
  @ c9f19f1dacb8e49b345eb89b42f96144db5f7269, GPL-2.0-or-later. Credit for
  the gs101 port goes to its authors; our only patch shortens the boot
  menu timeout.
- **tinycbd:** [github.com/m8l8th814n-eng/tinycbd](https://github.com/m8l8th814n-eng/tinycbd)
  @ 4040055106680fd695816c8a1b88e2c447309278. **It has no licence**, so it
  is not included here: `oriole-modem` fetches it at build time and only
  our patch (`0001-handover-imei-and-cpsha.patch`) is in this repository.
  Without a licence the built `oriole-modem` package **must not be
  redistributed**; build it yourself.
- **Firmware:** `firmware-google-oriole` downloads Google's Pixel 6
  factory image (CP1A.260405.005) from dl.google.com and extracts the
  firmware from it. That firmware is proprietary to Google and its
  suppliers. It is never part of this repository, and the built firmware
  packages **must not be redistributed**.
- Upstream sources of the forked packages in `temp/` (Mesa, libcamera,
  ModemManager, callaudiod, mmsd-tng, mobile-broadband-provider-info,
  Waydroid) under their own licences.

## Interoperability

The modem plugin, the GNSS daemon, the sensor tools and the Waydroid HALs
are our own code, written to talk to the phone's hardware and firmware.
Message formats were learned by observing the hardware and the stock
software. No vendor code or data tables are copied into them.
`android/hwcomposer/patch-hwcomposer.py` patches the Waydroid vendor
image's own `hwcomposer.waydroid.so` on your phone; no binary is shipped.

## No release binaries

This is a source-only release. Because of the firmware and tinycbd
restrictions above, there are no prebuilt images or packages.

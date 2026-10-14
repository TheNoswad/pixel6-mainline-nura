# Hardware notes

How each part of the Pixel 6 is brought up, and what is still missing.
Kernel patch numbers refer to `device/testing/linux-postmarketos-gs101`;
each patch's description has the details.

## Kernel

Base: [M8's gs101-mainline](https://github.com/m8l8th814n-eng/gs101-mainline)
@ 850061c7 (7.2.0-rc3), which carries mainline gs101 support plus out-of-tree
drivers (bcmdhd, AoC, cpif, trusty, gsa, samsung-iommu, ...). This repository
adds a config and 28 patches. They are written for this phone and this
tree, not as upstream submissions.

## Partitions and per-phone data

The stock bootloader chain, efs, efs_backup, modem_a/b, modem_userdata,
persist and friends are set read-only by udev at boot
(`61-oriole-stock-ro.rules`). Per-phone calibration is read at runtime
from persist, mounted read-only at `/run/oriole/persist`: speaker
calibration, haptics f0/redc/q, sensor factory calibration and the modem's
cpsha. Nothing per-phone is baked into a package.

## Firmware

`firmware-google-oriole` downloads Google's factory image for
CP1A.260405.005 at build time, extracts `vendor.img` and copies out the
firmware for audio (AoC, cs35l41), Bluetooth, GNSS, haptics, sensors and
Wi-Fi. Nothing proprietary is stored in this repository, and the built
package must not be redistributed.

## Display and GPU

The Mali-G78 runs under panfrost with Mesa from `temp/mesa` (adds the gs101
G78 model). Patch 0008 drives the GPU clock through ACPM so it scales
(151-572 MHz) instead of sitting at the boot rate. Patch 0017 orders the GPU
power-up after cpufreq: when the two raced at boot, about half the boots
came up without a display.

Flatpak apps need an `org.freedesktop.Platform.GL.oriole` extension built
with the same Mesa patch for each runtime branch (the device package sets
`FLATPAK_GL_DRIVERS=oriole`). That extension is not part of this
repository; without it Flatpak apps fall back to software rendering.

Vulkan: upstream PanVK has no support for Valhall v9 GPUs with the job
manager (G57/G68/G77/G78). `temp/mesa` adds it
(`panvk-valhall-v9-jm.patch`): compute, direct, indexed and indirect draws,
layered rendering (one draw per layer, as on Bifrost) and WSI. It reports
Vulkan 1.0. In VK-GL-CTS 1.4.4 (every 5th test of the default mustpass)
126127 tests pass; the few failures are tests that are flaky or failing
upstream on other Mali GPUs, or conformance checks. vkcube, vkmark and GTK4's
Vulkan renderer work. Not supported: geometry and tessellation shaders,
transform feedback, multiDrawIndirect.

Like upstream does for other little-tested GPUs, the driver only shows up
when `PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1` is set; install
`mesa-vulkan-panfrost` and run an app with that variable to use it.

## CPU, thermal, performance

- 0007: thermal cooling for every CPU cluster. Upstream only throttled one
  big core, and a long all-core load while charging hit the 105 C
  critical trip and shut down.
- Every cpufreq change is an ACPM IPC. `oriole-cpufreq-ratelimit` sets the
  schedutil rate limit to 10 ms, which removed most of the UI stutter.
- `oriole-ui-uclamp` gives Phosh's shell and compositor (phosh, phoc, and
  phrog on the greeter) a utilization floor so they don't run on the little
  cores at low clocks. It only applies to Phosh.

## Suspend

s2idle works with patch 0004 (keep the ACPM C2 hint during s2idle), with
bcmdhd unloaded around it (`oriole-bcmdhd-sleep`) since the Wi-Fi
firmware does not survive it. It saves almost nothing: power in s2idle is
about the same as awake idle, because the big consumers are blocks only
deep suspend turns off. Deep suspend (PSCI SYSTEM_SUSPEND) never resumes
yet; 0010 and 0011 fix two of the crashes on the way there.

## Wi-Fi (BCM4389, bcmdhd on PCIe)

- NetworkManager is kept off bcmdhd's extra Android interfaces
  (`90-bcmdhd-unmanaged.conf`); managing them crashed the driver.
- 0026: the PCIe MSI receiver could miss an interrupt edge and stay deaf,
  which looked like random firmware hangs a couple of times a day.
- 0027: on a firmware HANG the driver restarts the interface itself
  (Android's Wi-Fi HAL normally does this).
- MAC addresses come from the bootloader (`bootmac`).

## Bluetooth (BCM4389C1 on UART)

The patch RAM file from the firmware loads at boot. It sets the same
address on every phone, so 0018 marks that address invalid and `bootmac`
(patched in `main/bootmac`) sets the phone's real address from the
bootloader. A2DP and HFP work. `postmarketos-base-ui` is patched so
audio follows a newly connected headset and so only the active session's
WirePlumber registers the HFP profiles.

## Audio

Audio goes through the AoC DSP to two cs35l41 amplifiers (top/earpiece and
bottom). 0006 starts the speaker protection algorithm after the amplifier
PLL locks; before that the amps muted after a few streams. The device
package applies the phone's speaker calibration from persist and ships the
ALSA UCM configuration (HiFi and Voice Call). 0032 reads the amplifiers'
OTP trim over SPI in small PIO transfers and checks it: the gs101 SPI
DMA path corrupted the read, and the bad trim made the speakers distort.

## Modem (Samsung Shannon 5123 on PCIe)

- The kernel's cpif driver talks to the modem; 0012-0014 make the PCIe
  relink as reliable as the vendor driver's.
- `oriole-modem` boots the modem with M8's tinycbd (fetched at build time,
  plus a patch that fills in the IMEI and cpsha), and runs `oriole-rfsd`,
  our own remote file server for the modem's NV data and carrier config.
  The modem image and NV data come from the phone's own read-only modem
  and efs partitions; writes go to a copy in `/var/lib/oriole-modem`.
- `temp/modemmanager` adds a Shannon plugin speaking the modem's SIT
  protocol: registration, data, SMS and IMS for VoLTE calls.
- Call audio: `aoc-voiced` keeps the AoC voice path open during a call;
  `temp/callaudiod` switches the routing.
- MMS: `temp/mmsd-tng` and `temp/mobile-broadband-provider-info` fix APN
  matching for T-Mobile MVNOs.

Open: carrier settings are hardcoded for T-Mobile US
(`/etc/ModemManager/shannon.conf`); calls only work 2.5-3 minutes after
boot; emergency calls are untested.

## GNSS (BCM4775/4776 on SPI)

`bcm4776-gnssd` is our own driver for the receiver, with no Broadcom
userspace: it loads the firmware patch, configures the chip and serves
NMEA to geoclue and to Waydroid. Kernel side: 0022-0025 (fixes to the
vendor bcm47765 driver, regulators, SPI chip select). See
[main/bcm4776-gnssd/README.md](main/bcm4776-gnssd/README.md). Every fix
is a cold start (about 40 s outdoors); warm start is not done yet.

## Sensors (AoC sensor hub, USF)

The sensors sit behind the AoC's sensor hub. `oriole-sensors` loads the
sensor registry (vendor scripts plus this phone's factory calibration from
persist) and replaces iio-sensor-proxy with its own D-Bus service for
orientation, ambient light and compass; `oriole-autobrightness` is
optional. 0015 and 0016 fix AoC driver bugs that left the sensors
unpowered. See [main/oriole-sensors/README.md](main/oriole-sensors/README.md).

## Haptics (CS40L25A)

0028 adds Google's cs40l2x driver (GPL-2.0) ported to this kernel with
force-feedback support, and 0029 the device tree node. `oriole-haptics-cal`
loads this phone's calibration from persist, like the stock vibrator HAL.

## Charging

- 0001-0003: charge from PC ports, keep charging enabled at shutdown,
  report charging status to UPower.
- 0019: sink 9 V from USB-PD chargers (as stock does), about +2.2 A.
- 0030: real battery temperature. Firmware leaves the fuel gauge waiting
  for the host to write the temperature (Android does); the driver now
  lets the gauge read the battery thermistor itself.
- Plugging a charger into the switched-off phone boots the OS; there is no
  off-mode charging screen.

## Cameras

The front camera (IMX355) works through the gs101 MIPI-CSIS receiver and
libcamera's software ISP (`temp/libcamera` adds the pipeline). 0020 makes
the frame buffers importable by the GPU and 0021 fixes torn frames.
The ultra-wide (IMX386) works the same way: 0031 turns off its embedded
data lines (the short embedded-data line stalled the receiver), and
`temp/libcamera` adds an IMX386 sensor helper and an uncalibrated tuning
file. Preview is choppy and colours are off (no colour calibration yet).
The rear main camera (GN1) is disabled; the torch works.

## Game controllers

The kernel enables hidraw, joydev, xpad and the HID drivers for
PlayStation, Sony, Steam, Nintendo, Microsoft (Xbox over Bluetooth),
Logitech and Stadia controllers, with force feedback. Steam Input needs
user access to `/dev/uinput` and the controllers' hidraw nodes (for
example Alpine's `steam-devices` rules).

## Waydroid

Hardware GL, GPS and sensors need Android-side pieces built with the NDK:
see [android/README.md](android/README.md). Host side: kernel network
options, the iptables modules (device package), the GNSS socket bind mount
(`temp/waydroid`) and `waydroid-sensors-usf`.

## Not working

Double-tap to wake (the touch driver has no gesture mode), deep
suspend, the rear main camera, GPS warm start, carrier detection from the SIM.

# oriole-sensors: Pixel 6 AoC sensor hub (USF) + auto-brightness

All of oriole's sensors are attached to the AoC (Always-on Compute) on its
own buses: TMD3719 ALS/prox, LSM6DSR IMU, MMC56x3x mag, ICP10101 baro,
VD6282 spectral. The AoC firmware runs Google's USF sensor framework; Android
talks to it with the closed sensors.usf.so/libusf.so HAL. This is a small
reimplementation of the client side, reverse engineered from those
libraries (CP1A.260405.005 vendor.img, Ghidra decompile; libusf also
contains the server-side code, which documents the request handling).

Part of the Pixel 6 Nura port, not for upstream submission.

## Protocol (oriole_usf.py)

- Transport: `/dev/acd-com.google.usf` (aoc_channel_dev). One FlatBuffer per
  read()/write(), ≤ 1020 bytes. Bigger messages are sent as fragments
  (UsfMsg type 3) of the whole serialized message; batched sample events
  come back fragmented too.
- `UsfMsg {type, payload}`: 1 request, 2 response, 3 fragment, 5 event,
  9 sample event. Request `{req_type, req_id, server, payload}`, response
  `{req_type, req_id, status, payload}`.
- Servers by UUID (GET_SERVER, req 3): sensor manager
  `6b31cbf0744a14a1d4e8ae8a3e09fc4d`, registry `d76a14cc264deea1c6406f8b461682c0`.
  Sensors are servers too: their id is the server handle.
- Registry: nothing probes until the AP loads it. REGISTRY_LOAD_SCRIPT
  (req 0x18) sends a `.reg` script as text plus the device CDT
  (`/chosen/plat`: product<<16|stage<<12|major<<8|minor<<4|variant =
  0x36100 on this phone) and MLB (-1 unless pcbcfg starts with "MLB"); the
  AoC parses the script and skips scripts whose `?` CDT filter doesn't match.
  Load order like SensorHal::LoadRegistry: vendor `oriole_dvt.reg`,
  `append/motion_state.reg`, persist factory calibration `*_fac_cal.reg`,
  then `/ loaded=1`. About 1 s later SENSOR_LIST (req 5) returns 41 sensors:
  18-20 VD6282, 21/22 TMD3719 ALS (+raw), 23 prox, 24 TMD temp, 25 MMC56X3X
  mag, 26 accel, 27 gyro, 28-31 LSM6DSR temp/motion/stationary/free fall,
  32/33 ICP10101 pressure/temp, 34/35 touch gestures, 36-39 camera vsync,
  40 motion state, 41/42 auto brightness, 43 pickup, 44 double twist,
  45 game RV, 46 geomagnetic RV, 47 gravity, 48 linear accel, 49/53 prox
  gestures, 50 orientation, 51 quick pickup, 52 rotation vector,
  54 significant motion, 55/56 step counter/detector, 57 tilt,
  58 device orientation.
- SENSOR_INFO (req 4) → `{0 name, 1 type, 2 max range f32, 3 min period
  ns i64, 6 max period ns i64, 9 resolution f32, 10 registry path, ...}`.
  START_SAMPLING (req 8) `{0 reporting mode (1 continuous, 2 on-change),
  1 period ns, 2 max latency ns}` → `{0 sampling handle}`; STOP (req 9)
  `{0 handle}`. The ALS runs at ~25 Hz whatever the period (integration is
  synced to the display), so the daemon uses 1 s max latency (one wakeup/s).
- Sample event payload: `u32 1, u32 0, u32 handle, u16 type, u8 count,
  u8 value bytes`, then per sample `u64 timestamp + f32 values`.
  TMD3719 Ambient Light: `values[0]` = lux (factory calibrated), then raw
  channel counts.

## Kernel side

- 0015 (linux-postmarketos-gs101): aoc_channel_dev empty-list bug in
  `aocc_sh_mem_doorbell_probe()`.
- 0016: aoc_core failed to get `sensor_1v8`/`sensor_3v3` at probe and never
  retried, so `vdd_sensors` stayed off and the AoC probed unpowered sensors.
  Now retried on every enable (oriole-usf-setup writes `sensor_power_enable`).

## Services

- `oriole-usf.service` (oriole-usf-setup, oneshot): rails, channel device,
  registry. Idempotent; the proxy reruns it after an AoC error.
- `oriole-sensorproxy.service`: owns `net.hadess.SensorProxy` on the system
  bus (the iio-sensor-proxy API, which is masked: it finds no IIO sensors
  here). LightLevel (lux, panel leakage subtracted), AccelerometerOrientation
  + AccelerometerTilt (LSM6DSR, 100 ms), ProximityNear (TMD3719, on-change),
  CompassHeading (the AoC's fused Orientation Sensor azimuth, degrees from
  magnetic north, 0.5 degree steps, 200 ms). A sensor samples only while a
  client holds a claim (claims drop when the client leaves the bus), and
  light/accel/compass pause while DRM dpms is off. KWin/Plasma get auto-rotate through Qt's iio-sensor-proxy
  sensors backend. Test without root: `ORIOLE_SENSORPROXY_SESSION=1` under
  `dbus-run-session`. Config /etc/oriole-sensorproxy.conf (leak_table,
  accel_period_ms, debug=1).
- Waydroid gets its sensors from main/waydroid-sensors-usf (waydroid-sensord,
  its own USF channel), not from this proxy.
- `oriole-autobrightness.service`: PowerDevil (6.7) has no ambient-light
  support, so this claims Light while the display is on and sets the raw
  sysfs backlight (0–4095): brightness = curve(lux) × 2^offset, smoothed in
  log(lux) (brighten τ 2 s, darken τ 6 s), 6 % deadband, 400 ms ramps.
  Moving the brightness slider sets `offset` (saved in
  /var/lib/oriole-autobrightness/offset, starts at 0). Config
  /etc/oriole-autobrightness.conf (curve, taus, debug=1). Disable:
  `systemctl disable --now oriole-autobrightness`.

Panel leakage: in a ~1.7 lux room the ALS read 4.8 lux at backlight 100 and
98 lux at 4095 (leak.py), and it depends on what is on screen; the proxy
subtracts a backlight -> lux table, which a simulation showed keeps the
loop stable for ±50 % content error. The stock HAL models leakage per frame
from display content (DisplayInfoService/LeakageCalculator), and the AoC
is given the panel TE2 timing (tmd3719 te2_alignment/sync_delay_ns) to
integrate between OLED emission pulses; the mainline s6e3fc3 panel driver
doesn't set TE2 up, which is the likely reason the leakage is this high.

Known limit: any other backlight writer (Plasma idle dimming) is taken as a
slider move by oriole-autobrightness.

## Package layout

- /usr/lib/oriole-sensors/oriole_usf.py, /usr/libexec/oriole-usf-setup,
  oriole-sensorproxy, oriole-autobrightness.
- Registry: /usr/share/oriole-sensors/reg-order lists the scripts; each is
  taken from /usr/share/firmware-google-oriole/sensors (factory image) or
  /run/oriole/persist/sensors/registry (this phone's factory calibration,
  read-only).
- oriole-usf and oriole-sensorproxy are enabled, oriole-autobrightness is
  opt-in. iio-sensor-proxy is kept from starting by a drop-in.

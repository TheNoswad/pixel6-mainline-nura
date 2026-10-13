# waydroid-sensors-usf: Android sensors for Waydroid on the Pixel 6

Waydroid's vendor image has a stub sensors HAL. When `waydroid-sensord` is
in `PATH`, Waydroid leaves that stub off (no `waydroid.stub_sensors_hal=1`)
and runs `waydroid-sensord /dev/hwbinder` from the host on every container
start. The daemon registers `android.hardware.sensors@1.0::ISensors/default`
on the container's hwbinder (libgbinder). Upstream `waydroid-sensors` does
the same with sensorfw as the backend. This one talks to the Pixel 6 AoC
sensor hub directly over USF (protocol: `local/oriole-sensors/README.md`).

- **Sensors:** accelerometer, gyroscope, magnetometer, light, proximity,
  pressure, and the AoC's own fusion outputs. Those are rotation vector,
  game rotation vector, geomagnetic rotation vector, orientation (azimuth/
  pitch/roll), gravity and linear acceleration. Compass and map apps use
  the rotation vector or orientation. Android doesn't run its own fusion
  for sensor types the HAL already provides.
- **Coexistence:** each open of `/dev/acd-com.google.usf` is its own AoC
  channel, so this runs next to oriole-sensorproxy. The USF registry must
  already be loaded (oriole-usf.service). Until the sensors show up, the
  daemon retries every 3 s and registers the HAL only after that, because
  Android reads the sensor list once.
- **Power:** a sensor samples only while Android has it activated and the
  display is on (DRM DSI dpms). The sampling period comes from `batch()`,
  clamped to the sensor's range from USF SENSOR_INFO.
- **poll():** `poll(0)` (SensorService's probe at connect) returns BAD_VALUE
  at once; otherwise answered asynchronously. The request is blocked until events
  are queued. The queue holds 512 events and drops the oldest while
  Android isn't polling (frozen container).
- **Timestamps:** the AoC clock is mapped to CLOCK_BOOTTIME. The offset is
  the smallest arrival-minus-sample time seen, re-estimated every 10 s.
- **Single instance:** /run/waydroid-sensord.pid holds a lock. A new
  instance stops the old one, because Waydroid's `kill -9 $(pidof ...)`
  on container stop can miss it.
- **Debug:** `waydroid-sensord -v /dev/hwbinder` or
  `WAYDROID_SENSORD_DEBUG=1` logs activations, rates and display changes.

Not supported: direct channels, data injection, step counter and gesture
sensors.

Install, then restart the container (`systemctl restart
waydroid-container`, never while it is frozen). Waydroid writes
waydroid.prop on start, so the stub HAL goes away by itself.
Check from Android: `dumpsys sensorservice`.

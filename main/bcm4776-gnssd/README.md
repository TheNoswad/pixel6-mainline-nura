# bcm4776-gnssd: GNSS for the Pixel 6 without Broadcom userspace

The Pixel 6 (oriole) has a Broadcom BCM4776 GNSS chip on SPI, driven by the
vendor `bbd` kernel driver (`/dev/ttyBCM`, `/dev/bbd_control`, `nstandby` in
sysfs). Android runs two closed daemons on top of it, `lhd` (boots the chip)
and `gpsd` (the location engine). This daemon replaces both. Their protocol
was reverse engineered from captures of the stock daemons and from
decompiling them.

The only Broadcom code involved is the chip firmware. The chip computes
the position itself (on-chip "micro-nav"); the daemon runs the session and
turns the chip's position reports into NMEA. There is no host position
engine and no assistance data.

## Use

- `bcm4776-gnssd.socket` listens on `/run/gnss-share.sock`, the socket
  path gnss-share uses, with mode 0660 and group `geoclue`. The service
  starts when the first client connects.
- geoclue reads the socket through `/etc/geoclue/conf.d/90-bcm4776-gnssd.conf`
  (`[network-nmea] nmea-socket=`), so any geoclue app gets GPS.
- The chip is powered only while at least one client is connected. When
  the last one leaves, the daemon waits 10 s, then stops the session and
  puts the chip in standby.
- Output: `$GPGGA` + `$GPRMC` + `$GPGST` at 1 Hz once there is a fix. A
  cold start takes about 30-65 s under open sky. GST carries the chip's
  horizontal error estimate, split evenly over latitude and longitude.
- Satellites: NMEA 4.11 `GSV` per constellation and signal (GPS L1 is
  signal 1 and L5 is signal 7, in separate groups) and `$GNGSA` per system
  with the satellites the chip used. Both are also sent before the first
  fix. Talkers: GP (GPS, SBAS as 33-64), GL (GLONASS 65-96), GA, GB, GQ, GI.
  Each epoch goes out in a single write.
- Waydroid: `bcm4776-gnssd-waydroid.socket` (subpackage
  `bcm4776-gnssd-waydroid`) adds a second listener,
  `/run/waydroid-gnss/nmea.sock`, mode 0660, group `waydroid-gps` (gid 1021, Android's `gps`).
  The patched waydroid binds it into the container, where
  the Waydroid GNSS HAL (android/gnss-hal) gives Android a `gps` provider. Both sockets get the
  same stream; the chip runs while either has a client.

Options: `-v` logs every fix (UTC, error estimate, satellites, HDOP; never
coordinates). `-t FILE` writes all chip I/O as JSONL. `-p`/`-g` set the
firmware files, `-s` the socket (when not socket activated), `-n` the
`nstandby` attribute.

## Firmware

The firmware is read from `/lib/firmware/brcm/bcm4776/`:

- `SensorHub.patch` (`/vendor/firmware/SensorHub.patch`, used as is)
- `gll-patch-4776_A0.bin` (the ESW patch embedded in `/vendor/bin/hw/gpsd`)

Both are Broadcom's and come from your own device's stock vendor partition.
`bcm4776-fwcut VENDOR_DIR OUTDIR` (subpackage `bcm4776-gnssd-fwcut`) copies
the first and cuts the second out of gpsd. Nothing else from gpsd is kept.

## Protocol

From the outside in:

1. **SSI frames** on `/dev/ttyBCM`:
   `b0 00 | seq | len | flags | fields | payload | crc | b0 01`.
   - `b0`, `11` and `13` are escaped as `b0 03/04/05`.
   - CRC-8, poly 0x4d, over len..payload, sent nibble-swapped.
   - Flag bit 0x02 marks a reliable frame, with a counter the chip acks
     (flags 0x05, ack fields 0x01/0x04).
2. **TLVs** in the payload: `id u8 | len u8` (bit 7 set: 15-bit length) `| value`.
   - Boot: `46` version, `05` memory write, `37` call, `3a` banner.
   - `1e` carries the host->chip MeIf stream and `1f` the chip->host one
     (u16 length + a piece of the stream, at most 1000 bytes each).
3. **MeIf messages** in that stream: `c0 c1 | id u16 | len u16 | csum u16 | data | seq u16 | c0 c4`.
   - `c0` is escaped as `c0 c0`.
   - csum = 16-bit sum of all bytes except the csum and the trailer.

Boot (in place of lhd):
1. Toggle `nstandby`, send the `0x80` autobaud bytes, then sync.
2. Write the SensorHub.patch records to chip memory and jump to their
   entry point.
3. Resync with the running patch.

ESW patch (in place of gpsd's patch loader):
1. MeIf 0x8002 passthrough messages carry chip ROM loader commands
   (`fe .. fd 4c` write, `fe 00 fd 4a` byte-sum query, `ff 00 fd 05` start).
2. If the byte sums of the first blocks still match, the chip held on to
   the patch and the upload is skipped.

Session (gpsd's on-chip start):
1. Request INIT, then wait for the READY report.
2. Send the configuration (0006, 0010, 0017), request STARTING and wait
   for it.
3. Send the GPS time from the system clock (8187), request info (818e),
   the micro-nav config (8183), RUNNING, batching (0009) and the
   measurement config.
4. Every second a `SendBatchMeas` flush (0009/4005) returns one 0x8101
   COMPACTMEAS message. Its bit-packed POSITION blob holds the position,
   altitude, speed, track, UTC, DOPs, error estimate and satellites.
   Each satellite signal takes 7 bytes:
   - `id`: bits 0-5 are the PRN/slot, bits 6-7 mean "used in the fix".
   - `flags`: bits 0-3 are the constellation (0 GPS, 1 SBAS, 2 GLONASS,
     3 QZSS, 5 BeiDou, 6 Galileo, 7 NavIC) and bit 4 means "elevation and
     azimuth valid".
   - elevation and azimuth: s16/64 degrees each.
   - `cn0`: bits 0-5 are C/N0 in dB-Hz, bits 6-7 the band (0x80 = L5).
   This layout comes from gpsd's UpdateGlFixStatusFromMicroNavFixStatus.
   Every satellite matched its predicted sky position to 0 deg.

The chip reports each state change (0004: previous u16, new u16). Commands
sent while a transition is still running are lost, and the chip then
resets its link. So the daemon waits for each report and leaves 5 ms
between session messages, as gpsd does.

## Limitations

- Every start is a cold start. Standby loses the ESW patch and the
  satellite data, and the daemon does not yet save the chip's NVRAM
  reports (81d0) to send them back next time.
- geoclue derives accuracy from HDOP buckets and ignores GST.

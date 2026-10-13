#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Patch Waydroid's hwcomposer.waydroid.so (lineage-20 MAINLINE vendor 20260927,
BuildId 8937ba8f2556cc3e0050372676e63c9d).

Bug: xdg_toplevel_handle_configure() calls do_hotplug() on every configure,
even when the size is unchanged. Plasma Mobile sends a configure on every
activation/focus change, so display 0 is re-hotplugged every 1-3 s:
 - SurfaceFlinger reconnects the display each time; the race kills it
   (SIGSEGV in onComposerHalVsync / hasDisplayCapability, assert in
   getColorModes), system_server dies, apps stop opening;
 - do_hotplug deletes and recreates the touch FIFO; InputFlinger churns the
   wayland_touch device and the HWC hits "Failed to open pipe to
   InputFlinger" -> touches dropped, Android looks hung.

Fix: hotplug only when the configured size differs from the last size that
was hotplugged. display->full_width/full_height (only used as a calibration
fallback, before this path can run) hold the last hotplugged size.
Code cave in the zero padding after .plt (same R+X page, mapped from file).

  patch-hwcomposer.py <in.so> <out.so>
"""
import sys, hashlib, struct

SHA_IN = "c5847fd6b1228afb803f36a0818d037ce65369e6b95e96d85666b67f2158463f"
HOOK = 0x42da4      # movi v0.2d,#0 in xdg_toplevel_handle_configure, after the
                    # "calibrated" checks, before the persist.* overrides
RET_SKIP = 0x42e44  # function epilogue (stack check + return)
CAVE = 0x6d370      # padding after .plt (.plt ends 0x6d360, page ends 0x6e000)

def b(frm, to):
    return struct.pack("<I", 0x14000000 | (((to - frm) >> 2) & 0x3ffffff))

def beq(frm, to):
    return struct.pack("<I", 0x54000000 | ((((to - frm) >> 2) & 0x7ffff) << 5))

cave = bytes.fromhex(
    "899242b9"   # ldr  w9, [x20, #0x290]   last hotplugged width (full_width)
    "3f01156b"   # cmp  w9, w21              configured width
    "899642b9"   # ldr  w9, [x20, #0x294]   last hotplugged height
    "2001537a")  # ccmp w9, w19, #0, eq      configured height
cave += beq(CAVE + len(cave), RET_SKIP)      # same size -> return, no hotplug
cave += bytes.fromhex(
    "959202b9"   # str  w21, [x20, #0x290]
    "939602b9"   # str  w19, [x20, #0x294]
    "00e4006f")  # movi v0.2d, #0           (displaced instruction)
cave += b(CAVE + len(cave), HOOK + 4)        # back to adrp x0, ...

src, dst = sys.argv[1], sys.argv[2]
d = bytearray(open(src, "rb").read())
assert hashlib.sha256(bytes(d)).hexdigest() == SHA_IN, "unexpected input file"
assert bytes(d[HOOK:HOOK + 4]) == bytes.fromhex("00e4006f")
assert not any(d[CAVE:CAVE + len(cave)]) and not any(d[0x6d360:0x6e000])
d[HOOK:HOOK + 4] = b(HOOK, CAVE)
d[CAVE:CAVE + len(cave)] = cave
open(dst, "wb").write(d)
print("patched", dst, hashlib.sha256(bytes(d)).hexdigest())

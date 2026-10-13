# aoc-voiced

Keeps the voice call PCMs of the Google AoC sound card running while
ModemManager has a call that needs audio (dialing, ringing out, active or
held).

On Google Tensor phones (gs101 and later) the modem's call audio is routed
inside the AoC audio DSP. The voice endpoints are hostless: no samples are
read or written, but the AoC only opens the phone call path while the
downlink playback PCM (EP5, device 4) and the uplink capture PCM (EP4,
device 11) are prepared and started. The Android audio HAL does this with
tinyalsa for the length of a call; aoc-voiced does it for ModemManager.

It does not touch the mixer. Routing (earpiece or speaker, microphones) is
the "Voice Call" verb of the card's ALSA UCM configuration, which callaudiod
switches to when a call starts. aoc-voiced plays the role q6voiced plays on
Qualcomm phones; q6voiced itself does not fit because it opens one device
number for both directions and does not start the streams.

    aoc-voiced [-c CARD] [-p DEV] [-C DEV] [-r RATE]

    -c, --card      ALSA card id (default googleaocsndcar)
    -p, --playback  voice playback (downlink) device (default 4)
    -C, --capture   voice capture (uplink) device (default 11)
    -r, --rate      sample rate (default 48000)

The systemd unit runs it as a dynamic user in the audio group. It needs
read access to ModemManager's call objects on the system bus, which
ModemManager's D-Bus policy allows for every user.

# PocketWave OS Handoff

## What exists

Two binaries in `~/miniwave/pocketchip/` on chip@chip.local (192.168.0.105, pw: waveloop):

- **miniwave** — synth engine. ALSA-only, no JACK. All 6 instrument types. Listens on OSC :9000, HTTP :8080.
- **pocketwave-ui** — terminal UI. Talks to miniwave over OSC. Renders ANSI in stterm with 24pt font.

Source is also at `/home/elijah/miniwave/pocketchip/` on the dev machine. Build with `make` on-device (GCC 4.9, ALSA dev headers already installed).

## Current launch sequence (fragile)

```bash
# 1. start synth engine
nohup ~/miniwave/pocketchip/miniwave -c 1 -o hw:2,0 -P 256 -m 20:0 > /tmp/miniwave.log 2>&1 &

# 2. wait for OSC port
sleep 2

# 3. launch UI in stterm, strip decorations, fullscreen
DISPLAY=:0 stterm -T pocketwave -f "DejaVu Sans Mono:size=24" -e ~/miniwave/pocketchip/pocketwave-ui &
sleep 1
WID=$(DISPLAY=:0 xdotool search --name pocketwave)
DISPLAY=:0 xprop -id $WID -f _MOTIF_WM_HINTS 32c -set _MOTIF_WM_HINTS "2, 0, 0, 0, 0"
DISPLAY=:0 xdotool windowmove $WID 0 0
DISPLAY=:0 xdotool windowsize $WID 480 272
```

The UI also has `--launch` which does `execlp("stterm", ... "-f", "monospace:size=24", "-e", self)` — simpler but still needs the xdotool fullscreen step after.

## What needs to happen

### 1. Config persistence

Create `~/.config/pocketwave/config` (shell-sourceable):

```bash
AUDIO_DEV=hw:2,0
BUFFER_SIZE=256
MIDI_DEV=20:0
PRE_CONFIG=1
```

The launch script sources this. The pocketwave-ui config menu already restarts miniwave with new args — it just needs to also write to this file.

### 2. Startup service

Either systemd or a simple init script. Must:
- Source config file
- Start miniwave with args from config
- Wait for OSC :9000 to respond (poll with timeout)
- Launch pocketwave-ui fullscreen
- Restart miniwave if it dies

### 3. Display approach — pick one

**Option A: Bare VT (no X)**
- Install `kbd` package for `setfont` and `chvt`
- Use largest console font available (16px max without custom fonts)
- Auto-login on tty1, launch pocketwave-ui directly
- Pros: no X overhead, simpler, faster boot
- Cons: smaller text (16px max), no fontconfig

**Option B: Minimal X (current)**
- Keep X running but strip the desktop
- Auto-start stterm fullscreen via .xinitrc or WM config
- Configure WM to auto-fullscreen windows named "pocketwave" (eliminates xdotool hack)
- Pros: 24pt font, smooth rendering
- Cons: X overhead (~10MB RAM)

Recommendation: Option B. The 24pt font matters on the 480x272 screen and X is already running.

### 4. USB hot-plug handling

The sun4i-codec (hw:0) always works. USB audio (iRig Stream = hw:2) and USB MIDI (Kontrol M32 = 20:0) can be unplugged.

When USB audio disconnects, miniwave's ALSA write loop gets stuck in D state (kernel-level, can't be killed). Need:

- udev rules that detect USB audio/MIDI disconnect
- On disconnect: `pkill -9 miniwave`, wait, restart with fallback to hw:0
- On reconnect: restart with the preferred USB device

Example udev rule:
```
ACTION=="remove", SUBSYSTEM=="sound", RUN+="/usr/local/bin/pocketwave-restart"
```

### 5. ALSA mixer setup

The sun4i-codec (hw:0) needs mixer routes enabled on boot:
```bash
amixer -c 0 set 'Left Mixer Left DAC' on
amixer -c 0 set 'Right Mixer Right DAC' on
amixer -c 0 set 'Power Amplifier' 80%
amixer -c 0 set 'Power Amplifier Mute' on
```

The iRig Stream (hw:2) works out of the box, volume via `amixer -c 2 set 'USB Streaming Playback Volume'`.

### 6. Install paths

Suggested:
```
/usr/local/bin/miniwave
/usr/local/bin/pocketwave-ui
/usr/local/share/miniwave/web/    (symlink to common/web for HTTP UI)
~/.config/pocketwave/config       (runtime config)
~/.config/miniwave/rack.json      (synth state, already used by miniwave)
```

### 7. PocketCHIP keyboard notes

- Enter/Return key does NOT send `\r` or `\n` through stterm — we use Tab (`0x09`) instead for "select/confirm"
- Arrow keys work normally (`\033[A/B/C/D`)
- ESC works (`0x1b`)
- The MIDI monitor mode in pocketwave-ui shows raw key hex codes — useful for mapping

### 8. Device specs

- Allwinner R8, ARM Cortex-A8 @ 1GHz, single core, NEON
- 512MB RAM (~400MB free)
- 1.9GB free storage
- Debian Jessie (armhf), GCC 4.9.2, kernel 4.3
- Screen: 480x272, 32bpp framebuffer
- Audio: sun4i-codec (built-in, period min ~1024), USB audio supported
- MIDI: ALSA sequencer, USB MIDI supported

### 9. Performance

- miniwave with 1 FM synth channel: ~3% CPU at period 256
- period 128 on USB audio: ~5% CPU, stable
- period 64 on USB audio: works but occasional underruns under load
- sun4i-codec forces period to ~1024 regardless of requested size (use `hw:0,0` not `default`)
- `default` ALSA device goes through dmix, adds massive latency — always use `hw:N,0`

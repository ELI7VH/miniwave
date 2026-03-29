#!/bin/bash
# PocketWave launch script — starts miniwave engine (+ optionally UI)
# Usage: pocketwave-launch              (engine + stterm UI)
#        pocketwave-launch --engine-only (engine only, xinitrc handles UI)
set -e

ENGINE_ONLY=0
[ "$1" = "--engine-only" ] && ENGINE_ONLY=1

CONFIG="$HOME/.config/pocketwave/config"
LOGDIR="/tmp"

# Source config (or defaults)
AUDIO_DEV="hw:2,0"
AUDIO_FALLBACK="hw:0,0"
BUFFER_SIZE=256
MIDI_DEV="20:0"
CHANNELS=1

if [ -f "$CONFIG" ]; then
    . "$CONFIG"
fi

# Probe audio device — fall back if USB not present
CARD_NUM=$(echo "$AUDIO_DEV" | sed 's/hw:\([0-9]*\).*/\1/')
if ! aplay -l 2>/dev/null | grep -q "card $CARD_NUM:"; then
    echo "pocketwave: $AUDIO_DEV not found, falling back to $AUDIO_FALLBACK"
    AUDIO_DEV="$AUDIO_FALLBACK"
fi

# ALSA mixer setup for sun4i-codec
if echo "$AUDIO_DEV" | grep -q "hw:0"; then
    /usr/local/bin/pocketwave-alsa-setup
fi

# Kill any existing miniwave
pkill -9 miniwave 2>/dev/null || true
sleep 0.5

# Start miniwave engine
echo "pocketwave: starting miniwave on $AUDIO_DEV (buffer $BUFFER_SIZE)"
/usr/local/bin/miniwave \
    -c "$CHANNELS" \
    -o "$AUDIO_DEV" \
    -P "$BUFFER_SIZE" \
    -m "$MIDI_DEV" \
    > "$LOGDIR/miniwave.log" 2>&1 &

MINIWAVE_PID=$!
echo $MINIWAVE_PID > /tmp/miniwave.pid

# Wait for OSC port 9000 (up to 5 seconds)
echo "pocketwave: waiting for miniwave OSC..."
for i in $(seq 1 50); do
    if netstat -uln 2>/dev/null | grep -q ':9000 '; then
        echo "pocketwave: miniwave ready (${i}00ms)"
        break
    fi
    sleep 0.1
done

if ! netstat -uln 2>/dev/null | grep -q ':9000 '; then
    echo "pocketwave: WARNING — miniwave OSC not responding after 5s"
fi

# Engine-only mode: just wait for miniwave to die
if [ "$ENGINE_ONLY" = "1" ]; then
    echo "pocketwave: engine started (engine-only mode)"
    wait $MINIWAVE_PID
    exit $?
fi

# Full mode: launch pocketwave-ui in stterm
echo "pocketwave: launching UI"
exec stterm -T pocketwave -f "DejaVu Sans Mono:size=24" \
    -e /usr/local/bin/pocketwave-ui

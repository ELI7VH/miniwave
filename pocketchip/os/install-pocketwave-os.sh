#!/bin/bash
# PocketWave OS Installer for PocketCHIP
# Run as root: sudo ./install-pocketwave-os.sh
set -e

echo "==============================="
echo "  PocketWave OS Installer"
echo "==============================="
echo ""

# Check we're root
if [ "$(id -u)" -ne 0 ]; then
    echo "ERROR: run with sudo"
    exit 1
fi

CHIP_HOME="/home/chip"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# ── 1. Build binaries ──────────────────────────────────
echo "[1/7] Building binaries..."
cd "$CHIP_HOME/miniwave/pocketchip"
make clean && make
echo "  miniwave: $(wc -c < miniwave) bytes"
echo "  pocketwave-ui: $(wc -c < pocketwave-ui) bytes"

# ── 2. Install binaries ────────────────────────────────
echo "[2/7] Installing binaries..."
install -m 755 miniwave      /usr/local/bin/miniwave
install -m 755 pocketwave-ui /usr/local/bin/pocketwave-ui
install -m 755 "$SCRIPT_DIR/pocketwave-launch.sh"   /usr/local/bin/pocketwave-launch
install -m 755 "$SCRIPT_DIR/pocketwave-restart"     /usr/local/bin/pocketwave-restart
install -m 755 "$SCRIPT_DIR/pocketwave-alsa-setup"  /usr/local/bin/pocketwave-alsa-setup

# Symlink web UI
mkdir -p /usr/local/share/miniwave
ln -sf "$CHIP_HOME/miniwave/common/web" /usr/local/share/miniwave/web

echo "  Binaries installed to /usr/local/bin/"

# ── 3. Config ──────────────────────────────────────────
echo "[3/7] Setting up config..."
mkdir -p "$CHIP_HOME/.config/pocketwave"
if [ ! -f "$CHIP_HOME/.config/pocketwave/config" ]; then
    install -m 644 -o chip -g chip "$SCRIPT_DIR/pocketwave.conf" \
        "$CHIP_HOME/.config/pocketwave/config"
    echo "  Default config written"
else
    echo "  Config already exists, keeping"
fi

# ── 4. X11 autostart ──────────────────────────────────
echo "[4/7] Configuring X11 autostart..."
install -m 755 -o chip -g chip "$SCRIPT_DIR/xinitrc" "$CHIP_HOME/.xinitrc"

# Install unclutter if missing (hides cursor)
if ! command -v unclutter >/dev/null 2>&1; then
    echo "  Installing unclutter..."
    apt-get install -y unclutter >/dev/null 2>&1 || echo "  WARNING: unclutter not available"
fi

# ── 5. Systemd services ───────────────────────────────
echo "[5/7] Installing systemd services..."
install -m 644 "$SCRIPT_DIR/pocketwave-x.service" /etc/systemd/system/
install -m 644 "$SCRIPT_DIR/pocketwave.service"   /etc/systemd/system/

# ── 6. USB hotplug rules ──────────────────────────────
echo "[6/7] Installing udev rules..."
install -m 644 "$SCRIPT_DIR/99-pocketwave-usb.rules" /etc/udev/rules.d/
udevadm control --reload-rules

# ── 7. Disable stock desktop, enable PocketWave ───────
echo "[7/7] Switching boot target..."

# Disable stock
systemctl disable lightdm.service    2>/dev/null || true
systemctl disable pocket-home.service 2>/dev/null || true
systemctl stop lightdm.service       2>/dev/null || true
systemctl stop pocket-home.service   2>/dev/null || true

# Enable PocketWave
systemctl daemon-reload
systemctl enable pocketwave-x.service
systemctl enable pocketwave.service

echo ""
echo "==============================="
echo "  PocketWave OS installed!"
echo "==============================="
echo ""
echo "  Binaries:  /usr/local/bin/miniwave, pocketwave-ui"
echo "  Config:    ~/.config/pocketwave/config"
echo "  Logs:      /tmp/miniwave.log"
echo ""
echo "  Reboot to start PocketWave OS:"
echo "    sudo reboot"
echo ""
echo "  To revert:"
echo "    sudo systemctl disable pocketwave pocketwave-x"
echo "    sudo systemctl enable lightdm"
echo "    sudo reboot"

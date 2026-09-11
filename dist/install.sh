#!/usr/bin/env bash
set -e

cd "$(dirname "$(readlink -f "$0")")/.."

# Dependency check
command -v meson >/dev/null && pkg-config --exists libevdev || {
    echo "Missing dependency: meson + libevdev (dev). Install them (e.g." >&2
    echo "'sudo pacman -S meson libevdev') and re-run." >&2
    exit 1
}

# Build
rm -rf build
meson setup build --buildtype=release
meson compile -C build

# Drop older versions' service + udev rule if present
systemctl --user disable --now wl-wheel-scroll.service 2>/dev/null || true
sudo rm -f /usr/lib/systemd/user/wl-wheel-scroll.service \
           /usr/lib/udev/rules.d/99-wl-wheel-scroll.rules \
           /usr/lib/udev/rules.d/99-uinput.rules

# Pre-installation
sudo modprobe uinput
echo uinput | sudo tee /etc/modules-load.d/uinput.conf >/dev/null

# Installation
sudo install -Dm755 build/wl-wheel-scroll /usr/bin/wl-wheel-scroll
sudo install -Dm644 misc/wl-wheel-scroll.service /usr/lib/systemd/system/wl-wheel-scroll.service

sudo systemctl daemon-reload
sudo systemctl enable --now wl-wheel-scroll.service

cat <<EOF
==> wl-wheel-scroll installed and started (system service, runs as root).

  Check it detected your touchpad with:
       journalctl -u wl-wheel-scroll.service -b
EOF

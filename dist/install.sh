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
systemctl --user disable --now wheel-scroll.service 2>/dev/null || true
sudo rm -f /usr/lib/systemd/user/wheel-scroll.service \
           /usr/lib/udev/rules.d/99-wheel-scroll.rules \
           /usr/lib/udev/rules.d/99-uinput.rules

# Pre-installation
sudo modprobe uinput
echo uinput | sudo tee /etc/modules-load.d/uinput.conf >/dev/null

# Installation
sudo install -Dm755 build/wheel-scroll /usr/bin/wheel-scroll
sudo install -Dm644 misc/wheel-scroll.service /usr/lib/systemd/system/wheel-scroll.service

sudo systemctl daemon-reload
sudo systemctl enable --now wheel-scroll.service

cat <<EOF
==> wheel-scroll installed and started (system service, runs as root).

  Check it detected your touchpad with:
       journalctl -u wheel-scroll.service -b
EOF

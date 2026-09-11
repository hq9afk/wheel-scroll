#!/usr/bin/env bash
set -e

sudo systemctl disable --now wl-wheel-scroll.service 2>/dev/null || true
# older versions shipped a per-user service
systemctl --user disable --now wl-wheel-scroll.service 2>/dev/null || true

sudo rm -f /usr/bin/wl-wheel-scroll
sudo rm -f /usr/lib/systemd/system/wl-wheel-scroll.service
sudo rm -f /usr/lib/systemd/user/wl-wheel-scroll.service
sudo rm -f /usr/lib/udev/rules.d/99-wl-wheel-scroll.rules
sudo rm -f /etc/modules-load.d/uinput.conf

sudo systemctl daemon-reload
sudo udevadm control --reload 2>/dev/null || true

echo "==> wl-wheel-scroll uninstalled."

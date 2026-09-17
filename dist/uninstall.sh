#!/usr/bin/env bash
set -e

sudo systemctl disable --now wheel-scroll.service 2>/dev/null || true
# older versions shipped a per-user service
systemctl --user disable --now wheel-scroll.service 2>/dev/null || true

sudo rm -f /usr/bin/wheel-scroll
sudo rm -f /usr/lib/systemd/system/wheel-scroll.service
sudo rm -f /usr/lib/systemd/user/wheel-scroll.service
sudo rm -f /usr/lib/udev/rules.d/99-wheel-scroll.rules
sudo rm -f /etc/modules-load.d/uinput.conf

sudo systemctl daemon-reload
sudo udevadm control --reload 2>/dev/null || true

echo "==> wheel-scroll uninstalled."

# wl-wheel-scroll

Circular rim scrolling daemon for Wayland touchpads. Move your finger around the edge of the pad to scroll.

Built for the Panasonic Let's Note wheel pad but works on any touchpad with a circular rim zone.

## Prerequisite

```sh
sudo pacman -S meson libevdev
```

## Install

```sh
git clone https://github.com/hq9afk/wl-wheel-scroll.git
cd wl-wheel-scroll
dist/install.sh
```

## Uninstall

```sh
./dist/uninstall.sh
```

## Important

The script handles rim scrolling itself. Disable your compositor's built-in touchpad scroll to avoid conflicts.

## Tuning

The five tunables are compile-time constants at the top of [`main.cpp`](main.cpp) - edit them and re-run `./dist/install.sh`.

| Constant | Default | Description |
|----------|---------|---------|
| `RIM_THRESHOLD` | `0.75` | outer fraction of pad radius that triggers rim scrolling (0–1) |
| `ANGLE_PER_TICK` | `0.15` | radians of rotation per scroll tick — smaller = more sensitive |
| `SCROLL_SPEED` | `1` | `REL_WHEEL` units per tick |
| `TOUCH_TIMEOUT` | `0.5` | seconds before state resets if touch is lost |
| `HORIZONTAL_SCROLL` | `false` | scroll horizontally instead of vertically |

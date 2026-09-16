// wl-wheel-scroll — circular rim scrolling daemon for Wayland touchpads.
// Move your finger around the edge of the pad to scroll.

#include <libevdev/libevdev-uinput.h>
#include <libevdev/libevdev.h>

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace {

// Parameters

constexpr double RIM_THRESHOLD = 0.75;    // Outer fraction of pad radius that triggers rim scrolling (0-1).
constexpr double ANGLE_PER_TICK = 0.15;   // Radians of rotation per scroll tick — smaller = more sensitive.
constexpr int SCROLL_SPEED = 1;           // REL_WHEEL units per tick.
constexpr double TOUCH_TIMEOUT = 0.5;     // Seconds before state resets if touch is lost.
constexpr bool HORIZONTAL_SCROLL = false; // True to scroll horizontally instead of vertically.

constexpr const char *TOUCHPAD_KEYWORDS[] = {"synaptics"};
constexpr double PI = 3.14159265358979323846;
// How often the read loop wakes up with no data, to notice a signal or an idle touch_timeout. Just responsiveness, not precision.
constexpr int POLL_IDLE_MS = 100;

using Clock = std::chrono::steady_clock;

volatile sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

// Signed shortest angular distance in [-pi, pi].
double angle_diff(double a, double b) {
    double d = b - a;
    while (d > PI)
        d -= 2 * PI;
    while (d < -PI)
        d += 2 * PI;
    return d;
}

bool is_pos_code(unsigned int code) {
    return code == ABS_X || code == ABS_Y || code == ABS_MT_POSITION_X ||
           code == ABS_MT_POSITION_Y;
}

std::string lowercase(std::string s) {
    for (char &c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Scan /dev/input for a touchpad: has EV_ABS + ABS_X/ABS_MT_POSITION_X, and a name matching TOUCHPAD_KEYWORDS or containing "touchpad".
std::optional<std::string> find_touchpad() {
    DIR *dir = opendir("/dev/input");
    if (!dir)
        return std::nullopt;

    std::vector<std::string> names;
    for (struct dirent *entry; (entry = readdir(dir)) != nullptr;) {
        if (std::strncmp(entry->d_name, "event", 5) == 0)
            names.emplace_back(entry->d_name);
    }
    closedir(dir);
    std::sort(names.begin(), names.end());

    for (const auto &name : names) {
        std::string path = "/dev/input/" + name;
        int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0)
            continue;

        struct libevdev *dev = nullptr;
        if (libevdev_new_from_fd(fd, &dev) < 0) {
            close(fd);
            continue;
        }

        std::string dev_name = lowercase(libevdev_get_name(dev) ? libevdev_get_name(dev) : "");
        bool has_abs = libevdev_has_event_type(dev, EV_ABS);
        bool has_touch_axes = libevdev_has_event_code(dev, EV_ABS, ABS_X) || libevdev_has_event_code(dev, EV_ABS, ABS_MT_POSITION_X);
        bool name_match = false;
        for (const char *kw : TOUCHPAD_KEYWORDS) {
            if (dev_name.find(kw) != std::string::npos) {
                name_match = true;
                break;
            }
        }
        bool matched = has_abs && has_touch_axes && (name_match || dev_name.find("touchpad") != std::string::npos);

        libevdev_free(dev);
        close(fd);
        if (matched)
            return path;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Rim scroll state machine - one SYN_REPORT batch of events at a time.
// ---------------------------------------------------------------------------

struct State {
    std::vector<input_event> pending;
    bool tracking = false;
    bool in_rim = false;
    std::optional<double> prev_angle;
    double accum = 0.0;
    Clock::time_point last_event_t = Clock::now();
    double cur_x = 0.0;
    double cur_y = 0.0;
};

void reset(State &s) {
    s.tracking = false;
    s.in_rim = false;
    s.prev_angle.reset();
    s.accum = 0.0;
}

bool flush_scroll(struct libevdev_uinput *scroll_ui, double &accum) {
    int ticks = static_cast<int>(accum / ANGLE_PER_TICK); // truncates toward zero
    if (ticks == 0)
        return true;
    accum -= ticks * ANGLE_PER_TICK;
    unsigned int axis = HORIZONTAL_SCROLL ? REL_HWHEEL : REL_WHEEL;
    unsigned int axis_hi = HORIZONTAL_SCROLL ? REL_HWHEEL_HI_RES : REL_WHEEL_HI_RES;
    int rc = 0;
    rc |= libevdev_uinput_write_event(scroll_ui, EV_REL, axis, ticks * SCROLL_SPEED);
    rc |= libevdev_uinput_write_event(scroll_ui, EV_REL, axis_hi, ticks * SCROLL_SPEED * 120);
    rc |= libevdev_uinput_write_event(scroll_ui, EV_SYN, SYN_REPORT, 0);
    return rc == 0;
}

bool forward_pending(struct libevdev_uinput *tp_ui, State &s) {
    int rc = 0;
    for (const auto &e : s.pending)
        rc |= libevdev_uinput_write_event(tp_ui, e.type, e.code, e.value);
    rc |= libevdev_uinput_write_event(tp_ui, EV_SYN, SYN_REPORT, 0);
    s.pending.clear();
    return rc == 0;
}

bool process_event(const input_event &ev, State &s, double cx, double cy, double rim_r, struct libevdev_uinput *tp_ui, struct libevdev_uinput *scroll_ui) {
    auto now = Clock::now();
    if (s.tracking && std::chrono::duration<double>(now - s.last_event_t).count() > TOUCH_TIMEOUT) {
        reset(s);
    }

    if (ev.type == EV_ABS) {
        s.last_event_t = now;
        if (ev.code == ABS_X || ev.code == ABS_MT_POSITION_X) {
            s.cur_x = ev.value;
        } else if (ev.code == ABS_Y || ev.code == ABS_MT_POSITION_Y) {
            s.cur_y = ev.value;
        }
        s.pending.push_back(ev);
        return true;
    }

    if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
        bool ok = true;
        if (ev.value == 1) {
            s.tracking = true;
            s.last_event_t = now;
            s.prev_angle.reset();
            s.accum = 0.0;
            s.in_rim = false;
        } else {
            ok = flush_scroll(scroll_ui, s.accum);
            reset(s);
        }
        s.pending.push_back(ev);
        return ok;
    }

    if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
        if (!s.tracking)
            return forward_pending(tp_ui, s);

        double dx = s.cur_x - cx;
        double dy = s.cur_y - cy;
        double r = std::hypot(dx, dy);

        if (r >= rim_r) {
            double angle = std::atan2(dy, dx);
            bool ok = true;
            if (!s.in_rim) {
                s.prev_angle = angle;
                s.in_rim = true;
            } else {
                if (s.prev_angle) {
                    s.accum -= angle_diff(*s.prev_angle, angle);
                    ok = flush_scroll(scroll_ui, s.accum);
                }
                s.prev_angle = angle;
            }
            // drop position coords but keep slot / tracking-ID / pressure so libinput's multitouch state stays intact
            std::vector<input_event> passthrough;
            for (const auto &e : s.pending) {
                if (!(e.type == EV_ABS && is_pos_code(e.code)))
                    passthrough.push_back(e);
            }
            s.pending.clear();
            if (!passthrough.empty()) {
                int rc = 0;
                for (const auto &e : passthrough) {
                    rc |= libevdev_uinput_write_event(tp_ui, e.type, e.code, e.value);
                }
                rc |= libevdev_uinput_write_event(tp_ui, EV_SYN, SYN_REPORT, 0);
                ok = ok && rc == 0;
            }
            return ok;
        }

        bool ok = true;
        if (s.in_rim) {
            ok = flush_scroll(scroll_ui, s.accum);
            s.prev_angle.reset();
            s.accum = 0.0;
        }
        s.in_rim = false;
        return forward_pending(tp_ui, s) && ok;
    }

    s.pending.push_back(ev);
    return true;
}

// Reads the next event, transparently draining a SYN_DROPPED resync so the caller only ever sees a plain stream of events.
bool next_event(struct libevdev *dev, input_event &ev, bool &resyncing) {
    for (;;) {
        unsigned int flags = resyncing ? LIBEVDEV_READ_FLAG_SYNC : LIBEVDEV_READ_FLAG_NORMAL;
        int rc = libevdev_next_event(dev, flags, &ev);
        if (rc == LIBEVDEV_READ_STATUS_SUCCESS)
            return true;
        if (rc == LIBEVDEV_READ_STATUS_SYNC) {
            resyncing = true;
            return true;
        }
        if (rc == -EAGAIN) {
            if (resyncing) {
                resyncing = false;
                return false; // resync drained; caller polls again for real data
            }
            return false;
        }
        if (rc == -EINTR) {
            if (g_stop)
                return false;
            continue;
        }
        fprintf(stderr, "libevdev_next_event: %s\n", strerror(-rc));
        return false;
    }
}

int run(struct libevdev *dev, const std::string &path) {
    const struct input_absinfo *xi = libevdev_get_abs_info(dev, ABS_X);
    if (!xi)
        xi = libevdev_get_abs_info(dev, ABS_MT_POSITION_X);
    const struct input_absinfo *yi = libevdev_get_abs_info(dev, ABS_Y);
    if (!yi)
        yi = libevdev_get_abs_info(dev, ABS_MT_POSITION_Y);
    if (!xi || !yi) {
        fprintf(stderr, "ERROR: Could not read touchpad axis ranges.\n");
        return 1;
    }

    double x_min = xi->minimum, x_max = xi->maximum;
    double y_min = yi->minimum, y_max = yi->maximum;
    double cx = (x_min + x_max) / 2.0;
    double cy = (y_min + y_max) / 2.0;
    // smaller axis keeps the rim zone circular, not elliptical
    double r_max = std::min((x_max - x_min) / 2.0, (y_max - y_min) / 2.0);
    double rim_r = RIM_THRESHOLD * r_max;

    printf("Touchpad: %s (%s)\n", libevdev_get_name(dev), path.c_str());
    printf("  X range: %.0f\xe2\x80\x93%.0f  Y range: %.0f\xe2\x80\x93%.0f\n", x_min, x_max, y_min, y_max);
    printf("  Centre: (%.0f, %.0f)\n", cx, cy);

    // Virtual touchpad clone: same name change + all capabilities of `dev`.
    // Input properties are what make libinput classify this as a touchpad.
    libevdev_set_name(dev, "wl-wheel-scroll-tp");
    struct libevdev_uinput *tp_ui = nullptr;
    if (libevdev_uinput_create_from_device(dev, LIBEVDEV_UINPUT_OPEN_MANAGED, &tp_ui) < 0) {
        fprintf(stderr, "ERROR: could not create virtual touchpad.\n");
        return 1;
    }

    // Separate node so libinput doesn't ignore REL_WHEEL coming from a touchpad.
    struct libevdev *scroll_dev = libevdev_new();
    libevdev_set_name(scroll_dev, "wl-wheel-scroll");
    libevdev_enable_event_type(scroll_dev, EV_REL);
    libevdev_enable_event_code(scroll_dev, EV_REL, REL_WHEEL, nullptr);
    libevdev_enable_event_code(scroll_dev, EV_REL, REL_WHEEL_HI_RES, nullptr);
    libevdev_enable_event_code(scroll_dev, EV_REL, REL_HWHEEL, nullptr);
    libevdev_enable_event_code(scroll_dev, EV_REL, REL_HWHEEL_HI_RES, nullptr);
    struct libevdev_uinput *scroll_ui = nullptr;
    int rc = libevdev_uinput_create_from_device(scroll_dev, LIBEVDEV_UINPUT_OPEN_MANAGED, &scroll_ui);
    libevdev_free(scroll_dev);
    if (rc < 0) {
        fprintf(stderr, "ERROR: could not create virtual scroll device.\n");
        libevdev_uinput_destroy(tp_ui);
        return 1;
    }

    printf("  Touchpad virt: %s\n", libevdev_uinput_get_devnode(tp_ui));
    printf("  Scroll virt:   %s\n", libevdev_uinput_get_devnode(scroll_ui));
    printf("Running \xe2\x80\x94 Ctrl-C to stop.\n\n");
    fflush(stdout);

    State state;
    state.cur_x = cx;
    state.cur_y = cy;

    int fd = libevdev_get_fd(dev);
    bool resyncing = false;
    bool ok = true;
    while (!g_stop && ok) {
        struct pollfd pfd{fd, POLLIN, 0};
        if (poll(&pfd, 1, POLL_IDLE_MS) <= 0)
            continue;
        if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) {
            fprintf(stderr, "Touchpad device disappeared.\n");
            ok = false;
            break;
        }

        input_event ev;
        while (!g_stop && next_event(dev, ev, resyncing)) {
            ok = process_event(ev, state, cx, cy, rim_r, tp_ui, scroll_ui);
            if (!ok)
                break;
        }
    }

    libevdev_uinput_destroy(tp_ui);
    libevdev_uinput_destroy(scroll_ui);
    return ok ? 0 : 1;
}

} // namespace

int main(int argc, char **argv) {
    std::string path;
    if (argc > 1) {
        path = argv[1];
    } else if (auto found = find_touchpad()) {
        path = *found;
    } else {
        fprintf(stderr, "Could not auto-detect touchpad.\nRun:  libinput list-devices\nThen: wl-wheel-scroll /dev/input/eventX\n");
        return 1;
    }

    int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "Cannot open %s: %s\n", path.c_str(), strerror(errno));
        return 1;
    }

    struct libevdev *dev = nullptr;
    if (libevdev_new_from_fd(fd, &dev) < 0) {
        fprintf(stderr, "Cannot init libevdev on %s\n", path.c_str());
        close(fd);
        return 1;
    }

    if (libevdev_grab(dev, LIBEVDEV_GRAB) < 0) {
        // exclusive — libinput sees only the virtual devices
        fprintf(stderr, "Cannot grab %s (already grabbed?)\n", path.c_str());
        libevdev_free(dev);
        close(fd);
        return 1;
    }

    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    int result = run(dev, path);

    libevdev_grab(dev, LIBEVDEV_UNGRAB);
    libevdev_free(dev);
    close(fd);
    printf("\nShutting down.\n");
    return result;
}

/*
 * SPDX-FileCopyrightText: Neo
 * SPDX-License-Identifier: Apache-2.0
 *
 * neo_triggerd: maps the ingres shoulder triggers to screen touches.
 *
 * The xm_gamekey driver reports the triggers as KEY_F1 (left) / KEY_F2 (right),
 * held for as long as the trigger is pressed, and the pop-up hall sensors as
 * one-shot KEY_F3/KEY_F4 (left up/down) and KEY_F5/KEY_F6 (right up/down).
 *
 * Android delivers one touch stream per window: a touch from a second device
 * would cancel the finger already on the screen. So while a trigger is popped
 * up and mapping is enabled, this daemon grabs the real touchscreen ("fts")
 * and re-emits it on one virtual touchscreen with two extra multitouch slots,
 * one per trigger. A trigger press puts a finger down at the configured point
 * and keeps it down until the trigger is released (tap = tap, hold = hold).
 * When both triggers are retracted the grab is released and the virtual device
 * removed, so normal use has no extra hop. If the daemon dies the kernel drops
 * the grab and the real touchscreen works as before.
 *
 * Config (set by NeoParts):
 *   persist.sys.neo.trigger.enable  1 | 0
 *   persist.sys.neo.trigger.left    "x,y" in 1/10000 of the panel, natural
 *   persist.sys.neo.trigger.right   orientation (empty = trigger unmapped)
 */

#define LOG_TAG "neo_triggerd"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <string>

#include <android-base/properties.h>
#include <log/log.h>

namespace {

constexpr const char* kTouchName = "fts";
constexpr const char* kKeyName = "xm_gamekey";
constexpr const char* kHallDir = "/sys/devices/platform/soc/soc:gamekey";
constexpr const char* kVirtName = "neo_trigger_touch";

constexpr int kLeft = 0, kRight = 1;

struct Point {
    bool set = false;
    int x = 0, y = 0;  // 0..10000, natural panel orientation
};

struct Trigger {
    Point point;
    bool popped = false;   // hall sensor: trigger is up
    bool pressed = false;  // key currently held
    bool down = false;     // virtual finger currently down
    bool pending = false;  // state change waiting for a frame boundary
    int slot = 0;
};

struct State {
    bool enabled = false;
    Trigger trig[2];

    int touch_fd = -1, key_fd = -1, uinput_fd = -1;
    bool grabbed = false;

    input_absinfo ax{}, ay{}, amajor{}, aslot{}, atrack{};
    bool has_major = false;

    // real touchscreen stream tracking
    int cur_slot = 0;      // last ABS_MT_SLOT seen from the real device
    bool real_touch = false;
    bool in_frame = false; // forwarded events since the last SYN_REPORT
    bool sent_touch = false;
    int next_tracking_id = 60000;
};

State g;

int open_by_name(const char* want) {
    DIR* d = opendir("/dev/input");
    if (!d) return -1;
    int found = -1;
    while (dirent* e = readdir(d)) {
        if (strncmp(e->d_name, "event", 5)) continue;
        std::string path = std::string("/dev/input/") + e->d_name;
        int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        char name[128] = {};
        if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) >= 0 && !strcmp(name, want)) {
            found = fd;
            break;
        }
        close(fd);
    }
    closedir(d);
    return found;
}

bool read_hall(const char* node) {
    std::string path = std::string(kHallDir) + "/" + node;
    FILE* f = fopen(path.c_str(), "re");
    if (!f) return true;  // unknown: assume popped so the trigger works
    int v = 1;
    if (fscanf(f, "%d", &v) != 1) v = 1;
    fclose(f);
    return v != 0;
}

Point parse_point(const std::string& s) {
    Point p;
    int x, y;
    if (sscanf(s.c_str(), "%d,%d", &x, &y) == 2 && x >= 0 && x <= 10000 && y >= 0 && y <= 10000) {
        p.set = true;
        p.x = x;
        p.y = y;
    }
    return p;
}

void load_config() {
    using android::base::GetBoolProperty;
    using android::base::GetProperty;
    g.enabled = GetBoolProperty("persist.sys.neo.trigger.enable", false);
    g.trig[kLeft].point = parse_point(GetProperty("persist.sys.neo.trigger.left", ""));
    g.trig[kRight].point = parse_point(GetProperty("persist.sys.neo.trigger.right", ""));
    ALOGI("config: enabled=%d left=%d(%d,%d) right=%d(%d,%d)", g.enabled,
          g.trig[kLeft].point.set, g.trig[kLeft].point.x, g.trig[kLeft].point.y,
          g.trig[kRight].point.set, g.trig[kRight].point.x, g.trig[kRight].point.y);
}

void emit(int type, int code, int value) {
    input_event ev{};
    ev.type = type;
    ev.code = code;
    ev.value = value;
    if (write(g.uinput_fd, &ev, sizeof(ev)) < 0) ALOGE("uinput write: %s", strerror(errno));
}

void abs_setup(int fd, int code, const input_absinfo& info) {
    uinput_abs_setup s{};
    s.code = code;
    s.absinfo = info;
    ioctl(fd, UI_ABS_SETUP, &s);
}

bool create_virtual() {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        ALOGE("open /dev/uinput: %s", strerror(errno));
        return false;
    }
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);

    // mirror every key the real touchscreen can send (gestures, BTN_TOUCH...)
    unsigned long keybits[KEY_MAX / (8 * sizeof(long)) + 1] = {};
    ioctl(g.touch_fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits);
    for (int k = 0; k <= KEY_MAX; k++)
        if (keybits[k / (8 * sizeof(long))] & (1UL << (k % (8 * sizeof(long)))))
            ioctl(fd, UI_SET_KEYBIT, k);
    ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH);

    input_absinfo slot = g.aslot;
    slot.maximum = g.aslot.maximum + 2;  // two extra slots for the triggers
    g.trig[kLeft].slot = g.aslot.maximum + 1;
    g.trig[kRight].slot = g.aslot.maximum + 2;

    ioctl(fd, UI_SET_ABSBIT, ABS_MT_SLOT);
    ioctl(fd, UI_SET_ABSBIT, ABS_MT_TRACKING_ID);
    ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_X);
    ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_Y);
    abs_setup(fd, ABS_MT_SLOT, slot);
    abs_setup(fd, ABS_MT_TRACKING_ID, g.atrack);
    abs_setup(fd, ABS_MT_POSITION_X, g.ax);
    abs_setup(fd, ABS_MT_POSITION_Y, g.ay);
    if (g.has_major) {
        ioctl(fd, UI_SET_ABSBIT, ABS_MT_TOUCH_MAJOR);
        abs_setup(fd, ABS_MT_TOUCH_MAJOR, g.amajor);
    }

    uinput_setup us{};
    strncpy(us.name, kVirtName, sizeof(us.name) - 1);
    us.id.bustype = BUS_VIRTUAL;
    us.id.vendor = 0x2717;  // Xiaomi
    us.id.product = 0x4e54; // "NT"
    us.id.version = 1;
    if (ioctl(fd, UI_DEV_SETUP, &us) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
        ALOGE("uinput create: %s", strerror(errno));
        close(fd);
        return false;
    }
    g.uinput_fd = fd;
    return true;
}

bool any_virtual_down() {
    return g.trig[kLeft].down || g.trig[kRight].down;
}

void sync_touch_key() {
    bool want = g.real_touch || any_virtual_down();
    if (want != g.sent_touch) {
        emit(EV_KEY, BTN_TOUCH, want);
        g.sent_touch = want;
    }
}

int to_abs(const input_absinfo& a, int v) {
    return a.minimum + (int)((long long)v * (a.maximum - a.minimum) / 10000);
}

// Emit pending trigger changes as their own frame. Only called between frames
// of the real touchscreen, so slot selection stays consistent.
void flush_triggers() {
    bool any = false;
    for (Trigger& t : g.trig) {
        if (!t.pending) continue;
        t.pending = false;
        bool want = t.pressed && t.popped && t.point.set;
        if (want == t.down) continue;
        emit(EV_ABS, ABS_MT_SLOT, t.slot);
        if (want) {
            emit(EV_ABS, ABS_MT_TRACKING_ID, g.next_tracking_id);
            g.next_tracking_id = g.next_tracking_id >= 65000 ? 60000 : g.next_tracking_id + 1;
            emit(EV_ABS, ABS_MT_POSITION_X, to_abs(g.ax, t.point.x));
            emit(EV_ABS, ABS_MT_POSITION_Y, to_abs(g.ay, t.point.y));
            if (g.has_major) emit(EV_ABS, ABS_MT_TOUCH_MAJOR, (g.amajor.maximum - g.amajor.minimum) / 80);
        } else {
            emit(EV_ABS, ABS_MT_TRACKING_ID, -1);
        }
        t.down = want;
        any = true;
    }
    if (!any) return;
    emit(EV_ABS, ABS_MT_SLOT, g.cur_slot);  // the real device may omit ABS_MT_SLOT next
    sync_touch_key();
    emit(EV_SYN, SYN_REPORT, 0);
}

void release_all_virtual() {
    for (Trigger& t : g.trig) {
        if (t.down) {
            t.pressed = false;
            t.pending = true;
        }
    }
    flush_triggers();
}

bool want_active() {
    if (!g.enabled) return false;
    for (const Trigger& t : g.trig)
        if (t.popped && t.point.set) return true;
    return false;
}

void activate() {
    if (g.grabbed) return;
    if (!create_virtual()) return;
    if (ioctl(g.touch_fd, EVIOCGRAB, 1) < 0) {
        ALOGE("EVIOCGRAB: %s", strerror(errno));
        ioctl(g.uinput_fd, UI_DEV_DESTROY);
        close(g.uinput_fd);
        g.uinput_fd = -1;
        return;
    }
    g.grabbed = true;
    g.sent_touch = false;
    g.in_frame = false;
    ALOGI("active: touchscreen grabbed, virtual device created");
}

void deactivate() {
    if (!g.grabbed) return;
    release_all_virtual();
    ioctl(g.touch_fd, EVIOCGRAB, 0);
    ioctl(g.uinput_fd, UI_DEV_DESTROY);
    close(g.uinput_fd);
    g.uinput_fd = -1;
    g.grabbed = false;
    ALOGI("inactive: touchscreen released");
}

// Switch the grab only while no real finger is down, so Android never sees a
// gesture start on one device and end on another.
void update_activation() {
    if (g.real_touch) return;
    if (want_active()) activate();
    else deactivate();
}

void handle_touch_event(const input_event& ev) {
    if (ev.type == EV_ABS && ev.code == ABS_MT_SLOT) g.cur_slot = ev.value;
    if (ev.type == EV_KEY && ev.code == BTN_TOUCH) g.real_touch = ev.value != 0;

    if (!g.grabbed) {
        if (ev.type == EV_SYN && ev.code == SYN_REPORT && !g.real_touch) update_activation();
        return;
    }

    if (ev.type == EV_KEY && ev.code == BTN_TOUCH) return;  // merged in sync_touch_key()
    if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
        sync_touch_key();
        emit(EV_SYN, SYN_REPORT, 0);
        g.in_frame = false;
        flush_triggers();
        if (!g.real_touch && !any_virtual_down()) update_activation();
        return;
    }
    emit(ev.type, ev.code, ev.value);
    g.in_frame = true;
}

void handle_key_event(const input_event& ev) {
    if (ev.type != EV_KEY) return;
    int side = -1;
    switch (ev.code) {
        case KEY_F1: side = kLeft; break;
        case KEY_F2: side = kRight; break;
        case KEY_F3: if (ev.value) g.trig[kLeft].popped = true; break;
        case KEY_F4: if (ev.value) g.trig[kLeft].popped = false; break;
        case KEY_F5: if (ev.value) g.trig[kRight].popped = true; break;
        case KEY_F6: if (ev.value) g.trig[kRight].popped = false; break;
        default: return;
    }
    if (side >= 0) {
        Trigger& t = g.trig[side];
        // Popping the left trigger produces a short stray F1 press before F3;
        // presses while retracted are ignored, which drops it.
        if (ev.value == 2) return;  // autorepeat
        t.pressed = ev.value != 0 && t.popped;
        t.pending = true;
    } else {
        // a trigger went down: lift its finger if it was held
        for (Trigger& t : g.trig)
            if (!t.popped && t.down) {
                t.pressed = false;
                t.pending = true;
            }
    }
    if (g.grabbed && !g.in_frame) flush_triggers();
    if (!g.real_touch && !any_virtual_down()) update_activation();
}

bool init_devices() {
    g.touch_fd = open_by_name(kTouchName);
    g.key_fd = open_by_name(kKeyName);
    if (g.touch_fd < 0 || g.key_fd < 0) {
        ALOGE("input devices not found (touch=%d key=%d)", g.touch_fd, g.key_fd);
        return false;
    }
    if (ioctl(g.touch_fd, EVIOCGABS(ABS_MT_POSITION_X), &g.ax) < 0 ||
        ioctl(g.touch_fd, EVIOCGABS(ABS_MT_POSITION_Y), &g.ay) < 0 ||
        ioctl(g.touch_fd, EVIOCGABS(ABS_MT_SLOT), &g.aslot) < 0 ||
        ioctl(g.touch_fd, EVIOCGABS(ABS_MT_TRACKING_ID), &g.atrack) < 0) {
        ALOGE("touchscreen abs info: %s", strerror(errno));
        return false;
    }
    g.has_major = ioctl(g.touch_fd, EVIOCGABS(ABS_MT_TOUCH_MAJOR), &g.amajor) >= 0;
    g.cur_slot = g.aslot.value;

    unsigned long keys[KEY_MAX / (8 * sizeof(long)) + 1] = {};
    ioctl(g.touch_fd, EVIOCGKEY(sizeof(keys)), keys);
    g.real_touch = keys[BTN_TOUCH / (8 * sizeof(long))] & (1UL << (BTN_TOUCH % (8 * sizeof(long))));

    g.trig[kLeft].popped = read_hall("hall_left");
    g.trig[kRight].popped = read_hall("hall_right");
    ALOGI("x %d..%d y %d..%d slots %d, popped L=%d R=%d", g.ax.minimum, g.ax.maximum,
          g.ay.minimum, g.ay.maximum, g.aslot.maximum, g.trig[kLeft].popped, g.trig[kRight].popped);
    return true;
}

}  // namespace

int main() {
    if (!init_devices()) return 1;
    load_config();
    unsigned serial = __system_property_area_serial();
    update_activation();

    pollfd fds[2] = {{g.touch_fd, POLLIN, 0}, {g.key_fd, POLLIN, 0}};
    input_event evs[64];
    for (;;) {
        int r = poll(fds, 2, 1000);
        if (r < 0 && errno != EINTR) {
            ALOGE("poll: %s", strerror(errno));
            break;
        }
        for (int i = 0; i < 2; i++) {
            if (!(fds[i].revents & POLLIN)) continue;
            ssize_t n = read(fds[i].fd, evs, sizeof(evs));
            if (n <= 0) continue;
            for (ssize_t k = 0; k < n / (ssize_t)sizeof(input_event); k++) {
                if (i == 0) handle_touch_event(evs[k]);
                else handle_key_event(evs[k]);
            }
        }
        unsigned now = __system_property_area_serial();
        if (now != serial) {
            serial = now;
            bool was = g.enabled;
            Point l = g.trig[kLeft].point, rp = g.trig[kRight].point;
            load_config();
            if (was != g.enabled || l.set != g.trig[kLeft].point.set ||
                rp.set != g.trig[kRight].point.set) {
                for (Trigger& t : g.trig) t.pending = true;
                if (g.grabbed && !g.in_frame) flush_triggers();
                if (!g.real_touch && !any_virtual_down()) update_activation();
            }
        }
    }
    deactivate();
    return 1;
}

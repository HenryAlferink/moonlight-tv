/* Experimental webOS physical keyboard capture for streaming.
 * Grabs real Bluetooth/USB keyboards to keep OS shortcuts (Alt+F4,
 * Ctrl+Alt+Delete, Meta) away from the TV compositor, forwarding keys to
 * the Windows host. Magic Remote stays on SDL. Hotplug works mid-stream. */
#include "keyboard_meta.h"
#include "logging.h"
#include "stream/input/vk.h"
#include <Limelight.h>
#include <SDL.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <unistd.h>

#define META_MAX_DEVICES 8
#define META_SCAN_INTERVAL_US 500000

struct meta_device_t {
    int fd;
    char path[128];
    bool pressed[KEY_MAX + 1];
};

struct keyboard_meta_t {
    pthread_t thread;
    atomic_bool running;
    atomic_int active_count;
    atomic_int grab_attempts;
    atomic_int grab_failures;
    atomic_int last_grab_errno;
    atomic_int key_events;
    keyboard_meta_event_fn callback;
    void *userdata;
    struct meta_device_t devices[META_MAX_DEVICES];
    int count; /* reader thread only */
    bool ctrl, alt, shift, meta; /* reader thread only */
};

static bool has_bit(const unsigned char *bits, unsigned bit) {
    return (bits[bit / 8] & (1u << (bit % 8))) != 0;
}

static bool is_keyboard(int fd) {
    char name[256] = {0};
    if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0) return false;
    if (strstr(name, "LGE") || strstr(name, "M-RCU") ||
        strstr(name, "Builtin") || strstr(name, "gpio")) return false;
    unsigned char keys[(KEY_MAX / 8) + 1] = {0};
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) < 0) return false;
    /* Some Bluetooth keyboards split letter and modifier capabilities across
     * separate evdev nodes. Follow the proven webOS HID keyboard probe. */
    return has_bit(keys, KEY_A) || has_bit(keys, KEY_LEFTCTRL);
}

static bool already_open(const keyboard_meta_t *w, const char *path) {
    for (int i = 0; i < w->count; ++i)
        if (strcmp(w->devices[i].path, path) == 0) return true;
    return false;
}

static void remove_device(keyboard_meta_t *w, int index);

static void remove_stale_devices(keyboard_meta_t *w) {
    for (int i = w->count - 1; i >= 0; --i) {
        struct stat opened, current;
        if (fstat(w->devices[i].fd, &opened) != 0 ||
            stat(w->devices[i].path, &current) != 0 ||
            opened.st_ino != current.st_ino || opened.st_dev != current.st_dev)
            remove_device(w, i);
    }
}

static void scan_keyboards(keyboard_meta_t *w) {
    remove_stale_devices(w);
    DIR *dir = opendir("/dev/input");
    if (!dir) return;
    struct dirent *entry;
    while (w->count < META_MAX_DEVICES && (entry = readdir(dir))) {
        if (strncmp(entry->d_name, "event", 5) != 0) continue;
        char path[128];
        int length = snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);
        if (length < 0 || (size_t)length >= sizeof(path) || already_open(w, path)) continue;
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        if (!is_keyboard(fd) || fd >= FD_SETSIZE) {
            close(fd);
            continue;
        }
        atomic_fetch_add(&w->grab_attempts, 1);
        if (ioctl(fd, EVIOCGRAB, 1) < 0) {
            int grab_errno = errno;
            atomic_fetch_add(&w->grab_failures, 1);
            atomic_store(&w->last_grab_errno, grab_errno);
            commons_log_warn("Input", "Keyboard: EVIOCGRAB failed on %s: errno=%d",
                             path, grab_errno);
            close(fd);
            continue;
        }
        struct meta_device_t *device = &w->devices[w->count++];
        device->fd = fd;
        memset(device->pressed, 0, sizeof(device->pressed));
        snprintf(device->path, sizeof(device->path), "%s", path);
        atomic_store(&w->active_count, w->count);
        commons_log_info("Input", "Keyboard: grabbed physical device %s", path);
    }
    closedir(dir);
}

static short linux_key_to_vk(unsigned code) {
    switch (code) {
        case KEY_ESC: return VK_ESCAPE;
        case KEY_1: return VK_1;
        case KEY_2: return VK_2;
        case KEY_3: return VK_3;
        case KEY_4: return VK_4;
        case KEY_5: return VK_5;
        case KEY_6: return VK_6;
        case KEY_7: return VK_7;
        case KEY_8: return VK_8;
        case KEY_9: return VK_9;
        case KEY_0: return VK_0;
        case KEY_MINUS: return VK_OEM_MINUS;
        case KEY_EQUAL: return 0xBB; /* VK_OEM_PLUS */
        case KEY_BACKSPACE: return VK_BACK;
        case KEY_TAB: return VK_TAB;
        case KEY_Q: return VK_Q;
        case KEY_W: return VK_W;
        case KEY_E: return VK_E;
        case KEY_R: return VK_R;
        case KEY_T: return VK_T;
        case KEY_Y: return VK_Y;
        case KEY_U: return VK_U;
        case KEY_I: return VK_I;
        case KEY_O: return VK_O;
        case KEY_P: return VK_P;
        case KEY_LEFTBRACE: return VK_OEM_4;
        case KEY_RIGHTBRACE: return VK_OEM_6;
        case KEY_ENTER:
        case KEY_KPENTER: return VK_RETURN;
        case KEY_LEFTCTRL: return VK_LCONTROL;
        case KEY_RIGHTCTRL: return VK_RCONTROL;
        case KEY_A: return VK_A;
        case KEY_S: return VK_S;
        case KEY_D: return VK_D;
        case KEY_F: return VK_F;
        case KEY_G: return VK_G;
        case KEY_H: return VK_H;
        case KEY_J: return VK_J;
        case KEY_K: return VK_K;
        case KEY_L: return VK_L;
        case KEY_SEMICOLON: return VK_OEM_1;
        case KEY_APOSTROPHE: return VK_OEM_7;
        case KEY_GRAVE: return VK_OEM_3;
        case KEY_LEFTSHIFT: return VK_LSHIFT;
        case KEY_RIGHTSHIFT: return VK_RSHIFT;
        case KEY_BACKSLASH: return VK_OEM_5;
        case KEY_Z: return VK_Z;
        case KEY_X: return VK_X;
        case KEY_C: return VK_C;
        case KEY_V: return VK_V;
        case KEY_B: return VK_B;
        case KEY_N: return VK_N;
        case KEY_M: return VK_M;
        case KEY_COMMA: return VK_OEM_COMMA;
        case KEY_DOT: return VK_OEM_PERIOD;
        case KEY_SLASH: return VK_OEM_2;
        case KEY_LEFTALT: return VK_LMENU;
        case KEY_RIGHTALT: return VK_RMENU;
        case KEY_SPACE: return VK_SPACE;
        case KEY_CAPSLOCK: return VK_CAPITAL;
        case KEY_F1: return VK_F1;
        case KEY_F2: return VK_F2;
        case KEY_F3: return VK_F3;
        case KEY_F4: return VK_F4;
        case KEY_F5: return VK_F5;
        case KEY_F6: return VK_F6;
        case KEY_F7: return VK_F7;
        case KEY_F8: return VK_F8;
        case KEY_F9: return VK_F9;
        case KEY_F10: return VK_F10;
        case KEY_F11: return VK_F11;
        case KEY_F12: return VK_F12;
        case KEY_NUMLOCK: return VK_NUMLOCK;
        case KEY_SCROLLLOCK: return VK_SCROLL;
        case KEY_KP0: return VK_NUMPAD0;
        case KEY_KP1: return VK_NUMPAD1;
        case KEY_KP2: return VK_NUMPAD2;
        case KEY_KP3: return VK_NUMPAD3;
        case KEY_KP4: return VK_NUMPAD4;
        case KEY_KP5: return VK_NUMPAD5;
        case KEY_KP6: return VK_NUMPAD6;
        case KEY_KP7: return VK_NUMPAD7;
        case KEY_KP8: return VK_NUMPAD8;
        case KEY_KP9: return VK_NUMPAD9;
        case KEY_KPASTERISK: return VK_MULTIPLY;
        case KEY_KPMINUS: return VK_SUBTRACT;
        case KEY_KPPLUS: return VK_ADD;
        case KEY_KPDOT: return VK_DECIMAL;
        case KEY_KPSLASH: return VK_DIVIDE;
        case KEY_102ND: return VK_OEM_102;
        case KEY_SYSRQ: return VK_SNAPSHOT;
        case KEY_PAUSE: return VK_PAUSE;
        case KEY_HOME: return VK_HOME;
        case KEY_UP: return VK_UP;
        case KEY_PAGEUP: return VK_PRIOR;
        case KEY_LEFT: return VK_LEFT;
        case KEY_RIGHT: return VK_RIGHT;
        case KEY_END: return VK_END;
        case KEY_DOWN: return VK_DOWN;
        case KEY_PAGEDOWN: return VK_NEXT;
        case KEY_INSERT: return VK_INSERT;
        case KEY_DELETE: return VK_DELETE;
        case KEY_LEFTMETA: return VK_LWIN;
        case KEY_RIGHTMETA: return VK_RWIN;
#ifdef KEY_COMPOSE
        case KEY_COMPOSE: return VK_APPS;
#endif
#ifdef KEY_MENU
        case KEY_MENU: return VK_APPS;
#endif
        default: return 0;
    }
}


/* Key modifier state is rebuilt from held keys on all attached devices. */
static char keyboard_modifiers(keyboard_meta_t *w) {
    bool ctrl = false, alt = false, shift = false, meta = false;
    for (int i = 0; i < w->count; i++) {
        const bool *p = w->devices[i].pressed;
        ctrl |= p[KEY_LEFTCTRL] || p[KEY_RIGHTCTRL];
        alt |= p[KEY_LEFTALT] || p[KEY_RIGHTALT];
        shift |= p[KEY_LEFTSHIFT] || p[KEY_RIGHTSHIFT];
        meta |= p[KEY_LEFTMETA] || p[KEY_RIGHTMETA];
    }
    return (ctrl ? MODIFIER_CTRL : 0) | (alt ? MODIFIER_ALT : 0) |
           (shift ? MODIFIER_SHIFT : 0) | (meta ? MODIFIER_META : 0);
}

static void remove_device(keyboard_meta_t *w, int index) {
    struct meta_device_t *device = &w->devices[index];
    /* Release any held keys if Bluetooth disconnects during a chord. */
    for (int code = 0; code <= KEY_MAX; code++) {
        if (!device->pressed[code]) continue;
        device->pressed[code] = false;
        short vk = linux_key_to_vk((unsigned)code);
        if (vk) w->callback(vk, false, keyboard_modifiers(w), w->userdata);
    }
    ioctl(device->fd, EVIOCGRAB, 0);
    close(device->fd);
    commons_log_info("Input", "Keyboard: detached %s", device->path);
    for (int i = index; i < w->count - 1; ++i) w->devices[i] = w->devices[i + 1];
    --w->count;
    atomic_store(&w->active_count, w->count);
}

static void process_event(keyboard_meta_t *w, struct meta_device_t *device,
                          const struct input_event *event) {
    if (event->type != EV_KEY || event->code > KEY_MAX ||
        (event->value != 0 && event->value != 1)) return;
    bool down = event->value == 1;
    if (device->pressed[event->code] == down) return;
    device->pressed[event->code] = down;
    short vk = linux_key_to_vk(event->code);
    if (!vk) return;
    atomic_fetch_add(&w->key_events, 1);
    w->callback(vk, down, keyboard_modifiers(w), w->userdata);
}

static void *keyboard_meta_thread(void *context) {
    keyboard_meta_t *w = context;
    Uint32 last_scan = 0;
    while (atomic_load(&w->running)) {
        Uint32 now = SDL_GetTicks();
        if (last_scan == 0 || (Uint32)(now - last_scan) >= 500) {
            scan_keyboards(w);
            last_scan = now ? now : 1;
        }
        fd_set rfds;
        FD_ZERO(&rfds);
        int maxfd = -1;
        for (int i = 0; i < w->count; ++i) {
            FD_SET(w->devices[i].fd, &rfds);
            if (w->devices[i].fd > maxfd) maxfd = w->devices[i].fd;
        }
        struct timeval timeout = {0, 200000};
        int ret = select(maxfd + 1, &rfds, NULL, NULL, &timeout);
        if (ret <= 0) continue;
        for (int i = w->count - 1; i >= 0; --i) {
            struct meta_device_t *device = &w->devices[i];
            if (!FD_ISSET(device->fd, &rfds)) continue;
            bool detached = false;
            struct input_event event;
            for (;;) {
                ssize_t n = read(device->fd, &event, sizeof(event));
                if (n == (ssize_t)sizeof(event)) {
                    process_event(w, device, &event);
                } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    break;
                } else {
                    detached = true;
                    break;
                }
            }
            if (detached) remove_device(w, i);
        }
    }
    for (int i = w->count - 1; i >= 0; --i) remove_device(w, i);
    return NULL;
}

keyboard_meta_t *keyboard_meta_start(keyboard_meta_event_fn callback, void *userdata) {
    if (!callback) return NULL;
    keyboard_meta_t *w = calloc(1, sizeof(*w));
    if (!w) return NULL;
    w->callback = callback;
    w->userdata = userdata;
    atomic_init(&w->running, true);
    atomic_init(&w->active_count, 0);
    atomic_init(&w->grab_attempts, 0);
    atomic_init(&w->grab_failures, 0);
    atomic_init(&w->last_grab_errno, 0);
    atomic_init(&w->key_events, 0);
    if (pthread_create(&w->thread, NULL, keyboard_meta_thread, w) != 0) {
        free(w);
        return NULL;
    }
    return w;
}

void keyboard_meta_stop(keyboard_meta_t *watcher) {
    if (!watcher) return;
    atomic_store(&watcher->running, false);
    pthread_join(watcher->thread, NULL);
    free(watcher);
}

bool keyboard_meta_is_active(const keyboard_meta_t *watcher) {
    return watcher != NULL && atomic_load(&watcher->active_count) > 0;
}

void keyboard_meta_get_diagnostics(const keyboard_meta_t *watcher,
                                  keyboard_meta_diagnostics_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!watcher) return;
    out->grabbed = atomic_load(&watcher->active_count);
    out->attempts = atomic_load(&watcher->grab_attempts);
    out->failures = atomic_load(&watcher->grab_failures);
    out->last_errno = atomic_load(&watcher->last_grab_errno);
    out->events = atomic_load(&watcher->key_events);
}

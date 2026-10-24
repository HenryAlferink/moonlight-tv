/* Experimental, non-exclusive Meta-key fallback for LG webOS.
 * Listen for Win key through Linux evdev while SDL continues to handle other
 * keys and mice. Re-scan for Bluetooth reconnection. Never call EVIOCGRAB. */
#include "keyboard_meta.h"
#include "logging.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>
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
    bool left_down;
    bool right_down;
};

struct keyboard_meta_t {
    pthread_t thread;
    atomic_bool running;
    atomic_int active_count;
    keyboard_meta_event_fn callback;
    void *userdata;
    struct meta_device_t devices[META_MAX_DEVICES];
    int count; /* reader thread only */
};

/* Monotonic time prevents events from triggering another slow device scan. */
static uint64_t monotonic_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000) + (uint64_t)ts.tv_nsec / 1000u;
}

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
    return has_bit(keys, KEY_A) && has_bit(keys, KEY_LEFTCTRL) &&
           (has_bit(keys, KEY_LEFTMETA) || has_bit(keys, KEY_RIGHTMETA));
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
        struct meta_device_t *device = &w->devices[w->count++];
        device->fd = fd;
        device->left_down = false;
        device->right_down = false;
        snprintf(device->path, sizeof(device->path), "%s", path);
        atomic_store(&w->active_count, w->count);
        commons_log_info("Input", "WinKey: observing %s (non-exclusive)", path);
    }
    closedir(dir);
}

static void remove_device(keyboard_meta_t *w, int index) {
    struct meta_device_t *device = &w->devices[index];
    if (device->left_down) w->callback(false, false, w->userdata);
    if (device->right_down) w->callback(true, false, w->userdata);
    close(device->fd);
    commons_log_info("Input", "WinKey: detached %s", device->path);
    for (int i = index; i < w->count - 1; ++i) w->devices[i] = w->devices[i + 1];
    --w->count;
    atomic_store(&w->active_count, w->count);
}

static void process_event(keyboard_meta_t *w, struct meta_device_t *device,
                          const struct input_event *event) {
    if (event->type != EV_KEY || (event->value != 0 && event->value != 1)) return;
    bool right;
    bool *state;
    if (event->code == KEY_LEFTMETA) {
        right = false;
        state = &device->left_down;
    } else if (event->code == KEY_RIGHTMETA) {
        right = true;
        state = &device->right_down;
    } else return;
    bool down = event->value == 1;
    if (*state == down) return;
    *state = down;
    w->callback(right, down, w->userdata);
}

static void *keyboard_meta_thread(void *context) {
    keyboard_meta_t *w = context;
    uint64_t next_scan_us = 0;
    while (atomic_load(&w->running)) {
        uint64_t now_us = monotonic_us();
        if (now_us >= next_scan_us) {
            scan_keyboards(w);
            /* Re-enumerate every 500 ms, not after every key press/release. */
            next_scan_us = monotonic_us() + META_SCAN_INTERVAL_US;
        }
        fd_set rfds;
        FD_ZERO(&rfds);
        int maxfd = -1;
        for (int i = 0; i < w->count; ++i) {
            FD_SET(w->devices[i].fd, &rfds);
            if (w->devices[i].fd > maxfd) maxfd = w->devices[i].fd;
        }
        now_us = monotonic_us();
        uint64_t remaining = next_scan_us > now_us ? next_scan_us - now_us : 0;
        struct timeval timeout = {
            .tv_sec = (time_t)(remaining / UINT64_C(1000000)),
            .tv_usec = (suseconds_t)(remaining % UINT64_C(1000000)),
        };
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

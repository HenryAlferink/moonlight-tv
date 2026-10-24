#include "session_evmouse.h"
#include "evmouse.h"
#include "stream/session_priv.h"

#include <errno.h>
#include <assert.h>

#include "logging.h"

#if TARGET_WEBOS
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

/* The existing evmouse implementation opens devices only once. Bluetooth
 * hotplug replaces /dev/input/eventN, so the original fd no longer receives
 * events. Watch for changes to the set of physical mouse nodes and reopen
 * evmouse. Keyboard input remains entirely on its existing path. */
static SDL_bool mouse_should_stop(session_evmouse_t *mouse) {
    SDL_LockMutex(mouse->lock);
    SDL_bool stop = mouse->stop_requested;
    SDL_UnlockMutex(mouse->lock);
    return stop;
}

static SDL_bool mouse_has_bit(const unsigned char *bits, unsigned code) {
    return (bits[code / 8u] & (1u << (code % 8u))) != 0;
}

static uint64_t mouse_topology_signature(void) {
    DIR *dir = opendir("/dev/input");
    if (!dir) return 0;

    uint64_t signature = 0;
    unsigned int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "event", 5) != 0) continue;

        char path[128];
        int n = snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);
        if (n < 0 || (size_t)n >= sizeof(path)) continue;

        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        unsigned char keybits[(KEY_MAX / 8) + 1] = {0};
        unsigned char relbits[(REL_MAX / 8) + 1] = {0};
        SDL_bool is_mouse = ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) >= 0 &&
                            ioctl(fd, EVIOCGBIT(EV_REL, sizeof(relbits)), relbits) >= 0 &&
                            mouse_has_bit(keybits, BTN_MOUSE) &&
                            mouse_has_bit(relbits, REL_X) &&
                            mouse_has_bit(relbits, REL_Y);
        close(fd);
        if (!is_mouse) continue;

        struct stat st;
        if (stat(path, &st) != 0) continue;
        uint64_t device = UINT64_C(1469598103934665603);
        for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
            device = (device ^ *p) * UINT64_C(1099511628211);
        }
        device = (device ^ (uint64_t)st.st_ino) * UINT64_C(1099511628211);
        device = (device ^ (uint64_t)st.st_rdev) * UINT64_C(1099511628211);
        device = (device ^ (uint64_t)st.st_ctime) * UINT64_C(1099511628211);
        signature ^= device; /* enumeration order must not matter */
        count++;
    }
    closedir(dir);
    return signature ^ ((uint64_t)count << 32);
}

static int mouse_hotplug_monitor(void *userdata) {
    session_evmouse_t *mouse = userdata;
    while (!mouse_should_stop(mouse)) {
        SDL_Delay(500);
        SDL_LockMutex(mouse->lock);
        SDL_bool active = mouse->dev != NULL && !mouse->stop_requested;
        SDL_UnlockMutex(mouse->lock);
        if (!active) continue;

        uint64_t current = mouse_topology_signature();
        SDL_LockMutex(mouse->lock);
        if (!mouse->stop_requested && mouse->dev &&
            current != mouse->opened_mouse_signature) {
            commons_log_info("Session", "EvMouse topology changed; reopening mouse nodes");
            evmouse_interrupt(mouse->dev);
        }
        SDL_UnlockMutex(mouse->lock);
    }
    return 0;
}
#endif

static void mouse_listener(const evmouse_event_t *event, void *userdata);

static int mouse_worker(session_evmouse_t *mouse);

static void set_evmouse(session_evmouse_t *mouse, evmouse_t *dev);

void session_evmouse_init(session_evmouse_t *mouse, session_t *session) {
    mouse->session = session;
    mouse->lock = SDL_CreateMutex();
    mouse->cond = SDL_CreateCond();
    mouse->disabled = SDL_FALSE;
#if TARGET_WEBOS
    mouse->stop_requested = SDL_FALSE;
    mouse->opened_mouse_signature = 0;
    mouse->monitor_thread = NULL;
#endif
    mouse->thread = SDL_CreateThread((SDL_ThreadFunction) mouse_worker, "sessinput", mouse);
#if TARGET_WEBOS
    if (mouse->thread) {
        mouse->monitor_thread = SDL_CreateThread(mouse_hotplug_monitor, "evmouse-hotplug", mouse);
        if (!mouse->monitor_thread) {
            commons_log_warn("Session", "Unable to start EvMouse hotplug monitor");
        }
    }
#endif
}

void session_evmouse_deinit(session_evmouse_t *mouse) {
#if TARGET_WEBOS
    /* Ensure both threads exit even if the mouse was disconnected. */
    session_evmouse_interrupt(mouse);
    if (mouse->monitor_thread) {
        SDL_WaitThread(mouse->monitor_thread, NULL);
        mouse->monitor_thread = NULL;
    }
#endif
    if (mouse->thread != NULL) {
        SDL_WaitThread(mouse->thread, NULL);
        mouse->thread = NULL;
    }
    SDL_DestroyMutex(mouse->lock);
    mouse->lock = NULL;
    SDL_DestroyCond(mouse->cond);
    mouse->cond = NULL;
}

void session_evmouse_wait_ready(session_evmouse_t *mouse) {
    SDL_LockMutex(mouse->lock);
    while (mouse->dev == NULL
#if TARGET_WEBOS
           && !mouse->stop_requested
#endif
    ) {
        SDL_CondWait(mouse->cond, mouse->lock);
    }
    SDL_UnlockMutex(mouse->lock);
}

void session_evmouse_interrupt(session_evmouse_t *mouse) {
    SDL_LockMutex(mouse->lock);
#if TARGET_WEBOS
    mouse->stop_requested = SDL_TRUE;
    SDL_CondBroadcast(mouse->cond);
    if (mouse->dev) {
        evmouse_interrupt(mouse->dev);
    }
#else
    assert (mouse->dev != NULL);
    evmouse_interrupt(mouse->dev);
#endif
    SDL_UnlockMutex(mouse->lock);
}

void session_evmouse_disable(session_evmouse_t *mouse) {
    SDL_LockMutex(mouse->lock);
    if (!mouse->disabled) {
        commons_log_info("Session", "EvMouse disable input");
        mouse->disabled = SDL_TRUE;
    }
    SDL_UnlockMutex(mouse->lock);
}

void session_evmouse_enable(session_evmouse_t *mouse) {
    SDL_LockMutex(mouse->lock);
    if (mouse->disabled) {
        commons_log_info("Session", "EvMouse enable input");
        mouse->disabled = SDL_FALSE;
    }
    SDL_UnlockMutex(mouse->lock);
}

static int mouse_worker(session_evmouse_t *mouse) {
#if TARGET_WEBOS
    while (!mouse_should_stop(mouse)) {
        /* A new Bluetooth connection normally creates a new evdev event node.
         * Reopen the library's mouse after each topology change. */
        uint64_t signature = mouse_topology_signature();
        evmouse_t *dev = evmouse_open_default();
        if (!dev) {
            SDL_Delay(400);
            continue;
        }

        SDL_LockMutex(mouse->lock);
        SDL_bool stopping = mouse->stop_requested;
        if (!stopping) {
            mouse->opened_mouse_signature = signature;
            mouse->dev = dev;
            SDL_CondBroadcast(mouse->cond);
        }
        SDL_UnlockMutex(mouse->lock);
        if (stopping) {
            evmouse_close(dev);
            break;
        }

        commons_log_info("Session", "EvMouse opened (hotplug enabled)");
        /* Only enabled for "Use mouse hardware": hide the LG system cursor. */
        evmouse_set_grab(dev, SDL_TRUE);
        evmouse_listen(dev, mouse_listener, mouse);
        set_evmouse(mouse, NULL);
        evmouse_close(dev);
        commons_log_info("Session", "EvMouse closed; monitoring for reconnection");
        if (!mouse_should_stop(mouse)) SDL_Delay(100);
    }
    return 0;
#else
    evmouse_t *dev = evmouse_open_default();
    set_evmouse(mouse, dev);
    if (dev == NULL) {
        commons_log_warn("Session", "No mouse device available");
        return ENODEV;
    }
    commons_log_info("Session", "EvMouse opened");
    evmouse_listen(dev, mouse_listener, mouse);
    set_evmouse(mouse, NULL);
    evmouse_close(dev);
    commons_log_info("Session", "EvMouse closed");
    return 0;
#endif
}


static void set_evmouse(session_evmouse_t *mouse, evmouse_t *dev) {
    SDL_LockMutex(mouse->lock);
    mouse->dev = dev;
    SDL_CondSignal(mouse->cond);
    SDL_UnlockMutex(mouse->lock);
}

static void mouse_listener(const evmouse_event_t *event, void *userdata) {
    session_evmouse_t *mouse = userdata;
    SDL_LockMutex(mouse->lock);
    session_t *session = mouse->session;
    if (!session_accepting_input(session) || mouse->disabled) {
        SDL_UnlockMutex(mouse->lock);
        return;
    }
    switch (event->type) {
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP: {
            commons_log_info("Session", "Mouse button %d %s", event->button.button,
                             event->type == SDL_MOUSEBUTTONDOWN ? "down" : "up");
            stream_input_handle_mbutton(&session->input, &event->button);
            break;
        }
        case SDL_MOUSEMOTION: {
            stream_input_handle_mmotion(&session->input, &event->motion, true);
            break;
        }
        case SDL_MOUSEWHEEL: {
            stream_input_handle_mwheel(&session->input, &event->wheel);
            break;
        }
    }
    SDL_UnlockMutex(mouse->lock);
}
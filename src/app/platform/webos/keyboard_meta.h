#pragma once
#include <stdbool.h>

/* Exclusive capture of physical keyboards during a webOS stream.
 * Magic Remote remains handled by the original SDL input path. */
typedef struct keyboard_meta_t keyboard_meta_t;
typedef void (*keyboard_meta_event_fn)(short vk, bool down, char modifiers, void *userdata);
keyboard_meta_t *keyboard_meta_start(keyboard_meta_event_fn callback, void *userdata);
void keyboard_meta_stop(keyboard_meta_t *watcher);
bool keyboard_meta_is_active(const keyboard_meta_t *watcher);

/* TV overlay diagnostic counters, atomic snapshots from the keyboard reader. */
typedef struct keyboard_meta_diagnostics_t {
    int grabbed;    /* Currently grabbed event device nodes */
    int attempts;   /* Total matching keyboard nodes found since stream start */
    int failures;   /* EVIOCGRAB failures */
    int last_errno; /* Last failed ioctl errno (0 if none) */
    int events;     /* Recognized raw key events received by the reader */
    int sdl_events; /* SDL key events observed while grab was active */
} keyboard_meta_diagnostics_t;
void keyboard_meta_get_diagnostics(const keyboard_meta_t *watcher,
                                  keyboard_meta_diagnostics_t *out);

void keyboard_meta_note_sdl_event(keyboard_meta_t *watcher);

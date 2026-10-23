#pragma once
#include <stdbool.h>

/* Non-exclusive Linux input-event watcher for Meta/Windows keys only.
 * Unlike Aurora EVIOCGRAB, this retains Moonlight's SDL Bluetooth handling. */
typedef struct keyboard_meta_t keyboard_meta_t;
typedef void (*keyboard_meta_event_fn)(bool right, bool down, void *userdata);
keyboard_meta_t *keyboard_meta_start(keyboard_meta_event_fn callback, void *userdata);
void keyboard_meta_stop(keyboard_meta_t *watcher);
bool keyboard_meta_is_active(const keyboard_meta_t *watcher);

#pragma once
#include <stdbool.h>

/* Exclusive capture of physical keyboards during a webOS stream.
 * Magic Remote remains handled by the original SDL input path. */
typedef struct keyboard_meta_t keyboard_meta_t;
typedef void (*keyboard_meta_event_fn)(short vk, bool down, char modifiers, void *userdata);
keyboard_meta_t *keyboard_meta_start(keyboard_meta_event_fn callback, void *userdata);
void keyboard_meta_stop(keyboard_meta_t *watcher);
bool keyboard_meta_is_active(const keyboard_meta_t *watcher);


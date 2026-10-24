#include "app.h"

#include "ui/root.h"

#include "stream/input/session_input.h"
#include "stream/input/vk.h"
#include "stream/session.h"
#include "stream/session_priv.h"

#include <Limelight.h>
#include <SDL.h>

#include "util/bus.h"
#include "util/user_event.h"
#include "logging.h"
#include "app_webos.h"

bool stream_input_webos_intercept_remote_keys(stream_input_t *input, const SDL_KeyboardEvent *event, short *keyCode) {
    switch ((unsigned int) event->keysym.scancode) {
        case SDL_SCANCODE_WEBOS_EXIT: {
            if (event->state == SDL_PRESSED) {
                bus_pushevent(USER_OPEN_OVERLAY, NULL, NULL);
            }
            return true;
        }
        case SDL_SCANCODE_WEBOS_HOME: {
            if (event->state == SDL_RELEASED) {
                app_webos_open_ribbon();
            }
            return true;
        }
        case SDL_SCANCODE_WEBOS_BACK:
            *keyCode = VK_ESCAPE /* SDL_SCANCODE_ESCAPE */;
            return false;
        case SDL_SCANCODE_WEBOS_CH_UP:
            *keyCode = VK_PRIOR /* SDL_SCANCODE_PAGEUP */;
            return false;
        case SDL_SCANCODE_WEBOS_CH_DOWN:
            *keyCode = VK_NEXT /* SDL_SCANCODE_PAGEDOWN */;
            return false;
        case SDL_SCANCODE_WEBOS_YELLOW:
            /* Right click (press/release), including in hardware mouse mode. */
            if (!input->view_only && !event->repeat) {
                LiSendMouseButtonEvent(event->state == SDL_PRESSED
                                       ? BUTTON_ACTION_PRESS : BUTTON_ACTION_RELEASE,
                                       BUTTON_RIGHT);
            }
            return true;
        case SDL_SCANCODE_WEBOS_RED:
            /* Open the stream overlay only once per press. */
            if (event->state == SDL_PRESSED && !event->repeat) {
                bus_pushevent(USER_OPEN_OVERLAY, NULL, NULL);
            }
            return true;
        case SDL_SCANCODE_WEBOS_GREEN:
            /* Show desktop: Win+D. Keep both keys held until button release. */
            if (!input->view_only && !event->repeat) {
                if (event->state == SDL_PRESSED) {
                    LiSendKeyboardEvent(0x8000 | VK_LWIN, KEY_ACTION_DOWN, MODIFIER_META);
                    LiSendKeyboardEvent(0x8000 | VK_D, KEY_ACTION_DOWN, MODIFIER_META);
                } else {
                    LiSendKeyboardEvent(0x8000 | VK_D, KEY_ACTION_UP, MODIFIER_META);
                    LiSendKeyboardEvent(0x8000 | VK_LWIN, KEY_ACTION_UP, 0);
                }
            }
            return true;
        case SDL_SCANCODE_WEBOS_BLUE:
            /* Switch apps: Alt+Tab. Retain Alt until the button is released. */
            if (!input->view_only && !event->repeat) {
                if (event->state == SDL_PRESSED) {
                    LiSendKeyboardEvent(0x8000 | VK_LMENU, KEY_ACTION_DOWN, MODIFIER_ALT);
                    LiSendKeyboardEvent(0x8000 | VK_TAB, KEY_ACTION_DOWN, MODIFIER_ALT);
                } else {
                    LiSendKeyboardEvent(0x8000 | VK_TAB, KEY_ACTION_UP, MODIFIER_ALT);
                    LiSendKeyboardEvent(0x8000 | VK_LMENU, KEY_ACTION_UP, 0);
                }
            }
            return true;
        default:
            return false;
    }
}
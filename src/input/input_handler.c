#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#include <sprout/actions.h>
#include <sprout/host/host_window.h>
#include <sprout/input/input_handler.h>
#include <sprout/input/input_queue.h>
#include <sprout/input/gamepad_thread.h>
#include <sprout/overlay/overlay_renderer.h>

static int g_quit = 0;
static int g_mouse_rx = 0, g_mouse_ry = 0;
static Uint32 g_last_pause_time = 0;

void input_init(void) {
    g_quit = 0;
    g_last_pause_time = 0;
}

int input_mouse_rx(void) { return g_mouse_rx; }
int input_mouse_ry(void) { return g_mouse_ry; }
int input_quit_flag(void) { return g_quit; }
void input_reset_quit_flag(void) { g_quit = 0; }

static void tap_at(int x, int y) {
    pvz2_input_push_touch(PVZ2_TOUCH_DOWN, x, y);
    pvz2_input_push_touch(PVZ2_TOUCH_UP, x, y);
}

static int to_rx(int x) { return host_to_render_x(x); }
static int to_ry(int y) { return host_to_render_y(y); }

int input_handle_event(const SDL_Event *e) {
    switch (e->type) {
        case SDL_QUIT:
            g_quit = 1;
            return 1;
        case SDL_KEYDOWN:
        case SDL_KEYUP: {
            int is_down = (e->type == SDL_KEYDOWN);
            int sym = e->key.keysym.sym;
            /* Action dispatch only on first press (no repeat) */
            if (is_down && e->key.repeat == 0) {
                if (sym == g_action_state[ACT_PAUSE].kb) {
                    Uint32 now = SDL_GetTicks();
                    if (now - g_last_pause_time < 800 && g_last_pause_time != 0)
                        g_quit = 1;
                    tap_at(g_action_state[ACT_PAUSE].x, g_action_state[ACT_PAUSE].y);
                    g_last_pause_time = now;
                    return 1;
                }
                if (sym == g_action_state[ACT_FPS_TOGGLE].kb) { overlay_toggle_fps(); return 1; }
                if (sym == g_action_state[ACT_COORDS_TOGGLE].kb) { overlay_toggle_coords(); return 1; }
                if (sym == g_action_state[ACT_FULLSCREEN_TOGGLE].kb) { host_window_toggle_fullscreen(); return 1; }
                if (!SDL_IsTextInputActive()) {
                    for (int i = 0; i < ACT_NUM_SLOTS; ++i)
                        if (sym == g_action_state[ACT_SLOT_1 + i].kb) { tap_at(g_action_state[ACT_SLOT_1 + i].x, g_action_state[ACT_SLOT_1 + i].y); return 1; }
                    for (int i = 0; i < ACT_NUM_POWERS; ++i)
                        if (sym == g_action_state[ACT_POWER_1 + i].kb) { tap_at(g_action_state[ACT_POWER_1 + i].x, g_action_state[ACT_POWER_1 + i].y); return 1; }
                    if (sym == g_action_state[ACT_SHOVEL].kb) { tap_at(g_action_state[ACT_SHOVEL].x, g_action_state[ACT_SHOVEL].y); return 1; }
                    if (sym == g_action_state[ACT_NUTRIENTS].kb) { tap_at(g_action_state[ACT_NUTRIENTS].x, g_action_state[ACT_NUTRIENTS].y); return 1; }
                    if (sym == g_action_state[ACT_FAST_FORWARD].kb) { tap_at(g_action_state[ACT_FAST_FORWARD].x, g_action_state[ACT_FAST_FORWARD].y); return 1; }
                }
            }
            /* Always feed Enter/Backspace to the game regardless of action dispatch */
            int code = 0;
            if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) code = PVZ2_KEY_ENTER;
            else if (sym == SDLK_BACKSPACE) code = PVZ2_KEY_DEL;
            if (code) pvz2_input_push_key(code, is_down ? 1 : 0);
            return 1;
        }
        case SDL_MOUSEBUTTONDOWN:
            if (e->button.button == SDL_BUTTON_LEFT)
                pvz2_input_push_touch(PVZ2_TOUCH_DOWN, to_rx(e->button.x), to_ry(e->button.y));
            return 1;
        case SDL_MOUSEBUTTONUP:
            if (e->button.button == SDL_BUTTON_LEFT)
                pvz2_input_push_touch(PVZ2_TOUCH_UP, to_rx(e->button.x), to_ry(e->button.y));
            return 1;
        case SDL_MOUSEMOTION:
            g_mouse_rx = to_rx(e->motion.x);
            g_mouse_ry = to_ry(e->motion.y);
            if (e->motion.state & SDL_BUTTON_LMASK)
                pvz2_input_push_touch(PVZ2_TOUCH_MOVE, g_mouse_rx, g_mouse_ry);
            return 1;
        case SDL_TEXTINPUT:
            pvz2_input_push_text(e->text.text);
            return 1;
        case SDL_WINDOWEVENT:
            if (e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
                host_window_handle_resize();
            return 1;
    }
    return 0;
}

void input_sync_text(void) {
    int wanted = pvz2_input_keyboard_wanted();
    SDL_bool active = SDL_IsTextInputActive();
    if (wanted && !active) SDL_StartTextInput();
    else if (!wanted && active) SDL_StopTextInput();
}

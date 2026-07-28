#include <sprout/input/gamepad_thread.h>
#include <sprout/input/input_queue.h>
#include <SDL.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

namespace {

/* Shared with main thread for the GL cursor overlay. */
std::atomic<int> g_pad_x{480};
std::atomic<int> g_pad_y{270};

/* Thread control. */
std::atomic<bool> g_running{false};
std::thread g_thread;

/* Config copied at thread start. */
GamepadConfig g_gp_cfg;

/* State pulled from config once at thread start. */
int g_render_w = 960;
int g_render_h = 540;

/* Dpad edge detection (tracked on the thread). */
std::atomic<Uint32> g_pad_dpad_mask{0};

/* Button edge detection (both on thread). */
bool g_pad_touching = false;
bool b_prev = false;
bool start_prev = false;
bool gp_action_prev[GAMEPAD_NUM_ACTIONS] = {false};

const int PAD_DEAD = 8000;
const int PAD_RATE = 500;
const int PAD_STEP = 56;

void thread_func(int render_w, int render_h) {
    /* Open the first connected gamepad. */
    SDL_GameController *pad = nullptr;
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (SDL_IsGameController(i)) {
            pad = SDL_GameControllerOpen(i);
            if (pad) break;
        }
    }
    if (!pad) {
        g_running.store(false, std::memory_order_release);
        return;
    }

    /* Snapshot render resolution from caller. */
    g_render_w = render_w > 0 ? render_w : 960;
    g_render_h = render_h > 0 ? render_h : 540;

    g_pad_x.store(g_render_w / 2, std::memory_order_relaxed);
    g_pad_y.store(g_render_h / 2, std::memory_order_relaxed);

    const auto interval = std::chrono::milliseconds(8); /* ~125 Hz */

    while (g_running.load(std::memory_order_relaxed)) {
        auto t0 = std::chrono::steady_clock::now();

        /* --- Axis -> cursor movement --- */
        Sint16 ax = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX);
        Sint16 ay = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);

        if (abs(ax) < PAD_DEAD) ax = 0;
        if (abs(ay) < PAD_DEAD) ay = 0;

        /* Squared deadzone: map [-32767,32767] -> [-1,1] with quadratic curve. */
        float dx = 0.0f, dy = 0.0f;
        if (ax) { float raw = (float)ax / 32767.0f; dx = raw < 0 ? -(raw * raw) : (raw * raw); }
        if (ay) { float raw = (float)ay / 32767.0f; dy = raw < 0 ? -(raw * raw) : (raw * raw); }

        int px = g_pad_x.load(std::memory_order_relaxed);
        int py = g_pad_y.load(std::memory_order_relaxed);

        /* ~125 Hz means ~8 ms per tick -> dt ≈ 0.008 for the rate formula. */
        px += (int)(dx * PAD_RATE * 0.008f + 0.5f);
        py += (int)(dy * PAD_RATE * 0.008f + 0.5f);

        /* --- Dpad --- */
        Uint32 dpad = 0;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP))    dpad |= 1u;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN))  dpad |= 2u;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT))  dpad |= 4u;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) dpad |= 8u;

        Uint32 old_dpad = g_pad_dpad_mask.load(std::memory_order_relaxed);
        if ((dpad & 1u) && !(old_dpad & 1u)) py -= PAD_STEP;
        if ((dpad & 2u) && !(old_dpad & 2u)) py += PAD_STEP;
        if ((dpad & 4u) && !(old_dpad & 4u)) px -= PAD_STEP;
        if ((dpad & 8u) && !(old_dpad & 8u)) px += PAD_STEP;
        g_pad_dpad_mask.store(dpad, std::memory_order_relaxed);

        /* Clamp to render resolution. */
        if (px < 0) px = 0;
        if (px >= g_render_w) px = g_render_w - 1;
        if (py < 0) py = 0;
        if (py >= g_render_h) py = g_render_h - 1;

    g_pad_x.store(px, std::memory_order_relaxed);
    g_pad_y.store(py, std::memory_order_relaxed);

    /* --- A button -> touch (only when held) --- */
    bool a_down = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A) != 0;
    if (a_down && !g_pad_touching) {
        /* Pressed: begin touch at current position. */
        g_pad_touching = true;
        pvz2_input_push_touch(PVZ2_TOUCH_DOWN, px, py);
    } else if (a_down && g_pad_touching) {
        /* Held: push MOVE only if cursor actually moved (avoids flooding). */
        static int last_move_x = 0, last_move_y = 0;
        if (px != last_move_x || py != last_move_y) {
            last_move_x = px;
            last_move_y = py;
            pvz2_input_push_touch(PVZ2_TOUCH_MOVE, px, py);
        }
    } else if (!a_down && g_pad_touching) {
        /* Released: end touch. */
        g_pad_touching = false;
        pvz2_input_push_touch(PVZ2_TOUCH_UP, px, py);
    }

        /* --- B button -> back key --- */
        bool b_now = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_B) != 0;
        if (b_now && !b_prev) pvz2_input_push_key(PVZ2_KEY_BACK, 1);
        if (!b_now && b_prev) pvz2_input_push_key(PVZ2_KEY_BACK, 0);
        b_prev = b_now;

        /* --- Start -> menu key --- */
        bool start_now = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_START) != 0;
        if (start_now && !start_prev) pvz2_input_push_key(PVZ2_KEY_MENU, 1);
        if (!start_now && start_prev) pvz2_input_push_key(PVZ2_KEY_MENU, 0);
        start_prev = start_now;

        /* --- Configurable action buttons (slots, powers, shovel, etc.) --- */
        {
            static bool first_run = true;
            if (first_run) {
                std::memset(gp_action_prev, 0, sizeof(gp_action_prev));
                first_run = false;
            }
            for (int i = 0; i < GAMEPAD_NUM_ACTIONS; i++) {
                int btn = g_gp_cfg.gp_bindings[i];
                if (btn < 0 || btn >= SDL_CONTROLLER_BUTTON_MAX) continue;
                /* Skip reserved buttons (A=touch, B=back, etc.) to avoid double-firing. */
                bool reserved = false;
                for (int r = 0; r < GAMEPAD_MAX_RESERVED && g_gp_cfg.reserved_buttons[r] >= 0; ++r)
                    if (btn == g_gp_cfg.reserved_buttons[r]) { reserved = true; break; }
                if (reserved) continue;
                bool now = SDL_GameControllerGetButton(pad, (SDL_GameControllerButton)btn) != 0;
                if (now && !gp_action_prev[i]) {
                    int x = 0, y = 0;
                    if (i < GAMEPAD_NUM_SLOTS) {
                        x = g_gp_cfg.slot_x[i]; y = g_gp_cfg.slot_y[i];
                    } else if (i < GAMEPAD_NUM_SLOTS + GAMEPAD_NUM_POWERS) {
                        x = g_gp_cfg.power_x[i - GAMEPAD_NUM_SLOTS]; y = g_gp_cfg.power_y;
                    } else if (i == 10) {
                        x = g_gp_cfg.shovel_x; y = g_gp_cfg.shovel_y;
                    } else if (i == 11) {
                        x = g_gp_cfg.nutrients_x; y = g_gp_cfg.nutrients_y;
                    } else if (i == 12) {
                        x = g_gp_cfg.fast_x; y = g_gp_cfg.fast_y;
                    } else if (i == 13) {
                        x = g_gp_cfg.pause_x; y = g_gp_cfg.pause_y;
                    }
                    pvz2_input_push_touch(PVZ2_TOUCH_DOWN, x, y);
                    pvz2_input_push_touch(PVZ2_TOUCH_UP, x, y);
                }
                gp_action_prev[i] = now;
            }
        }

        /* Sleep for the remainder of the 8 ms interval. */
        auto elapsed = std::chrono::steady_clock::now() - t0;
        auto remaining = interval - elapsed;
        if (remaining > std::chrono::milliseconds(0)) {
            std::this_thread::sleep_for(remaining);
        }
    }

    SDL_GameControllerClose(pad);
}

} // anonymous namespace

extern "C" {

int pvz2_gamepad_start(int render_w, int render_h, const GamepadConfig *cfg) {
    if (g_running.load(std::memory_order_acquire)) return 1;

    if (cfg) std::memcpy(&g_gp_cfg, cfg, sizeof(g_gp_cfg));
    else std::memset(&g_gp_cfg, 0, sizeof(g_gp_cfg));

    g_running.store(true, std::memory_order_release);
    g_thread = std::thread(thread_func, render_w, render_h);

    /* Give the thread a moment to open the gamepad. */
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    return g_running.load(std::memory_order_acquire) ? 1 : 0;
}

void pvz2_gamepad_stop(void) {
    if (!g_running.load(std::memory_order_acquire)) return;
    g_running.store(false, std::memory_order_release);
    if (g_thread.joinable()) g_thread.join();
}

int pvz2_gamepad_get_x(void) {
    return g_pad_x.load(std::memory_order_relaxed);
}

int pvz2_gamepad_get_y(void) {
    return g_pad_y.load(std::memory_order_relaxed);
}

int pvz2_gamepad_is_active(void) {
    return g_running.load(std::memory_order_relaxed) ? 1 : 0;
}

} // extern "C"

#include <stdio.h>
#include <stdlib.h>
#include <SDL.h>
#include <glad/gl.h>
#include <sprout/config.h>
#include <sprout/game_parameters.h>
#include <sprout/gfx/frame_limiter.h>
#include <sprout/gfx/gl_requirements.h>
#include <sprout/gfx/video_mode.h>
#include <sprout/input/input_queue.h>
#include <sprout/input/gamepad_thread.h>
#include <sprout/input/input_handler.h>
#include <sprout/pvz2_session.h>
#include <sprout/host/host_window.h>
#include <sprout/overlay/overlay_renderer.h>
#include <sprout/actions.h>
#include <sprout/log/log.h>

static char g_config_ini_path[1024];

static const char *config_ini_path(void) {
    if (!g_config_ini_path[0]) {
        char *base = SDL_GetBasePath();
        SDL_snprintf(g_config_ini_path, sizeof(g_config_ini_path), "%sconfig.ini", base ? base : "");
        if (base) SDL_free(base);
    }
    return g_config_ini_path;
}

static void pump_events(void) {
    static Uint32 last_pump = 0;
    Uint32 now = SDL_GetTicks();
    if (now - last_pump < 100) return;
    last_pump = now;
    SDL_Event event;
    while (SDL_PollEvent(&event))
        input_handle_event(&event);
    input_sync_text();
    if (host_window_get()) {
        char status[160], title[224];
        pvz2_session_status(status, sizeof(status));
        SDL_snprintf(title, sizeof(title), "Sprout - %s", status);
        SDL_SetWindowTitle(host_window_get(), title);
    }
}

int main(int argc, char **argv) {
    parse_game_parameters(argc, argv);
    log_init(0);
    log_info("build OK");
    log_info("game_path=%s home_path=%s", game_parameters.game_path, game_parameters.home_path);

    if (SDL_Init(SDL_INIT_VIDEO) < 0) { log_error("SDL_Init: %s", SDL_GetError()); return 1; }
    SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);

    pvz2_config_load(config_ini_path(), NULL);
    const pvz2_config_t *cfg = pvz2_config();
    log_info("so=%s obb=%s", cfg->so_path, cfg->obb_path);

    int rw = 960, rh = 540, start_fs = 0;
    pvz2_choose_window_size(&rw, &rh, &start_fs);
    pvz2_set_render_size(rw, rh);

    if (!host_window_create("Sprout", rw, rh, start_fs)) {
        SDL_Quit();
        return 1;
    }

    if (!pvz2_gl_check_requirements(host_window_get())) {
        host_window_destroy();
        SDL_Quit();
        return 1;
    }

    int glad_version = gladLoadGL((GLADloadfunc)SDL_GL_GetProcAddress);
    if (!glad_version) { log_error("gladLoadGL failed"); host_window_destroy(); SDL_Quit(); return 1; }
    log_info("GL: %s (glad %d.%d)", (const char *)glGetString(GL_VERSION),
             GLAD_VERSION_MAJOR(glad_version), GLAD_VERSION_MINOR(glad_version));

    pvz2_frame_limit_init();
    pvz2_session_set_host_pump(pump_events);

    glClearColor(0.06f, 0.07f, 0.10f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    SDL_GL_SwapWindow(host_window_get());
    SDL_PumpEvents();

    pvz2_session_t *session = pvz2_session_start(cfg->so_path);
    if (!session) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Sprout",
            "Could not start the game session.\n\n"
            "Make sure lib/libPVZ2.so and lib/main.*.obb exist\n"
            "next to sprout.exe, and check config.ini paths.",
            host_window_get());
    }

    if (session) {
        host_window_handle_resize();
        input_init();
        actions_reset_defaults();
        actions_load_overrides(config_ini_path());
        overlay_init();

        /* Build GamepadConfig from action state */
        GamepadConfig gp_cfg;
        for (int i = 0; i < ACT_NUM_SLOTS; ++i) {
            gp_cfg.slot_x[i] = g_action_state[ACT_SLOT_1 + i].x;
            gp_cfg.slot_y[i] = g_action_state[ACT_SLOT_1 + i].y;
        }
        for (int i = 0; i < ACT_NUM_POWERS; ++i) gp_cfg.power_x[i] = g_action_state[ACT_POWER_1 + i].x;
        gp_cfg.power_y = g_action_state[ACT_POWER_1].y;
        gp_cfg.shovel_x = g_action_state[ACT_SHOVEL].x; gp_cfg.shovel_y = g_action_state[ACT_SHOVEL].y;
        gp_cfg.nutrients_x = g_action_state[ACT_NUTRIENTS].x; gp_cfg.nutrients_y = g_action_state[ACT_NUTRIENTS].y;
        gp_cfg.fast_x = g_action_state[ACT_FAST_FORWARD].x; gp_cfg.fast_y = g_action_state[ACT_FAST_FORWARD].y;
        gp_cfg.pause_x = g_action_state[ACT_PAUSE].x; gp_cfg.pause_y = g_action_state[ACT_PAUSE].y;
        for (int i = 0; i < ACT_NUM_ACTIONS; ++i) gp_cfg.gp_bindings[i] = g_action_state[i].gp;
        gp_cfg.reserved_buttons[0] = 0; /* A */
        gp_cfg.reserved_buttons[1] = 1; /* B */
        gp_cfg.reserved_buttons[2] = -1;
        gp_cfg.reserved_buttons[3] = -1;
        pvz2_gamepad_start(rw, rh, &gp_cfg);

        log_info("session started");
        while (!input_quit_flag()) {
            SDL_Event event;
            while (SDL_PollEvent(&event))
                input_handle_event(&event);
            input_sync_text();
            if (input_quit_flag()) break;

            glBindVertexArray(0);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glClearColor(0.06f, 0.07f, 0.10f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            if (!pvz2_session_frame(session)) break;

            if (pvz2_gamepad_is_active())
                overlay_draw_gamepad_cursor(pvz2_gamepad_get_x(), pvz2_gamepad_get_y());
            overlay_draw_coords(input_mouse_rx(), input_mouse_ry());
            overlay_tick_fps();
            overlay_draw_fps();

            host_window_swap();
            pvz2_frame_limit_wait();
        }
        pvz2_session_end(session);
        log_info("session ended");
    }

    pvz2_gamepad_stop();
    overlay_shutdown();
    host_window_destroy();
    SDL_Quit();
    log_shutdown();
    return session ? 0 : 1;
}

#include <sprout/gfx/frame_limiter.h>
#include <sprout/config.h>

#include <SDL.h>

static int g_fps_limit = 0;
static Uint64 g_perf_freq = 0;

void pvz2_frame_limit_init(void) {
    const pvz2_config_t *cfg = pvz2_config();
    g_fps_limit = cfg->fps_limit;
    g_perf_freq = SDL_GetPerformanceFrequency();

    if (cfg->vsync && g_fps_limit > 0) {
        if (SDL_GL_SetSwapInterval(-1) == -1)
            SDL_GL_SetSwapInterval(1);
    } else {
        SDL_GL_SetSwapInterval(0);
    }
}

void pvz2_frame_limit_wait(void) {
    if (g_fps_limit <= 0) return;

    static Uint64 last = 0;
    Uint64 now = SDL_GetPerformanceCounter();
    if (last == 0) { last = now; return; }

    Uint64 target_ticks = g_perf_freq / (Uint64)g_fps_limit;
    Uint64 elapsed = now - last;
    if (elapsed < target_ticks) {
        Uint64 remain = target_ticks - elapsed;

        /* Convert to ms for SDL_Delay (bulk of the wait). */
        Uint64 remain_ms = remain * 1000 / g_perf_freq;

        /* Leave ~1ms for a busy-wait, giving the OS scheduler a chance. */
        if (remain_ms > 1) {
            SDL_Delay((Uint32)(remain_ms - 1));
        }

        /* Spin for the final microsecond precision. */
        while (SDL_GetPerformanceCounter() - last < target_ticks) {
            /* busy wait */;
        }
    }
    last = SDL_GetPerformanceCounter();
}

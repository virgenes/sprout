#include <sprout/gfx/video_mode.h>
#include <sprout/config.h>

#include <cstdio>
#include <cstring>

#include <SDL.h>

void pvz2_choose_window_size(int *width, int *height, int *fullscreen) {
    const pvz2_config_t *cfg = pvz2_config();
    int w = 960, h = 540;
    *fullscreen = cfg->video_fullscreen;

    if (cfg->video_width > 0 && cfg->video_height > 0) {
        w = cfg->video_width;
        h = cfg->video_height;
    } else {
        SDL_DisplayMode mode;
        if (SDL_GetDesktopDisplayMode(0, &mode) == 0 && mode.w > 0 && mode.h > 0) {
            if (std::strcmp(cfg->video_mode, "native") == 0) {
                w = mode.w;
                h = mode.h;
            } else {
                h = PVZ2_AUTO_BASE_HEIGHT;
                w = h * mode.w / mode.h;
            }
        }
    }

    if (w <= 0) w = 960;
    if (h <= 0) h = 540;

    *width = w;
    *height = h;
}

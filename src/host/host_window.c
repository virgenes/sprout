#include <SDL.h>
#include <glad/gl.h>
#include <sprout/gfx/video_mode.h>
#include <sprout/host/host_window.h>
#include <sprout/pvz2_session.h>
#include <sprout/log/log.h>

static SDL_Window *g_win = NULL;
static SDL_GLContext g_ctx = NULL;
static int g_render_w = 960, g_render_h = 540;
static int g_win_w = 960, g_win_h = 540;
static int g_fullscreen = 0;

int host_window_create(const char *title, int rw, int rh, int start_fs) {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    g_render_w = rw; g_render_h = rh;
    g_win_w = rw; g_win_h = rh;

    g_win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             rw, rh, SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!g_win) { log_error("SDL_CreateWindow: %s", SDL_GetError()); return 0; }

    g_ctx = SDL_GL_CreateContext(g_win);
    if (!g_ctx) { log_error("SDL_GL_CreateContext: %s", SDL_GetError()); return 0; }
    SDL_GL_MakeCurrent(g_win, g_ctx);

    if (start_fs) {
        g_fullscreen = 1;
        SDL_SetWindowFullscreen(g_win, SDL_WINDOW_FULLSCREEN_DESKTOP);
    }

    log_info("window: %dx%d%s", rw, rh, start_fs ? " (fullscreen)" : "");
    return 1;
}

void host_window_destroy(void) {
    if (g_ctx) { SDL_GL_DeleteContext(g_ctx); g_ctx = NULL; }
    if (g_win) { SDL_DestroyWindow(g_win); g_win = NULL; }
}

void host_window_swap(void) {
    SDL_GL_SwapWindow(g_win);
}

void host_window_toggle_fullscreen(void) {
    g_fullscreen = !g_fullscreen;
    SDL_SetWindowFullscreen(g_win, g_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

void host_window_handle_resize(void) {
    if (!g_win) return;
    int dw = 0, dh = 0;
    SDL_GL_GetDrawableSize(g_win, &dw, &dh);
    if (dw > 0 && dh > 0) {
        g_win_w = dw; g_win_h = dh;
        pvz2_set_drawable_size(dw, dh);
        pvz2_session_request_resize(dw, dh);
    }
}

SDL_Window *host_window_get(void) { return g_win; }
int host_window_render_w(void) { return g_render_w; }
int host_window_render_h(void) { return g_render_h; }
int host_window_win_w(void) { return g_win_w; }
int host_window_win_h(void) { return g_win_h; }
int host_window_is_fullscreen(void) { return g_fullscreen; }

int host_to_render_x(int cx) { return g_win_w > 0 ? cx * g_render_w / g_win_w : cx; }
int host_to_render_y(int cy) { return g_win_h > 0 ? cy * g_render_h / g_win_h : cy; }

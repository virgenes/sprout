#ifndef SPROUT_HOST_HOST_WINDOW_H
#define SPROUT_HOST_HOST_WINDOW_H

#include <SDL.h>

/* Create window + GL context. Returns 0 on failure. */
int host_window_create(const char *title, int render_w, int render_h, int start_fullscreen);

/* Destroy window and context. */
void host_window_destroy(void);

/* Swap buffers and pump events (call once per frame). */
void host_window_swap(void);

/* Toggle fullscreen on/off. */
void host_window_toggle_fullscreen(void);

/* Handle a SIZE_CHANGED window event. */
void host_window_handle_resize(void);

/* Getters */
SDL_Window *host_window_get(void);
int host_window_render_w(void);
int host_window_render_h(void);
int host_window_win_w(void);
int host_window_win_h(void);
int host_window_is_fullscreen(void);

/* Convert client coordinates to render coordinates. */
int host_to_render_x(int client_x);
int host_to_render_y(int client_y);

#endif

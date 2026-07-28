#ifndef SPROUT_OVERLAY_OVERLAY_RENDERER_H
#define SPROUT_OVERLAY_OVERLAY_RENDERER_H

/* One-time GL setup for overlay textures. */
void overlay_init(void);

/* Free GL resources. */
void overlay_shutdown(void);

/* Call once per frame (not on the gamepad thread). */
void overlay_tick_fps(void);

/* Draw overlays (call after pvz2_session_frame) */
void overlay_draw_fps(void);
void overlay_draw_coords(int mouse_rx, int mouse_ry);
void overlay_draw_gamepad_cursor(int cx, int cy);

/* Toggle visibility */
void overlay_toggle_fps(void);
void overlay_toggle_coords(void);
int overlay_visible_fps(void);
int overlay_visible_coords(void);

#endif

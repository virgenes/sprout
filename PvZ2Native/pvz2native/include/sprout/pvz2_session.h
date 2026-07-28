#ifndef SPROUT_PVZ2_SESSION_H
#define SPROUT_PVZ2_SESSION_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A running instance of the emulated game: the loaded .so image plus all the
 * guest runtime state that has to survive between frames.
 *
 * The host owns the frame loop. It has to: SDL must pump its message queue
 * and swap the GL buffers around each Native_onDrawFrame, and neither is
 * possible if one call runs the whole boot plus every frame to completion. */
typedef struct pvz2_session pvz2_session_t;

/* Called periodically from inside long guest calls, on the thread that drives
 * them. The boot sequence runs for minutes without returning to the host, so
 * without this the window never pumps its message queue and the OS marks it
 * "Not responding" for the whole startup. Only ever invoked on the thread that
 * called pvz2_session_start / pvz2_session_frame, never on a guest worker, so
 * it is safe to touch the window and GL context from it. */
typedef void (*pvz2_host_pump_fn)(void);
void pvz2_session_set_host_pump(pvz2_host_pump_fn fn);

/* Copies a short description of what the guest is currently doing (which boot
 * phase, which constructor) into `out`. The boot takes minutes with a dark
 * window, so surfacing this -- e.g. in the window title -- is the only way to
 * tell a working startup from a hang. */
void pvz2_session_status(char *out, size_t n);

/* Loads the .so and runs the full boot sequence (init_array, JNI_OnLoad, the
 * registered-native lifecycle). Returns NULL if the image cannot be loaded. */
pvz2_session_t *pvz2_session_start(const char *so_path);

/* Runs a single Native_onDrawFrame. Returns non-zero while the session is
 * still usable; the host decides when to stop. */
int pvz2_session_frame(pvz2_session_t *session);

/* The fixed resolution the engine renders its scene at (the size reported to
 * Graphics_GetScreenSizeInPixels). The window may be any size and the composite
 * is scaled to it, so the host needs this to map window-pixel input back into
 * the engine's coordinate space. Safe to call before pvz2_session_start. */
void pvz2_render_size(int *width, int *height);
void pvz2_set_render_size(int width, int height);

/* Tell the compositor the current window drawable size, so the final pass fills
 * the window instead of the engine's render size. Call once the window exists
 * and again whenever it is resized. */
void pvz2_set_drawable_size(int width, int height);

/* Ask for the engine to re-render at a new resolution after a window resize.
 * Deferred: the actual onSurfaceChanged runs on the frame thread at the start of
 * the next pvz2_session_frame, so this is safe to call from the SDL event
 * handler. Coalesced -- only the most recent size is applied. */
void pvz2_session_request_resize(int width, int height);

/* Joins any leftover guest threads and frees the image. */
void pvz2_session_end(pvz2_session_t *session);

/* Volume control.  0.0 = silent, 1.0 = full.  Applied as sample-level gain. */
void pvz2_session_set_volume(float vol);
float pvz2_session_get_volume();

#ifdef __cplusplus
}
#endif

#endif

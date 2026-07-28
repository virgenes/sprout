#ifndef SPROUT_CONFIG_H
#define SPROUT_CONFIG_H

#include <stdint.h>
#include <sprout/log/log.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pvz2_config {
    /* [log] */
    int verbose;
    int trace;
    int pc_sample;
    int input;
    int log_channels[LOG_CHAN_COUNT]; /* per-channel enable (1 = on) */

    /* [runtime] */
    int no_page_table;
    unsigned heap_quarantine;

    /* [gl] */
    int gl_debug_clear;
    int gl_no_viewport_fix;
    int gl_flat_fragment;
    int gl_strict;
    int gl_diagnostics;

    /* [game] */
    char user_locale[32];
    int emulate_iap;

    /* [video] */
    char video_mode[16];
    int video_width;
    int video_height;
    int video_fullscreen;
    int vsync;
    int fps_limit;

    /* Network status: 0=none, 1=mobile, 2=wifi */
    int network_status;
    int persist_saves;

    /* [graphics] */
    char graphics_quality[16];
    char graphics_shadows[16];
    char render_scale[8];

    /* [paths] */
    char so_path[512];
    char obb_path[512];
    char save_dir[512];
} pvz2_config_t;

#define PVZ2_AUTO_BASE_HEIGHT 540

void pvz2_config_load(const char *ini_path, const char *base_dir);
const pvz2_config_t *pvz2_config(void);

#ifdef __cplusplus
}
#endif

#endif

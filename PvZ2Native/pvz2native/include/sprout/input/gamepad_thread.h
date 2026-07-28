#ifndef SPROUT_INPUT_GAMEPAD_THREAD_H
#define SPROUT_INPUT_GAMEPAD_THREAD_H

#include <stdint.h>

#define GAMEPAD_NUM_ACTIONS 14
#define GAMEPAD_NUM_SLOTS 7
#define GAMEPAD_NUM_POWERS 3
#define GAMEPAD_MAX_RESERVED 4

/* Action order: 0-6 = slots, 7-9 = powers, 10 = shovel, 11 = nutrients,
 * 12 = fast_forward, 13 = pause. Matches ACT_* enum in actions.h. */
typedef struct {
    int slot_x[GAMEPAD_NUM_SLOTS], slot_y[GAMEPAD_NUM_SLOTS];
    int power_x[GAMEPAD_NUM_POWERS], power_y;
    int shovel_x, shovel_y;
    int nutrients_x, nutrients_y;
    int fast_x, fast_y;
    int pause_x, pause_y;
    int gp_bindings[GAMEPAD_NUM_ACTIONS];
    /* Buttons handled by the gamepad thread itself (e.g. A=touch, B=back).
     * These are skipped in the action scan to avoid double-firing. */
    int reserved_buttons[GAMEPAD_MAX_RESERVED];
} GamepadConfig;

#ifdef __cplusplus
extern "C" {
#endif

int pvz2_gamepad_start(int render_w, int render_h, const GamepadConfig *cfg);
void pvz2_gamepad_stop(void);
int pvz2_gamepad_get_x(void);
int pvz2_gamepad_get_y(void);
int pvz2_gamepad_is_active(void);

#ifdef __cplusplus
}
#endif

#endif

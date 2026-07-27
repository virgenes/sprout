#ifndef SPROUT_ACTIONS_H
#define SPROUT_ACTIONS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Action indices — matches launcher order exactly */
enum {
    ACT_SLOT_1, ACT_SLOT_2, ACT_SLOT_3, ACT_SLOT_4,
    ACT_SLOT_5, ACT_SLOT_6, ACT_SLOT_7,
    ACT_POWER_1, ACT_POWER_2, ACT_POWER_3,
    ACT_SHOVEL, ACT_NUTRIENTS, ACT_FAST_FORWARD, ACT_PAUSE,
    ACT_FPS_TOGGLE, ACT_COORDS_TOGGLE, ACT_FULLSCREEN_TOGGLE,
    ACT_NUM_KEY_TOTAL
};
#define ACT_NUM_ACTIONS 14
#define ACT_NUM_SLOTS 7
#define ACT_NUM_POWERS 3
#define ACT_NUM_TOGGLES 3

/* Default values — used by both game and launcher.
 * Keyboard: '0'-'9'=48-57, 'a'-'z'=97-122, Enter=13, F3=0x4000003A, etc.
 * Gamepad: 0=A,1=B,2=X,3=Y,4=Back,5=Guide,6=Start,7=L-Stick,8=R-Stick,
 *          9=LB,10=RB,11=D-Up,12=D-Down,13=D-Left,14=D-Right, -1=unbound */
typedef struct {
    const char *name;       /* config.ini key name */
    const char *label;      /* human-readable label for launcher UI (narrow) */
    const wchar_t *wlabel;  /* wide label for launcher */
    int def_kb;
    int def_gp;
    int def_x, def_y;       /* -1 = position not applicable */
} ActionDef;

extern const ActionDef kActionDefs[ACT_NUM_KEY_TOTAL];

/* Runtime state per action */
typedef struct {
    int kb;
    int gp;
    int x, y;
} ActionState;

/* Owned by actions.c */
extern ActionState g_action_state[ACT_NUM_KEY_TOTAL];

/* Load [controls] overrides from config.ini. Path must be the full path
 * including filename. Only modifies the 14 non-toggle actions. */
void actions_load_overrides(const char *config_path);

/* Reset runtime state to kActionDefs defaults */
void actions_reset_defaults(void);

#ifdef __cplusplus
}
#endif

#endif

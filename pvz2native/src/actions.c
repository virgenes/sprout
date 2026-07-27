#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sprout/actions.h>

ActionState g_action_state[ACT_NUM_KEY_TOTAL];

static const int kDefPosX[ACT_NUM_KEY_TOTAL] = {
    43, 43, 41, 40, 44, 42, 42,   /* slots */
    666, 738, 808,                   /* powers */
    907, 474, 867, 328,              /* shovel, nutrients, fast, pause */
    -1, -1, -1                        /* toggles */
};
static const int kDefPosY[ACT_NUM_KEY_TOTAL] = {
    85, 141, 191, 246, 299, 352, 405, /* slots */
    513, 513, 513,                     /* powers */
    511, 510, 29, 26,                  /* shovel, nutrients, fast, pause */
    -1, -1, -1                         /* toggles */
};

const ActionDef kActionDefs[ACT_NUM_KEY_TOTAL] = {
    {"slot_1",       "Plant Slot 1",     L"Plant Slot 1",      '1',  2,  43, 85},
    {"slot_2",       "Plant Slot 2",     L"Plant Slot 2",      '2',  3,  43, 141},
    {"slot_3",       "Plant Slot 3",     L"Plant Slot 3",      '3',  9,  41, 191},
    {"slot_4",       "Plant Slot 4",     L"Plant Slot 4",      '4',  10, 40, 246},
    {"slot_5",       "Plant Slot 5",     L"Plant Slot 5",      '5',  4,  44, 299},
    {"slot_6",       "Plant Slot 6",     L"Plant Slot 6",      '6',  11, 42, 352},
    {"slot_7",       "Plant Slot 7",     L"Plant Slot 7",      '7',  12, 42, 405},
    {"power_1",      "Power 1 (J)",      L"Power 1 (J)",       'j',  7,  666, 513},
    {"power_2",      "Power 2 (K)",      L"Power 2 (K)",       'k',  8,  738, 513},
    {"power_3",      "Power 3 (L)",      L"Power 3 (L)",       'l',  13, 808, 513},
    {"shovel",       "Shovel (Q)",       L"Shovel (Q)",        'q',  5,  907, 511},
    {"nutrients",    "Nutrients (G)",    L"Nutrients (G)",     'g',  -1, 474, 510},
    {"fast_forward", "Fast-Forward (H)", L"Fast-Forward (H)",  'h',  14, 867, 29},
    {"pause",        "Pause (Enter)",    L"Pause / Back (Enter)", 13, 6, 328, 26},
    {"fps_toggle",   "FPS Toggle",       L"FPS Toggle",        0x4000003A, -1, -1, -1},
    {"coords_toggle","Coords Toggle",    L"Coords Toggle",     0x4000003B, -1, -1, -1},
    {"fullscreen_toggle","Fullscreen Toggle", L"Fullscreen Toggle", 0x40000044, -1, -1, -1},
};

void actions_load_overrides(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256], sec[64] = "";
    while (fgets(line, sizeof(line), f)) {
        char *t = line;
        while (*t == ' ' || *t == '\t') ++t;
        if (t[0] == '[') {
            char *cl = strchr(t + 1, ']');
            if (cl) { *cl = '\0'; strncpy(sec, t + 1, sizeof(sec) - 1); sec[sizeof(sec) - 1] = '\0'; }
            continue;
        }
        if (strcmp(sec, "controls") != 0) continue;
        char *eq = strchr(t, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = t, *v = eq + 1;
        while (*k == ' ' || *k == '\t') ++k;
        { char *ke = k + strlen(k) - 1; while (ke >= k && (*ke == ' ' || *ke == '\t')) *ke-- = '\0'; }
        while (*v == ' ' || *v == '\t') ++v;
        char *nl = strchr(v, '\n'); if (nl) *nl = '\0';
        nl = strchr(v, '\r'); if (nl) *nl = '\0';
        int iv = atoi(v);
        int is_gp = (strncmp(k, "gp_", 3) == 0);
        const char *match = is_gp ? k + 3 : k;
        for (int i = 0; i < ACT_NUM_ACTIONS; ++i) {
            if (strcmp(match, kActionDefs[i].name) == 0) {
                if (is_gp) g_action_state[i].gp = iv;
                else if (iv >= 0) g_action_state[i].kb = iv;
                break;
            }
        }
        if (!is_gp) {
            for (int i = 0; i < ACT_NUM_ACTIONS; ++i) {
                size_t nlen = strlen(kActionDefs[i].name);
                if (strncmp(k, kActionDefs[i].name, nlen) == 0 && k[nlen] == '_') {
                    int *p = NULL;
                    int slot = i;
                    if (slot < ACT_NUM_SLOTS) {
                        if (strcmp(k + nlen, "_x") == 0) p = &g_action_state[slot].x;
                        else if (strcmp(k + nlen, "_y") == 0) p = &g_action_state[slot].y;
                    } else if (slot < ACT_NUM_SLOTS + ACT_NUM_POWERS) {
                        if (strcmp(k + nlen, "_x") == 0) p = &g_action_state[slot].x;
                        else if (strcmp(k + nlen, "_y") == 0) p = &g_action_state[slot].y;
                    } else {
                        if (strcmp(k + nlen, "_x") == 0) p = &g_action_state[slot].x;
                        else if (strcmp(k + nlen, "_y") == 0) p = &g_action_state[slot].y;
                    }
                    if (p) { *p = iv; break; }
                }
            }
        }
    }
    fclose(f);
}

void actions_reset_defaults(void) {
    for (int i = 0; i < ACT_NUM_KEY_TOTAL; ++i) {
        g_action_state[i].kb = kActionDefs[i].def_kb;
        g_action_state[i].gp = kActionDefs[i].def_gp;
        g_action_state[i].x  = kActionDefs[i].def_x;
        g_action_state[i].y  = kActionDefs[i].def_y;
    }
}

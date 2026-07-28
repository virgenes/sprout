#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sprout/actions.h>

static int failures = 0;
#define TEST(cond, ...) do { if (!(cond)) { fprintf(stderr, "FAIL: " __VA_ARGS__); fprintf(stderr, "\n"); failures++; } else { printf("PASS: " __VA_ARGS__); printf("\n"); } } while(0)

int main(void) {
    printf("=== actions.c unit tests ===\n\n");

    /* Check action count constants are consistent */
    TEST(ACT_NUM_ACTIONS == 14, "ACT_NUM_ACTIONS == 14");
    TEST(ACT_NUM_SLOTS == 7, "ACT_NUM_SLOTS == 7");
    TEST(ACT_NUM_POWERS == 3, "ACT_NUM_POWERS == 3");
    TEST(ACT_NUM_TOGGLES == 3, "ACT_NUM_TOGGLES == 3");
    TEST(ACT_NUM_KEY_TOTAL == ACT_NUM_ACTIONS + ACT_NUM_TOGGLES, "ACT_NUM_KEY_TOTAL == ACT_NUM_ACTIONS + ACT_NUM_TOGGLES");

    /* Check action names are non-null and unique */
    for (int i = 0; i < ACT_NUM_KEY_TOTAL; i++) {
        TEST(kActionDefs[i].name != NULL, "kActionDefs[%d].name != NULL", i);
        TEST(kActionDefs[i].label != NULL, "kActionDefs[%d].label != NULL", i);
        TEST(kActionDefs[i].wlabel != NULL, "kActionDefs[%d].wlabel != NULL", i);
        for (int j = i + 1; j < ACT_NUM_KEY_TOTAL; j++) {
            if (kActionDefs[i].name && kActionDefs[j].name)
                TEST(strcmp(kActionDefs[i].name, kActionDefs[j].name) != 0,
                     "kActionDefs[%d].name '%s' != kActionDefs[%d].name '%s'",
                     i, kActionDefs[i].name, j, kActionDefs[j].name);
        }
    }

    /* Actions (indices 0..13) should have valid default keycodes */
    for (int i = 0; i < ACT_NUM_ACTIONS; i++) {
        TEST(kActionDefs[i].def_kb != 0, "kActionDefs[%d] ('%s').def_kb != 0", i, kActionDefs[i].name);
    }

    /* Slots have positions, toggles don't */
    for (int i = 0; i < ACT_NUM_SLOTS; i++) {
        TEST(kActionDefs[i].def_x > 0, "slot[%d].def_x > 0", i);
        TEST(kActionDefs[i].def_y > 0, "slot[%d].def_y > 0", i);
    }
    for (int i = ACT_NUM_ACTIONS; i < ACT_NUM_KEY_TOTAL; i++) {
        TEST(kActionDefs[i].def_x == -1, "toggle[%d].def_x == -1", i - ACT_NUM_ACTIONS);
        TEST(kActionDefs[i].def_y == -1, "toggle[%d].def_y == -1", i - ACT_NUM_ACTIONS);
    }

    /* Test actions_load_overrides with a temp file */
    {
        FILE *f = fopen("_test_config.ini", "w");
        TEST(f != NULL, "create temp config file");
        if (f) {
            fprintf(f, "[controls]\nslot_1 = 99\ngp_slot_1 = 14\npower_2 = 120\n");
            fclose(f);
        }

        actions_reset_defaults();
        actions_load_overrides("_test_config.ini");

        TEST(g_action_state[ACT_SLOT_1].kb == 99, "slot_1 kb override == 99");
        TEST(g_action_state[ACT_SLOT_1].gp == 14, "slot_1 gp override == 14");
        TEST(g_action_state[ACT_POWER_2].kb == 120, "power_2 kb override == 120");
        TEST(g_action_state[ACT_SLOT_2].kb == kActionDefs[ACT_SLOT_2].def_kb, "slot_2 kb unchanged after partial override");

        remove("_test_config.ini");
    }

    /* Test actions_reset_defaults */
    {
        actions_reset_defaults();
        for (int i = 0; i < ACT_NUM_KEY_TOTAL; i++) {
            TEST(g_action_state[i].kb == kActionDefs[i].def_kb, "reset: action[%d].kb == default", i);
            TEST(g_action_state[i].gp == kActionDefs[i].def_gp, "reset: action[%d].gp == default", i);
            TEST(g_action_state[i].x  == kActionDefs[i].def_x,  "reset: action[%d].x == default", i);
            TEST(g_action_state[i].y  == kActionDefs[i].def_y,  "reset: action[%d].y == default", i);
        }
    }

    printf("\n=== %s ===\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}

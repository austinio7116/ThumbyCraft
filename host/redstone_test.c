/*
 * Redstone repeater (DELAY block) smoke test. Builds
 *   LEVER_ON -> wire -> [repeater facing +Z] -> wire -> LAMP
 * on a flat row, ticks the redstone sim, and checks the lamp lights with
 * the expected one-tick-ish delay. Then flips the lever off and checks
 * the lamp goes dark. Confirms the repeater propagates power at all.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "craft_world.h"
#include "craft_redstone.h"
#include "craft_torches.h"
#include "craft_blocks.h"
#include "craft_types.h"

void craft_main_init(uint16_t *fb, uint32_t seed);
uint32_t craft_platform_rand32(void) { return (uint32_t)rand(); }
int craft_save_slot_used(int slot) { (void)slot; return 0; }
const uint16_t *craft_save_slot_thumb(int slot) { (void)slot; return NULL; }
static uint16_t g_fb[CRAFT_FB_W * CRAFT_FB_H];

/* Run N redstone ticks (the sim self-gates at ~5 Hz, so feed >0.2 s each). */
static void ticks(int n) { for (int i = 0; i < n; i++) craft_redstone_tick(0.25f); }

int main(void) {
    craft_main_init(g_fb, 4242);
    /* Build the circuit near the window centre at a fixed y. */
    int X = 0, Y = 42, Z0 = 0;
    /* Clear the row to air first. */
    for (int z = 0; z < 8; z++) craft_world_set(X, Y, Z0 + z, BLK_AIR);

    craft_world_set(X, Y, Z0 + 0, BLK_LEVER_ON);          /* source */
    craft_world_set(X, Y, Z0 + 1, BLK_REDSTONE_WIRE);     /* input wire */
    craft_world_set(X, Y, Z0 + 2, BLK_DELAY);             /* repeater */
    craft_torches_record_orient(X, Y, Z0 + 2, FACE_PZ);   /* faces +Z: in from -Z, out to +Z */
    craft_world_set(X, Y, Z0 + 3, BLK_REDSTONE_WIRE);     /* output wire */
    craft_world_set(X, Y, Z0 + 4, BLK_LAMP);              /* sink */
    craft_torches_rebuild();
    craft_redstone_rescan();

    printf("[redstone_test] lever->wire->repeater->wire->lamp\n");
    ticks(10);
    BlockId lamp = craft_world_get(X, Y, Z0 + 4);
    BlockId rep  = craft_world_get(X, Y, Z0 + 2);
    BlockId ow   = craft_world_get(X, Y, Z0 + 3);
    printf("  lever ON  -> repeater=%s outwire=%s lamp=%s\n",
           rep == BLK_DELAY_ON ? "ON" : "off",
           ow == BLK_REDSTONE_WIRE_ON ? "ON" : "off",
           lamp == BLK_LAMP_ON ? "LIT" : "dark");
    int ok_on = (lamp == BLK_LAMP_ON);

    /* Flip lever off — lamp should go dark after the sim settles. */
    craft_world_set(X, Y, Z0 + 0, BLK_LEVER_OFF);
    craft_redstone_rescan();
    ticks(10);
    lamp = craft_world_get(X, Y, Z0 + 4);
    rep  = craft_world_get(X, Y, Z0 + 2);
    printf("  lever OFF -> repeater=%s lamp=%s\n",
           rep == BLK_DELAY_ON ? "ON" : "off",
           lamp == BLK_LAMP_ON ? "LIT" : "dark");
    int ok_off = (lamp != BLK_LAMP_ON);

    printf("[redstone_test] %s\n", (ok_on && ok_off) ? "PASS — repeater works"
                                                      : "FAIL");
    return (ok_on && ok_off) ? 0 : 1;
}

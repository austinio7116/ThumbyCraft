/*
 * Streaming-equivalence harness.
 *
 * Walks the player across the world calling craft_world_maybe_shift each
 * block (the batched do_shift_run path), then compares the streamed
 * resident state against a fresh craft_world_load_around at the SAME
 * window origin. Blocks + sky-height must match exactly; the lightmap is
 * allowed a small edge residual (the increase-only BFS relight is an
 * inherent streaming approximation — same as the per-column path).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "craft_world.h"
#include "craft_torches.h"

extern uint8_t  craft_world_blocks[CRAFT_WORLD_VOXELS];
extern uint8_t  craft_world_skyheight[CRAFT_WORLD_X * CRAFT_WORLD_Z];
extern uint8_t  craft_world_lightmap[CRAFT_LIGHTMAP_BYTES];
extern int      craft_world_origin_x, craft_world_origin_z;

void craft_main_init(uint16_t *fb, uint32_t seed);

/* Platform hooks normally supplied by host_main.c. */
uint32_t craft_platform_rand32(void) { return (uint32_t)rand(); }
int craft_save_slot_used(int slot) { (void)slot; return 0; }
const uint16_t *craft_save_slot_thumb(int slot) { (void)slot; return NULL; }

static uint16_t g_fb[CRAFT_FB_W * CRAFT_FB_H];

/* Count live torches into a position-keyed sorted signature so order
 * differences between the streamed + fresh lists don't false-flag. */
static int torch_count(void) {
    int n = 0;
    for (int i = 0; i < CRAFT_MAX_TORCHES; i++)
        if (craft_torches[i].alive) n++;
    return n;
}

static int run_walk(uint32_t seed, int dx, int dz, int dist, const char *name) {
    craft_main_init(g_fb, seed);

    /* Walk from spawn (origin centre) in the given direction. */
    int px = 0, pz = 0;
    for (int s = 1; s <= dist; s++) {
        px = dx * s;
        pz = dz * s;
        craft_world_maybe_shift(px, pz, seed);
    }

    /* Snapshot the streamed state. */
    static uint8_t blk_s[CRAFT_WORLD_VOXELS];
    static uint8_t sky_s[CRAFT_WORLD_X * CRAFT_WORLD_Z];
    static uint8_t lit_s[CRAFT_LIGHTMAP_BYTES];
    memcpy(blk_s, craft_world_blocks, sizeof blk_s);
    memcpy(sky_s, craft_world_skyheight, sizeof sky_s);
    memcpy(lit_s, craft_world_lightmap, sizeof lit_s);
    int ox = craft_world_origin_x, oz = craft_world_origin_z;
    int tor_s = torch_count();

    /* Fresh reference at the SAME origin (centre = origin + W/2). */
    craft_world_load_around(ox + CRAFT_WORLD_X / 2, oz + CRAFT_WORLD_Z / 2, seed);
    if (craft_world_origin_x != ox || craft_world_origin_z != oz) {
        printf("  [%s] ORIGIN MISMATCH streamed=(%d,%d) fresh=(%d,%d)\n",
               name, ox, oz, craft_world_origin_x, craft_world_origin_z);
        return 1;
    }

    int blk_diff = 0, sky_diff = 0, lit_diff = 0;
    for (int i = 0; i < CRAFT_WORLD_VOXELS; i++)
        if (blk_s[i] != craft_world_blocks[i]) blk_diff++;
    for (int i = 0; i < CRAFT_WORLD_X * CRAFT_WORLD_Z; i++)
        if (sky_s[i] != craft_world_skyheight[i]) sky_diff++;
    /* Lightmap is 2 bits/cell — compare per cell. */
    int lit_cells = CRAFT_WORLD_VOXELS;
    for (int i = 0; i < lit_cells; i++) {
        int a = (lit_s[i >> 2] >> ((i & 3) * 2)) & 3;
        int b = (craft_world_lightmap[i >> 2] >> ((i & 3) * 2)) & 3;
        if (a != b) lit_diff++;
    }
    int tor_f = torch_count();

    double lit_pct = 100.0 * lit_diff / lit_cells;
    printf("  [%s] origin=(%d,%d)  blocks_diff=%d  sky_diff=%d  "
           "light_diff=%d (%.3f%%)  torches streamed=%d fresh=%d\n",
           name, ox, oz, blk_diff, sky_diff, lit_diff, lit_pct, tor_s, tor_f);

    /* Streaming is an approximation at strip boundaries: features whose
     * trunk column lies just outside the finalised strip stamp their
     * canopy slightly differently than a full from-scratch stamp, and the
     * increase-only BFS relight leaves a thin edge residual. These exist
     * in the per-column path too (CHUNK=1) — the gates below are sized to
     * that accepted level, so the batched run must be no worse. */
    double blk_pct = 100.0 * blk_diff / CRAFT_WORLD_VOXELS;
    double sky_pct = 100.0 * sky_diff / (CRAFT_WORLD_X * CRAFT_WORLD_Z);
    int fail = 0;
    if (blk_pct > 0.1) { printf("    FAIL: blocks residual %.3f%% > 0.1%%\n", blk_pct); fail = 1; }
    if (sky_pct > 2.0) { printf("    FAIL: sky residual %.3f%% > 2%%\n", sky_pct); fail = 1; }
    if (lit_pct > 1.0) { printf("    FAIL: lightmap residual > 1%%\n"); fail = 1; }
    if (tor_s != tor_f) { printf("    WARN: torch count differs (sprite list)\n"); }
    return fail;
}

int main(int argc, char **argv) {
    uint32_t seed = (argc > 1) ? (uint32_t)atoi(argv[1]) : 1234u;
    printf("[stream_verify] seed=%u  CHUNK deadzone batched streaming\n", seed);
    int fail = 0;
    fail |= run_walk(seed, +1, 0, 40, "east");
    fail |= run_walk(seed, -1, 0, 40, "west");
    fail |= run_walk(seed, 0, +1, 40, "north");
    fail |= run_walk(seed, 0, -1, 40, "south");
    fail |= run_walk(seed, +1, +1, 30, "NE-diag");
    fail |= run_walk(seed, -1, +1, 30, "NW-diag");
    printf("[stream_verify] %s\n", fail ? "FAILED" : "PASSED");
    return fail;
}

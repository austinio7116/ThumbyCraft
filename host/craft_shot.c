/*
 * Headless screenshot harness — renders one frame of a chosen biome to
 * a 24-bit BMP (no SDL window). Dev-only tool for comparing biome
 * colours / textures and the cutout-CROSS plants.
 *
 *   ./build_host/craft_shot <biome 0-7> [seed] [out.bmp]
 *
 * Biome index matches CraftBiome: 0 plains, 1 forest, 2 desert,
 * 3 taiga, 4 swamp, 5 mountains, 6 jungle, 7 savanna. Uses the
 * craft_gen_force_biome override so the whole window is that biome.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "craft_render.h"
#include "craft_world.h"
#include "craft_blocks.h"
#include "craft_torches.h"

void craft_main_init(uint16_t *fb, uint32_t seed);
void craft_torches_record_orient(int wx, int wy, int wz, int face);
extern int craft_gen_force_biome;
uint32_t craft_platform_rand32(void) { return (uint32_t)rand(); }
int craft_save_slot_used(int s) { (void)s; return 0; }
const uint16_t *craft_save_slot_thumb(int s) { (void)s; return NULL; }

static uint16_t g_fb[CRAFT_FB_W * CRAFT_FB_H];

static void write_bmp(const char *path) {
    int W = CRAFT_FB_W, H = CRAFT_FB_H;
    int rowsz = (W * 3 + 3) & ~3, dataSz = rowsz * H, off = 54, fsz = off + dataSz;
    uint8_t hdr[54] = {0};
    hdr[0]='B'; hdr[1]='M';
    hdr[2]=fsz; hdr[3]=fsz>>8; hdr[4]=fsz>>16; hdr[5]=fsz>>24;
    hdr[10]=off; hdr[14]=40;
    hdr[18]=W; hdr[19]=W>>8; hdr[22]=H; hdr[23]=H>>8;
    hdr[26]=1; hdr[28]=24;
    FILE *f = fopen(path, "wb");
    fwrite(hdr, 1, 54, f);
    uint8_t *row = malloc(rowsz);
    for (int y = H - 1; y >= 0; y--) {           /* BMP is bottom-up */
        memset(row, 0, rowsz);
        for (int x = 0; x < W; x++) {
            uint16_t c = g_fb[y * W + x];
            row[x*3+0] = (c & 0x1F) * 255 / 31;        /* B */
            row[x*3+1] = ((c >> 5) & 0x3F) * 255 / 63; /* G */
            row[x*3+2] = ((c >> 11) & 0x1F) * 255 / 31;/* R */
        }
        fwrite(row, 1, rowsz, f);
    }
    free(row);
    fclose(f);
}

int main(int argc, char **argv) {
    int biome   = argc > 1 ? atoi(argv[1]) : 0;
    uint32_t seed = argc > 2 ? (uint32_t)atoi(argv[2]) : 4242;
    const char *out = argc > 3 ? argv[3] : "/tmp/biome.bmp";

    craft_gen_force_biome = biome;
    craft_main_init(g_fb, seed);
    craft_render_set_time(90.0f);     /* fixed midday light for fair compare */

    int cx = craft_world_origin_x + CRAFT_WORLD_X / 2;
    int cz = craft_world_origin_z + CRAFT_WORLD_Z / 2;
    int lx = cx - craft_world_origin_x, lz = cz - craft_world_origin_z;
    int gy = craft_world_skyheight[lz * CRAFT_WORLD_X + lx];

    /* Slightly elevated 3/4 view so terrain surface, trees, plants and
     * sky all read. Same framing for every biome. */
    CraftCamera cam = {
        .pos = { (float)cx + 0.5f, (float)gy + 4.0f, (float)cz + 0.5f },
        .yaw = 0.7f, .pitch = -0.32f, .fov = 1.4f
    };

    /* DOORTEST: build a stone wall with a doorway + door, plus a trap-
     * door, right in front of an eye-level camera looking +Z. */
    if (getenv("DOORTEST")) {
        int wz = cz + 4;
        for (int dx = -3; dx <= 3; dx++)
            for (int dy = 1; dy <= 3; dy++) {
                if (dx == 0 && dy <= 2) continue;        /* doorway gap */
                craft_world_set(cx + dx, gy + dy, wz, BLK_STONE);
            }
        BlockId dstate = getenv("DOOROPEN") ? BLK_DOOR_ON : BLK_DOOR_OFF;
        craft_world_set(cx, gy + 1, wz, dstate);   /* door */
        craft_torches_record_orient(cx, gy + 1, wz, FACE_PZ);
        craft_world_set(cx, gy + 2, wz, dstate);
        craft_torches_record_orient(cx, gy + 2, wz, FACE_PZ);
        /* a trapdoor sitting on a block to the right */
        craft_world_set(cx + 3, gy + 1, cz + 2, BLK_STONE);
        craft_world_set(cx + 3, gy + 2, cz + 2, BLK_TRAPDOOR_OFF);
        craft_torches_record_orient(cx + 3, gy + 2, cz + 2, FACE_PY);
        /* Ladder on the wall just left of the doorway (faces -Z, toward
         * camera). */
        craft_world_set(cx - 2, gy + 1, wz - 1, BLK_LADDER);
        craft_world_set(cx - 2, gy + 2, wz - 1, BLK_LADDER);
        craft_torches_record_orient(cx - 2, gy + 1, wz - 1, FACE_NZ);
        craft_torches_record_orient(cx - 2, gy + 2, wz - 1, FACE_NZ);
        /* Pressure pad on the floor ahead. */
        craft_world_set(cx + 2, gy + 1, cz + 2, BLK_PRESSURE_PAD);
        /* Redstone wire run on the floor with a T-junction. */
        craft_world_set(cx - 1, gy + 1, cz + 1, BLK_REDSTONE_WIRE);
        craft_world_set(cx - 1, gy + 1, cz + 2, BLK_REDSTONE_WIRE_ON);
        craft_world_set(cx - 1, gy + 1, cz + 3, BLK_REDSTONE_WIRE);
        craft_world_set(cx,     gy + 1, cz + 2, BLK_REDSTONE_WIRE_ON);
        /* A hanging vine column right in front of the wall, just right of
         * the doorway — 3 cells tall against the grey stone so it's
         * unmistakable. */
        for (int vy = 1; vy <= 3; vy++)
            craft_world_set(cx + 2, gy + vy, wz - 1, BLK_VINE);
        cam.pos.x = (float)cx + 0.5f; cam.pos.y = (float)gy + 2.2f;
        cam.pos.z = (float)cz + 0.5f; cam.yaw = 0.0f; cam.pitch = -0.12f;
    }
    craft_render_begin(&cam);
    craft_render_strip(&cam, g_fb, 0, CRAFT_FB_H);
    craft_torches_render(&cam, g_fb);
    write_bmp(out);
    printf("biome %d -> %s (gy=%d)\n", biome, out, gy);
    return 0;
}

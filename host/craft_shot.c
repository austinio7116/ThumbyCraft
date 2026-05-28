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
#include "craft_gen.h"

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

    /* PALMVIEW: scan the window for a palm trunk (wood directly on sand)
     * nearest the centre and frame it at eye level. */
    if (getenv("PALMVIEW")) {
        int best = 1 << 30, pwx = 0, pwy = 0, pwz = 0; bool found = false;
        for (int llz = 0; llz < CRAFT_WORLD_Z; llz++)
            for (int llx = 0; llx < CRAFT_WORLD_X; llx++)
                for (int y = 1; y < CRAFT_WORLD_Y - 8; y++) {
                    int wx = craft_world_origin_x + llx, wz = craft_world_origin_z + llz;
                    if (craft_world_get(wx, y, wz) == BLK_WOOD &&
                        craft_world_get(wx, y - 1, wz) == BLK_SAND) {
                        int d = (llx - CRAFT_WORLD_X/2)*(llx - CRAFT_WORLD_X/2) +
                                (llz - CRAFT_WORLD_Z/2)*(llz - CRAFT_WORLD_Z/2);
                        if (d < best) { best = d; pwx = wx; pwy = y; pwz = wz; found = true; }
                        break;
                    }
                }
        if (found) {
            printf("palm at (%d,%d,%d)\n", pwx, pwy, pwz);
            cam.pos.x = (float)pwx - 14.0f; cam.pos.y = (float)pwy + 4.0f;
            cam.pos.z = (float)pwz + 0.5f; cam.yaw = 1.5708f; cam.pitch = 0.30f;
        }
    }

    /* DUNDUMP: print an ASCII cross-section of the dungeon band so the
     * corridors/walls can be verified without an underground render.
     * '.'=air  '#'=cobble  ' '=stone/other. Exits after printing. */
    if (getenv("DUNDUMP")) {
        static const int ys[] = {28, 24, 20, 17, 16, 15, 14, 13};
        for (unsigned i = 0; i < sizeof ys / sizeof ys[0]; i++) {
            int yy = ys[i];
            printf("--- y=%d ---\n", yy);
            for (int lz = 0; lz < CRAFT_WORLD_Z; lz++) {
                for (int lx = 0; lx < CRAFT_WORLD_X; lx++) {
                    BlockId b = craft_world_get(craft_world_origin_x + lx, yy,
                                                craft_world_origin_z + lz);
                    putchar(b == BLK_AIR ? '.' : b == BLK_COBBLE ? '#' :
                            b == BLK_CHEST ? 'C' : ' ');
                }
                putchar('\n');
            }
        }
        return 0;
    }

    /* BLKCOUNT: count blossom-leaf + flower-vine cells in the window. */
    if (getenv("BLKCOUNT")) {
        long blossom = 0, fvine = 0, leaves = 0;
        for (int y = 0; y < CRAFT_WORLD_Y; y++)
        for (int z = 0; z < CRAFT_WORLD_Z; z++)
        for (int x = 0; x < CRAFT_WORLD_X; x++) {
            BlockId b = craft_world_get(craft_world_origin_x+x, y, craft_world_origin_z+z);
            if (b == BLK_BLOSSOM_LEAVES) blossom++;
            else if (b == BLK_FLOWER_VINE) fvine++;
            else if (b == BLK_VINE) leaves++;  /* reuse leaves col for VINE */
        }
        printf("blossom=%ld fvine=%ld vine=%ld\n", blossom, fvine, leaves);
        return 0;
    }

    /* BIOMEHIST: per-biome cell counts in the window (natural biome). */
    if (getenv("BIOMEHIST")) {
        long c[8] = {0};
        for (int i = 0; i < CRAFT_WORLD_X * CRAFT_WORLD_Z; i++) {
            uint8_t b = craft_world_biome[i];
            if (b < 8) c[b]++;
        }
        printf("%ld %ld %ld %ld %ld %ld %ld %ld\n",
               c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7]);
        return 0;
    }

    /* SHAFTPROBE: find a dungeon trapdoor and print the vertical column
     * through it + the 3×3 surface surround. */
    if (getenv("SHAFTPROBE")) {
        for (int lz = 0; lz < CRAFT_WORLD_Z; lz++)
        for (int lx = 0; lx < CRAFT_WORLD_X; lx++) {
            int wx = craft_world_origin_x + lx, wz = craft_world_origin_z + lz;
            int ty = -1;
            for (int y = CRAFT_WORLD_Y - 1; y > 18; y--)
                if (craft_world_get(wx, y, wz) == BLK_TRAPDOOR_OFF) { ty = y; break; }
            if (ty < 0) continue;
            printf("trapdoor at (%d,%d,%d)\n", wx, ty, wz);
            printf("vertical column under it (top->dungeon):\n");
            for (int y = ty + 1; y >= 12; y--) {
                BlockId b = craft_world_get(wx, y, wz);
                printf(" y=%2d %c\n", y, b==BLK_AIR?'.':b==BLK_TRAPDOOR_OFF?'T':
                       b==BLK_COBBLE?'#':b==BLK_STONE?'S':b==BLK_CHEST?'C':'?');
            }
            printf("3x3 surface ring at y=%d:\n", ty);
            for (int dz = -1; dz <= 1; dz++) {
                for (int dx = -1; dx <= 1; dx++) {
                    BlockId b = craft_world_get(wx+dx, ty, wz+dz);
                    putchar(b==BLK_TRAPDOOR_OFF?'T':b==BLK_STONE?'S':
                            b==BLK_AIR?'.':b==BLK_GRASS?'g':'?');
                }
                putchar('\n');
            }
            return 0;
        }
        printf("no trapdoor found\n");
        return 0;
    }

    /* FORTVIEW: locate the nearest forest fort and frame it at a low
     * 3/4 angle so the keep + compound + skeletons read against sky. */
    if (getenv("FORTVIEW")) {
        int fx, fy, fz;
        if (craft_gen_nearest_fort(cx, cz, seed, &fx, &fy, &fz)) {
            printf("fort at (%d,%d,%d)\n", fx, fy, fz);
            if (getenv("FORTDUMP")) {
                int ox = fx - 4, oz = fz - 4;   /* origin corner of 9×9 */
                for (int rel = 1; rel <= 10; rel++) {
                    printf("-- dy=%d --\n", rel);
                    for (int lz = 0; lz < 9; lz++) {
                        for (int lx = 0; lx < 9; lx++) {
                            BlockId b = craft_world_get(ox+lx, fy+rel, oz+lz);
                            putchar(b==BLK_STONE?'S':b==BLK_COBBLE?'#':
                                    b==BLK_GLASS?'o':b==BLK_CHEST?'C':
                                    b==BLK_AIR?'.':'?');
                        }
                        putchar('\n');
                    }
                }
            }
            /* High 3/4 aerial looking down at the fort, clamped to the
             * loaded window so the camera never leaves generated space. */
            float px = (float)fx - 9.0f, pz = (float)fz - 9.0f;
            float lo_x = craft_world_origin_x + 2.0f;
            float hi_x = craft_world_origin_x + CRAFT_WORLD_X - 2.0f;
            float lo_z = craft_world_origin_z + 2.0f;
            float hi_z = craft_world_origin_z + CRAFT_WORLD_Z - 2.0f;
            if (px < lo_x) px = lo_x; if (px > hi_x) px = hi_x;
            if (pz < lo_z) pz = lo_z; if (pz > hi_z) pz = hi_z;
            cam.pos.x = px; cam.pos.y = (float)fy + 17.0f;
            cam.pos.z = pz; cam.yaw = 0.785f; cam.pitch = -0.62f;
            cam.fov = 1.45f;
        } else {
            printf("no fort near (%d,%d)\n", cx, cz);
        }
    }

    /* VINEVIEW: find the vine block nearest the window centre and frame
     * its tree's canopy from a short distance, looking slightly up so the
     * hanging vines + canopy underside read clearly against sky. */
    if (getenv("VINEVIEW")) {
        int best = 1 << 30, vwx = 0, vwy = 0, vwz = 0; bool found = false;
        for (int llz = 0; llz < CRAFT_WORLD_Z; llz++)
            for (int llx = 0; llx < CRAFT_WORLD_X; llx++)
                for (int y = CRAFT_WORLD_Y - 2; y >= 1; y--) {
                    int wx = craft_world_origin_x + llx, wz = craft_world_origin_z + llz;
                    BlockId b = craft_world_get(wx, y, wz);
                    if (b == BLK_VINE || b == BLK_FLOWER_VINE) {
                        int d = (llx - CRAFT_WORLD_X/2)*(llx - CRAFT_WORLD_X/2) +
                                (llz - CRAFT_WORLD_Z/2)*(llz - CRAFT_WORLD_Z/2);
                        if (d < best) { best = d; vwx = wx; vwy = y; vwz = wz; found = true; }
                        break;
                    }
                }
        if (found) {
            printf("vine at (%d,%d,%d)\n", vwx, vwy, vwz);
            cam.pos.x = (float)vwx - 11.0f; cam.pos.y = (float)vwy + 3.0f;
            cam.pos.z = (float)vwz + 0.5f; cam.yaw = 1.5708f; cam.pitch = -0.08f;
            cam.fov = 1.1f;
        }
    }

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

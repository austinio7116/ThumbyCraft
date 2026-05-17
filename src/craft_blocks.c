/*
 * ThumbyCraft — block table + procedural texture atlas.
 *
 * Textures generated at startup so we don't burn flash on baked
 * pixel data. Patterns are intentionally chunky/painterly — the
 * raycaster samples at 16×16 native then any distance gives a
 * smooth Notch-ish look at 128px.
 */
#include "craft_blocks.h"
#include <string.h>

uint16_t craft_textures[CRAFT_TEX_COUNT * CRAFT_TEX_PIXELS];

/* Map (block, face) → slot index in the atlas. Slots layout:
 *   3 slots per block — 0 = top, 1 = side, 2 = bottom.
 * Most blocks reuse the side texture for all six faces; grass + wood
 * + sand differentiate top/side/bottom. */
static int slot_for(BlockId blk, Face face) {
    int base = blk * 3;
    switch (face) {
        case FACE_PY: return base + 0;     /* top    */
        case FACE_NY: return base + 2;     /* bottom */
        default:      return base + 1;     /* side (all four) */
    }
}

const uint16_t *craft_block_texture(BlockId blk, Face face) {
    int s = slot_for(blk, face);
    return &craft_textures[s * CRAFT_TEX_PIXELS];
}

const char *craft_block_name(BlockId blk) {
    switch (blk) {
        case BLK_AIR:    return "air";
        case BLK_STONE:  return "stone";
        case BLK_DIRT:   return "dirt";
        case BLK_GRASS:  return "grass";
        case BLK_SAND:   return "sand";
        case BLK_WOOD:   return "wood";
        case BLK_LEAVES: return "leaves";
        case BLK_WATER:  return "water";
        case BLK_COBBLE: return "cobble";
        case BLK_PLANK:  return "plank";
        case BLK_GLASS:  return "glass";
        default:         return "?";
    }
}

/* xorshift32 with a deterministic seed so each (block, face, pixel)
 * gets the same colour every boot — required for the renderer to
 * not shimmer between frames. */
static uint32_t xs32(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x; return x;
}

static void fill_solid(uint16_t *dst, uint16_t c) {
    for (int i = 0; i < CRAFT_TEX_PIXELS; i++) dst[i] = c;
}

/* Add ±jitter to each channel, clamped, write back. */
static void speckle(uint16_t *dst, uint32_t seed, int r, int g, int b, int jit) {
    uint32_t s = seed;
    for (int i = 0; i < CRAFT_TEX_PIXELS; i++) {
        int j = ((int)(xs32(&s) & 0xff) - 128) * jit / 128;
        dst[i] = rgb565(r + j, g + j, b + j);
    }
}

/* Brick-like mortar pattern at the grid lines — used by cobble. */
static void cobble_pattern(uint16_t *dst, uint32_t seed) {
    uint32_t s = seed;
    for (int y = 0; y < CRAFT_TEX_SIZE; y++) {
        for (int x = 0; x < CRAFT_TEX_SIZE; x++) {
            int rx = x % 8, ry = y % 4;
            int row = (y / 4) & 1;
            if (row) rx = (x + 4) % 8;
            int border = (ry == 0 || rx == 0);
            int base = border ? 70 : 130;
            int j = ((int)(xs32(&s) & 0x3f) - 32);
            int c = base + j;
            dst[y * CRAFT_TEX_SIZE + x] = rgb565(c, c, c);
        }
    }
}

/* Horizontal plank stripes. */
static void plank_pattern(uint16_t *dst, uint32_t seed) {
    uint32_t s = seed;
    for (int y = 0; y < CRAFT_TEX_SIZE; y++) {
        int band = y / 4;
        int base_r = 160 - band * 8;
        int base_g = 110 - band * 6;
        int base_b = 70  - band * 4;
        int edge = (y % 4 == 0);
        for (int x = 0; x < CRAFT_TEX_SIZE; x++) {
            int j = ((int)(xs32(&s) & 0x1f) - 16);
            int c_r = base_r + j - (edge ? 30 : 0);
            int c_g = base_g + j - (edge ? 25 : 0);
            int c_b = base_b + j - (edge ? 20 : 0);
            dst[y * CRAFT_TEX_SIZE + x] = rgb565(c_r, c_g, c_b);
        }
    }
}

/* Vertical wood-grain rings. */
static void wood_side_pattern(uint16_t *dst, uint32_t seed) {
    uint32_t s = seed;
    for (int y = 0; y < CRAFT_TEX_SIZE; y++) {
        for (int x = 0; x < CRAFT_TEX_SIZE; x++) {
            int xx = x - 8;
            int ring = (xx * xx);
            int base_r = 110 + (ring & 0x1f);
            int base_g = 70  + (ring & 0xf);
            int base_b = 35;
            int j = ((int)(xs32(&s) & 0x1f) - 16);
            dst[y * CRAFT_TEX_SIZE + x] =
                rgb565(base_r + j, base_g + j / 2, base_b + j / 4);
        }
    }
}

/* Water — striped pattern that suggests gentle wave bands. */
static void water_pattern(uint16_t *dst, uint32_t seed) {
    uint32_t s = seed;
    for (int y = 0; y < CRAFT_TEX_SIZE; y++) {
        int band = (y / 2) & 1 ? 20 : 0;
        for (int x = 0; x < CRAFT_TEX_SIZE; x++) {
            int j = ((int)(xs32(&s) & 0x1f) - 16);
            dst[y * CRAFT_TEX_SIZE + x] =
                rgb565(30 + j / 2, 90 + j / 2, 180 + band + j / 2);
        }
    }
}

void craft_blocks_animate_water(float t) {
    uint16_t *top  = &craft_textures[(BLK_WATER * 3 + 0) * CRAFT_TEX_PIXELS];
    uint16_t *side = &craft_textures[(BLK_WATER * 3 + 1) * CRAFT_TEX_PIXELS];
    int offset = (int)(t * 8.0f);
    for (int y = 0; y < CRAFT_TEX_SIZE; y++) {
        int phase = ((y + offset) / 2) & 1;
        int band = phase ? 18 : -6;
        for (int x = 0; x < CRAFT_TEX_SIZE; x++) {
            int jitter = (((x * 73 + y * 41 + offset * 13) & 0x1f) - 16) / 2;
            uint16_t c = rgb565(30 + jitter, 90 + jitter, 180 + band + jitter);
            side[y * CRAFT_TEX_SIZE + x] = c;
            top [y * CRAFT_TEX_SIZE + x] = c;
        }
    }
}

/* Glass — mostly transparent-feeling pale tile with a darker frame. */
static void glass_pattern(uint16_t *dst, uint32_t seed) {
    (void)seed;
    for (int y = 0; y < CRAFT_TEX_SIZE; y++) {
        for (int x = 0; x < CRAFT_TEX_SIZE; x++) {
            int border = (x == 0 || y == 0 || x == 15 || y == 15);
            int c = border ? 150 : 220;
            int b = border ? 170 : 235;
            dst[y * CRAFT_TEX_SIZE + x] = rgb565(c, c, b);
        }
    }
}

/* Leaves — clumpy green with sparse darker pixels for depth. */
static void leaves_pattern(uint16_t *dst, uint32_t seed) {
    uint32_t s = seed;
    for (int i = 0; i < CRAFT_TEX_PIXELS; i++) {
        uint32_t r = xs32(&s);
        int dark = (r & 0x7) == 0;
        int g = dark ? 60 : 110 + (int)(r & 0x1f);
        dst[i] = rgb565(20, g, 30);
    }
}

void craft_blocks_build_textures(void) {
    /* AIR slot is never sampled but zero-init the rows anyway. */
    fill_solid(&craft_textures[(BLK_AIR * 3 + 0) * CRAFT_TEX_PIXELS], 0);
    fill_solid(&craft_textures[(BLK_AIR * 3 + 1) * CRAFT_TEX_PIXELS], 0);
    fill_solid(&craft_textures[(BLK_AIR * 3 + 2) * CRAFT_TEX_PIXELS], 0);

    /* STONE — uniform speckled grey. */
    speckle(&craft_textures[(BLK_STONE * 3 + 0) * CRAFT_TEX_PIXELS], 0xC0FFEE, 130, 130, 130, 60);
    speckle(&craft_textures[(BLK_STONE * 3 + 1) * CRAFT_TEX_PIXELS], 0xC0FFEE, 130, 130, 130, 60);
    speckle(&craft_textures[(BLK_STONE * 3 + 2) * CRAFT_TEX_PIXELS], 0xC0FFEE, 130, 130, 130, 60);

    /* DIRT — earthy brown. */
    speckle(&craft_textures[(BLK_DIRT * 3 + 0) * CRAFT_TEX_PIXELS], 0xDABBED, 130, 90, 50, 50);
    speckle(&craft_textures[(BLK_DIRT * 3 + 1) * CRAFT_TEX_PIXELS], 0xDABBED, 130, 90, 50, 50);
    speckle(&craft_textures[(BLK_DIRT * 3 + 2) * CRAFT_TEX_PIXELS], 0xDABBED, 130, 90, 50, 50);

    /* GRASS — green top, dirt-with-green-edge side, dirt bottom. */
    speckle(&craft_textures[(BLK_GRASS * 3 + 0) * CRAFT_TEX_PIXELS], 0x1EAF1E, 70, 160, 50, 40);
    /* Side: paint dirt then green over top 5 rows. */
    {
        uint16_t *side = &craft_textures[(BLK_GRASS * 3 + 1) * CRAFT_TEX_PIXELS];
        speckle(side, 0xDABBED, 130, 90, 50, 50);
        uint32_t s = 0x6166;
        for (int y = 0; y < 5; y++) {
            for (int x = 0; x < CRAFT_TEX_SIZE; x++) {
                int j = ((int)(xs32(&s) & 0x3f) - 32);
                int gg = 150 + j / 2 - y * 8;
                int rr = 60 + j / 2;
                side[y * CRAFT_TEX_SIZE + x] = rgb565(rr, gg, 40);
            }
        }
    }
    speckle(&craft_textures[(BLK_GRASS * 3 + 2) * CRAFT_TEX_PIXELS], 0xDABBED, 130, 90, 50, 50);

    /* SAND — pale yellow. */
    speckle(&craft_textures[(BLK_SAND * 3 + 0) * CRAFT_TEX_PIXELS], 0x5A1, 220, 200, 130, 30);
    speckle(&craft_textures[(BLK_SAND * 3 + 1) * CRAFT_TEX_PIXELS], 0x5A1, 220, 200, 130, 30);
    speckle(&craft_textures[(BLK_SAND * 3 + 2) * CRAFT_TEX_PIXELS], 0x5A1, 220, 200, 130, 30);

    /* WOOD — ring on caps, grain on sides. */
    {
        uint16_t *top = &craft_textures[(BLK_WOOD * 3 + 0) * CRAFT_TEX_PIXELS];
        uint32_t s = 0xDEAD;
        for (int y = 0; y < CRAFT_TEX_SIZE; y++) {
            for (int x = 0; x < CRAFT_TEX_SIZE; x++) {
                int dx = x - 8, dy = y - 8;
                int r2 = dx * dx + dy * dy;
                int ring = (r2 / 4) & 3;
                int base = 110 - ring * 12;
                int j = ((int)(xs32(&s) & 0xf) - 8);
                top[y * CRAFT_TEX_SIZE + x] = rgb565(base + j, base * 7 / 10 + j, base * 4 / 10);
            }
        }
    }
    wood_side_pattern(&craft_textures[(BLK_WOOD * 3 + 1) * CRAFT_TEX_PIXELS], 0xBEEF);
    /* Bottom: same as top. */
    memcpy(&craft_textures[(BLK_WOOD * 3 + 2) * CRAFT_TEX_PIXELS],
           &craft_textures[(BLK_WOOD * 3 + 0) * CRAFT_TEX_PIXELS],
           sizeof(uint16_t) * CRAFT_TEX_PIXELS);

    /* LEAVES — clumpy green. */
    leaves_pattern(&craft_textures[(BLK_LEAVES * 3 + 0) * CRAFT_TEX_PIXELS], 0xACE);
    leaves_pattern(&craft_textures[(BLK_LEAVES * 3 + 1) * CRAFT_TEX_PIXELS], 0xACE);
    leaves_pattern(&craft_textures[(BLK_LEAVES * 3 + 2) * CRAFT_TEX_PIXELS], 0xACE);

    /* WATER — blue stripes. */
    water_pattern(&craft_textures[(BLK_WATER * 3 + 0) * CRAFT_TEX_PIXELS], 0x10ADED);
    water_pattern(&craft_textures[(BLK_WATER * 3 + 1) * CRAFT_TEX_PIXELS], 0x10ADED);
    water_pattern(&craft_textures[(BLK_WATER * 3 + 2) * CRAFT_TEX_PIXELS], 0x10ADED);

    /* COBBLESTONE — brick mortar pattern. */
    cobble_pattern(&craft_textures[(BLK_COBBLE * 3 + 0) * CRAFT_TEX_PIXELS], 0xC0B);
    cobble_pattern(&craft_textures[(BLK_COBBLE * 3 + 1) * CRAFT_TEX_PIXELS], 0xC0B);
    cobble_pattern(&craft_textures[(BLK_COBBLE * 3 + 2) * CRAFT_TEX_PIXELS], 0xC0B);

    /* PLANK — horizontal bands. */
    plank_pattern(&craft_textures[(BLK_PLANK * 3 + 0) * CRAFT_TEX_PIXELS], 0xFADE);
    plank_pattern(&craft_textures[(BLK_PLANK * 3 + 1) * CRAFT_TEX_PIXELS], 0xFADE);
    plank_pattern(&craft_textures[(BLK_PLANK * 3 + 2) * CRAFT_TEX_PIXELS], 0xFADE);

    /* GLASS — pale tile w/ darker frame. */
    glass_pattern(&craft_textures[(BLK_GLASS * 3 + 0) * CRAFT_TEX_PIXELS], 0);
    glass_pattern(&craft_textures[(BLK_GLASS * 3 + 1) * CRAFT_TEX_PIXELS], 0);
    glass_pattern(&craft_textures[(BLK_GLASS * 3 + 2) * CRAFT_TEX_PIXELS], 0);
}

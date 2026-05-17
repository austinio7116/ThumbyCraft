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

/* Atlas storage. In baked mode the bulk lives in flash (.rodata) and
 * the only writable bytes are the two slots that animate_water mutates
 * every frame. */
#ifdef CRAFT_TEXTURES_BAKED
extern const uint16_t craft_textures_baked[CRAFT_TEX_COUNT * CRAFT_TEX_PIXELS];
/* Slot 0 = water top, slot 1 = water side. animate_water writes here
 * and craft_block_texture redirects water lookups to this scratch. */
static uint16_t craft_water_anim_tex[2 * CRAFT_TEX_PIXELS];
#else
uint16_t craft_textures[CRAFT_TEX_COUNT * CRAFT_TEX_PIXELS];
#endif

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
#ifdef CRAFT_TEXTURES_BAKED
    /* Animated water — return the writable scratch instead of flash.
     * Bottom face (FACE_NY) is never animated, so it still uses
     * the baked copy. */
    if (blk == BLK_WATER) {
        if (face == FACE_PY) return &craft_water_anim_tex[0 * CRAFT_TEX_PIXELS];
        if (face != FACE_NY) return &craft_water_anim_tex[1 * CRAFT_TEX_PIXELS];
    }
    return &craft_textures_baked[s * CRAFT_TEX_PIXELS];
#else
    return &craft_textures[s * CRAFT_TEX_PIXELS];
#endif
}

const char *craft_block_name(BlockId blk) {
    switch (blk) {
        case BLK_AIR:           return "air";
        case BLK_STONE:         return "stone";
        case BLK_DIRT:          return "dirt";
        case BLK_GRASS:         return "grass";
        case BLK_SAND:          return "sand";
        case BLK_WOOD:          return "wood";
        case BLK_LEAVES:        return "leaves";
        case BLK_WATER:         return "water";
        case BLK_COBBLE:        return "cobble";
        case BLK_PLANK:         return "plank";
        case BLK_GLASS:         return "glass";
        case BLK_COAL_ORE:      return "coal ore";
        case BLK_TORCH:         return "torch";
        case BLK_IRON_ORE:      return "iron ore";
        case BLK_STICK:         return "stick";
        case BLK_IRON_INGOT:    return "iron";
        case BLK_PICKAXE_WOOD:  return "wood pick";
        case BLK_PICKAXE_STONE: return "stone pick";
        case BLK_PICKAXE_IRON:  return "iron pick";
        case BLK_SWORD_WOOD:    return "wood sword";
        case BLK_SWORD_STONE:   return "stone sword";
        case BLK_SWORD_IRON:    return "iron sword";
        case BLK_BOW:           return "bow";
        case BLK_ARROW:         return "arrow";
        default:                return "?";
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
#ifdef CRAFT_TEXTURES_BAKED
    uint16_t *top  = &craft_water_anim_tex[0 * CRAFT_TEX_PIXELS];
    uint16_t *side = &craft_water_anim_tex[1 * CRAFT_TEX_PIXELS];
#else
    uint16_t *top  = &craft_textures[(BLK_WATER * 3 + 0) * CRAFT_TEX_PIXELS];
    uint16_t *side = &craft_textures[(BLK_WATER * 3 + 1) * CRAFT_TEX_PIXELS];
#endif
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

#ifdef CRAFT_TEXTURES_BAKED
void craft_blocks_build_textures(void) {
    /* No-op — atlas lives in flash. We still need to seed the water
     * animation scratch with the baked water side/top so the first
     * frame before animate_water runs isn't garbage. */
    memcpy(&craft_water_anim_tex[0 * CRAFT_TEX_PIXELS],
           &craft_textures_baked[(BLK_WATER * 3 + 0) * CRAFT_TEX_PIXELS],
           sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    memcpy(&craft_water_anim_tex[1 * CRAFT_TEX_PIXELS],
           &craft_textures_baked[(BLK_WATER * 3 + 1) * CRAFT_TEX_PIXELS],
           sizeof(uint16_t) * CRAFT_TEX_PIXELS);
}
#else
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

    /* COAL ORE — speckled grey stone with dark clusters. */
    {
        uint16_t *side = &craft_textures[(BLK_COAL_ORE * 3 + 1) * CRAFT_TEX_PIXELS];
        speckle(side, 0xC0A1, 110, 110, 115, 50);
        /* Sprinkle ~6 coal clusters. */
        uint32_t s = 0xC0A100u;
        for (int n = 0; n < 24; n++) {
            int cx = (int)(xs32(&s) % CRAFT_TEX_SIZE);
            int cy = (int)(xs32(&s) % CRAFT_TEX_SIZE);
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    int x = cx + dx, y = cy + dy;
                    if ((unsigned)x >= CRAFT_TEX_SIZE) continue;
                    if ((unsigned)y >= CRAFT_TEX_SIZE) continue;
                    if ((xs32(&s) & 3) == 0) continue;
                    side[y * CRAFT_TEX_SIZE + x] = rgb565(20, 20, 25);
                }
            }
        }
        memcpy(&craft_textures[(BLK_COAL_ORE * 3 + 0) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
        memcpy(&craft_textures[(BLK_COAL_ORE * 3 + 2) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    }

    /* TORCH — thin brown stem with a bright orange flame at the top.
     * Drawn the same on all 6 faces (cube approximation). */
    {
        uint16_t *side = &craft_textures[(BLK_TORCH * 3 + 1) * CRAFT_TEX_PIXELS];
        /* Dim grey background — torches are see-through-ish in real
         * Minecraft, but our raycaster doesn't do partial transparency. */
        for (int i = 0; i < CRAFT_TEX_PIXELS; i++) side[i] = rgb565(20, 18, 25);
        /* Brown stem 2 px wide in the centre, rows 8..14. */
        for (int y = 8; y < 15; y++) {
            side[y * CRAFT_TEX_SIZE + 7] = rgb565(110, 70, 30);
            side[y * CRAFT_TEX_SIZE + 8] = rgb565(140, 95, 45);
        }
        /* Flame: 3-wide warm gradient near the top. */
        uint16_t f_core = rgb565(255, 230, 100);
        uint16_t f_mid  = rgb565(255, 170, 40);
        uint16_t f_low  = rgb565(220, 80, 20);
        side[3 * CRAFT_TEX_SIZE + 8] = f_low;
        for (int y = 4; y < 8; y++) {
            side[y * CRAFT_TEX_SIZE + 6] = (y == 7) ? f_low : 0;
            side[y * CRAFT_TEX_SIZE + 7] = (y == 4) ? f_mid : (y == 7 ? f_core : f_mid);
            side[y * CRAFT_TEX_SIZE + 8] = (y == 7) ? f_core : f_core;
            side[y * CRAFT_TEX_SIZE + 9] = (y == 7) ? f_low : f_mid;
            side[y * CRAFT_TEX_SIZE + 10] = (y == 7) ? f_low : 0;
        }
        memcpy(&craft_textures[(BLK_TORCH * 3 + 0) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
        memcpy(&craft_textures[(BLK_TORCH * 3 + 2) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    }

    /* IRON ORE — stone base with rusty orange flecks. */
    {
        uint16_t *side = &craft_textures[(BLK_IRON_ORE * 3 + 1) * CRAFT_TEX_PIXELS];
        speckle(side, 0x1207E, 130, 130, 130, 50);
        uint32_t s = 0x1207EBu;
        for (int n = 0; n < 18; n++) {
            int cx = (int)(xs32(&s) % CRAFT_TEX_SIZE);
            int cy = (int)(xs32(&s) % CRAFT_TEX_SIZE);
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    int x = cx + dx, y = cy + dy;
                    if ((unsigned)x >= CRAFT_TEX_SIZE) continue;
                    if ((unsigned)y >= CRAFT_TEX_SIZE) continue;
                    if ((xs32(&s) & 3) == 0) continue;
                    int j = (int)(xs32(&s) & 0x1F);
                    side[y * CRAFT_TEX_SIZE + x] =
                        rgb565(190 + j, 110 + j / 2, 50 + j / 4);
                }
            }
        }
        memcpy(&craft_textures[(BLK_IRON_ORE * 3 + 0) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
        memcpy(&craft_textures[(BLK_IRON_ORE * 3 + 2) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    }

    /* --- Inventory-only items — single-face icons, dark backdrop. */

    /* STICK — vertical thin brown line. */
    {
        uint16_t *side = &craft_textures[(BLK_STICK * 3 + 1) * CRAFT_TEX_PIXELS];
        for (int i = 0; i < CRAFT_TEX_PIXELS; i++) side[i] = rgb565(40, 40, 50);
        uint16_t a = rgb565(160, 110, 60), b = rgb565(120, 80, 40);
        for (int y = 2; y < 14; y++) {
            side[y * CRAFT_TEX_SIZE + 7] = a;
            side[y * CRAFT_TEX_SIZE + 8] = b;
        }
        memcpy(&craft_textures[(BLK_STICK * 3 + 0) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
        memcpy(&craft_textures[(BLK_STICK * 3 + 2) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    }

    /* IRON INGOT — horizontal silver bar. */
    {
        uint16_t *side = &craft_textures[(BLK_IRON_INGOT * 3 + 1) * CRAFT_TEX_PIXELS];
        for (int i = 0; i < CRAFT_TEX_PIXELS; i++) side[i] = rgb565(40, 40, 50);
        uint16_t a = rgb565(220, 220, 230), b = rgb565(170, 170, 180);
        for (int x = 3; x < 13; x++) {
            side[6 * CRAFT_TEX_SIZE + x] = b;
            side[7 * CRAFT_TEX_SIZE + x] = a;
            side[8 * CRAFT_TEX_SIZE + x] = a;
            side[9 * CRAFT_TEX_SIZE + x] = b;
        }
        memcpy(&craft_textures[(BLK_IRON_INGOT * 3 + 0) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
        memcpy(&craft_textures[(BLK_IRON_INGOT * 3 + 2) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    }

    /* Common tool icon — stone-style pickaxe with tunable head colour.
     * Non-static: rgb565 isn't a constant expression so a static
     * initializer wouldn't compile. Trivial stack init at startup. */
    const uint16_t TIER_PICK[3][2] = {
        { rgb565(155, 110, 60), rgb565(115, 80, 40) },
        { rgb565(130, 130, 135), rgb565(85, 85, 90) },
        { rgb565(225, 225, 235), rgb565(170, 170, 180) },
    };
    BlockId pick_ids[3] = { BLK_PICKAXE_WOOD, BLK_PICKAXE_STONE, BLK_PICKAXE_IRON };
    for (int tier = 0; tier < 3; tier++) {
        uint16_t *side = &craft_textures[(pick_ids[tier] * 3 + 1) * CRAFT_TEX_PIXELS];
        for (int i = 0; i < CRAFT_TEX_PIXELS; i++) side[i] = rgb565(40, 40, 50);
        uint16_t head   = TIER_PICK[tier][0];
        uint16_t head_d = TIER_PICK[tier][1];
        for (int x = 2; x < 14; x++) {
            side[2 * CRAFT_TEX_SIZE + x] = (x == 2 || x == 13) ? head_d : head;
            side[3 * CRAFT_TEX_SIZE + x] = head;
        }
        side[4 * CRAFT_TEX_SIZE + 7] = head_d;
        side[4 * CRAFT_TEX_SIZE + 8] = head_d;
        uint16_t wood = rgb565(150, 100, 50), wood_d = rgb565(110, 70, 35);
        for (int i = 0; i < 10; i++) {
            int x = 7 + i / 2;
            int y = 5 + i;
            if ((unsigned)x < CRAFT_TEX_SIZE && (unsigned)y < CRAFT_TEX_SIZE)
                side[y * CRAFT_TEX_SIZE + x] = (i & 1) ? wood : wood_d;
            x++;
            if ((unsigned)x < CRAFT_TEX_SIZE && (unsigned)y < CRAFT_TEX_SIZE)
                side[y * CRAFT_TEX_SIZE + x] = wood;
        }
        memcpy(&craft_textures[(pick_ids[tier] * 3 + 0) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
        memcpy(&craft_textures[(pick_ids[tier] * 3 + 2) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    }

    const uint16_t TIER_SWORD[3][2] = {
        { rgb565(170, 130, 70), rgb565(120, 90, 50) },
        { rgb565(140, 140, 145), rgb565(95, 95, 100) },
        { rgb565(230, 230, 240), rgb565(170, 170, 180) },
    };
    BlockId sword_ids[3] = { BLK_SWORD_WOOD, BLK_SWORD_STONE, BLK_SWORD_IRON };
    for (int tier = 0; tier < 3; tier++) {
        uint16_t *side = &craft_textures[(sword_ids[tier] * 3 + 1) * CRAFT_TEX_PIXELS];
        for (int i = 0; i < CRAFT_TEX_PIXELS; i++) side[i] = rgb565(40, 40, 50);
        uint16_t blade = TIER_SWORD[tier][0], blade_d = TIER_SWORD[tier][1];
        /* Blade column, rows 2..10. */
        for (int y = 2; y < 11; y++) {
            side[y * CRAFT_TEX_SIZE + 7] = blade_d;
            side[y * CRAFT_TEX_SIZE + 8] = blade;
        }
        /* Tip — taper at top. */
        side[1 * CRAFT_TEX_SIZE + 8] = blade;
        /* Hilt cross at row 11-12. */
        uint16_t hilt = rgb565(130, 95, 50);
        for (int x = 5; x < 11; x++) side[11 * CRAFT_TEX_SIZE + x] = hilt;
        /* Handle below hilt. */
        uint16_t handle = rgb565(115, 75, 40);
        side[12 * CRAFT_TEX_SIZE + 7] = handle;
        side[12 * CRAFT_TEX_SIZE + 8] = handle;
        side[13 * CRAFT_TEX_SIZE + 7] = handle;
        side[13 * CRAFT_TEX_SIZE + 8] = handle;
        memcpy(&craft_textures[(sword_ids[tier] * 3 + 0) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
        memcpy(&craft_textures[(sword_ids[tier] * 3 + 2) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    }

    /* BOW — horizontal C-curve in wood with a vertical white string. */
    {
        uint16_t *side = &craft_textures[(BLK_BOW * 3 + 1) * CRAFT_TEX_PIXELS];
        for (int i = 0; i < CRAFT_TEX_PIXELS; i++) side[i] = rgb565(40, 40, 50);
        uint16_t wood = rgb565(140, 95, 45), wood_d = rgb565(100, 65, 30);
        uint16_t str  = rgb565(220, 220, 230);
        /* Curve arch — two arcs from rows 3..13, columns 4..6 (top) and
         * 9..11 (bottom), with a back rail at col 4 / 11. */
        for (int y = 3; y < 6; y++) {
            side[y * CRAFT_TEX_SIZE + 5] = wood;
            side[y * CRAFT_TEX_SIZE + 4] = wood_d;
        }
        for (int y = 11; y < 14; y++) {
            side[y * CRAFT_TEX_SIZE + 5] = wood;
            side[y * CRAFT_TEX_SIZE + 4] = wood_d;
        }
        for (int y = 5; y < 12; y++) {
            side[y * CRAFT_TEX_SIZE + 3] = wood;
        }
        /* Bowstring — vertical white line on the inner side. */
        for (int y = 3; y < 14; y++) {
            side[y * CRAFT_TEX_SIZE + 7] = str;
        }
        memcpy(&craft_textures[(BLK_BOW * 3 + 0) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
        memcpy(&craft_textures[(BLK_BOW * 3 + 2) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    }

    /* ARROW — diagonal shaft with white flight + dark tip. */
    {
        uint16_t *side = &craft_textures[(BLK_ARROW * 3 + 1) * CRAFT_TEX_PIXELS];
        for (int i = 0; i < CRAFT_TEX_PIXELS; i++) side[i] = rgb565(40, 40, 50);
        uint16_t shaft = rgb565(150, 110, 70);
        uint16_t tip   = rgb565(80, 80, 90);
        uint16_t fletch= rgb565(230, 230, 230);
        /* Shaft running TL → BR. */
        for (int i = 2; i < 14; i++) {
            int x = i, y = i;
            if ((unsigned)x < CRAFT_TEX_SIZE && (unsigned)y < CRAFT_TEX_SIZE)
                side[y * CRAFT_TEX_SIZE + x] = shaft;
        }
        /* Tip cluster at top-left. */
        side[1 * CRAFT_TEX_SIZE + 1] = tip;
        side[2 * CRAFT_TEX_SIZE + 1] = tip;
        side[1 * CRAFT_TEX_SIZE + 2] = tip;
        /* Fletching cluster at bottom-right. */
        side[13 * CRAFT_TEX_SIZE + 14] = fletch;
        side[14 * CRAFT_TEX_SIZE + 13] = fletch;
        side[14 * CRAFT_TEX_SIZE + 14] = fletch;
        memcpy(&craft_textures[(BLK_ARROW * 3 + 0) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
        memcpy(&craft_textures[(BLK_ARROW * 3 + 2) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    }
}
#endif /* CRAFT_TEXTURES_BAKED */

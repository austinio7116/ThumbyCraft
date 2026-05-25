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
#include <math.h>

/* Atlas storage. In baked mode the bulk lives in flash (.rodata) and
 * the only writable bytes are the two slots that animate_water mutates
 * every frame. */
#ifdef CRAFT_TEXTURES_BAKED
extern const uint16_t craft_textures_baked[CRAFT_TEX_COUNT * CRAFT_TEX_PIXELS];
/* Two pre-rendered water frames, [frame][face: 0=top,1=side]. The
 * frames are visibly distinct (different jitter + band offset) so the
 * alternation reads as obvious surface motion. animate_water just
 * toggles which one craft_block_texture returns. */
static uint16_t craft_water_frames[2][2 * CRAFT_TEX_PIXELS];
static int      craft_water_frame_idx;
static bool     craft_water_frames_built;
/* Lava uses the same 2-frame animated-tile scheme as water (top + side
 * share one tile each). Toggled at ~2 Hz — lava crawls slower than
 * water. */
static uint16_t craft_lava_frames[2][CRAFT_TEX_PIXELS];
static int      craft_lava_frame_idx;
/* Portal — swirling purple, two animated frames like lava. */
static uint16_t craft_portal_frames[2][CRAFT_TEX_PIXELS];
static int      craft_portal_frame_idx;
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
    /* Map state-variant IDs to the atlas slot of their base. All
     * water levels share BLK_WATER_L0's animated 2-frame slot;
     * redstone _ON variants currently share their _OFF texture
     * (per-state visuals are a follow-up — the IDs are already in
     * place, just the per-variant baking still TBD). */
    BlockId tex_blk = blk;
    if (craft_is_water_id((uint8_t)blk))    tex_blk = BLK_WATER_L0;
    else if (craft_is_lava_id((uint8_t)blk)) tex_blk = BLK_LAVA;
    else if (blk == BLK_OBSERVER_ON)        tex_blk = BLK_OBSERVER;
    else if (blk == BLK_NOTE_BLOCK_ON)      tex_blk = BLK_NOTE_BLOCK;
    else if (blk == BLK_NOT_GATE_ON)        tex_blk = BLK_NOT_GATE;
    else if (blk == BLK_DELAY_ON)           tex_blk = BLK_DELAY;
    else if (blk == BLK_DISPENSER_ON)       tex_blk = BLK_DISPENSER;
    else if (blk == BLK_TARGET_ON)          tex_blk = BLK_TARGET;
    else if (blk == BLK_STICKY_PISTON_OFF)  tex_blk = BLK_PISTON_OFF;
    else if (blk == BLK_STICKY_PISTON_ON)   tex_blk = BLK_PISTON_ON;
    int s = slot_for(tex_blk, face);
#ifdef CRAFT_TEXTURES_BAKED
    if (tex_blk == BLK_WATER_L0) {
        int fr = craft_water_frame_idx & 1;
        if (face == FACE_PY) return &craft_water_frames[fr][0 * CRAFT_TEX_PIXELS];
        if (face != FACE_NY) return &craft_water_frames[fr][1 * CRAFT_TEX_PIXELS];
    }
    if (tex_blk == BLK_LAVA)   return craft_lava_frames[craft_lava_frame_idx & 1];
    if (tex_blk == BLK_PORTAL) return craft_portal_frames[craft_portal_frame_idx & 1];
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
        case BLK_WATER_L0:      return "water";
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
        case BLK_FURNACE:       return "furnace";
        case BLK_CHEST:         return "chest";
        case BLK_SILVER_ORE:    return "silver ore";
        case BLK_GOLD_ORE:      return "gold ore";
        case BLK_DIAMOND_ORE:   return "diamond ore";
        case BLK_REDSTONE_ORE:  return "redstone ore";
        case BLK_SILVER_INGOT:  return "silver";
        case BLK_GOLD_INGOT:    return "gold";
        case BLK_DIAMOND:       return "diamond";
        case BLK_REDSTONE:      return "redstone";
        case BLK_PICKAXE_SILVER:  return "silver pick";
        case BLK_PICKAXE_GOLD:    return "gold pick";
        case BLK_PICKAXE_DIAMOND: return "diamond pick";
        case BLK_SWORD_SILVER:    return "silver sword";
        case BLK_SWORD_GOLD:      return "gold sword";
        case BLK_SWORD_DIAMOND:   return "diamond sword";
        case BLK_SILVER_BLOCK:    return "silver block";
        case BLK_GOLD_BLOCK:      return "gold block";
        case BLK_DIAMOND_BLOCK:   return "diamond block";
        case BLK_REDSTONE_BLOCK:  return "redstone block";
        case BLK_LEVER_OFF:       return "lever";
        case BLK_LEVER_ON:        return "lever (on)";
        case BLK_REDSTONE_WIRE:
        case BLK_REDSTONE_WIRE_ON: return "wire";
        case BLK_LADDER:        return "ladder";
        case BLK_TRAPDOOR_OFF:
        case BLK_TRAPDOOR_ON:   return "trapdoor";
        case BLK_DOOR_OFF:
        case BLK_DOOR_ON:       return "door";
        case BLK_PRESSURE_PAD:  return "pressure pad";
        case BLK_PISTON_OFF:
        case BLK_PISTON_ON:     return "piston";
        case BLK_PISTON_ARM:    return "piston arm";
        case BLK_STICKY_PISTON_OFF:
        case BLK_STICKY_PISTON_ON: return "sticky piston";
        case BLK_TNT:           return "TNT";
        case BLK_TNT_FUSED:     return "TNT!";
        case BLK_OBSERVER:
        case BLK_OBSERVER_ON:   return "observer";
        case BLK_NOTE_BLOCK:
        case BLK_NOTE_BLOCK_ON: return "note block";
        case BLK_LAMP:
        case BLK_LAMP_ON:       return "lamp";
        case BLK_NOT_GATE:
        case BLK_NOT_GATE_ON:   return "NOT gate";
        case BLK_DELAY:
        case BLK_DELAY_ON:      return "delay";
        case BLK_DISPENSER:
        case BLK_DISPENSER_ON:  return "dispenser";
        case BLK_TARGET:
        case BLK_TARGET_ON:     return "target";
        case BLK_SLIME_BLOCK:   return "slime block";
        case BLK_SLIMEBALL:     return "slimeball";
        case BLK_SNOW:          return "snow";
        case BLK_SNOWY_ROCK:    return "snowy rock";
        case BLK_ICE:           return "ice";
        case BLK_LAVA:          return "lava";
        case BLK_LAVA_L1:
        case BLK_LAVA_L2:
        case BLK_LAVA_L3:       return "lava";
        case BLK_OBSIDIAN:      return "obsidian";
        case BLK_GRAVEL:        return "gravel";
        case BLK_FLINT:         return "flint";
        case BLK_PORTAL:        return "portal";
        case BLK_SANDSTONE:     return "sandstone";
        case BLK_CACTUS:        return "cactus";
        case BLK_VINE:          return "vine";
        case BLK_LILY_PAD:      return "lily pad";
        case BLK_WATER_L1:
        case BLK_WATER_L2:
        case BLK_WATER_L3:
        case BLK_WATER_L4:
        case BLK_WATER_L5:
        case BLK_WATER_L6:
        case BLK_WATER_L7:      return "water";
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

/* Bake one water frame into out_top + out_side. Variant 0 / 1 produce
 * two visibly distinct patterns (different band offset + different
 * jitter seed) that alternate to give clear surface motion. */
static void bake_water_frame(uint16_t *out_top, uint16_t *out_side, int variant) {
    uint32_t s = 0xC0FFEEu ^ (variant ? 0x9E3779B9u : 0u);
    int band_shift = variant ? 1 : 0;
    for (int y = 0; y < CRAFT_TEX_SIZE; y++) {
        int phase = ((y + band_shift) / 2) & 1;
        int band  = phase ? 18 : -6;
        for (int x = 0; x < CRAFT_TEX_SIZE; x++) {
            int j = ((int)(xs32(&s) & 0x1f) - 16) / 2;
            uint16_t c = rgb565(30 + j, 90 + j, 180 + band + j);
            out_side[y * CRAFT_TEX_SIZE + x] = c;
            out_top [y * CRAFT_TEX_SIZE + x] = c;
        }
    }
}

/* Molten lava tile — cracked basalt crust: dark cooled plates split by
 * a glowing network of cracks (Voronoi cell boundaries). High contrast
 * (near-black crust vs white-hot cracks), connected crack lines rather
 * than stripes or scattered spots. Seeds wrap across the 16px edge so
 * the pattern tiles seamlessly between adjacent lava cells. The variant
 * nudges seed positions + crack glow so the cracks shimmer/crawl
 * between the two animation frames. One tile serves every face. */
static void bake_lava_frame(uint16_t *out, int variant) {
    enum { LAVA_SEEDS = 13 };
    float sx[LAVA_SEEDS], sy[LAVA_SEEDS];
    uint32_t s = 0x1A1AF0u ^ (variant ? 0x9E3779B9u : 0u);
    for (int i = 0; i < LAVA_SEEDS; i++) {
        sx[i] = (float)(xs32(&s) & 0xFF) * (16.0f / 256.0f);
        sy[i] = (float)(xs32(&s) & 0xFF) * (16.0f / 256.0f);
    }
    const float F = 6.2831853f / 16.0f;   /* base freq — periodic over 16px */
    float ph   = variant ? 1.7f : 0.0f;   /* frame phase → the swirl crawls */
    int   glow = variant ? 12 : 0;        /* crack brightness pulse */
    for (int y = 0; y < CRAFT_TEX_SIZE; y++) {
        for (int x = 0; x < CRAFT_TEX_SIZE; x++) {
            /* Domain warp — two octaves of sinusoidal swirl. All terms
             * are periodic over 16px, so the tile still wraps seamlessly
             * between adjacent lava cells, but the Voronoi boundaries
             * bend into organic, varied curves instead of straight
             * polygon edges. */
            float wx = x + 3.2f * sinf(F * 2.0f * y + ph)
                         + 1.7f * sinf(F * 3.0f * y - ph * 0.5f);
            float wy = y + 3.2f * cosf(F * 2.0f * x - ph)
                         + 1.7f * cosf(F * 3.0f * x + ph * 0.5f);
            float d1 = 1e9f, d2 = 1e9f;   /* nearest + second-nearest seed */
            for (int i = 0; i < LAVA_SEEDS; i++) {
                float dx = wx - sx[i], dy = wy - sy[i];
                dx -= 16.0f * floorf(dx / 16.0f + 0.5f);   /* toroidal wrap */
                dy -= 16.0f * floorf(dy / 16.0f + 0.5f);
                float dd = dx * dx + dy * dy;
                if (dd < d1)      { d2 = d1; d1 = dd; }
                else if (dd < d2) { d2 = dd; }
            }
            float edge = d2 - d1;         /* small → on a plate boundary */
            /* Low-frequency "temperature" field — some patches of crust
             * glow warmer than others, breaking up flat dark plates. */
            float warm = 0.5f + 0.5f * sinf(F * (x + y) + ph * 1.3f);
            uint32_t n = (uint32_t)(x * 73856093) ^ (uint32_t)(y * 19349663);
            int j = (int)(n & 7);         /* faint per-pixel break-up */
            int rr, gg, bb;
            if (edge < 16.0f) {           /* glowing crack */
                int hot = (int)(16.0f - edge);   /* 0..16, hottest at seam */
                rr = 188 + hot * 4 + glow;       /* → ~255 white-hot */
                gg = 55  + hot * 9 + glow;       /* → ~200 */
                bb = 8   + hot * 4;              /* → ~72 */
            } else {                      /* dark cooled crust */
                int crust = d1 > 26.0f ? 26 : (int)d1;
                rr = 28 + (int)(warm * 26.0f) + crust / 2 + j;  /* ~28..70 */
                gg = 6  + (int)(warm * 8.0f)  + crust / 3;      /* ~6..20 */
                bb = 3;
            }
            if (rr < 0) rr = 0; if (rr > 255) rr = 255;
            if (gg < 0) gg = 0; if (gg > 255) gg = 255;
            if (bb < 0) bb = 0; if (bb > 255) bb = 255;
            out[y * CRAFT_TEX_SIZE + x] = rgb565(rr, gg, bb);
        }
    }
}

/* Frozen ice tile — the same domain-warped Voronoi as lava, but static
 * and blue: mid-blue cracked plates split by bright frosty white-blue
 * seams, with a low-frequency shade field so no two plates look alike.
 * Periodic over 16px (toroidal seeds + period-16 warp), so it tiles
 * seamlessly and the renderer's per-cell offset/flip works on it too. */
static void bake_ice_tile(uint16_t *out) {
    enum { ICE_SEEDS = 13 };
    float sx[ICE_SEEDS], sy[ICE_SEEDS];
    uint32_t s = 0x1CE50Fu;
    for (int i = 0; i < ICE_SEEDS; i++) {
        sx[i] = (float)(xs32(&s) & 0xFF) * (16.0f / 256.0f);
        sy[i] = (float)(xs32(&s) & 0xFF) * (16.0f / 256.0f);
    }
    const float F = 6.2831853f / 16.0f;
    for (int y = 0; y < CRAFT_TEX_SIZE; y++) {
        for (int x = 0; x < CRAFT_TEX_SIZE; x++) {
            float wx = x + 3.0f * sinf(F * 2.0f * y)
                         + 1.6f * sinf(F * 3.0f * y);
            float wy = y + 3.0f * cosf(F * 2.0f * x)
                         + 1.6f * cosf(F * 3.0f * x);
            float d1 = 1e9f, d2 = 1e9f;
            for (int i = 0; i < ICE_SEEDS; i++) {
                float dx = wx - sx[i], dy = wy - sy[i];
                dx -= 16.0f * floorf(dx / 16.0f + 0.5f);
                dy -= 16.0f * floorf(dy / 16.0f + 0.5f);
                float dd = dx * dx + dy * dy;
                if (dd < d1)      { d2 = d1; d1 = dd; }
                else if (dd < d2) { d2 = dd; }
            }
            float edge = d2 - d1;
            float cool = 0.5f + 0.5f * sinf(F * (x + y));   /* low-freq shade */
            uint32_t n = (uint32_t)(x * 73856093) ^ (uint32_t)(y * 19349663);
            int j = (int)(n & 7);
            int rr, gg, bb;
            if (edge < 16.0f) {                 /* bright frosty crack seam */
                int hot = (int)(16.0f - edge);  /* 0..16, brightest at seam */
                rr = 150 + hot * 6;             /* → ~246, near-white */
                gg = 198 + hot * 3;             /* → ~246 */
                bb = 235 + hot;                 /* → ~251 */
            } else {                            /* mid-blue ice plate */
                int frost = d1 > 30.0f ? 30 : (int)d1;   /* paler toward centre */
                rr = 58 + (int)(cool * 34.0f) + frost / 2 + j;   /* ~58..110 */
                gg = 124 + (int)(cool * 34.0f) + frost / 2 + j;  /* ~124..175 */
                bb = 198 + (int)(cool * 26.0f) + frost / 4;      /* ~198..230 */
            }
            if (rr < 0) rr = 0; if (rr > 255) rr = 255;
            if (gg < 0) gg = 0; if (gg > 255) gg = 255;
            if (bb < 0) bb = 0; if (bb > 255) bb = 255;
            out[y * CRAFT_TEX_SIZE + x] = rgb565(rr, gg, bb);
        }
    }
}

/* Portal — a swirling purple haze with bright sparkle flecks. Two
 * octaves of periodic sinusoidal warp (seamless over 16px) drive
 * violet brightness bands that swirl; the variant offsets the phase so
 * the swirl rotates, and the sparkles twinkle between frames. */
static void bake_portal_frame(uint16_t *out, int variant) {
    const float F = 6.2831853f / 16.0f;
    float ph = variant ? 2.1f : 0.0f;
    uint32_t s = 0x90A7A1u ^ (variant ? 0x9E3779B9u : 0u);
    for (int y = 0; y < CRAFT_TEX_SIZE; y++) {
        for (int x = 0; x < CRAFT_TEX_SIZE; x++) {
            float wx = x + 3.0f * sinf(F * 2.0f * y + ph)
                         + 1.6f * sinf(F * 3.0f * y - ph);
            float wy = y + 3.0f * cosf(F * 2.0f * x - ph)
                         + 1.6f * cosf(F * 3.0f * x + ph);
            float swirl = sinf(F * (wx + wy)) * 0.5f
                        + sinf(F * 2.0f * wx - ph) * 0.5f;   /* -1..1 */
            int p = (int)(96.0f + 70.0f * swirl);            /* ~26..166 */
            if (p < 0) p = 0;
            int rr = p + 40;          /* violet: strong R + B, weak G */
            int gg = p / 4;
            int bb = p + 70;
            uint32_t r = xs32(&s);
            if ((r & 0x3f) == 0) {    /* sparse twinkle */
                rr = 235; gg = 200; bb = 255;
            }
            if (rr < 0) rr = 0; if (rr > 255) rr = 255;
            if (gg < 0) gg = 0; if (gg > 255) gg = 255;
            if (bb < 0) bb = 0; if (bb > 255) bb = 255;
            out[y * CRAFT_TEX_SIZE + x] = rgb565(rr, gg, bb);
        }
    }
}

void craft_blocks_animate_water(float t) {
#ifdef CRAFT_TEXTURES_BAKED
    /* Lazy first-time build — guard for the case where build_textures
     * hasn't run yet (shouldn't normally happen, but harmless). */
    if (!craft_water_frames_built) {
        bake_water_frame(&craft_water_frames[0][0 * CRAFT_TEX_PIXELS],
                         &craft_water_frames[0][1 * CRAFT_TEX_PIXELS], 0);
        bake_water_frame(&craft_water_frames[1][0 * CRAFT_TEX_PIXELS],
                         &craft_water_frames[1][1 * CRAFT_TEX_PIXELS], 1);
        bake_lava_frame(craft_lava_frames[0], 0);
        bake_lava_frame(craft_lava_frames[1], 1);
        bake_portal_frame(craft_portal_frames[0], 0);
        bake_portal_frame(craft_portal_frames[1], 1);
        craft_water_frames_built = true;
    }
    /* 4 Hz toggle — same cadence as classic Minecraft water. */
    craft_water_frame_idx = ((int)(t * 4.0f)) & 1;
    /* Lava crawls slower — 2 Hz. */
    craft_lava_frame_idx = ((int)(t * 2.0f)) & 1;
    /* Portal swirls at 3 Hz. */
    craft_portal_frame_idx = ((int)(t * 3.0f)) & 1;
#else
    /* Unbaked (host development) path — write the chosen frame
     * straight into the atlas slots used by the renderer. */
    uint16_t *top  = &craft_textures[(BLK_WATER * 3 + 0) * CRAFT_TEX_PIXELS];
    uint16_t *side = &craft_textures[(BLK_WATER * 3 + 1) * CRAFT_TEX_PIXELS];
    int variant = ((int)(t * 4.0f)) & 1;
    bake_water_frame(top, side, variant);
    int lvar = ((int)(t * 2.0f)) & 1;
    bake_lava_frame(&craft_textures[(BLK_LAVA * 3 + 0) * CRAFT_TEX_PIXELS], lvar);
    bake_lava_frame(&craft_textures[(BLK_LAVA * 3 + 1) * CRAFT_TEX_PIXELS], lvar);
    bake_lava_frame(&craft_textures[(BLK_LAVA * 3 + 2) * CRAFT_TEX_PIXELS], lvar);
    int pvar = ((int)(t * 3.0f)) & 1;
    bake_portal_frame(&craft_textures[(BLK_PORTAL * 3 + 0) * CRAFT_TEX_PIXELS], pvar);
    bake_portal_frame(&craft_textures[(BLK_PORTAL * 3 + 1) * CRAFT_TEX_PIXELS], pvar);
    bake_portal_frame(&craft_textures[(BLK_PORTAL * 3 + 2) * CRAFT_TEX_PIXELS], pvar);
#endif
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
    /* Atlas lives in flash; we just need to bake the two animated
     * water frames once into the in-RAM frame pair. */
    bake_water_frame(&craft_water_frames[0][0 * CRAFT_TEX_PIXELS],
                     &craft_water_frames[0][1 * CRAFT_TEX_PIXELS], 0);
    bake_water_frame(&craft_water_frames[1][0 * CRAFT_TEX_PIXELS],
                     &craft_water_frames[1][1 * CRAFT_TEX_PIXELS], 1);
    bake_lava_frame(craft_lava_frames[0], 0);
    bake_lava_frame(craft_lava_frames[1], 1);
    bake_portal_frame(craft_portal_frames[0], 0);
    bake_portal_frame(craft_portal_frames[1], 1);
    craft_water_frames_built = true;
    craft_water_frame_idx = 0;
    craft_lava_frame_idx = 0;
    craft_portal_frame_idx = 0;
}
#else
/* Generate an ore tile: speckled-stone base with ~18 small material
 * flecks. Mirrored to all three faces. */
static void ore_with_flecks(BlockId blk, uint32_t seed,
                            int fr, int fg, int fb) {
    uint16_t *side = &craft_textures[(blk * 3 + 1) * CRAFT_TEX_PIXELS];
    speckle(side, seed, 130, 130, 130, 50);
    uint32_t s = seed ^ 0xABCD0001u;
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
                int r = fr + j, g = fg + j, b = fb + j;
                if (r > 255) r = 255;
                if (g > 255) g = 255;
                if (b > 255) b = 255;
                side[y * CRAFT_TEX_SIZE + x] = rgb565(r, g, b);
            }
        }
    }
    memcpy(&craft_textures[(blk * 3 + 0) * CRAFT_TEX_PIXELS],
           side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    memcpy(&craft_textures[(blk * 3 + 2) * CRAFT_TEX_PIXELS],
           side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
}

/* Inventory item tile: dark backdrop + a horizontal "bar" of two
 * shades from row 6 to 9, cols 3..12. Used by ingots; pale+dark
 * gives a metallic look. */
static void ingot_bar_tex(BlockId blk, int r, int g, int b) {
    uint16_t *side = &craft_textures[(blk * 3 + 1) * CRAFT_TEX_PIXELS];
    for (int i = 0; i < CRAFT_TEX_PIXELS; i++) side[i] = rgb565(40, 40, 50);
    uint16_t bright = rgb565(r, g, b);
    int dr = r * 3 / 4, dg = g * 3 / 4, db = b * 3 / 4;
    uint16_t dark   = rgb565(dr, dg, db);
    for (int x = 3; x < 13; x++) {
        side[6 * CRAFT_TEX_SIZE + x] = dark;
        side[7 * CRAFT_TEX_SIZE + x] = bright;
        side[8 * CRAFT_TEX_SIZE + x] = bright;
        side[9 * CRAFT_TEX_SIZE + x] = dark;
    }
    memcpy(&craft_textures[(blk * 3 + 0) * CRAFT_TEX_PIXELS],
           side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    memcpy(&craft_textures[(blk * 3 + 2) * CRAFT_TEX_PIXELS],
           side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
}

/* Inventory item tile: dark backdrop + a diamond-shaped gem. */
static void gem_tex(BlockId blk, int r, int g, int b) {
    uint16_t *side = &craft_textures[(blk * 3 + 1) * CRAFT_TEX_PIXELS];
    for (int i = 0; i < CRAFT_TEX_PIXELS; i++) side[i] = rgb565(40, 40, 50);
    uint16_t bright = rgb565(r, g, b);
    int dr = r * 3 / 4, dg = g * 3 / 4, db = b * 3 / 4;
    uint16_t dark   = rgb565(dr, dg, db);
    int hr = r + (255 - r) / 2, hg = g + (255 - g) / 2, hb = b + (255 - b) / 2;
    uint16_t hi = rgb565(hr, hg, hb);
    /* Diamond outline rows: spread = |row - 7|. */
    for (int y = 3; y < 13; y++) {
        int spread = (y >= 8) ? (12 - y) : (y - 3);
        for (int dx = -spread; dx <= spread; dx++) {
            int x = 7 + dx;
            if ((unsigned)x >= CRAFT_TEX_SIZE) continue;
            uint16_t c = bright;
            if (dx == -spread || dx == spread) c = dark;
            if (dx == -spread + 1 && y >= 4 && y <= 7) c = hi;
            side[y * CRAFT_TEX_SIZE + x] = c;
        }
    }
    memcpy(&craft_textures[(blk * 3 + 0) * CRAFT_TEX_PIXELS],
           side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    memcpy(&craft_textures[(blk * 3 + 2) * CRAFT_TEX_PIXELS],
           side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
}

/* Inventory item tile: dark backdrop + 8 scattered specks of `c`. */
static void dust_tex(BlockId blk, uint32_t seed, int r, int g, int b) {
    uint16_t *side = &craft_textures[(blk * 3 + 1) * CRAFT_TEX_PIXELS];
    for (int i = 0; i < CRAFT_TEX_PIXELS; i++) side[i] = rgb565(40, 40, 50);
    uint16_t c = rgb565(r, g, b);
    uint32_t s = seed;
    for (int n = 0; n < 14; n++) {
        int cx = 3 + (int)(xs32(&s) % 10);
        int cy = 3 + (int)(xs32(&s) % 10);
        side[cy * CRAFT_TEX_SIZE + cx] = c;
        if ((xs32(&s) & 1) && cx + 1 < CRAFT_TEX_SIZE)
            side[cy * CRAFT_TEX_SIZE + cx + 1] = c;
    }
    memcpy(&craft_textures[(blk * 3 + 0) * CRAFT_TEX_PIXELS],
           side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    memcpy(&craft_textures[(blk * 3 + 2) * CRAFT_TEX_PIXELS],
           side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
}

/* Storage block (silver/gold/diamond/redstone block): solid colour
 * with a slightly-darker border so faces visibly separate. Same on
 * top / side / bottom. */
static void solid_block_tex(BlockId blk, int r, int g, int b) {
    uint16_t bright = rgb565(r, g, b);
    int dr = r * 5 / 6, dg = g * 5 / 6, db = b * 5 / 6;
    uint16_t mid    = rgb565(dr, dg, db);
    int br = r * 2 / 3, bg = g * 2 / 3, bb = b * 2 / 3;
    uint16_t dark   = rgb565(br, bg, bb);
    uint16_t *side = &craft_textures[(blk * 3 + 1) * CRAFT_TEX_PIXELS];
    for (int y = 0; y < CRAFT_TEX_SIZE; y++) {
        for (int x = 0; x < CRAFT_TEX_SIZE; x++) {
            uint16_t c = bright;
            if (x == 0 || y == 0 || x == 15 || y == 15) c = dark;
            else if ((x ^ y) & 1) c = mid;
            side[y * CRAFT_TEX_SIZE + x] = c;
        }
    }
    memcpy(&craft_textures[(blk * 3 + 0) * CRAFT_TEX_PIXELS],
           side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    memcpy(&craft_textures[(blk * 3 + 2) * CRAFT_TEX_PIXELS],
           side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
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
    const uint16_t TIER_PICK[6][2] = {
        { rgb565(155, 110, 60),  rgb565(115, 80, 40)   }, /* wood */
        { rgb565(130, 130, 135), rgb565(85, 85, 90)    }, /* stone */
        { rgb565(225, 225, 235), rgb565(170, 170, 180) }, /* iron */
        { rgb565(190, 210, 225), rgb565(140, 165, 185) }, /* silver */
        { rgb565(255, 215, 60),  rgb565(195, 155, 30)  }, /* gold */
        { rgb565(120, 240, 250), rgb565(70, 175, 200)  }, /* diamond */
    };
    BlockId pick_ids[6] = {
        BLK_PICKAXE_WOOD, BLK_PICKAXE_STONE, BLK_PICKAXE_IRON,
        BLK_PICKAXE_SILVER, BLK_PICKAXE_GOLD, BLK_PICKAXE_DIAMOND,
    };
    for (int tier = 0; tier < 6; tier++) {
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

    const uint16_t TIER_SWORD[6][2] = {
        { rgb565(170, 130, 70),  rgb565(120, 90, 50)   }, /* wood */
        { rgb565(140, 140, 145), rgb565(95, 95, 100)   }, /* stone */
        { rgb565(230, 230, 240), rgb565(170, 170, 180) }, /* iron */
        { rgb565(195, 215, 230), rgb565(145, 170, 190) }, /* silver */
        { rgb565(255, 215, 60),  rgb565(195, 155, 30)  }, /* gold */
        { rgb565(130, 245, 255), rgb565(75, 180, 210)  }, /* diamond */
    };
    BlockId sword_ids[6] = {
        BLK_SWORD_WOOD, BLK_SWORD_STONE, BLK_SWORD_IRON,
        BLK_SWORD_SILVER, BLK_SWORD_GOLD, BLK_SWORD_DIAMOND,
    };
    for (int tier = 0; tier < 6; tier++) {
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

    /* FURNACE — cobble-style sides + top, and a darker face panel
     * with a glowing mouth on the FRONT (we paint that into the
     * "side" slot since the renderer uses side for all four
     * horizontal faces — the mouth shows on every side, which is
     * fine without a directional facing bit). */
    {
        /* Top + bottom: stone-grey speckle, similar to cobble. */
        uint16_t *top = &craft_textures[(BLK_FURNACE * 3 + 0) * CRAFT_TEX_PIXELS];
        speckle(top, 0xF00DBEEFu, 120, 120, 130, 40);
        memcpy(&craft_textures[(BLK_FURNACE * 3 + 2) * CRAFT_TEX_PIXELS],
               top, sizeof(uint16_t) * CRAFT_TEX_PIXELS);

        /* Side: stone speckle + dark recessed face panel with a
         * 2-row glow at the bottom of the mouth. */
        uint16_t *side = &craft_textures[(BLK_FURNACE * 3 + 1) * CRAFT_TEX_PIXELS];
        speckle(side, 0xF00DCAFEu, 110, 110, 120, 35);
        /* Iron-frame rectangle around the mouth at rows 3..13, cols 2..13. */
        uint16_t frame_d = rgb565(60, 60, 70);
        uint16_t frame   = rgb565(95, 95, 110);
        for (int x = 2; x < 14; x++) {
            side[3  * CRAFT_TEX_SIZE + x] = frame;
            side[13 * CRAFT_TEX_SIZE + x] = frame_d;
        }
        for (int y = 3; y < 14; y++) {
            side[y * CRAFT_TEX_SIZE + 2]  = frame;
            side[y * CRAFT_TEX_SIZE + 13] = frame_d;
        }
        /* Mouth interior — dark void with a warm fire glow at the
         * bottom 3 rows so it reads as "burning" at a glance. */
        uint16_t void_c = rgb565(15, 15, 20);
        uint16_t fire1  = rgb565(220, 100, 30);
        uint16_t fire2  = rgb565(255, 180, 60);
        for (int y = 4; y < 13; y++) {
            for (int x = 3; x < 13; x++) {
                side[y * CRAFT_TEX_SIZE + x] = void_c;
            }
        }
        for (int x = 3; x < 13; x++) {
            side[10 * CRAFT_TEX_SIZE + x] = fire1;
            side[11 * CRAFT_TEX_SIZE + x] = fire2;
            side[12 * CRAFT_TEX_SIZE + x] = fire1;
        }
    }

    /* CHEST — plank body with iron strapping and a small dark keyhole
     * front-and-centre. Top has a visible lid seam. */
    {
        uint16_t *top  = &craft_textures[(BLK_CHEST * 3 + 0) * CRAFT_TEX_PIXELS];
        uint16_t *side = &craft_textures[(BLK_CHEST * 3 + 1) * CRAFT_TEX_PIXELS];
        uint16_t plank_a = rgb565(170, 115, 60);
        uint16_t plank_b = rgb565(140, 90, 45);
        uint16_t plank_d = rgb565(100, 65, 30);
        uint16_t iron    = rgb565(95,  95, 110);
        uint16_t iron_d  = rgb565(60,  60, 70);
        uint16_t hole    = rgb565(15, 15, 20);
        /* Top: alternating plank stripes + a central seam where the lid opens. */
        for (int y = 0; y < CRAFT_TEX_SIZE; y++) {
            uint16_t base = ((y / 3) & 1) ? plank_a : plank_b;
            for (int x = 0; x < CRAFT_TEX_SIZE; x++) {
                top[y * CRAFT_TEX_SIZE + x] = base;
            }
            /* Lid seam — horizontal dark line down the middle. */
            if (y == 7 || y == 8) {
                for (int x = 1; x < CRAFT_TEX_SIZE - 1; x++)
                    top[y * CRAFT_TEX_SIZE + x] = plank_d;
            }
        }
        /* Iron corner studs on top. */
        top[1 * CRAFT_TEX_SIZE + 1] = iron;
        top[1 * CRAFT_TEX_SIZE + 14] = iron;
        top[14 * CRAFT_TEX_SIZE + 1] = iron;
        top[14 * CRAFT_TEX_SIZE + 14] = iron;
        memcpy(&craft_textures[(BLK_CHEST * 3 + 2) * CRAFT_TEX_PIXELS],
               top, sizeof(uint16_t) * CRAFT_TEX_PIXELS);

        /* Side: plank stripes + vertical iron bands at the corners
         * + horizontal iron bar across the top (where the hinges
         * would be) + a small keyhole rectangle just below centre. */
        for (int y = 0; y < CRAFT_TEX_SIZE; y++) {
            uint16_t base = ((y / 3) & 1) ? plank_a : plank_b;
            for (int x = 0; x < CRAFT_TEX_SIZE; x++) {
                side[y * CRAFT_TEX_SIZE + x] = base;
            }
        }
        /* Top iron band — hinges. */
        for (int x = 0; x < CRAFT_TEX_SIZE; x++) {
            side[1 * CRAFT_TEX_SIZE + x] = iron;
            side[2 * CRAFT_TEX_SIZE + x] = iron_d;
        }
        /* Vertical corner bands. */
        for (int y = 0; y < CRAFT_TEX_SIZE; y++) {
            side[y * CRAFT_TEX_SIZE + 0]  = iron_d;
            side[y * CRAFT_TEX_SIZE + 15] = iron_d;
        }
        /* Keyhole — 2×2 dark cluster mid-front. */
        side[8  * CRAFT_TEX_SIZE + 7] = hole;
        side[8  * CRAFT_TEX_SIZE + 8] = hole;
        side[9  * CRAFT_TEX_SIZE + 7] = hole;
        side[9  * CRAFT_TEX_SIZE + 8] = hole;
        /* Iron lock-plate around the keyhole. */
        side[7  * CRAFT_TEX_SIZE + 6] = iron;
        side[7  * CRAFT_TEX_SIZE + 9] = iron;
        side[10 * CRAFT_TEX_SIZE + 6] = iron;
        side[10 * CRAFT_TEX_SIZE + 9] = iron;
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

    /* Precious-metal ores — speckled stone with material flecks. */
    ore_with_flecks(BLK_SILVER_ORE,   0x511A011u, 190, 210, 225);
    ore_with_flecks(BLK_GOLD_ORE,     0x6011D11u, 220, 180,  40);
    ore_with_flecks(BLK_DIAMOND_ORE,  0xD1A1011u,  90, 200, 220);
    ore_with_flecks(BLK_REDSTONE_ORE, 0xED5701Cu, 200,  40,  40);

    /* Ingots / gem / dust. */
    ingot_bar_tex(BLK_SILVER_INGOT, 215, 225, 235);
    ingot_bar_tex(BLK_GOLD_INGOT,   245, 210,  55);
    gem_tex      (BLK_DIAMOND,      130, 240, 250);
    dust_tex     (BLK_REDSTONE,     0xDEADBEEFu, 230,  40,  40);

    /* Storage blocks — solid colour with hatched mid + dark border. */
    solid_block_tex(BLK_SILVER_BLOCK,   210, 220, 230);
    solid_block_tex(BLK_GOLD_BLOCK,     245, 210,  55);
    solid_block_tex(BLK_DIAMOND_BLOCK,  130, 240, 250);
    solid_block_tex(BLK_REDSTONE_BLOCK, 210,  40,  40);

    /* LEVER — high-contrast: dark stone base plate at the bottom,
     * a chunky wood handle running diagonally to a bright ball-tip.
     * ON state mirrors the handle and lights the tip bright red so
     * it reads as "pulled". Same texture on every face. */
    for (int on = 0; on < 2; on++) {
        BlockId blk = on ? BLK_LEVER_ON : BLK_LEVER_OFF;
        uint16_t *side = &craft_textures[(blk * 3 + 1) * CRAFT_TEX_PIXELS];
        /* Dark stone backdrop. */
        for (int i = 0; i < CRAFT_TEX_PIXELS; i++) side[i] = rgb565(60, 60, 70);
        /* Mounting plate — solid grey block at the bottom centre. */
        uint16_t plate    = rgb565(160, 160, 170);
        uint16_t plate_d  = rgb565(95, 95, 105);
        for (int y = 10; y < CRAFT_TEX_SIZE; y++) {
            for (int x = 3; x < 13; x++) {
                bool edge = (y == 10 || y == 15 || x == 3 || x == 12);
                side[y * CRAFT_TEX_SIZE + x] = edge ? plate_d : plate;
            }
        }
        /* Handle — 3-wide diagonal stalk to make it readable. */
        uint16_t wood   = rgb565(190, 130, 60);
        uint16_t wood_d = rgb565(110, 75, 35);
        uint16_t tip    = on ? rgb565(255, 80, 60) : rgb565(220, 220, 230);
        uint16_t tip_d  = on ? rgb565(170, 30, 20) : rgb565(150, 150, 160);
        for (int i = 0; i < 6; i++) {
            int yy = 10 - i;
            int xx = on ? (7 + i) : (7 - i);
            if (yy < 0 || yy >= CRAFT_TEX_SIZE) continue;
            for (int dx = -1; dx <= 1; dx++) {
                int x = xx + dx;
                if (x < 0 || x >= CRAFT_TEX_SIZE) continue;
                side[yy * CRAFT_TEX_SIZE + x] = (dx == 0) ? wood : wood_d;
            }
        }
        /* Ball tip — 3×3 cluster at the top of the handle. */
        int tx = on ? 12 : 2;
        int ty = 4;
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                int x = tx + dx, y = ty + dy;
                if ((unsigned)x >= CRAFT_TEX_SIZE) continue;
                if ((unsigned)y >= CRAFT_TEX_SIZE) continue;
                bool centre = (dx == 0 && dy == 0);
                side[y * CRAFT_TEX_SIZE + x] = centre ? tip : tip_d;
            }
        }
        memcpy(&craft_textures[(blk * 3 + 0) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
        memcpy(&craft_textures[(blk * 3 + 2) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    }

    /* LADDER — wooden rails + rungs on a dark backdrop (rendered as
     * a 2D vertical sprite via craft_torches, like wire). */
    {
        uint16_t *side = &craft_textures[(BLK_LADDER * 3 + 1) * CRAFT_TEX_PIXELS];
        for (int i = 0; i < CRAFT_TEX_PIXELS; i++) side[i] = rgb565(20, 20, 25);
        uint16_t rail = rgb565(140, 90, 40);
        uint16_t rung = rgb565(170, 115, 55);
        for (int y = 0; y < CRAFT_TEX_SIZE; y++) {
            side[y * CRAFT_TEX_SIZE + 3] = rail;
            side[y * CRAFT_TEX_SIZE + 12] = rail;
        }
        for (int ry = 1; ry < CRAFT_TEX_SIZE; ry += 4) {
            for (int x = 4; x < 12; x++) {
                side[ry * CRAFT_TEX_SIZE + x] = rung;
            }
        }
        memcpy(&craft_textures[(BLK_LADDER * 3 + 0) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
        memcpy(&craft_textures[(BLK_LADDER * 3 + 2) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    }

    /* TRAPDOOR — horizontal wood planks with iron strapping. Open
     * variant uses the same texture (we only render closed cells as
     * cubes; open cells are passable + invisible for v1). */
    for (int variant = 0; variant < 2; variant++) {
        BlockId blk = (variant == 0) ? BLK_TRAPDOOR_OFF : BLK_TRAPDOOR_ON;
        uint16_t *side = &craft_textures[(blk * 3 + 1) * CRAFT_TEX_PIXELS];
        uint16_t plank = rgb565(150, 100, 50);
        uint16_t plank_d = rgb565(110, 70, 35);
        uint16_t iron = rgb565(90, 90, 100);
        for (int y = 0; y < CRAFT_TEX_SIZE; y++) {
            for (int x = 0; x < CRAFT_TEX_SIZE; x++) {
                uint16_t c = ((y / 3) & 1) ? plank : plank_d;
                side[y * CRAFT_TEX_SIZE + x] = c;
            }
        }
        for (int x = 1; x < 15; x++) {
            side[2  * CRAFT_TEX_SIZE + x] = iron;
            side[13 * CRAFT_TEX_SIZE + x] = iron;
        }
        memcpy(&craft_textures[(blk * 3 + 0) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
        memcpy(&craft_textures[(blk * 3 + 2) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    }

    /* DOOR — vertical plank face with hinge band on one side.
     * Closed = solid wall; open = passable (same texture either way). */
    for (int variant = 0; variant < 2; variant++) {
        BlockId blk = (variant == 0) ? BLK_DOOR_OFF : BLK_DOOR_ON;
        uint16_t *side = &craft_textures[(blk * 3 + 1) * CRAFT_TEX_PIXELS];
        uint16_t plank = rgb565(155, 110, 60);
        uint16_t plank_d = rgb565(110, 75, 35);
        uint16_t iron = rgb565(85, 85, 95);
        uint16_t knob = rgb565(220, 200, 60);
        for (int y = 0; y < CRAFT_TEX_SIZE; y++) {
            for (int x = 0; x < CRAFT_TEX_SIZE; x++) {
                uint16_t c = ((x / 3) & 1) ? plank : plank_d;
                side[y * CRAFT_TEX_SIZE + x] = c;
            }
        }
        /* Hinge strap on left edge. */
        for (int y = 0; y < CRAFT_TEX_SIZE; y++) {
            side[y * CRAFT_TEX_SIZE + 0] = iron;
            side[y * CRAFT_TEX_SIZE + 1] = iron;
        }
        /* Knob. */
        side[7 * CRAFT_TEX_SIZE + 13] = knob;
        side[8 * CRAFT_TEX_SIZE + 13] = knob;
        memcpy(&craft_textures[(blk * 3 + 0) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
        memcpy(&craft_textures[(blk * 3 + 2) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    }

    /* PRESSURE PAD — flat stone slab, rendered as 2D sprite via the
     * torch system (same path as wires). */
    {
        uint16_t *side = &craft_textures[(BLK_PRESSURE_PAD * 3 + 1) * CRAFT_TEX_PIXELS];
        for (int i = 0; i < CRAFT_TEX_PIXELS; i++) side[i] = rgb565(20, 20, 25);
        uint16_t pad = rgb565(150, 150, 160);
        uint16_t pad_d = rgb565(110, 110, 120);
        for (int y = 4; y < 13; y++) {
            for (int x = 2; x < 14; x++) {
                bool edge = (y == 4 || y == 12 || x == 2 || x == 13);
                side[y * CRAFT_TEX_SIZE + x] = edge ? pad_d : pad;
            }
        }
        memcpy(&craft_textures[(BLK_PRESSURE_PAD * 3 + 0) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
        memcpy(&craft_textures[(BLK_PRESSURE_PAD * 3 + 2) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    }

    /* PISTON — iron-banded stone block with a bright square on the
     * face that pushes (one side gets a different texture, but for
     * v1 every face shows the same — orientation goes through the
     * lever pipeline later). Active variant brightens the face. */
    for (int variant = 0; variant < 2; variant++) {
        BlockId blk = (variant == 0) ? BLK_PISTON_OFF : BLK_PISTON_ON;
        uint16_t *side = &craft_textures[(blk * 3 + 1) * CRAFT_TEX_PIXELS];
        speckle(side, 0xD15700u + variant, 120, 120, 130, 35);
        uint16_t band = rgb565(85, 85, 100);
        for (int x = 0; x < CRAFT_TEX_SIZE; x++) {
            side[1 * CRAFT_TEX_SIZE + x]  = band;
            side[14 * CRAFT_TEX_SIZE + x] = band;
        }
        for (int y = 0; y < CRAFT_TEX_SIZE; y++) {
            side[y * CRAFT_TEX_SIZE + 1]  = band;
            side[y * CRAFT_TEX_SIZE + 14] = band;
        }
        uint16_t face = variant ? rgb565(220, 180, 90) : rgb565(170, 130, 60);
        for (int y = 5; y < 11; y++) {
            for (int x = 5; x < 11; x++) {
                side[y * CRAFT_TEX_SIZE + x] = face;
            }
        }
        memcpy(&craft_textures[(blk * 3 + 0) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
        memcpy(&craft_textures[(blk * 3 + 2) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    }

    /* PISTON_ARM — thinner extension shape; same iron band. */
    {
        uint16_t *side = &craft_textures[(BLK_PISTON_ARM * 3 + 1) * CRAFT_TEX_PIXELS];
        for (int i = 0; i < CRAFT_TEX_PIXELS; i++) side[i] = rgb565(140, 140, 150);
        uint16_t band = rgb565(85, 85, 100);
        for (int y = 0; y < CRAFT_TEX_SIZE; y++) {
            side[y * CRAFT_TEX_SIZE + 0] = band;
            side[y * CRAFT_TEX_SIZE + 15] = band;
        }
        for (int x = 0; x < CRAFT_TEX_SIZE; x++) {
            side[0 * CRAFT_TEX_SIZE + x] = band;
            side[15 * CRAFT_TEX_SIZE + x] = band;
        }
        memcpy(&craft_textures[(BLK_PISTON_ARM * 3 + 0) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
        memcpy(&craft_textures[(BLK_PISTON_ARM * 3 + 2) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    }

    /* TNT — red-and-white striped block. Fused variant adds a glowing
     * core and brighter stripes to read as "about to explode". */
    for (int variant = 0; variant < 2; variant++) {
        BlockId blk = (variant == 0) ? BLK_TNT : BLK_TNT_FUSED;
        uint16_t *side = &craft_textures[(blk * 3 + 1) * CRAFT_TEX_PIXELS];
        uint16_t red    = variant ? rgb565(255, 100, 80) : rgb565(200, 50, 40);
        uint16_t white  = variant ? rgb565(255, 255, 200) : rgb565(220, 220, 220);
        uint16_t letter = rgb565(20, 20, 25);
        for (int y = 0; y < CRAFT_TEX_SIZE; y++) {
            for (int x = 0; x < CRAFT_TEX_SIZE; x++) {
                bool stripe = (y >= 4 && y < 12);
                side[y * CRAFT_TEX_SIZE + x] = stripe ? red : white;
            }
        }
        /* "TNT" letters — 3 dark verticals on the red band. */
        for (int y = 6; y < 10; y++) {
            side[y * CRAFT_TEX_SIZE + 4]  = letter;
            side[y * CRAFT_TEX_SIZE + 8]  = letter;
            side[y * CRAFT_TEX_SIZE + 12] = letter;
        }
        side[5  * CRAFT_TEX_SIZE + 8] = letter;     /* T crossbar tops */
        side[5  * CRAFT_TEX_SIZE + 4] = letter;
        side[5  * CRAFT_TEX_SIZE + 12] = letter;
        memcpy(&craft_textures[(blk * 3 + 0) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
        memcpy(&craft_textures[(blk * 3 + 2) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    }

    /* WIRE — thin red cross on a dark backdrop. WIRE_ON brightens
     * the cross; WIRE off uses a dim red so unpowered wires are
     * visually distinct from powered ones. */
    for (int on = 0; on < 2; on++) {
        BlockId blk = on ? BLK_REDSTONE_WIRE_ON : BLK_REDSTONE_WIRE;
        uint16_t *side = &craft_textures[(blk * 3 + 1) * CRAFT_TEX_PIXELS];
        for (int i = 0; i < CRAFT_TEX_PIXELS; i++) side[i] = rgb565(20, 20, 25);
        uint16_t line = on ? rgb565(255, 60, 50) : rgb565(120, 30, 30);
        uint16_t mid  = on ? rgb565(255, 100, 80) : rgb565(150, 50, 40);
        for (int x = 1; x < 15; x++) {
            side[7 * CRAFT_TEX_SIZE + x] = line;
            side[8 * CRAFT_TEX_SIZE + x] = mid;
        }
        for (int y = 1; y < 15; y++) {
            side[y * CRAFT_TEX_SIZE + 7] = line;
            side[y * CRAFT_TEX_SIZE + 8] = mid;
        }
        memcpy(&craft_textures[(blk * 3 + 0) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
        memcpy(&craft_textures[(blk * 3 + 2) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    }

    /* OBSERVER — dark grey block, single bright "lens" eye on the
     * side and a tiny back vent. Direction marker is the lens. */
    {
        uint16_t *side = &craft_textures[(BLK_OBSERVER * 3 + 1) * CRAFT_TEX_PIXELS];
        speckle(side, 0x0B5, 75, 75, 80, 30);
        /* Centred 4×4 cyan lens with brighter centre. */
        uint16_t lens   = rgb565(140, 220, 220);
        uint16_t pupil  = rgb565(40, 180, 220);
        for (int y = 6; y < 10; y++) {
            for (int x = 6; x < 10; x++) {
                side[y * CRAFT_TEX_SIZE + x] = lens;
            }
        }
        side[7 * CRAFT_TEX_SIZE + 7] = pupil;
        side[7 * CRAFT_TEX_SIZE + 8] = pupil;
        side[8 * CRAFT_TEX_SIZE + 7] = pupil;
        side[8 * CRAFT_TEX_SIZE + 8] = pupil;
        memcpy(&craft_textures[(BLK_OBSERVER * 3 + 0) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
        memcpy(&craft_textures[(BLK_OBSERVER * 3 + 2) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    }

    /* NOTE BLOCK — plank-coloured cube with a black eighth-note glyph
     * for unmistakable identification. */
    {
        uint16_t *side = &craft_textures[(BLK_NOTE_BLOCK * 3 + 1) * CRAFT_TEX_PIXELS];
        plank_pattern(side, 0xF00D);
        uint16_t glyph = rgb565(20, 20, 25);
        /* Stem */
        for (int y = 4; y < 12; y++) side[y * CRAFT_TEX_SIZE + 9] = glyph;
        /* Filled note head */
        for (int y = 11; y < 14; y++) {
            for (int x = 6; x < 10; x++) {
                side[y * CRAFT_TEX_SIZE + x] = glyph;
            }
        }
        /* Flag */
        side[4 * CRAFT_TEX_SIZE + 9]  = glyph;
        side[4 * CRAFT_TEX_SIZE + 10] = glyph;
        side[5 * CRAFT_TEX_SIZE + 11] = glyph;
        side[6 * CRAFT_TEX_SIZE + 11] = glyph;
        memcpy(&craft_textures[(BLK_NOTE_BLOCK * 3 + 0) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
        memcpy(&craft_textures[(BLK_NOTE_BLOCK * 3 + 2) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    }

    /* LAMP — warm-glass cube, soft cross pattern. The lit variant is
     * produced live by the renderer's brightness path so we only
     * bake the off colour here. */
    {
        uint16_t *side = &craft_textures[(BLK_LAMP * 3 + 1) * CRAFT_TEX_PIXELS];
        speckle(side, 0x1A3D, 130, 100, 60, 25);
        uint16_t accent = rgb565(180, 140, 80);
        for (int x = 1; x < 15; x++) {
            side[3 * CRAFT_TEX_SIZE + x]  = accent;
            side[12 * CRAFT_TEX_SIZE + x] = accent;
        }
        for (int y = 1; y < 15; y++) {
            side[y * CRAFT_TEX_SIZE + 3]  = accent;
            side[y * CRAFT_TEX_SIZE + 12] = accent;
        }
        memcpy(&craft_textures[(BLK_LAMP * 3 + 0) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
        memcpy(&craft_textures[(BLK_LAMP * 3 + 2) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    }

    /* LAMP_ON — lit: bright warm glow with a brighter cross. Distinct
     * from the dark off tile so the lamp visibly lights up; it's also
     * a light source (see craft_world_rebuild_lightmap). */
    {
        uint16_t *side = &craft_textures[(BLK_LAMP_ON * 3 + 1) * CRAFT_TEX_PIXELS];
        speckle(side, 0x1A3E, 255, 224, 150, 16);
        uint16_t accent = rgb565(255, 250, 215);
        for (int x = 1; x < 15; x++) {
            side[3 * CRAFT_TEX_SIZE + x]  = accent;
            side[12 * CRAFT_TEX_SIZE + x] = accent;
        }
        for (int y = 1; y < 15; y++) {
            side[y * CRAFT_TEX_SIZE + 3]  = accent;
            side[y * CRAFT_TEX_SIZE + 12] = accent;
        }
        memcpy(&craft_textures[(BLK_LAMP_ON * 3 + 0) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
        memcpy(&craft_textures[(BLK_LAMP_ON * 3 + 2) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    }

    /* NOT GATE — stone-coloured with a centred white "¬" glyph and a
     * direction arrow ridge. Output state is rendered as-is for now;
     * sweeping a state-aware texture variant in is a follow-up. */
    {
        uint16_t *side = &craft_textures[(BLK_NOT_GATE * 3 + 1) * CRAFT_TEX_PIXELS];
        speckle(side, 0x1701, 120, 120, 130, 30);
        uint16_t glyph = rgb565(240, 240, 240);
        /* Horizontal bar */
        for (int x = 4; x < 12; x++) side[7 * CRAFT_TEX_SIZE + x] = glyph;
        /* Right-side drop tick (the "¬" hook). */
        side[8  * CRAFT_TEX_SIZE + 11] = glyph;
        side[9  * CRAFT_TEX_SIZE + 11] = glyph;
        /* Direction arrow on the bottom edge. */
        uint16_t arrow = rgb565(80, 200, 80);
        side[13 * CRAFT_TEX_SIZE + 6]  = arrow;
        side[13 * CRAFT_TEX_SIZE + 9]  = arrow;
        side[14 * CRAFT_TEX_SIZE + 7]  = arrow;
        side[14 * CRAFT_TEX_SIZE + 8]  = arrow;
        memcpy(&craft_textures[(BLK_NOT_GATE * 3 + 0) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
        memcpy(&craft_textures[(BLK_NOT_GATE * 3 + 2) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    }

    /* DELAY — stone slab with a row of pip indicators showing the
     * current 1..4 tick setting. We bake the "1" variant; live re-
     * skin per cell uses the orient-hash byte to count pips. */
    {
        uint16_t *side = &craft_textures[(BLK_DELAY * 3 + 1) * CRAFT_TEX_PIXELS];
        speckle(side, 0xDE1A, 140, 140, 145, 25);
        uint16_t pip = rgb565(220, 80, 60);
        /* Single lit pip at the centre (setting = 1). */
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++)
                side[(8 + dy) * CRAFT_TEX_SIZE + (8 + dx)] = pip;
        /* Direction arrow on the bottom edge. */
        uint16_t arrow = rgb565(80, 200, 80);
        side[13 * CRAFT_TEX_SIZE + 6]  = arrow;
        side[13 * CRAFT_TEX_SIZE + 9]  = arrow;
        side[14 * CRAFT_TEX_SIZE + 7]  = arrow;
        side[14 * CRAFT_TEX_SIZE + 8]  = arrow;
        memcpy(&craft_textures[(BLK_DELAY * 3 + 0) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
        memcpy(&craft_textures[(BLK_DELAY * 3 + 2) * CRAFT_TEX_PIXELS],
               side, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    }

    /* DISPENSER — cobble body with a dark circular muzzle on every
     * face (close enough to read as "the hole shoots that way"). */
    for (int f = 0; f < 3; f++) {
        uint16_t *t = &craft_textures[(BLK_DISPENSER * 3 + f) * CRAFT_TEX_PIXELS];
        cobble_pattern(t, 0xD15B + f);
        /* Dark muzzle disc centred at (8,8), radius ~4. */
        for (int y = 0; y < CRAFT_TEX_SIZE; y++)
            for (int x = 0; x < CRAFT_TEX_SIZE; x++) {
                int dx = x - 8, dy = y - 8;
                if (dx*dx + dy*dy <= 16)
                    t[y * CRAFT_TEX_SIZE + x] = rgb565(25, 25, 28);
            }
    }

    /* TARGET — concentric red/white rings, dartboard style. */
    for (int f = 0; f < 3; f++) {
        uint16_t *t = &craft_textures[(BLK_TARGET * 3 + f) * CRAFT_TEX_PIXELS];
        for (int y = 0; y < CRAFT_TEX_SIZE; y++)
            for (int x = 0; x < CRAFT_TEX_SIZE; x++) {
                int dx = x - 8, dy = y - 8;
                int r2 = dx*dx + dy*dy;
                uint16_t c;
                if      (r2 <= 4)   c = rgb565(220, 60, 50);
                else if (r2 <= 16)  c = rgb565(235, 230, 215);
                else if (r2 <= 36)  c = rgb565(220, 60, 50);
                else                c = rgb565(235, 230, 215);
                t[y * CRAFT_TEX_SIZE + x] = c;
            }
    }

    /* SLIME BLOCK — translucent-looking green with a darker inner
     * grid, a lighter speckle for the gel sheen. */
    for (int f = 0; f < 3; f++) {
        uint16_t *t = &craft_textures[(BLK_SLIME_BLOCK * 3 + f) * CRAFT_TEX_PIXELS];
        speckle(t, 0x511E0000u + (uint32_t)f, 110, 200, 110, 30);
        uint16_t grid = rgb565(70, 150, 70);
        for (int i = 0; i < CRAFT_TEX_SIZE; i++) {
            t[2 * CRAFT_TEX_SIZE + i] = grid;
            t[13 * CRAFT_TEX_SIZE + i] = grid;
            t[i * CRAFT_TEX_SIZE + 2] = grid;
            t[i * CRAFT_TEX_SIZE + 13] = grid;
        }
    }

    /* SLIMEBALL — item icon: a green blob with a highlight on a dark
     * backdrop (the engine has no texel transparency — every pixel
     * shows, so the tile is filled). */
    {
        uint16_t *t = &craft_textures[(BLK_SLIMEBALL * 3 + 1) * CRAFT_TEX_PIXELS];
        fill_solid(t, rgb565(28, 34, 28));
        for (int y = 0; y < CRAFT_TEX_SIZE; y++)
            for (int x = 0; x < CRAFT_TEX_SIZE; x++) {
                int dx = x - 8, dy = y - 8;
                if (dx*dx + dy*dy <= 20)
                    t[y * CRAFT_TEX_SIZE + x] = rgb565(120, 210, 120);
                if (dx*dx + dy*dy <= 4)
                    t[y * CRAFT_TEX_SIZE + x] = rgb565(170, 240, 170);
            }
        memcpy(&craft_textures[(BLK_SLIMEBALL * 3 + 0) * CRAFT_TEX_PIXELS],
               t, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
        memcpy(&craft_textures[(BLK_SLIMEBALL * 3 + 2) * CRAFT_TEX_PIXELS],
               t, sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    }

    /* SNOW — snow over a dirt/grass band (white top, snow-capped dirt
     * sides). The "snow dusting on ground" look for the taiga. Mountain
     * caps use BLK_SNOWY_ROCK instead so they don't get a dirt band. */
    {
        uint16_t *top = &craft_textures[(BLK_SNOW * 3 + 0) * CRAFT_TEX_PIXELS];
        speckle(top, 0x5009u, 235, 240, 250, 12);
        uint16_t *side = &craft_textures[(BLK_SNOW * 3 + 1) * CRAFT_TEX_PIXELS];
        speckle(side, 0x5102, 120, 90, 70, 18);   /* dirt base */
        for (int y = 0; y < 5; y++)               /* snow cap on top rows */
            for (int x = 0; x < CRAFT_TEX_SIZE; x++)
                side[y * CRAFT_TEX_SIZE + x] = rgb565(235, 240, 250);
        memcpy(&craft_textures[(BLK_SNOW * 3 + 2) * CRAFT_TEX_PIXELS],
               &craft_textures[(BLK_DIRT * 3 + 1) * CRAFT_TEX_PIXELS],
               sizeof(uint16_t) * CRAFT_TEX_PIXELS);   /* dirt bottom */
    }

    /* SNOWY ROCK — mountain cap: white snow top, grey stone sides with
     * a white snow rim along the upper edge, stone bottom. Reads as
     * snow lying on rock (no dirt band). */
    {
        uint16_t *top = &craft_textures[(BLK_SNOWY_ROCK * 3 + 0) * CRAFT_TEX_PIXELS];
        speckle(top, 0x5A09u, 235, 240, 250, 12);
        uint16_t *side = &craft_textures[(BLK_SNOWY_ROCK * 3 + 1) * CRAFT_TEX_PIXELS];
        speckle(side, 0x5A0Cu, 120, 122, 130, 22);   /* stone-grey base */
        for (int y = 0; y < 4; y++)                  /* snow rim on top rows */
            for (int x = 0; x < CRAFT_TEX_SIZE; x++)
                side[y * CRAFT_TEX_SIZE + x] = rgb565(232, 238, 248);
        memcpy(&craft_textures[(BLK_SNOWY_ROCK * 3 + 2) * CRAFT_TEX_PIXELS],
               &craft_textures[(BLK_STONE * 3 + 1) * CRAFT_TEX_PIXELS],
               sizeof(uint16_t) * CRAFT_TEX_PIXELS);   /* stone bottom */
    }

    /* ICE — frozen sheet: warped-Voronoi cracked plates like the lava
     * tile but static and blue (mid-blue plates, bright frosty seams).
     * Per-cell offset/flip in the renderer keeps a lake from looking
     * tiled. Same tile on all faces. */
    {
        uint16_t *t0 = &craft_textures[(BLK_ICE * 3 + 0) * CRAFT_TEX_PIXELS];
        bake_ice_tile(t0);
        memcpy(&craft_textures[(BLK_ICE * 3 + 1) * CRAFT_TEX_PIXELS], t0,
               sizeof(uint16_t) * CRAFT_TEX_PIXELS);
        memcpy(&craft_textures[(BLK_ICE * 3 + 2) * CRAFT_TEX_PIXELS], t0,
               sizeof(uint16_t) * CRAFT_TEX_PIXELS);
    }

    /* LAVA / PORTAL — initial (frame 0) tiles on all faces. The live
     * tiles are re-baked every frame by craft_blocks_animate_water; this
     * just seeds the slots so they're never uninitialised before the
     * first animate call. */
    for (int f = 0; f < 3; f++) {
        bake_lava_frame(&craft_textures[(BLK_LAVA * 3 + f) * CRAFT_TEX_PIXELS], 0);
        bake_portal_frame(&craft_textures[(BLK_PORTAL * 3 + f) * CRAFT_TEX_PIXELS], 0);
    }

    /* GRAVEL — grey speckle with scattered darker pebbles, all faces. */
    for (int f = 0; f < 3; f++) {
        uint16_t *t = &craft_textures[(BLK_GRAVEL * 3 + f) * CRAFT_TEX_PIXELS];
        speckle(t, 0x6AA7E1u + f, 140, 138, 134, 40);
        uint32_t s = 0x6AA7E1u ^ (f * 0x1000u);
        for (int n = 0; n < 14; n++) {            /* darker pebbles */
            int cx = (int)(xs32(&s) & 15);
            int cy = (int)(xs32(&s) & 15);
            t[cy * CRAFT_TEX_SIZE + cx] = rgb565(92, 90, 86);
        }
    }

    /* FLINT — item icon only (never a world cell): dark grey shard. */
    for (int f = 0; f < 3; f++) {
        uint16_t *t = &craft_textures[(BLK_FLINT * 3 + f) * CRAFT_TEX_PIXELS];
        speckle(t, 0xF11A7u + f, 60, 60, 66, 18);
    }

    /* OBSIDIAN — near-black with a faint purple sheen + sparse brighter
     * violet flecks, all faces alike. */
    for (int f = 0; f < 3; f++) {
        uint16_t *t = &craft_textures[(BLK_OBSIDIAN * 3 + f) * CRAFT_TEX_PIXELS];
        speckle(t, 0x0B51D1u + f, 26, 18, 38, 14);   /* dark purple-black */
        uint32_t s = 0x0B51D1u ^ (f * 0x1000u);
        for (int n = 0; n < 10; n++) {               /* sparse violet flecks */
            int cx = (int)(xs32(&s) & 15);
            int cy = (int)(xs32(&s) & 15);
            t[cy * CRAFT_TEX_SIZE + cx] = rgb565(78, 54, 110);
        }
    }

    /* SANDSTONE — tan with faint horizontal banding. */
    for (int f = 0; f < 3; f++) {
        uint16_t *t = &craft_textures[(BLK_SANDSTONE * 3 + f) * CRAFT_TEX_PIXELS];
        speckle(t, 0x5A5Du + f, 210, 195, 150, 14);
        for (int y = 0; y < CRAFT_TEX_SIZE; y += 5)
            for (int x = 0; x < CRAFT_TEX_SIZE; x++)
                t[y * CRAFT_TEX_SIZE + x] = rgb565(185, 170, 128);
    }

    /* CACTUS — green with darker vertical ribs and a notched top. */
    for (int f = 0; f < 3; f++) {
        uint16_t *t = &craft_textures[(BLK_CACTUS * 3 + f) * CRAFT_TEX_PIXELS];
        speckle(t, 0xCAC0u + f, 60, 130, 60, 16);
        if (f == 1) {                              /* side ribs */
            for (int x = 3; x < CRAFT_TEX_SIZE; x += 5)
                for (int y = 0; y < CRAFT_TEX_SIZE; y++)
                    t[y * CRAFT_TEX_SIZE + x] = rgb565(35, 90, 40);
        }
    }

    /* VINE — drawn in-world as a 3-strand sprite cuboid; this atlas
     * tile is only the hotbar icon, so it's a full image (dark
     * backdrop + green strands, like the ladder). */
    for (int f = 0; f < 3; f++) {
        uint16_t *t = &craft_textures[(BLK_VINE * 3 + f) * CRAFT_TEX_PIXELS];
        fill_solid(t, rgb565(20, 28, 18));
        for (int x = 1; x < CRAFT_TEX_SIZE; x += 4)
            for (int y = 0; y < CRAFT_TEX_SIZE; y++)
                if (((x * 3 + y) & 7) != 0)
                    t[y * CRAFT_TEX_SIZE + x] = rgb565(55, 120, 45);
    }

    /* LILY PAD — drawn in-world as a flat sprite slab; this tile is
     * the hotbar icon: a green disc on a water-blue backdrop. */
    for (int f = 0; f < 3; f++) {
        uint16_t *t = &craft_textures[(BLK_LILY_PAD * 3 + f) * CRAFT_TEX_PIXELS];
        fill_solid(t, rgb565(40, 80, 110));
        for (int y = 0; y < CRAFT_TEX_SIZE; y++)
            for (int x = 0; x < CRAFT_TEX_SIZE; x++) {
                int dx = x - 8, dy = y - 8;
                if (dx*dx + dy*dy <= 42 && !(dx > 0 && dy > 0 && dx + dy > 6))
                    t[y * CRAFT_TEX_SIZE + x] = rgb565(70, 150, 70);
            }
    }
}
#endif /* CRAFT_TEXTURES_BAKED */

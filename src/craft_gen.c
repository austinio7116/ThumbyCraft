/*
 * ThumbyCraft — terrain generation.
 *
 * Three-octave value noise drives a heightmap. Surface block depends
 * on height vs water level. Trees scatter at low density on grass.
 *
 * Everything is a pure function of (x, y, z, seed) — that's what
 * lets the save layer reconstruct the base world without holding
 * a 256 KB second copy of the world in SRAM.
 *
 * craft_gen_world is just craft_gen_block_at applied to every cell.
 */
#include "craft_gen.h"
#include "craft_world.h"

static uint32_t hash3(int x, int y, int z) {
    uint32_t h = (uint32_t)(x * 374761393) ^
                 (uint32_t)(y * 668265263) ^
                 (uint32_t)(z * 2147483647);
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return h;
}

static float frand(uint32_t seed) {
    return (seed & 0xFFFF) / 65535.0f;
}

static float smoothstep(float t) { return t * t * (3.0f - 2.0f * t); }

static float val_noise2(float x, float y, uint32_t seed) {
    int ix = (int)floorf(x), iy = (int)floorf(y);
    float fx = x - ix, fy = y - iy;
    fx = smoothstep(fx); fy = smoothstep(fy);
    float v00 = frand(hash3(ix,     iy,     seed));
    float v10 = frand(hash3(ix + 1, iy,     seed));
    float v01 = frand(hash3(ix,     iy + 1, seed));
    float v11 = frand(hash3(ix + 1, iy + 1, seed));
    float a = v00 * (1 - fx) + v10 * fx;
    float b = v01 * (1 - fx) + v11 * fx;
    return a * (1 - fy) + b * fy;
}

static float fbm(float x, float y, uint32_t seed) {
    float s = 0, amp = 1, freq = 1, norm = 0;
    for (int i = 0; i < 4; i++) {
        s += val_noise2(x * freq, y * freq, seed + (uint32_t)(i * 1009)) * amp;
        norm += amp;
        amp *= 0.5f;
        freq *= 2.0f;
    }
    return s / norm;
}

/* 3D value noise — trilinear interpolation of 8 corner hashes. Used
 * only by the cave generator; the heightmap stays 2D. */
static float val_noise3(float x, float y, float z, uint32_t seed) {
    int ix = (int)floorf(x), iy = (int)floorf(y), iz = (int)floorf(z);
    float fx = smoothstep(x - ix);
    float fy = smoothstep(y - iy);
    float fz = smoothstep(z - iz);
    float v000 = frand(hash3(ix,     iy,     iz)     ^ seed);
    float v100 = frand(hash3(ix + 1, iy,     iz)     ^ seed);
    float v010 = frand(hash3(ix,     iy + 1, iz)     ^ seed);
    float v110 = frand(hash3(ix + 1, iy + 1, iz)     ^ seed);
    float v001 = frand(hash3(ix,     iy,     iz + 1) ^ seed);
    float v101 = frand(hash3(ix + 1, iy,     iz + 1) ^ seed);
    float v011 = frand(hash3(ix,     iy + 1, iz + 1) ^ seed);
    float v111 = frand(hash3(ix + 1, iy + 1, iz + 1) ^ seed);
    float a00 = v000 * (1 - fx) + v100 * fx;
    float a10 = v010 * (1 - fx) + v110 * fx;
    float a01 = v001 * (1 - fx) + v101 * fx;
    float a11 = v011 * (1 - fx) + v111 * fx;
    float b0  = a00 * (1 - fy) + a10 * fy;
    float b1  = a01 * (1 - fy) + a11 * fy;
    return b0 * (1 - fz) + b1 * fz;
}

/* Biome noise — low-frequency band so mountain regions are dozens
 * of blocks wide rather than per-cell. Output in [0, 1]. */
static float biome_at(int x, int z, uint32_t seed) {
    float nx = (float)x * 0.015f;
    float nz = (float)z * 0.015f;
    return fbm(nx, nz, seed ^ 0x88112233u);
}

/* Mountain factor in [0, 1]: 0 = lowland, 1 = full mountain. Smooth
 * ramp from 0.55 (foothills) to 0.75 (peak). */
static float mountain_factor(int x, int z, uint32_t seed) {
    float b = biome_at(x, z, seed);
    if (b < 0.55f) return 0.0f;
    if (b > 0.75f) return 1.0f;
    return (b - 0.55f) / 0.20f;
}

/* Flatland factor in [0, 1] — independent slow biome noise that
 * marks regions where terrain compresses to a near-uniform low
 * elevation. Rivers form preferentially here because the natural
 * ground stays close to water level over long stretches, giving
 * river noise something flat to carve through instead of canyons
 * cut into rolling hills.
 *
 * Very low frequency (0.004) so flatland patches span hundreds of
 * blocks — large enough to host a winding river along their length. */
static float flatland_factor(int x, int z, uint32_t seed) {
    float n = fbm((float)x * 0.004f, (float)z * 0.004f, seed ^ 0xF1A71A4Du);
    if (n < 0.55f) return 0.0f;
    if (n > 0.78f) return 1.0f;
    return (n - 0.55f) / 0.23f;
}

/* River shape is inlined in height_at — see below. The two
 * concentric zones (channel + bank slope) share the same noise
 * sample, so it's cheaper to compute it once in-place. */

void craft_gen_invalidate_height_cache(void) {
    /* No-op kept for the public API. An earlier 11 KB cache was
     * pulled because it overflowed BSS; the cheaper tree_at /
     * hut_origin_at hash-first reorder achieves most of the same
     * shift-cost win without any storage. */
}

static int height_at(int x, int z, uint32_t seed) {
    float nx = (float)x * 0.06f;
    float nz = (float)z * 0.06f;
    float h  = fbm(nx, nz, seed);

    /* Flatland biome compresses the height variance and drops the
     * base level — in fully flat regions terrain hugs water level
     * with at most a couple of cells of variation. */
    float f = flatland_factor(x, z, seed);
    float h_scaled = h * (1.0f - f * 0.82f) + f * 0.18f;
    int height = (int)(h_scaled * 24.0f) + CRAFT_WATER_LEVEL - 4;

    /* Mountains add elevation but are inhibited by flatland (they
     * shouldn't co-exist; biome decides which is which). */
    float m = mountain_factor(x, z, seed) * (1.0f - f);
    height += (int)(m * 22.0f);

    /* River carving + BANK SLOPE.
     *
     * Two concentric zones controlled by the same low-frequency noise:
     *
     *   |n − 0.5| < RIVER_HALF (0.055)  — carved channel below water.
     *     Bed depth tapers 1→3 cells across the half-width.
     *
     *   RIVER_HALF ≤ |n − 0.5| < BANK_HALF (0.115) — bank slope.
     *     Natural terrain is pulled DOWN toward water level
     *     progressively as we approach the channel edge, eliminating
     *     the cliff between river and bank.
     *
     * Gated on mountain_factor only (mountains shrug off rivers).
     * The previous WL+6 height gate caused sheer cliffs wherever
     * the river-noise band ran through non-flatland — now the bank
     * slope always applies, smoothly pulling any neighbouring
     * elevation down to the channel edge. */
    {
        float n = fbm((float)x * 0.003f, (float)z * 0.003f,
                      seed ^ 0x7E417A11u);
        float dist = fabsf(n - 0.5f);
        const float river_half = 0.055f;
        const float bank_half  = 0.115f;
        if (dist < bank_half && m < 0.2f) {
            if (dist < river_half) {
                /* Carved channel. Bed depth tapers 1→3 cells.
                 * No min-h clamp — previously the carve was floored
                 * at (natural − 4), which on flatland with natural
                 * h = 32 clamped the river bed back UP to 28 = WL,
                 * leaving sand at WL with no water above it. Letting
                 * the bed reach WL−1..WL−3 lets the air-fill loop
                 * actually drop WATER into y > h. */
                float rs = 1.0f - dist / river_half;
                int depth = 1 + (int)(rs * 2.5f);
                if (depth > 3) depth = 3;
                int river_h = CRAFT_WATER_LEVEL - depth;
                if (river_h < height) height = river_h;
            } else {
                /* Bank slope — lerp height toward WATER_LEVEL as we
                 * approach the channel edge. bank_t = 0 at the outer
                 * edge of the bank, 1 at the river edge. Scale the
                 * lerp by (1 - mountain_factor) so partial-mountain
                 * areas slope partially instead of cliffing. */
                float bank_t = (bank_half - dist) / (bank_half - river_half);
                bank_t *= (1.0f - m);
                int target = CRAFT_WATER_LEVEL;
                if (height > target) {
                    int new_h = height - (int)((height - target) * bank_t + 0.5f);
                    if (new_h < height) height = new_h;
                }
            }
        }
    }

    if (height < 1) height = 1;
    if (height >= CRAFT_WORLD_Y - 4) height = CRAFT_WORLD_Y - 4;
    return height;
}

/* Is (x, y, z) inside a cave? Two cave types mixed together so the
 * underground reads as a system rather than a single mould:
 *
 *   1) Cheese chambers — threshold on summed 3D noise. Rounded
 *      pockets, the existing style. Threshold relaxed from 0.66 to
 *      0.62 so they're discoverable without spending too long
 *      digging.
 *
 *   2) Spaghetti tunnels — long thin worms formed by the
 *      intersection of two independent 3D noises both near 0.5.
 *      The intersection of two scalar fields traces a 1D curve, so
 *      pixels in band on both → continuous tube.
 *
 * Only valid for cells below the surface; callers gate on y < h - 3
 * before invoking. Mountains naturally get more cave volume because
 * their h is taller, expanding the underground envelope. */
static bool is_cave(int x, int y, int z, uint32_t seed) {
    /* Cheese chambers — rounded pocket density.
     *
     * Threshold 0.66 (was 0.62) brings cave fill back to roughly
     * 8-10% of below-surface cells; 0.62 was carving close to 20%
     * which made the underground dominate and exposed cave mouths
     * across every river bank's cliff face. */
    float n1 = val_noise3(x * 0.10f, y * 0.16f, z * 0.10f, seed ^ 0xCAFE5u);
    float n2 = val_noise3(x * 0.21f, y * 0.30f, z * 0.21f, seed ^ 0xCAFE6u);
    float v  = n1 * 0.65f + n2 * 0.35f;
    if (v > 0.66f) return true;
    /* Spaghetti tunnels — long thin worms. Y-scale matches X/Z so
     * tunnels twist in all three directions, not just horizontally. */
    float na = val_noise3(x * 0.085f, y * 0.085f, z * 0.085f, seed ^ 0xCAFE7u);
    if (fabsf(na - 0.5f) >= 0.055f) return false;
    float nb = val_noise3(x * 0.085f, y * 0.085f, z * 0.085f, seed ^ 0xCAFE8u);
    if (fabsf(nb - 0.5f) >= 0.055f) return false;
    return true;
}

/* Is there a tree spawned at column (x, z) for this seed?
 * No world-bounds gating — trees can exist anywhere in the infinite
 * world; the predicate is purely deterministic on (x, z, seed).
 *
 * Hash-first ordering matters: tree_at is called 49×CRAFT_WORLD_X×
 * CRAFT_WORLD_Z times per window load. The cheap (r & 0x7F)
 * comparison drops 127/128 of all calls before the expensive
 * height_at fbm computation runs — turns chunk-shift cost from
 * dominated by tree-neighbour scans into background noise. */
static bool tree_at(int x, int z, uint32_t seed) {
    uint32_t r = hash3(x, z, seed ^ 0xA1B2C3D4u);
    if ((r & 0x7F) != 0) return false;
    int h = height_at(x, z, seed);
    return h > CRAFT_WATER_LEVEL + 1;
}

typedef enum {
    TREE_OAK = 0,        /* Standard Minecraft small oak — 5-tall trunk */
    TREE_OAK_LARGE,      /* Minecraft big oak — 8-tall trunk with branches */
    TREE_PINE,           /* Tall conifer — pointed tip, wide layered base */
} TreeType;

/* Pick a tree shape per position deterministically. Mountain biome
 * is all pine; grassland is mostly standard oak with occasional
 * large oaks for variety. */
static TreeType tree_type_at(int x, int z, uint32_t seed) {
    float m = mountain_factor(x, z, seed);
    if (m > 0.4f) return TREE_PINE;
    uint32_t r = hash3(x, z, seed ^ 0x7E47A1B5u);
    int roll = (int)((r >> 3) & 0xFF);
    /* Grassland: ~75% standard oak, ~25% large oak. */
    if (roll < 192) return TREE_OAK;
    return TREE_OAK_LARGE;
}

/* Per-tree variant byte — used to rotate branch directions and so on.
 * Deterministic on (x, z, seed). */
static int tree_variant_at(int x, int z, uint32_t seed) {
    return (int)(hash3(x, z, seed ^ 0xBADBE1FFu) & 0xFF);
}

/* Per-shape block lookup. Each function returns the tree block at the
 * given (dx, dz) offset from the trunk base for the given absolute y,
 * or BLK_AIR if this cell isn't part of the tree. trunk_base is the
 * surface block's y; the trunk starts one cell above. */
/* Helpers — Chebyshev distance and "is a corner of a square ring". */
static inline int abs_i(int v) { return v < 0 ? -v : v; }
static inline int max_chess(int dx, int dz) {
    int a = abs_i(dx), b = abs_i(dz);
    return a > b ? a : b;
}

/* STANDARD MINECRAFT OAK
 *
 * 5-tall trunk. Canopy follows the small-oak pattern exactly:
 *
 *   y = top + 1   3×3
 *   y = top       3×3  (with corner-cell randomness in MC; we keep full)
 *   y = top - 1   5×5 minus the 4 single corners
 *   y = top - 2   5×5 minus the 4 single corners
 *
 * Trunk passes through the two lower leaf layers; the top two leaf
 * layers cap above the trunk. */
static BlockId tree_block_oak(int dx, int dz, int y, int trunk_base) {
    int trunk_y = trunk_base + 1;
    int top = trunk_y + 4;             /* 5-tall trunk (y = trunk_y..top) */
    if (dx == 0 && dz == 0 && y >= trunk_y && y <= top) return BLK_WOOD;
    int ady = y - top;
    int adx = abs_i(dx), adz = abs_i(dz);
    int chess = max_chess(dx, dz);
    /* Bottom two leaf layers — wide 5×5 minus corners. */
    if (ady == -2 || ady == -1) {
        if (chess <= 2 && !(adx == 2 && adz == 2)) {
            if (!(dx == 0 && dz == 0)) return BLK_LEAVES;
        }
    }
    /* Top two leaf layers — 3×3. */
    if (ady == 0 || ady == 1) {
        if (chess <= 1) {
            if (!(dx == 0 && dz == 0 && ady == 0)) return BLK_LEAVES;
        }
    }
    return BLK_AIR;
}

/* LARGE OAK (Minecraft "big oak" approximation)
 *
 * 8-tall trunk with two side branches at mid-height extending in
 * opposite directions (axis from variant bit 0). Each branch is 2
 * wood blocks plus a 3×3 leaf cluster (in the branch's Y plane and
 * one cell above) around the branch tip. Main crown sits at the top
 * of the trunk with the same layered shape as a small oak. */
static BlockId tree_block_oak_large(int variant, int dx, int dz, int y, int trunk_base) {
    int trunk_y = trunk_base + 1;
    int top = trunk_y + 7;             /* 8-tall trunk */
    if (dx == 0 && dz == 0 && y >= trunk_y && y <= top) return BLK_WOOD;

    /* Branch axis from variant — 0 = X, 1 = Z. Branches go opposite
     * directions along that axis. */
    int axis = variant & 1;
    int b1x = (axis == 0) ?  1 : 0;
    int b1z = (axis == 0) ?  0 : 1;
    int b2x = -b1x;
    int b2z = -b1z;

    /* Branch 1 lower (y = trunk_y + 3) in +axis, branch 2 higher
     * (y = trunk_y + 5) in -axis. Each: 2 cells of wood out. */
    int b1y = trunk_y + 3;
    int b2y = trunk_y + 5;
    if (y == b1y) {
        if (dx == b1x && dz == b1z) return BLK_WOOD;
        if (dx == 2*b1x && dz == 2*b1z) return BLK_WOOD;
    }
    if (y == b2y) {
        if (dx == b2x && dz == b2z) return BLK_WOOD;
        if (dx == 2*b2x && dz == 2*b2z) return BLK_WOOD;
    }

    /* Leaf clusters at branch tips — 3×3 in the branch's Y plane,
     * plus a 3-cell cap one above. */
    int adx = abs_i(dx), adz = abs_i(dz);
    int chess = max_chess(dx, dz);
    for (int b = 0; b < 2; b++) {
        int by = (b == 0) ? b1y : b2y;
        int btx = (b == 0) ? 2*b1x : 2*b2x;
        int btz = (b == 0) ? 2*b1z : 2*b2z;
        int tdx = dx - btx;
        int tdz = dz - btz;
        int tchess = max_chess(tdx, tdz);
        if (y == by && tchess <= 1) {
            if (!(tdx == 0 && tdz == 0)) {
                if (!(dx == 0 && dz == 0)) return BLK_LEAVES;
            }
        }
        if (y == by + 1 && tchess <= 1 &&
            (abs_i(tdx) + abs_i(tdz)) <= 1) {
            if (!(dx == 0 && dz == 0)) return BLK_LEAVES;
        }
    }

    /* Main crown at trunk top — same layered shape as standard oak. */
    int ady = y - top;
    if (ady == -2 || ady == -1) {
        if (chess <= 2 && !(adx == 2 && adz == 2)) {
            if (!(dx == 0 && dz == 0)) return BLK_LEAVES;
        }
    }
    if (ady == 0 || ady == 1) {
        if (chess <= 1) {
            if (!(dx == 0 && dz == 0 && ady == 0)) return BLK_LEAVES;
        }
    }
    return BLK_AIR;
}

/* PINE — tall conifer with a pointed top and a wider layered base.
 *
 * 8-tall trunk; tip leaf sits one cell above. Canopy alternates
 * narrow / wide skirts on the way down for that layered spruce look:
 *
 *   y = top + 1     single leaf (tip)
 *   y = top         + (4 cardinals around trunk top)
 *   y = top - 1     3×3 (full ring)
 *   y = top - 2     +  (skirt gap — narrower for layered look)
 *   y = top - 3     3×3
 *   y = top - 4     5×5 minus single corners (wide tier)
 *   y = top - 5     5×5 minus single corners (widest base)
 */
static BlockId tree_block_pine(int dx, int dz, int y, int trunk_base) {
    int trunk_y = trunk_base + 1;
    int top = trunk_y + 7;             /* 8-tall trunk */
    if (dx == 0 && dz == 0 && y >= trunk_y && y <= top) return BLK_WOOD;
    int ady = y - top;
    int adx = abs_i(dx), adz = abs_i(dz);
    int chess = max_chess(dx, dz);
    int manh = adx + adz;
    /* Tip. */
    if (ady == 1 && dx == 0 && dz == 0) return BLK_LEAVES;
    /* Cardinal cross around the trunk's topmost cell. */
    if (ady == 0 && manh == 1) return BLK_LEAVES;
    /* Two 3×3 layers and the skirt-gap between them. */
    if (ady == -1) {
        if (chess <= 1 && !(dx == 0 && dz == 0)) return BLK_LEAVES;
    }
    if (ady == -2) {
        /* + only — narrow gap layer for the layered profile. */
        if (manh == 1) return BLK_LEAVES;
    }
    if (ady == -3) {
        if (chess <= 1 && !(dx == 0 && dz == 0)) return BLK_LEAVES;
    }
    /* Wide 5×5-minus-corners tiers near the base. */
    if (ady == -4 || ady == -5) {
        if (chess <= 2 && !(adx == 2 && adz == 2)) {
            if (!(dx == 0 && dz == 0)) return BLK_LEAVES;
        }
    }
    return BLK_AIR;
}

static BlockId tree_block_at(TreeType t, int variant, int dx, int dz,
                             int y, int trunk_base) {
    switch (t) {
        case TREE_OAK_LARGE: return tree_block_oak_large(variant, dx, dz, y, trunk_base);
        case TREE_PINE:      return tree_block_pine(dx, dz, y, trunk_base);
        case TREE_OAK:
        default:             return tree_block_oak(dx, dz, y, trunk_base);
    }
}

/* --- Wooden huts -----------------------------------------------------
 *
 * Small 5×5 plank cabins. A hut "exists" at world column (hx, hz) iff
 * hut_origin_at(hx, hz, seed) returns true. The footprint occupies
 * cells [hx..hx+4, hz..hz+4]; walls run y=gy+1..gy+3 around the
 * perimeter, roof at y=gy+4 covers the full 5×5, and a 2-cell door
 * (y=gy+1..2) opens on one side picked by the variant byte. The hut
 * floor (y=gy) is left untouched so the player walks in on the natural
 * surface block (grass).
 *
 * Generation is deterministic per (hx, hz, seed) and must be applied
 * identically in craft_gen_block_at and craft_gen_column — the save
 * diff layer relies on per-cell agreement. */
#define HUT_W 5
#define HUT_H 5

static bool hut_origin_at(int hx, int hz, uint32_t seed) {
    uint32_t r = hash3(hx, hz, seed ^ 0xCAB1F00Du);
    /* ~1 in 4 096 columns → roughly one hut per 64×64 region.
     * 4× denser than the original 1/16384 so the player actually
     * encounters one while exploring. */
    if ((r & 0xFFF) != 0) return false;
    /* Lowland-only — mountain biome shrugs huts off. */
    float m = mountain_factor(hx, hz, seed);
    if (m > 0.20f) return false;
    /* Above water, naturally flat across the whole footprint. */
    int ref_h = height_at(hx, hz, seed);
    if (ref_h <= CRAFT_WATER_LEVEL + 1) return false;
    int min_h = ref_h, max_h = ref_h;
    for (int dz = 0; dz < HUT_H; dz++) {
        for (int dx = 0; dx < HUT_W; dx++) {
            int h = height_at(hx + dx, hz + dz, seed);
            if (h < min_h) min_h = h;
            if (h > max_h) max_h = h;
        }
    }
    /* Reject sloped sites — walls would hang in air or bury. */
    if (max_h - min_h > 1) return false;
    return true;
}

/* Floor Y for the hut at (hx, hz) — taken as the minimum of the
 * footprint so walls always start from a complete grass base. */
static int hut_floor_y(int hx, int hz, uint32_t seed) {
    int min_h = height_at(hx, hz, seed);
    for (int dz = 0; dz < HUT_H; dz++) {
        for (int dx = 0; dx < HUT_W; dx++) {
            int h = height_at(hx + dx, hz + dz, seed);
            if (h < min_h) min_h = h;
        }
    }
    return min_h;
}

/* Per-hut variant byte — bits 0-1 pick the door wall direction. */
static int hut_variant(int hx, int hz, uint32_t seed) {
    return (int)(hash3(hx, hz, seed ^ 0x110D5EEDu) & 0xFFu);
}

/* Local hut chest position — fixed interior corner so the predicate
 * stays a single equality test. dx=1, dz=1, dy=1: one cell in from
 * the south-west corner of the hut, on the floor. Far enough from
 * any wall's door opening to be reachable from every variant. */
#define HUT_CHEST_DX 1
#define HUT_CHEST_DZ 1
#define HUT_CHEST_DY 1

/* What block sits at hut-local (dx, dz, dy)?
 *   dy = 0  : floor (always AIR — keep natural surface)
 *   dy = 1-3: wall perimeter (PLANK) / interior (AIR) / door opening (AIR)
 *            with a BLK_CHEST at the fixed corner cell
 *   dy = 4  : roof (full PLANK 5×5)
 * dx, dz in [0, HUT_W-1] / [0, HUT_H-1]. */
static BlockId hut_block_local(int dx, int dz, int dy, int variant) {
    if (dy <= 0 || dy > 4) return BLK_AIR;
    if (dy == 4) {
        /* Roof — full square. */
        if (dx >= 0 && dx < HUT_W && dz >= 0 && dz < HUT_H) return BLK_PLANK;
        return BLK_AIR;
    }
    /* Chest sits on the floor at a fixed interior corner. */
    if (dy == HUT_CHEST_DY && dx == HUT_CHEST_DX && dz == HUT_CHEST_DZ) {
        return BLK_CHEST;
    }
    /* Walls — perimeter only. */
    bool on_perim = (dx == 0 || dx == HUT_W - 1 || dz == 0 || dz == HUT_H - 1);
    if (!on_perim) return BLK_AIR;
    /* Door — 2-cell opening at middle of the chosen wall, dy 1..2. */
    if (dy <= 2) {
        int dir = variant & 3;
        switch (dir) {
            case 0: if (dz == 0          && dx == HUT_W / 2) return BLK_AIR; break;  /* south */
            case 1: if (dz == HUT_H - 1  && dx == HUT_W / 2) return BLK_AIR; break;  /* north */
            case 2: if (dx == HUT_W - 1  && dz == HUT_H / 2) return BLK_AIR; break;  /* east */
            case 3: if (dx == 0          && dz == HUT_H / 2) return BLK_AIR; break;  /* west */
        }
    }
    return BLK_PLANK;
}

/* If world cell (x, y, z) lies inside any hut footprint, set *covered
 * and return the hut block (which may be AIR for interior/door). The
 * 5×5 scan in (dx, dz) is small enough to run per-cell from
 * craft_gen_block_at without measurable cost. */
static BlockId hut_cell(int x, int y, int z, uint32_t seed, bool *covered) {
    *covered = false;
    for (int dz = -(HUT_H - 1); dz <= 0; dz++) {
        for (int dx = -(HUT_W - 1); dx <= 0; dx++) {
            int hx = x + dx, hz = z + dz;
            if (!hut_origin_at(hx, hz, seed)) continue;
            int gy = hut_floor_y(hx, hz, seed);
            int dy = y - gy;
            if (dy <= 0 || dy > 4) continue;  /* outside hut Y range */
            *covered = true;
            int variant = hut_variant(hx, hz, seed);
            return hut_block_local(-dx, -dz, dy, variant);
        }
    }
    return BLK_AIR;
}

BlockId craft_gen_block_at(int x, int y, int z, uint32_t seed) {
    /* Only Y is bounded — X and Z extend infinitely. */
    if ((unsigned)y >= CRAFT_WORLD_Y) return BLK_AIR;
    (void)x;

    int h = height_at(x, z, seed);

    /* Underground / surface columns. Caves carve out the deep stone
     * layer only — never touches the topmost dirt or surface block,
     * so the silhouette of the world from above is unaffected. The
     * y>=2 floor keeps a stone "bedrock" layer below caves, and the
     * y<h-8 ceiling keeps the top 5 cells of stone solid so caves
     * stay genuinely subterranean (matches craft_gen_column). */
    if (y <  h - 3) {
        if (y >= 2 && y < h - 8 && is_cave(x, y, z, seed)) return BLK_AIR;
        return BLK_STONE;
    }
    if (y <  h)     return BLK_DIRT;
    if (y == h) {
        if (h <= CRAFT_WATER_LEVEL + 1) return BLK_SAND;
        return BLK_GRASS;
    }
    /* Above ground but below water: water or air. */
    if (y <= CRAFT_WATER_LEVEL) return BLK_WATER;

    /* Hut check FIRST so walls/interior override any tree blocks that
     * would otherwise land in the footprint. Hut floor (y=gy) is not
     * "covered" — that's the natural surface. */
    {
        bool covered;
        BlockId hb = hut_cell(x, y, z, seed, &covered);
        if (covered) return hb;
    }

    /* Tree check — scan a 7×7 neighbourhood of columns. */
    for (int dz = -3; dz <= 3; dz++) {
        for (int dx = -3; dx <= 3; dx++) {
            int tx = x + dx, tz = z + dz;
            if (!tree_at(tx, tz, seed)) continue;
            int th = height_at(tx, tz, seed);
            TreeType tt = tree_type_at(tx, tz, seed);
            int tv = tree_variant_at(tx, tz, seed);
            BlockId b = tree_block_at(tt, tv, -dx, -dz, y, th);
            if (b != BLK_AIR) return b;
        }
    }
    return BLK_AIR;
}

void craft_gen_column(int wx, int wz, uint32_t seed,
                      uint8_t out[/* CRAFT_WORLD_Y */]) {
    int h = height_at(wx, wz, seed);
    if (h < 1) h = 1;
    if (h >= CRAFT_WORLD_Y - 4) h = CRAFT_WORLD_Y - 4;

    float m = mountain_factor(wx, wz, seed);
    /* Ore placement chance — denser in mountain biome.
     * Iron is rarer than coal in both biomes. Test: (hash & mask)==0. */
    uint32_t coal_mask = (m > 0.5f) ? 0x0F : 0x3F;   /* ~1/16 vs 1/64 */
    uint32_t iron_mask = (m > 0.5f) ? 0x1F : 0x7F;   /* ~1/32 vs 1/128 */

    /* Terrain pass — stone (or coal ore) / dirt / surface / water / air. */
    int wl = CRAFT_WATER_LEVEL;
    /* Mountain peaks: surface is stone rather than grass once we're
     * well above tree line. */
    BlockId surface;
    if (h <= wl + 1)                    surface = BLK_SAND;
    else if (m > 0.5f && h > wl + 18)   surface = BLK_STONE;
    else                                surface = BLK_GRASS;
    /* Cave depth floor — caves only carve below (h - 8) so the top
     * 5 cells under any surface stay solid. Without this, hill
     * columns next to rivers expose 3-4 cells of cave mouths in
     * the cliff band (h-3..h-1 are dirt; cave carving runs up to
     * h-4, sometimes adjacent to surface). With the river-bank
     * smoothing the cliff is mostly gone, but keeping caves
     * genuinely subterranean is the right default anyway. */
    int cave_top = h - 8;
    for (int y = 0; y < h - 3; y++) {
        /* Cave carve before ore placement — caves remove a cell
         * entirely so ore doesn't get assigned to it. y<2 stays
         * solid as a "bedrock" floor. */
        if (y >= 2 && y < cave_top && is_cave(wx, y, wz, seed)) {
            out[y] = BLK_AIR;
            continue;
        }
        uint32_t r = hash3(wx, y, wz) ^ (seed * 1370529931u);
        BlockId b = BLK_STONE;
        /* Depth-gated precious ores tested before coal/iron so the
         * rarer veins win when several would hit at the same cell. */
        if      (y < 12 && (r & 0xFFu) == 0) b = BLK_DIAMOND_ORE;   /* 1/256 below y=12 */
        else if (y < 16 && (r & 0x3Fu) == 0) b = BLK_REDSTONE_ORE;  /* 1/64  below y=16 */
        else if (y < 20 && (r & 0x7Fu) == 0) b = BLK_GOLD_ORE;      /* 1/128 below y=20 */
        else if (y < 30 && (r & 0x7Fu) == 0) b = BLK_SILVER_ORE;    /* 1/128 below y=30 */
        else if ((r & coal_mask) == 0)       b = BLK_COAL_ORE;
        else if ((r & iron_mask) == 0)       b = BLK_IRON_ORE;
        out[y] = b;
    }
    for (int y = h - 3; y < h; y++) {
        /* Mountains: replace dirt sub-surface with stone too. */
        out[y] = (m > 0.5f && h > wl + 18) ? BLK_STONE : BLK_DIRT;
    }
    out[h] = surface;
    /* Above-surface fill — fused single loop so GCC can't lower the
     * old two-loop form to a pair of memsets where a negative count
     * (h > wl) underflows to a huge unsigned and smashes the stack. */
    for (int y = h + 1; y < CRAFT_WORLD_Y; y++) {
        out[y] = (y <= wl) ? BLK_WATER : BLK_AIR;
    }

    /* Tree pass — scan 7×7 neighbour columns. tree_at + height_at are
     * cached implicitly via height_at being deterministic + cheap.
     * Only neighbours that actually have a tree contribute. */
    for (int dz = -3; dz <= 3; dz++) {
        for (int dx = -3; dx <= 3; dx++) {
            int tx = wx + dx, tz = wz + dz;
            if (!tree_at(tx, tz, seed)) continue;
            int th = height_at(tx, tz, seed);
            TreeType tt = tree_type_at(tx, tz, seed);
            int tv = tree_variant_at(tx, tz, seed);
            for (int y = th + 1; y < CRAFT_WORLD_Y; y++) {
                BlockId b = tree_block_at(tt, tv, -dx, -dz, y, th);
                if (b != BLK_AIR && out[y] == BLK_AIR) out[y] = b;
            }
        }
    }

    /* Hut pass — runs AFTER trees so walls/interior overwrite any tree
     * blocks that landed in the footprint. Scan 5×5 candidate hut
     * origins whose footprint covers this column. */
    for (int dz = -(HUT_H - 1); dz <= 0; dz++) {
        for (int dx = -(HUT_W - 1); dx <= 0; dx++) {
            int hx = wx + dx, hz = wz + dz;
            if (!hut_origin_at(hx, hz, seed)) continue;
            int gy = hut_floor_y(hx, hz, seed);
            int variant = hut_variant(hx, hz, seed);
            for (int dy = 1; dy <= 4; dy++) {
                int y = gy + dy;
                if (y < 0 || y >= CRAFT_WORLD_Y) continue;
                /* Stamp unconditionally — interior AIR clears any tree
                 * block sitting where the hut wants empty space. */
                out[y] = (uint8_t)hut_block_local(-dx, -dz, dy, variant);
            }
        }
    }
}

void craft_gen_world(uint32_t seed) {
    craft_world_clear();
    for (int y = 0; y < CRAFT_WORLD_Y; y++) {
        for (int z = 0; z < CRAFT_WORLD_Z; z++) {
            for (int x = 0; x < CRAFT_WORLD_X; x++) {
                BlockId b = craft_gen_block_at(x, y, z, seed);
                if (b != BLK_AIR) craft_world_set(x, y, z, b);
            }
        }
    }
    craft_world_dirty = 0;
}

Vec3 craft_gen_spawn(void) {
    /* Search outward from the window centre for a grass/sand column
     * with two clear cells of headroom — that's what the player AABB
     * needs (PLAYER_HEIGHT=1.7 m, so feet at gy+1 and y0..y1 range
     * covers cells gy+1 and gy+2). Previously we returned the first
     * grass we found, which let the player spawn under a tree trunk
     * or inside a hut wall and be permanently stuck. */
    int cx = craft_world_origin_x + CRAFT_WORLD_X / 2;
    int cz = craft_world_origin_z + CRAFT_WORLD_Z / 2;
    for (int radius = 0; radius < CRAFT_WORLD_X / 2; radius++) {
        for (int dz = -radius; dz <= radius; dz++) {
            for (int dx = -radius; dx <= radius; dx++) {
                int x = cx + dx, z = cz + dz;
                /* Find topmost grass/sand cell. */
                int gy = -1;
                for (int y = CRAFT_WORLD_Y - 2; y > 0; y--) {
                    BlockId blk = craft_world_get(x, y, z);
                    if (blk == BLK_GRASS || blk == BLK_SAND) {
                        gy = y;
                        break;
                    }
                }
                if (gy < 0) continue;
                /* Headroom check — both cells the player will occupy
                 * must be non-solid. craft_block_solid lets torches
                 * and water count as passable; trees, walls, chests
                 * etc. all block. */
                BlockId head1 = craft_world_get(x, gy + 1, z);
                BlockId head2 = craft_world_get(x, gy + 2, z);
                if (craft_block_solid(head1) || craft_block_solid(head2)) continue;
                return v3((float)x + 0.5f,
                          (float)gy + 1.0f + 1.6f,
                          (float)z + 0.5f);
            }
        }
    }
    return v3((float)cx + 0.5f, (float)CRAFT_WATER_LEVEL + 2.0f, (float)cz + 0.5f);
}

bool craft_gen_is_hut_chest(int wx, int wy, int wz, uint32_t seed) {
    /* Walk back over every (hx, hz) origin whose 5×5 footprint could
     * cover (wx, wz). For each that's actually a hut, check whether
     * (wx, wy, wz) lines up with that hut's chest cell. */
    for (int dz = -(HUT_H - 1); dz <= 0; dz++) {
        for (int dx = -(HUT_W - 1); dx <= 0; dx++) {
            int hx = wx + dx, hz = wz + dz;
            if (!hut_origin_at(hx, hz, seed)) continue;
            int gy = hut_floor_y(hx, hz, seed);
            if (wy != gy + HUT_CHEST_DY) continue;
            if (-dx != HUT_CHEST_DX) continue;
            if (-dz != HUT_CHEST_DZ) continue;
            return true;
        }
    }
    return false;
}

void craft_gen_seed_hut_chest(CraftChest *c, int wx, int wy, int wz,
                              uint32_t seed) {
    uint32_t r = hash3(wx, wy, wz) ^ (seed * 0xA1B2C3D4u);
    int slot = 0;
    /* Always: 2-4 sticks. Cheap basic crafting fodder. */
    c->slots[slot].blk = BLK_STICK;
    c->slots[slot].n   = 2 + (uint8_t)((r >> 0) & 3);
    slot++;
    /* Always: 2-4 planks. */
    c->slots[slot].blk = BLK_PLANK;
    c->slots[slot].n   = 2 + (uint8_t)((r >> 2) & 3);
    slot++;
    /* 60 %: 1-2 torches. */
    if ((r & 0x30) != 0) {
        c->slots[slot].blk = BLK_TORCH;
        c->slots[slot].n   = 1 + (uint8_t)((r >> 6) & 1);
        slot++;
    }
    /* 35 %: bow + 4-12 arrows. */
    if (((r >> 7) & 7) < 3) {
        c->slots[slot].blk = BLK_BOW;
        c->slots[slot].n   = 1;
        slot++;
        c->slots[slot].blk = BLK_ARROW;
        c->slots[slot].n   = 4 + (uint8_t)((r >> 10) & 7);
        slot++;
    }
    /* 30 %: 1-2 iron ingots. */
    if (((r >> 14) & 7) < 2) {
        c->slots[slot].blk = BLK_IRON_INGOT;
        c->slots[slot].n   = 1 + (uint8_t)((r >> 17) & 1);
        slot++;
    }
    /* 25 %: a tier-rolled pickaxe (wood / stone / iron weighted). */
    if (((r >> 18) & 3) == 0) {
        uint32_t tier = (r >> 20) & 7;
        BlockId pick = BLK_PICKAXE_WOOD;
        if (tier >= 6)      pick = BLK_PICKAXE_IRON;
        else if (tier >= 3) pick = BLK_PICKAXE_STONE;
        c->slots[slot].blk = pick;
        c->slots[slot].n   = 1;
        slot++;
    }
    /* 15 %: a wood / stone sword. */
    if (((r >> 23) & 7) == 0) {
        c->slots[slot].blk = (((r >> 26) & 1) ? BLK_SWORD_STONE : BLK_SWORD_WOOD);
        c->slots[slot].n   = 1;
        slot++;
    }
    (void)slot;
}

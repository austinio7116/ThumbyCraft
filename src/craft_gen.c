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

/* River factor in [0, 1]. Sharp ridge noise — narrow band around
 * where the underlying fbm crosses 0.5. The 25× multiplier makes
 * the activated band only a few world units wide so rivers come
 * out as small streams rather than valleys. */
static float river_factor(int x, int z, uint32_t seed) {
    float n  = fbm((float)x * 0.012f, (float)z * 0.012f, seed ^ 0x7E417A11u);
    float dr = n - 0.5f;
    float ridge = 1.0f - fabsf(dr) * 25.0f;
    if (ridge < 0.0f) ridge = 0.0f;
    return ridge;
}

/* Returns 0 = no river, 1-2 = depth below water level. The narrow
 * ridge above means total channel is roughly 3-5 cells wide with
 * the deepest 1-2 cells at the very centre. */
static int river_carve_depth(int x, int z, uint32_t seed) {
    float r = river_factor(x, z, seed);
    if (r < 0.30f) return 0;
    /* Mountains shrug off rivers. */
    float m = mountain_factor(x, z, seed);
    if (m > 0.3f) return 0;
    float t = (r - 0.30f) / 0.70f;            /* 0..1 across active band */
    int depth = (int)(t * 2.5f);              /* 0, 1, 2 */
    if (depth > 2) depth = 2;
    return depth;
}

static int height_at(int x, int z, uint32_t seed) {
    float nx = (float)x * 0.06f;
    float nz = (float)z * 0.06f;
    float h  = fbm(nx, nz, seed);
    int  height = (int)(h * 24.0f) + CRAFT_WATER_LEVEL - 4;
    /* Mountain biome adds up to ~22 blocks of extra elevation. */
    float m = mountain_factor(x, z, seed);
    height += (int)(m * 22.0f);
    /* River carving — ONLY where the natural terrain is already a
     * lowland (within 2 blocks of water level). Without this gate the
     * winding ridge noise sliced canyons through any hill it crossed.
     * Now rivers only form where the noise's path happens to coincide
     * with naturally low ground — which is how real rivers behave. */
    if (height <= CRAFT_WATER_LEVEL + 2) {
        int rd = river_carve_depth(x, z, seed);
        if (rd > 0) {
            int river_h = CRAFT_WATER_LEVEL - rd;
            if (river_h < height) height = river_h;
        }
    }
    if (height < 1) height = 1;
    if (height >= CRAFT_WORLD_Y - 4) height = CRAFT_WORLD_Y - 4;
    return height;
}

/* Is (x, y, z) inside a cave?  Two-octave 3D value noise with
 * stretched Y so caves form mostly-horizontal chambers rather than
 * vertical shafts. Returns true above the threshold (which gives
 * ~6% cave density). Only valid for cells strictly below the
 * surface; callers should not carve grass, dirt, or water. */
static bool is_cave(int x, int y, int z, uint32_t seed) {
    /* Two octaves of 3D noise summed, biased toward thin connected
     * tunnels. Y-scale tighter than X/Z so caves spread sideways. */
    float n1 = val_noise3(x * 0.10f, y * 0.16f, z * 0.10f, seed ^ 0xCAFE5u);
    float n2 = val_noise3(x * 0.21f, y * 0.30f, z * 0.21f, seed ^ 0xCAFE6u);
    float v  = n1 * 0.65f + n2 * 0.35f;
    return v > 0.66f;
}

/* Is there a tree spawned at column (x, z) for this seed?
 * No world-bounds gating — trees can exist anywhere in the infinite
 * world; the predicate is purely deterministic on (x, z, seed). */
static bool tree_at(int x, int z, uint32_t seed) {
    int h = height_at(x, z, seed);
    if (h <= CRAFT_WATER_LEVEL + 1) return false;
    uint32_t r = hash3(x, z, seed ^ 0xA1B2C3D4u);
    return ((r & 0x7F) == 0);
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
    /* ~1 in 16 384 columns → roughly one hut per 128×128 region. */
    if ((r & 0x3FFF) != 0) return false;
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

/* What block sits at hut-local (dx, dz, dy)?
 *   dy = 0  : floor (always AIR — keep natural surface)
 *   dy = 1-3: wall perimeter (PLANK) / interior (AIR) / door opening (AIR)
 *   dy = 4  : roof (full PLANK 5×5)
 * dx, dz in [0, HUT_W-1] / [0, HUT_H-1]. */
static BlockId hut_block_local(int dx, int dz, int dy, int variant) {
    if (dy <= 0 || dy > 4) return BLK_AIR;
    if (dy == 4) {
        /* Roof — full square. */
        if (dx >= 0 && dx < HUT_W && dz >= 0 && dz < HUT_H) return BLK_PLANK;
        return BLK_AIR;
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
     * y>=2 floor keeps a stone "bedrock" layer below caves. */
    if (y <  h - 3) {
        if (y >= 2 && is_cave(x, y, z, seed)) return BLK_AIR;
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
    for (int y = 0; y < h - 3; y++) {
        /* Cave carve before ore placement — caves remove a cell
         * entirely so ore doesn't get assigned to it. y<2 stays
         * solid as a "bedrock" floor. */
        if (y >= 2 && is_cave(wx, y, wz, seed)) {
            out[y] = BLK_AIR;
            continue;
        }
        uint32_t r = hash3(wx, y, wz) ^ (seed * 1370529931u);
        BlockId b = BLK_STONE;
        if ((r & coal_mask) == 0)      b = BLK_COAL_ORE;
        else if ((r & iron_mask) == 0) b = BLK_IRON_ORE;
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
    /* Centre of the currently loaded window. */
    int cx = craft_world_origin_x + CRAFT_WORLD_X / 2;
    int cz = craft_world_origin_z + CRAFT_WORLD_Z / 2;
    for (int radius = 0; radius < CRAFT_WORLD_X / 2; radius++) {
        for (int dz = -radius; dz <= radius; dz++) {
            for (int dx = -radius; dx <= radius; dx++) {
                int x = cx + dx, z = cz + dz;
                for (int y = CRAFT_WORLD_Y - 2; y > 0; y--) {
                    BlockId blk = craft_world_get(x, y, z);
                    if (blk == BLK_GRASS || blk == BLK_SAND) {
                        return v3((float)x + 0.5f,
                                  (float)y + 1.0f + 1.6f,
                                  (float)z + 0.5f);
                    }
                }
            }
        }
    }
    return v3((float)cx + 0.5f, (float)CRAFT_WATER_LEVEL + 2.0f, (float)cz + 0.5f);
}

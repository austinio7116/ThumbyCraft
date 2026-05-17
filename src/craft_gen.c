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

/* River factor in [0, 1]. Ridge noise — value peaks where the
 * underlying fbm crosses 0.5, creating winding 1D lines through
 * 2D space. Wider falloff multiplier (5×) gives gentler bank
 * slopes than the previous 8× value. */
static float river_factor(int x, int z, uint32_t seed) {
    float n  = fbm((float)x * 0.012f, (float)z * 0.012f, seed ^ 0x7E417A11u);
    float dr = n - 0.5f;
    float ridge = 1.0f - fabsf(dr) * 5.0f;
    if (ridge < 0.0f) ridge = 0.0f;
    return ridge;
}

/* Rivers depress the height below water level when the ridge is
 * strong enough AND we're not in a mountain. Returns the depression
 * in blocks (0 = no river here).
 *
 * Softer than before: max 2 blocks deep (was 4), and the activation
 * threshold + slope are tuned so the BANKS taper gently down to the
 * channel over many blocks rather than dropping into a canyon. */
static int river_carve_depth(int x, int z, uint32_t seed) {
    float r = river_factor(x, z, seed);
    if (r < 0.30f) return 0;
    /* Mountains shrug off rivers — keeps them in lowlands. */
    float m = mountain_factor(x, z, seed);
    if (m > 0.3f) return 0;
    /* Linear ramp from r=0.30 (depth=0, river edge) to r=1.0
     * (depth=2, channel centre). The wide active band means a river
     * smoothly sinks into existing terrain over many blocks instead
     * of cutting a sharp canyon. */
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
    /* River carving — clamp the surface down so water naturally
     * pools in the channel. */
    int rd = river_carve_depth(x, z, seed);
    if (rd > 0) {
        int river_h = CRAFT_WATER_LEVEL - rd;
        if (river_h < height) height = river_h;
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

/* Tree shape: trunk = 4 tall wood at column (x, base+1..base+4), where
 * base = surface y. Top of trunk = base+4. Leaf canopy follows the
 * original place_tree layout. */
static BlockId tree_block_at(int dx, int dz, int y, int trunk_base) {
    int trunk_y = trunk_base + 1;
    int top = trunk_y + 3;             /* top of trunk */
    if (dx == 0 && dz == 0 && y >= trunk_y && y < trunk_y + 4) return BLK_WOOD;
    /* Lower canopy at top..top+1 within radius² ≤ 5. */
    for (int dy = 0; dy < 2; dy++) {
        if (y == top + dy) {
            int r2 = dx * dx + dz * dz;
            if (r2 > 5) continue;
            if (dx == 0 && dz == 0) continue;   /* trunk occupies this */
            return BLK_LEAVES;
        }
    }
    /* Crown at top+2: centre + 4 cardinal neighbours. */
    if (y == top + 2) {
        if (dx == 0 && dz == 0) return BLK_LEAVES;
        if ((dx == 1 || dx == -1) && dz == 0) return BLK_LEAVES;
        if (dx == 0 && (dz == 1 || dz == -1)) return BLK_LEAVES;
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

    /* Tree check — scan a 7×7 neighbourhood of columns. */
    for (int dz = -3; dz <= 3; dz++) {
        for (int dx = -3; dx <= 3; dx++) {
            int tx = x + dx, tz = z + dz;
            if (!tree_at(tx, tz, seed)) continue;
            int th = height_at(tx, tz, seed);
            BlockId b = tree_block_at(-dx, -dz, y, th);
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
            for (int y = th + 1; y < CRAFT_WORLD_Y; y++) {
                BlockId b = tree_block_at(-dx, -dz, y, th);
                if (b != BLK_AIR && out[y] == BLK_AIR) out[y] = b;
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

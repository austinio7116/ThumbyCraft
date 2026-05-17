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

static int height_at(int x, int z, uint32_t seed) {
    float nx = (float)x * 0.06f;
    float nz = (float)z * 0.06f;
    float h  = fbm(nx, nz, seed);
    int  height = (int)(h * 24.0f) + CRAFT_WATER_LEVEL - 4;
    if (height < 1) height = 1;
    if (height >= CRAFT_WORLD_Y - 8) height = CRAFT_WORLD_Y - 8;
    return height;
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

    /* Underground / surface columns. */
    if (y <  h - 3) return BLK_STONE;
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
    if (h >= CRAFT_WORLD_Y - 8) h = CRAFT_WORLD_Y - 8;

    /* Terrain pass — stone / dirt / surface / water / air. */
    int wl = CRAFT_WATER_LEVEL;
    BlockId surface = (h <= wl + 1) ? BLK_SAND : BLK_GRASS;
    for (int y = 0; y < h - 3; y++)         out[y] = BLK_STONE;
    for (int y = h - 3; y < h; y++)         out[y] = BLK_DIRT;
    out[h] = surface;
    int start_water = h + 1;
    for (int y = start_water; y <= wl; y++) out[y] = BLK_WATER;
    int start_air = (start_water > wl + 1) ? start_water : wl + 1;
    for (int y = start_air; y < CRAFT_WORLD_Y; y++) out[y] = BLK_AIR;

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

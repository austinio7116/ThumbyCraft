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
#include "craft_blocks.h"      /* FACE_* for baked redstone trap orients */
#include "craft_torches.h"     /* craft_torches_record_orient (trap dispensers) */

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

/* --- Climate model (temperature × humidity) ---------------------- *
 * Beta-1.8-style biome assignment: two independent low-frequency
 * noise fields (regions ~150 cells wide) pick the lowland biome via
 * a small lookup. Mountain noise (mountain_factor) overrides to the
 * Extreme-Hills equivalent where elevation is high. Used for surface
 * blocks, tree selection, and swamp terrain lowering. */
typedef enum {
    CBIOME_PLAINS = 0,
    CBIOME_FOREST,
    CBIOME_DESERT,
    CBIOME_TAIGA,        /* cold conifer forest */
    CBIOME_SWAMP,        /* warm, very wet, low + flat, giant trees */
    CBIOME_MOUNTAINS,    /* extreme hills */
    CBIOME_JUNGLE,       /* hot + wet — tall dense trees + vines */
    CBIOME_SAVANNA,      /* hot + moderate — acacias, dry grass */
} CraftBiome;
#define CBIOME_COUNT 8

static float temperature_at(int x, int z, uint32_t seed) {
    return fbm((float)x * 0.0062f, (float)z * 0.0062f, seed ^ 0x713D17E5u);
}
static float humidity_at(int x, int z, uint32_t seed) {
    return fbm((float)x * 0.0062f, (float)z * 0.0062f, seed ^ 0x4D015700u);
}

/* Swamp-ness in [0,1] — high where it's warm AND very humid. Pulled
 * out of the classifier because height_at needs it (swamps sit flat
 * at water level) without paying for the full biome decision. */
static inline float swamp_from_factors(float t, float hu) {
    /* Only TEMPERATE wetland is swamp; cold is taiga, hot+wet is
     * jungle (which keeps rolling terrain, not flat wetland). */
    if (t < 0.38f || t > 0.66f) return 0.0f;
    if (hu < 0.62f) return 0.0f;
    float s = (hu - 0.62f) / 0.18f;
    return s > 1.0f ? 1.0f : s;
}
static float swamp_factor(int x, int z, uint32_t seed) {
    return swamp_from_factors(temperature_at(x, z, seed),
                              humidity_at(x, z, seed));
}

/* Dev-only biome override (screenshot harness). When >= 0, every
 * column classifies as this biome so we can render a clean per-biome
 * comparison scene. -1 = normal noise-driven classification. */
int craft_gen_force_biome = -1;

/* Pure classifier from precomputed factors — lets callers that
 * already have the mountain factor (craft_gen_column) avoid a
 * redundant noise eval. */
static CraftBiome biome_classify(float m, float t, float hu) {
    if (craft_gen_force_biome >= 0) return (CraftBiome)craft_gen_force_biome;
    if (m > 0.5f) return CBIOME_MOUNTAINS;          /* elevation wins */
    if (t < 0.38f) return CBIOME_TAIGA;             /* cold */
    if (t > 0.66f) {                                 /* hot band */
        if (hu > 0.60f) return CBIOME_JUNGLE;        /* hot + wet */
        if (hu < 0.33f) return CBIOME_DESERT;        /* hot + dry */
        return CBIOME_SAVANNA;                        /* hot + moderate */
    }
    if (hu > 0.62f) return CBIOME_SWAMP;             /* temperate + wet */
    if (hu > 0.48f) return CBIOME_FOREST;            /* temperate + moist */
    return CBIOME_PLAINS;
}

static CraftBiome craft_biome_at(int x, int z, uint32_t seed) {
    return biome_classify(mountain_factor(x, z, seed),
                          temperature_at(x, z, seed),
                          humidity_at(x, z, seed));
}

/* Cheap biome lookup for cells inside the resident window — reads the
 * per-column biome map that craft_gen_column already filled, avoiding
 * three fresh fbm evals. Falls back to the full classify for the rare
 * out-of-window query (e.g. a chest probe near the edge). Used by the
 * building spawn path, which runs over the whole window on load. */
static CraftBiome biome_at_cached(int x, int z, uint32_t seed) {
    int blx = x - craft_world_origin_x;
    int blz = z - craft_world_origin_z;
    if ((unsigned)blx < CRAFT_WORLD_X && (unsigned)blz < CRAFT_WORLD_Z)
        return (CraftBiome)craft_world_biome[blz * CRAFT_WORLD_X + blx];
    return craft_biome_at(x, z, seed);
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

/* Height from already-computed climate factors — lets craft_gen_column
 * compute mountain/flatland/temperature/humidity ONCE and share them
 * with the height calc instead of each recomputing the same fbm fields.
 * `mf` is the raw mountain_factor (unscaled); `f` flatland; `t`,`hu`
 * temperature/humidity. Bit-identical to the old height_at. */
static int height_from_factors(int x, int z, uint32_t seed,
                               float mf, float f, float t, float hu) {
    float nx = (float)x * 0.06f;
    float nz = (float)z * 0.06f;
    float h  = fbm(nx, nz, seed);

    /* Flatland biome compresses the height variance and drops the
     * base level — in fully flat regions terrain hugs water level
     * with at most a couple of cells of variation. */
    float h_scaled = h * (1.0f - f * 0.82f) + f * 0.18f;
    int height = (int)(h_scaled * 24.0f) + CRAFT_WATER_LEVEL - 4;

    /* Mountains add elevation but are inhibited by flatland (they
     * shouldn't co-exist; biome decides which is which). */
    float m = mf * (1.0f - f);
    height += (int)(m * 22.0f);

    /* Swamps sit flat and low — pull warm, very-humid lowland columns
     * down toward water level so they read as wetland, not hills.
     * Gated on m (<0.2) so mountains aren't drowned. */
    if (m < 0.2f) {
        float sw = swamp_from_factors(t, hu);
        if (sw > 0.0f) {
            int target = CRAFT_WATER_LEVEL - 1;
            if (height > target) {
                float keep = 1.0f - sw * 0.9f;
                height = target + (int)((float)(height - target) * keep + 0.5f);
            }
        }
    }

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

/* Standalone height — computes the climate factors itself. Used by all
 * callers except craft_gen_column (which shares its own factors). */
static int height_at(int x, int z, uint32_t seed) {
    return height_from_factors(x, z, seed,
                               mountain_factor(x, z, seed),
                               flatland_factor(x, z, seed),
                               temperature_at(x, z, seed),
                               humidity_at(x, z, seed));
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
    /* Cheap pre-gate ~1/32 (keeps the climate noise out of the hot
     * reject path; every per-biome mask is a superset of these bits). */
    if ((r & 0x1F) != 0) return false;
    int hh = height_at(x, z, seed);
    /* Warm-beach palms: shoreline sand (h at the waterline) in warm
     * climates, ~1/64 of warm shoreline columns — scattered, not lined.
     * Checked before the biome switch so a desert beach (otherwise
     * "no trees") grows them. */
    if (hh >= CRAFT_WATER_LEVEL && hh <= CRAFT_WATER_LEVEL + 1) {
        return ((r & 0x3F) == 0) && temperature_at(x, z, seed) > 0.55f;
    }
    CraftBiome b = craft_biome_at(x, z, seed);
    uint32_t mask;
    switch (b) {
        case CBIOME_DESERT:    return false;     /* no trees (cactus is separate) */
        case CBIOME_JUNGLE:    mask = 0x1F; break;  /* ~1/32 — dense canopy */
        case CBIOME_FOREST:    mask = 0x1F; break;  /* ~1/32 — dense */
        case CBIOME_TAIGA:     mask = 0x3F; break;  /* ~1/64 */
        case CBIOME_MOUNTAINS: mask = 0x7F; break;  /* ~1/128 — sparse pine */
        case CBIOME_SAVANNA:   mask = 0x1FF; break; /* ~1/512 — sparse acacias */
        case CBIOME_SWAMP:     mask = 0x7F; break;  /* ~1/128 oaks; ~1/8 of them giant */
        case CBIOME_PLAINS:
        default:               mask = 0xFF; break;  /* ~1/256 — sparse */
    }
    if ((r & mask) != 0) return false;
    return hh > CRAFT_WATER_LEVEL + 1;
}

/* Some warm-climate trees bloom: their canopy leaves become
 * BLK_BLOSSOM_LEAVES, which the renderer recolours per-tree to a bloom
 * colour (pink/white/yellow/magenta). ~1/3 of trees in warm climates. */
static bool tree_blossoms_at(int x, int z, uint32_t seed) {
    if (temperature_at(x, z, seed) <= 0.5f) return false;
    uint32_t r = hash3(x, z, seed ^ 0x8105500Du);
    return (r % 3u) == 0u;
}

typedef enum {
    TREE_OAK = 0,        /* Standard Minecraft small oak — 5-tall trunk */
    TREE_OAK_LARGE,      /* Minecraft big oak — 8-tall trunk with branches */
    TREE_PINE,           /* Tall conifer — pointed tip, wide layered base */
    TREE_SWAMP_GIANT,    /* Swamp signature — 2×2 trunk, broad drooping canopy */
    TREE_JUNGLE,         /* Tall single trunk, layered crown, hanging vines */
    TREE_ACACIA,         /* Savanna — slanted trunk, flat wide canopy */
    TREE_PALM,           /* Warm beaches — curved trunk, drooping fronds */
} TreeType;

/* Pick a tree shape per position deterministically, by biome:
 *   Mountains / Taiga → pine
 *   Swamp             → giant swamp tree
 *   Forest            → oak, ~⅓ large oak (lusher)
 *   Plains / other    → oak, ~¼ large oak */
static TreeType tree_type_at(int x, int z, uint32_t seed) {
    /* Beach columns (shoreline sand) grow palms — matches the warm-beach
     * branch in tree_at. */
    int h = height_at(x, z, seed);
    if (h >= CRAFT_WATER_LEVEL && h <= CRAFT_WATER_LEVEL + 1) return TREE_PALM;
    CraftBiome b = craft_biome_at(x, z, seed);
    if (b == CBIOME_MOUNTAINS || b == CBIOME_TAIGA) return TREE_PINE;
    if (b == CBIOME_SWAMP) {
        /* Mostly normal swamp oaks; an occasional giant as a landmark
         * (~1/8 of swamp trees) so the biome isn't a forest of giants. */
        uint32_t r = hash3(x, z, seed ^ 0x5A11AB1Eu);
        return ((r & 0x7) == 0) ? TREE_SWAMP_GIANT : TREE_OAK;
    }
    if (b == CBIOME_JUNGLE) {
        /* Dense jungle: mostly small (oak) understory with ~1/4 tall
         * jungle trees rising above — not a wall of identical giants. */
        uint32_t r = hash3(x, z, seed ^ 0x10661E00u);
        return ((r & 0x3) == 0) ? TREE_JUNGLE : TREE_OAK;
    }
    if (b == CBIOME_SAVANNA) return TREE_ACACIA;
    uint32_t r = hash3(x, z, seed ^ 0x7E47A1B5u);
    int roll = (int)((r >> 3) & 0xFF);
    int large_cut = (b == CBIOME_FOREST) ? 170 : 192;   /* lower = more big oaks */
    return (roll < large_cut) ? TREE_OAK : TREE_OAK_LARGE;
}

/* Per-tree variant byte — used to rotate branch directions and so on.
 * Deterministic on (x, z, seed). */
static int tree_variant_at(int x, int z, uint32_t seed) {
    return (int)(hash3(x, z, seed ^ 0xBADBE1FFu) & 0xFF);
}

/* Palm lean: bias the variant's low 2 bits toward the lowest of the
 * four neighbour columns (the sea), so beach palms lean out over the
 * water. Upper bits keep the hash for per-frond length variation. */
static int palm_variant_at(int x, int z, uint32_t seed) {
    const int D = 3;
    int best = height_at(x + D, z, seed), axis = 0, sgn = 0;     /* lean +X */
    int h;
    h = height_at(x - D, z, seed); if (h < best) { best = h; axis = 0; sgn = 1; }  /* -X */
    h = height_at(x, z + D, seed); if (h < best) { best = h; axis = 1; sgn = 0; }  /* +Z */
    h = height_at(x, z - D, seed); if (h < best) { best = h; axis = 1; sgn = 1; }  /* -Z */
    return (tree_variant_at(x, z, seed) & ~3) | (axis ? 1 : 0) | (sgn ? 2 : 0);
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

/* Stable per-leaf-cell pseudo-random, seeded by the tree's variant
 * byte + the cell offset. Pure function → identical across chunk
 * reloads (no leaf flicker), but differs per tree and per cell so a
 * canopy can have a ragged, non-geometric edge. */
static inline uint32_t leaf_jitter(int variant, int dx, int dz, int y) {
    uint32_t h = (uint32_t)variant * 2654435761u
               ^ (uint32_t)(dx * 73856093)
               ^ (uint32_t)(dz * 19349663)
               ^ (uint32_t)(y  * 83492791);
    h ^= h >> 13; h *= 0x85EBCA6Bu; h ^= h >> 16;
    return h;
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

/* GIANT SWAMP TREE — the swamp biome's signature. A 2×2 trunk ~11
 * cells tall topped by a broad, flat, multi-tier canopy that droops
 * at the rim into hanging leaf "vines". Canopy reach must match
 * tree_radius(TREE_SWAMP_GIANT) = 7.
 *
 * The trunk occupies the 2×2 footprint dx,dz ∈ {0,1}; the canopy is
 * measured from the trunk centre (0.5, 0.5) using squared radius so
 * there's no sqrt in the hot path. */
static BlockId tree_block_swamp_giant(int variant, int dx, int dz,
                                      int y, int trunk_base) {
    int trunk_y = trunk_base + 1;
    int top = trunk_y + 17;            /* 18-tall 2×2 trunk — towering */
    bool in_trunk_xz = (dx == 0 || dx == 1) && (dz == 0 || dz == 1);
    if (in_trunk_xz && y >= trunk_y && y <= top) return BLK_WOOD;

    /* Two LEAFY limbs on opposite sides (variant-chosen axis), set LOW
     * on the trunk and staggered in height so they read as distinct
     * branches well below the crown — each a 3-cell arm that rises one
     * cell, tipped with a small leaf cluster. */
    int axis = variant & 1;
    for (int s = 0; s < 2; s++) {
        int sgn = s ? -1 : 1;
        int by  = trunk_y + (s ? 11 : 8);
        int ax  = axis ? 0 : sgn;
        int az  = axis ? sgn : 0;
        for (int r = 1; r <= 3; r++) {
            int wy = by + (r >= 2 ? 1 : 0);   /* arm lifts after 1 cell */
            if (y == wy && dx == ax * r && dz == az * r) return BLK_WOOD;
        }
        /* Leaf cluster around the tip (offset 3, one cell up). */
        int ldx = dx - ax * 3, ldz = dz - az * 3, ldy = y - (by + 1);
        if (ldy >= -1 && ldy <= 1) {
            int reach = (ldy == 0) ? 2 : 1;
            if (abs_i(ldx) + abs_i(ldz) <= reach) return BLK_LEAVES;
        }
    }

    /* Distance² from the trunk centre at (0.5, 0.5). */
    float cx = (float)dx - 0.5f, cz = (float)dz - 0.5f;
    float rad2 = cx * cx + cz * cz;
    int ady = y - top;

    /* Thin UMBRELLA crown — a wide flat disc with a small domed cap
     * and a 1-cell underside lip at the rim. Bold silhouette, not a
     * leaf blob. Rim cells are nibbled by a position-seeded hash so
     * the edge is ragged and each tree (variant) differs — interior
     * cells are always kept so the shape stays solid. */
    if (ady == 1) {                       /* small domed cap */
        if (rad2 <= 3.5f * 3.5f) {
            if (rad2 <= 2.0f * 2.0f ||
                (leaf_jitter(variant, dx, dz, y) & 7) != 0) return BLK_LEAVES;
        }
    }
    if (ady == 0) {                       /* the wide flat canopy */
        if (rad2 <= 6.9f * 6.9f && !in_trunk_xz) {
            if (rad2 <= 5.0f * 5.0f ||
                (leaf_jitter(variant, dx, dz, y) & 3) != 0) return BLK_LEAVES;
        }
    }
    if (ady == -1) {                      /* underside lip — edge only */
        if (rad2 >= 4.0f * 4.0f && rad2 <= 6.9f * 6.9f) {
            if ((leaf_jitter(variant, dx, dz, y) & 1) == 0) return BLK_LEAVES;
        }
    }
    /* Hanging VINES — real vine sprites dangling from the rim, each a
     * 2-4 cell run below the underside lip. Distinct from the leaves
     * so the swamp's tendrils read as vines, not foliage. */
    if (ady <= -2 && ady >= -6) {
        if (rad2 >= 4.5f * 4.5f && rad2 <= 6.9f * 6.9f) {
            int key = dx * 7 + dz * 13;
            if ((key & 3) == 0) {
                int hang = 2 + ((key >> 2) & 1) + ((dx ^ dz) & 1);  /* 2-4 */
                if ((-ady - 1) <= hang) return BLK_VINE;
            }
        }
    }
    return BLK_AIR;
}

/* JUNGLE — a "mini giant": a single trunk (9 cells) crowned by a
 * rounded three-layer bushy canopy with PROMINENT vines dangling off
 * the rim — the jungle's signature. Taller than a large oak (8) but
 * well short of the swamp giant (18) so it reads as a mid-tier tree,
 * not a giant. Smaller and rounder than the giant's flat umbrella.
 * Canopy reach = 2. */
static BlockId tree_block_jungle(int variant, int dx, int dz, int y, int trunk_base) {
    (void)variant;
    int trunk_y = trunk_base + 1;
    int top = trunk_y + 8;             /* 9-tall trunk */
    if (dx == 0 && dz == 0 && y >= trunk_y && y <= top) return BLK_WOOD;
    int adx = abs_i(dx), adz = abs_i(dz);
    int chess = max_chess(dx, dz);
    int ady = y - top;
    /* Rounded crown: two wide 5×5-minus-corner layers, a 3×3, a cap. */
    if (ady == -2 || ady == -1) {
        if (chess <= 2 && !(adx == 2 && adz == 2))
            if (!(dx == 0 && dz == 0)) return BLK_LEAVES;
    }
    if (ady == 0) {
        if (chess <= 1) return BLK_LEAVES;
    }
    if (ady == 1) {
        if (dx == 0 && dz == 0) return BLK_LEAVES;
    }
    /* Dangling vines — a canopy curtain: hung under any leaf cell across
     * the whole underside (interior + edges, not the bare clipped
     * corners), ~1/4 of them, each a hash-varied length (1..6 cells) so
     * they drip out of the foliage rather than spiking off the corners.
     * A whole run shares one type (flowers along its length, or plain). */
    if (ady <= -3 && chess >= 1 && chess <= 2 && !(adx == 2 && adz == 2)) {
        uint32_t lh = (uint32_t)(dx * 374761393) ^ (uint32_t)(dz * 668265263);
        lh ^= lh >> 13; lh *= 0x9E3779B1u; lh ^= lh >> 15;
        int len = 1 + (int)(lh % 6u);          /* 1..6 cells */
        if ((lh & 3u) == 0u && ady >= -2 - len)
            return (((dx * 7 + dz * 13) & 3) == 0) ? BLK_FLOWER_VINE : BLK_VINE;
    }
    return BLK_AIR;
}

/* ACACIA — the savanna silhouette: a short straight trunk that kinks
 * diagonally, topped by a thin flat umbrella. Variant chooses the
 * lean direction. Canopy reach = 5 (slant 2 + disc 3). */
static BlockId tree_block_acacia(int variant, int dx, int dz, int y, int trunk_base) {
    int trunk_y = trunk_base + 1;
    int straight_top = trunk_y + 4;             /* 5-cell straight base */
    if (dx == 0 && dz == 0 && y >= trunk_y && y <= straight_top) return BLK_WOOD;
    int axis = variant & 1;
    int sgn  = (variant & 2) ? -1 : 1;
    int sdx  = axis ? 0 : sgn;
    int sdz  = axis ? sgn : 0;
    /* Two diagonal kink cells lifting the canopy off-centre. */
    if (y == straight_top + 1 && dx == sdx && dz == sdz) return BLK_WOOD;
    if (y == straight_top + 2 && dx == 2 * sdx && dz == 2 * sdz) return BLK_WOOD;
    /* Flat umbrella centred on the kink tip. */
    int ldx = dx - 2 * sdx, ldz = dz - 2 * sdz;
    int lchess = max_chess(ldx, ldz);
    int ldy = y - (straight_top + 2);
    if (ldy == 1) {                              /* wide flat disc */
        if (lchess <= 3 && !(abs_i(ldx) == 3 && abs_i(ldz) == 3)) return BLK_LEAVES;
    }
    if (ldy == 2) {                              /* thin centre cap */
        if (lchess <= 1) return BLK_LEAVES;
    }
    return BLK_AIR;
}

/* PALM — a tall trunk that curves/leans toward the water (lean
 * direction comes from the variant, set by palm_variant_at), topped by
 * long fronds that radiate out and droop. Fronds are BLK_PALM_LEAF
 * (kept green, not biome-tinted) and step out-then-down so consecutive
 * cells touch only diagonally — natural, un-blocky fronds. */
static BlockId tree_block_palm(int variant, int dx, int dz, int y, int trunk_base) {
    int trunk_y = trunk_base + 1;
    const int H = 11;                      /* tall trunk */
    int axis = variant & 1;
    int sgn  = (variant & 2) ? -1 : 1;
    int sdx  = axis ? 0 : sgn;             /* lean direction (toward water) */
    int sdz  = axis ? sgn : 0;
    int rel  = y - trunk_y;
    /* Curved trunk: lean grows with height up to offset 3 at the tip. */
    int loff = (rel <= 1) ? 0 : (rel <= 4) ? 1 : (rel <= 7) ? 2 : 3;
    if (rel >= 0 && rel <= H) {
        if (dx == loff * sdx && dz == loff * sdz) return BLK_WOOD;
    }
    /* Crown at the leaning tip (trunk offset 3). */
    int tdx = dx - 3 * sdx, tdz = dz - 3 * sdz;
    int adx = abs_i(tdx), adz = abs_i(tdz);
    int cy  = y - (trunk_y + H);
    if (adx <= 1 && adz <= 1 && cy == 0) return BLK_PALM_LEAF;   /* hub */
    /* Long fronds: radiate out and droop. Cells step out-then-down so
     * consecutive cells touch only diagonally — natural, un-blocky. */
    int onaxis = (tdz == 0 && adx > 0) || (tdx == 0 && adz > 0);
    int ondiag = (adx == adz && adx > 0);
    if (onaxis) {
        int d   = adx + adz;                              /* 1..5 */
        int dir = (tdz == 0) ? (tdx > 0 ? 0 : 1) : (tdz > 0 ? 2 : 3);
        int len = 4 + (int)((variant >> (dir + 2)) & 1);  /* 4 or 5 */
        int droop = (d <= 2) ? 0 : (d == 3) ? -1 : (d == 4) ? -2 : -3;
        if (d <= len && cy == droop) return BLK_PALM_LEAF;
    } else if (ondiag) {
        int d   = adx;                                    /* 1..4 */
        int dir = 4 + (tdx > 0 ? 0 : 1) + (tdz > 0 ? 0 : 2);
        int len = 3 + (int)((variant >> (dir % 6 + 1)) & 1);  /* 3 or 4 */
        int droop = (d <= 1) ? 0 : (d == 2) ? -1 : (d == 3) ? -2 : -3;
        if (d <= len && cy == droop) return BLK_PALM_LEAF;
    }
    return BLK_AIR;
}

static BlockId tree_block_at(TreeType t, int variant, int dx, int dz,
                             int y, int trunk_base) {
    switch (t) {
        case TREE_OAK_LARGE: return tree_block_oak_large(variant, dx, dz, y, trunk_base);
        case TREE_PINE:      return tree_block_pine(dx, dz, y, trunk_base);
        case TREE_SWAMP_GIANT: return tree_block_swamp_giant(variant, dx, dz, y, trunk_base);
        case TREE_JUNGLE:    return tree_block_jungle(variant, dx, dz, y, trunk_base);
        case TREE_ACACIA:    return tree_block_acacia(variant, dx, dz, y, trunk_base);
        case TREE_PALM:      return tree_block_palm(variant, dx, dz, y, trunk_base);
        case TREE_OAK:
        default:             return tree_block_oak(dx, dz, y, trunk_base);
    }
}

/* --- Buildings ---------------------------------------------------
 *
 * Eight building designs spawn deterministically in flat lowland
 * tiles. A building "exists" at column (hx, hz) iff hut_origin_at
 * returns true; per-type W, H and top-dy come from hut_w / hut_h /
 * hut_top so each design uses only the footprint it actually needs.
 *
 * The world-scan code uses HUT_W=HUT_H=7 as the max bounding-box
 * dimensions; smaller designs return AIR for cells outside their own
 * W×H footprint. HUT_TOP_DY=7 accommodates the tallest design
 * (watchtower / church steeple). Generation is deterministic per
 * (hx, hz, seed) and must be applied identically in
 * craft_gen_block_at and craft_gen_column — the save diff layer
 * relies on per-cell agreement.
 *
 *  Type             Footprint   Height  Materials
 *  ---------------  ----------  ------  ------------------------------
 *  0 A-Frame Lodge  5×5×5       gabled  PLANK + WOOD corners + GLASS
 *  1 Hipped Cottage 5×5×5       pyramid PLANK + WOOD corners + GLASS
 *  2 Longhouse      7×3×5       gabled  PLANK + WOOD corners + GLASS
 *  3 L-Hipped Cabin 5×5 L 5     hipped  PLANK + WOOD corners
 *  4 L-Gabled Cabin 5×5 L 5     gabled  PLANK + WOOD corners
 *  5 Watchtower     3×3×7       crenel. STONE + COBBLE + GLASS + TORCH
 *  6 Church         5×5×7       gable+  STONE + PLANK + WOOD steeple
 *                                steepl. + GLASS + TORCH
 *  7 Castle Keep    7×7×6       battl.  STONE + COBBLE battlements
 *  8 Desert Temple  9×9×7       pyramid SANDSTONE stepped pyramid +
 *                                        trapped treasure room (desert)
 *  9 Desert Ziggurat9×9×5       keep    SANDSTONE walled keep + roof
 *                                        terrace + trapped room (desert)
 */
#define HUT_W       9
#define HUT_H       9
#define HUT_TOP_DY  7

enum HutType {
    HUT_TYPE_AFRAME    = 0,
    HUT_TYPE_HIPPED    = 1,
    HUT_TYPE_LONGHOUSE = 2,
    HUT_TYPE_L_HIPPED  = 3,
    HUT_TYPE_L_GABLED  = 4,
    HUT_TYPE_TOWER     = 5,
    HUT_TYPE_CHURCH    = 6,
    HUT_TYPE_CASTLE    = 7,
    HUT_TYPE_TEMPLE    = 8,   /* desert: stepped sandstone pyramid */
    HUT_TYPE_ZIGGURAT  = 9,   /* desert: sandstone walled keep */
    HUT_TYPE_FORT      = 10,  /* forest: stone skeleton outpost (walled
                                 compound + corner keep) */
};

static int hut_w(int type) {
    switch (type) {
        case HUT_TYPE_LONGHOUSE: return 7;
        case HUT_TYPE_TOWER:     return 3;
        case HUT_TYPE_CASTLE:    return 7;
        case HUT_TYPE_TEMPLE:
        case HUT_TYPE_ZIGGURAT:  return 9;
        case HUT_TYPE_FORT:      return 9;
        default:                 return 5;
    }
}
static int hut_h(int type) {
    switch (type) {
        case HUT_TYPE_LONGHOUSE: return 3;
        case HUT_TYPE_TOWER:     return 3;
        case HUT_TYPE_CASTLE:    return 7;
        case HUT_TYPE_TEMPLE:
        case HUT_TYPE_ZIGGURAT:  return 9;
        case HUT_TYPE_FORT:      return 9;
        default:                 return 5;
    }
}
static int hut_top(int type) {
    switch (type) {
        case HUT_TYPE_TOWER:    return 7;
        case HUT_TYPE_CHURCH:   return 7;
        case HUT_TYPE_CASTLE:   return 6;
        case HUT_TYPE_TEMPLE:   return 7;   /* stepped pyramid cap */
        case HUT_TYPE_ZIGGURAT: return 5;   /* roof terrace + battlements */
        case HUT_TYPE_FORT:     return 10;  /* corner keep rises tall */
        default:                return 5;
    }
}
static int hut_chest_dx(int type) {
    switch (type) {
        case HUT_TYPE_LONGHOUSE: return 1;   /* back corner of 7×3 hall */
        case HUT_TYPE_CASTLE:    return 3;   /* centre of 7×7 keep */
        case HUT_TYPE_TEMPLE:
        case HUT_TYPE_ZIGGURAT:  return 4;   /* dir-invariant room centre */
        case HUT_TYPE_FORT:      return 1;   /* inside the corner keep */
        default:                 return 1;
    }
}
static int hut_chest_dz(int type) {
    switch (type) {
        case HUT_TYPE_CASTLE:    return 3;   /* centre of 7×7 keep */
        case HUT_TYPE_TEMPLE:
        case HUT_TYPE_ZIGGURAT:  return 4;   /* dir-invariant room centre */
        case HUT_TYPE_FORT:      return 1;   /* inside the corner keep */
        default:                 return 1;
    }
}
#define HUT_CHEST_DY 1

/* Door direction — 0=south, 1=north, 2=east, 3=west. */
static int hut_door_dir(int hx, int hz, uint32_t seed) {
    return (int)(hash3(hx, hz, seed ^ 0x110D5EEDu) & 3);
}

/* Building type — 8 visual designs, weighted so the cottage family
 * (gabled / hipped / longhouse / L-variants — types 0-4) is most
 * common and the landmark builds (tower / church / castle) are
 * rarer. Chest loot tier is rolled independently from a separate
 * hash dimension, so a plain cottage can still hide a legendary
 * chest. */
/* Temples are a biome-gated landmark, NOT one of the village rolls:
 *   desert  — dense (~1/1024 origins), the desert's only building
 *   jungle  — rare jungle pyramids (~1/16384), alongside normal villages
 *   else    — never.
 * Uses the same origin hash as hut_origin_at so the temple/village
 * decision stays consistent between the two. */
static bool hut_is_temple_site(int hx, int hz, uint32_t seed, CraftBiome b) {
    uint32_t rt = hash3(hx, hz, seed ^ 0xCAB1F00Du);
    if (b == CBIOME_DESERT) return (rt & 0x3FFu)  == 0;   /* ~1/1024  */
    if (b == CBIOME_JUNGLE) return (rt & 0x3FFFu) == 0;   /* ~1/16384 */
    return false;
}

/* Forest skeleton outpost site — rare (~1/1024 forest columns), its
 * own hash dimension so it doesn't compete with the village roll. */
static bool hut_is_fort_site(int hx, int hz, uint32_t seed, CraftBiome b) {
    return b == CBIOME_FOREST &&
           (hash3(hx, hz, seed ^ 0x5CE1E70Eu) & 0x3FFu) == 0;
}

static int hut_type(int hx, int hz, uint32_t seed) {
    CraftBiome b = biome_at_cached(hx, hz, seed);
    /* Forest forts take precedence over a village roll on the same column. */
    if (hut_is_fort_site(hx, hz, seed, b)) return HUT_TYPE_FORT;
    /* Temple sites (desert always, jungle rarely) yield a pyramid or
     * ziggurat — ~65% / ~35%. Everything else rolls a village design. */
    if (hut_is_temple_site(hx, hz, seed, b)) {
        uint32_t dr = hash3(hx, hz, seed ^ 0x7E3D17u) & 0xFFu;
        return (dr < 166) ? HUT_TYPE_TEMPLE : HUT_TYPE_ZIGGURAT;
    }
    uint32_t r = hash3(hx, hz, seed ^ 0xC0FFEEEEu) & 0xFFu;
    if (r <  46) return HUT_TYPE_AFRAME;       /* ~18% */
    if (r <  92) return HUT_TYPE_HIPPED;       /* ~18% */
    if (r < 128) return HUT_TYPE_LONGHOUSE;    /* ~14% */
    if (r < 164) return HUT_TYPE_L_HIPPED;     /* ~14% */
    if (r < 200) return HUT_TYPE_L_GABLED;     /* ~14% */
    if (r < 224) return HUT_TYPE_TOWER;        /* ~9%  */
    if (r < 244) return HUT_TYPE_CHURCH;       /* ~8%  */
    return HUT_TYPE_CASTLE;                    /* ~5%  */
}

static bool hut_origin_at(int hx, int hz, uint32_t seed) {
    /* Two independent hashes: the temple/origin hash and a village hash.
     * Pre-reject the vast majority cheaply before any noise/biome work —
     * a column hosts something only if a temple hash (densest, 1/1024)
     * or a village hash (1/4096) hits. */
    uint32_t rt = hash3(hx, hz, seed ^ 0xCAB1F00Du);
    uint32_t rv = hash3(hx, hz, seed ^ 0x5117A6E5u);
    uint32_t rf = hash3(hx, hz, seed ^ 0x5CE1E70Eu);
    bool maybe_temple  = ((rt & 0x3FFu) == 0);
    bool maybe_village = ((rv & 0xFFFu) == 0);
    bool maybe_fort    = ((rf & 0x3FFu) == 0);
    if (!maybe_temple && !maybe_village && !maybe_fort) return false;
    /* Lowland-only — mountain biome shrugs buildings off. */
    float m = mountain_factor(hx, hz, seed);
    if (m > 0.20f) return false;
    /* Above water, naturally flat across THIS type's actual footprint. */
    int ref_h = height_at(hx, hz, seed);
    if (ref_h <= CRAFT_WATER_LEVEL + 1) return false;
    /* Per-biome presence: desert → dense temples only (no villages);
     * jungle → rare temples + normal villages; elsewhere → villages
     * only. Temples never appear outside desert/jungle. */
    CraftBiome b   = biome_at_cached(hx, hz, seed);
    bool temple    = hut_is_temple_site(hx, hz, seed, b);
    bool fort      = maybe_fort && (b == CBIOME_FOREST);
    bool village   = (b != CBIOME_DESERT) && maybe_village;
    if (!temple && !village && !fort) return false;
    int type = hut_type(hx, hz, seed);
    int W = hut_w(type), H = hut_h(type);
    int min_h = ref_h, max_h = ref_h;
    for (int dz = 0; dz < H; dz++) {
        for (int dx = 0; dx < W; dx++) {
            int h = height_at(hx + dx, hz + dz, seed);
            if (h < min_h) min_h = h;
            if (h > max_h) max_h = h;
        }
    }
    /* Reject sloped sites — walls would hang in air or bury. Temples
     * get a touch more tolerance: their thick sandstone base hides a
     * 1-cell step, and desert flats large enough for a 9×9 footprint
     * are scarce, so being too strict makes them near-impossible to
     * find. */
    int flat_tol = (type == HUT_TYPE_FORT) ? 3
                 : (type == HUT_TYPE_TEMPLE || type == HUT_TYPE_ZIGGURAT) ? 2
                 : 1;
    if (max_h - min_h > flat_tol) return false;
    return true;
}

/* Floor Y for the building at (hx, hz) — taken as the minimum of
 * the per-type footprint so walls always start from a complete
 * grass base. */
static int hut_floor_y(int hx, int hz, uint32_t seed) {
    int type = hut_type(hx, hz, seed);
    int W = hut_w(type), H = hut_h(type);
    int min_h = height_at(hx, hz, seed);
    for (int dz = 0; dz < H; dz++) {
        for (int dx = 0; dx < W; dx++) {
            int h = height_at(hx + dx, hz + dz, seed);
            if (h < min_h) min_h = h;
        }
    }
    return min_h;
}

/* --- Per-cell rule helpers --------------------------------------- */

static bool hut_is_perim(int dx, int dz, int W, int H) {
    return (dx == 0 || dx == W - 1 || dz == 0 || dz == H - 1);
}
static bool hut_is_corner(int dx, int dz, int W, int H) {
    return (dx == 0 || dx == W - 1) && (dz == 0 || dz == H - 1);
}
/* Door opening — 1 cell wide, 2 cells tall (dy 1..2), centre of
 * the wall selected by dir. */
static bool hut_is_door(int dx, int dz, int dy, int dir, int W, int H) {
    if (dy < 1 || dy > 2) return false;
    switch (dir) {
        case 0: return dz == 0     && dx == W / 2;     /* south */
        case 1: return dz == H - 1 && dx == W / 2;     /* north */
        case 2: return dx == W - 1 && dz == H / 2;     /* east */
        case 3: return dx == 0     && dz == H / 2;     /* west */
    }
    return false;
}
/* Centre cell of the wall opposite to the door (chest-height
 * window). */
static bool hut_is_back_centre(int dx, int dz, int dir, int W, int H) {
    switch (dir) {
        case 0: return dz == H - 1 && dx == W / 2;
        case 1: return dz == 0     && dx == W / 2;
        case 2: return dx == 0     && dz == H / 2;
        case 3: return dx == W - 1 && dz == H / 2;
    }
    return false;
}
/* Pair of wall cells flanking the centre of the back wall — used by
 * the hipped cottage's twin back-windows. */
static bool hut_is_back_pair(int dx, int dz, int dir, int W, int H) {
    switch (dir) {
        case 0: return dz == H - 1 && (dx == 1 || dx == W - 2);
        case 1: return dz == 0     && (dx == 1 || dx == W - 2);
        case 2: return dx == 0     && (dz == 1 || dz == H - 2);
        case 3: return dx == W - 1 && (dz == 1 || dz == H - 2);
    }
    return false;
}
/* True if door is on a wall parallel to the X axis (i.e. dz=0 or
 * dz=H-1) — so the ridge axis of a gable runs along Z. */
static bool hut_door_on_z_wall(int dir) { return dir == 0 || dir == 1; }

/* --- Per-design block rules --------------------------------------
 *
 * Each helper returns the block ID for one local (dx, dz, dy) cell
 * for ONE building type. dx and dz are already clamped to the
 * type's actual W×H footprint; dy is in [1, top]. The chest cell
 * at (chest_dx, chest_dz, 1) is handled by the dispatcher BEFORE
 * these rules see it, so they can ignore chests entirely. */

/* T0: A-Frame Lodge. 5×5×5. Plank walls, wood corner posts,
 * back-wall GLASS, plank gabled roof — 3-wide slab + 1-wide ridge
 * spanning the building along the door-perpendicular axis. */
static BlockId hut_block_aframe(int dx, int dz, int dy, int dir, int W, int H) {
    bool z_axis = hut_door_on_z_wall(dir);
    if (dy == 5) {
        if (z_axis) { if (dx == W / 2) return BLK_PLANK; }
        else        { if (dz == H / 2) return BLK_PLANK; }
        return BLK_AIR;
    }
    if (dy == 4) {
        if (z_axis) { if (dx >= 1 && dx <= W - 2) return BLK_PLANK; }
        else        { if (dz >= 1 && dz <= H - 2) return BLK_PLANK; }
        return BLK_AIR;
    }
    if (!hut_is_perim(dx, dz, W, H)) return BLK_AIR;
    if (hut_is_door(dx, dz, dy, dir, W, H)) return BLK_AIR;
    if (dy == 2 && hut_is_back_centre(dx, dz, dir, W, H)) return BLK_GLASS;
    if (hut_is_corner(dx, dz, W, H)) return BLK_WOOD;
    return BLK_PLANK;
}

/* T1: Hipped Cottage. 5×5×5. Plank walls, wood corner posts, two
 * back-wall GLASS windows flanking the centre, plank 4-sided
 * pyramid: 5×5 wall top → 3×3 inner ring → "+" cap. */
static BlockId hut_block_hipped(int dx, int dz, int dy, int dir, int W, int H) {
    if (dy == 5) {
        if (dx >= 1 && dx <= W - 2 && dz >= 1 && dz <= H - 2) {
            if (dx == W / 2 || dz == H / 2) return BLK_PLANK;
        }
        return BLK_AIR;
    }
    if (dy == 4) {
        if (dx >= 1 && dx <= W - 2 && dz >= 1 && dz <= H - 2) {
            bool inner_perim = (dx == 1 || dx == W - 2 || dz == 1 || dz == H - 2);
            if (inner_perim) return BLK_PLANK;
        }
        return BLK_AIR;
    }
    if (!hut_is_perim(dx, dz, W, H)) return BLK_AIR;
    if (hut_is_door(dx, dz, dy, dir, W, H)) return BLK_AIR;
    if (dy == 2 && hut_is_back_pair(dx, dz, dir, W, H)) return BLK_GLASS;
    if (hut_is_corner(dx, dz, W, H)) return BLK_WOOD;
    return BLK_PLANK;
}

/* T2: Longhouse. 7×3×5. Plank walls, wood corner posts. Long axis
 * is whichever of X or Z is longer (X here since W=7, H=3). A
 * gabled roof runs along that long axis: 5-wide inner slab at
 * dy=4 (inset 1 from gable ends), 1-wide ridge at dy=5 spanning
 * the whole length, plus a gable extension at each gable end so
 * the gable triangle shows from the short walls. Single back-
 * centre GLASS window. */
static BlockId hut_block_longhouse(int dx, int dz, int dy, int dir, int W, int H) {
    /* Long axis: whichever dimension is largest. For 7×3 footprint
     * this is the X axis, so ridge runs along X (varies in dx,
     * fixed at dz=H/2). */
    bool x_long = (W >= H);
    if (dy == 5) {
        if (x_long) { if (dz == H / 2) return BLK_PLANK; }
        else        { if (dx == W / 2) return BLK_PLANK; }
        return BLK_AIR;
    }
    if (dy == 4) {
        /* Slab inset 1 from the gable ends. */
        if (x_long) {
            if (dx >= 1 && dx <= W - 2) return BLK_PLANK;
            /* Gable end extension at the middle row so the ridge has
             * something to land on at the gable end. */
            if ((dx == 0 || dx == W - 1) && dz == H / 2) return BLK_PLANK;
        } else {
            if (dz >= 1 && dz <= H - 2) return BLK_PLANK;
            if ((dz == 0 || dz == H - 1) && dx == W / 2) return BLK_PLANK;
        }
        return BLK_AIR;
    }
    if (!hut_is_perim(dx, dz, W, H)) return BLK_AIR;
    if (hut_is_door(dx, dz, dy, dir, W, H)) return BLK_AIR;
    if (dy == 2 && hut_is_back_centre(dx, dz, dir, W, H)) return BLK_GLASS;
    if (hut_is_corner(dx, dz, W, H)) return BLK_WOOD;
    return BLK_PLANK;
}

/* L-shape cutout: the building occupies a 5×5 bounding box but the
 * (dx≥3, dz≥3) 2×2 corner is OUTSIDE the structure (AIR all the
 * way up). Both L-cottage variants share this cutout. */
static bool hut_l_outside(int dx, int dz) {
    return dx >= 3 && dz >= 3;
}

/* T3: L-Hipped Cabin. 5×5 L-shape, 5 tall. Plank walls, wood
 * corner posts. Roof = 1-wide plank ridge running over the long
 * (dz=0..4) wing at dx=1, at dy=4. The short wing gets a flat
 * plank cap at dy=4. Result: a clear "L" silhouette with the
 * ridge over the longer arm. */
static BlockId hut_block_l_hipped(int dx, int dz, int dy, int dir, int W, int H) {
    if (hut_l_outside(dx, dz)) return BLK_AIR;
    /* Roof. */
    if (dy == 5) {
        /* Single ridge plank over the long wing (the 5-row column at
         * dx ≤ 2). Centre of the long wing is dx=1. */
        if (dz <= 4 && dx == 1) return BLK_PLANK;
        return BLK_AIR;
    }
    if (dy == 4) {
        /* Cover the L: inner cells get a plank cap. Skip cells that
         * are outside the L (already returned above) — and skip the
         * footprint perimeter so the eaves don't double-up with the
         * wall tops. Inset 1 cell from the L's outer boundary. */
        bool inside_l = !hut_l_outside(dx, dz);
        if (!inside_l) return BLK_AIR;
        /* Inset rule: any inner cell that is not flush with the L's
         * outer edge. Use a simple "neighbour-test": a cell is part
         * of the roof slab if its four neighbours (±dx, ±dz) are
         * all inside the L. */
        bool nx_ok = (dx > 0) && !hut_l_outside(dx - 1, dz);
        bool px_ok = (dx < W - 1) && !hut_l_outside(dx + 1, dz);
        bool nz_ok = (dz > 0) && !hut_l_outside(dx, dz - 1);
        bool pz_ok = (dz < H - 1) && !hut_l_outside(dx, dz + 1);
        if (nx_ok && px_ok && nz_ok && pz_ok) return BLK_PLANK;
        return BLK_AIR;
    }
    /* Walls: an L-shape perimeter is "any cell inside the L whose
     * 4-neighbour set includes at least one cell OUTSIDE the L (or
     * outside the 5×5 bounding box)". */
    bool nx_out = (dx == 0) || hut_l_outside(dx - 1, dz);
    bool px_out = (dx == W - 1) || hut_l_outside(dx + 1, dz);
    bool nz_out = (dz == 0) || hut_l_outside(dx, dz - 1);
    bool pz_out = (dz == H - 1) || hut_l_outside(dx, dz + 1);
    bool on_perim = nx_out || px_out || nz_out || pz_out;
    if (!on_perim) return BLK_AIR;
    if (hut_is_door(dx, dz, dy, dir, W, H)) return BLK_AIR;
    /* Identify "corner" cells of the L (where 2+ outer-faces meet)
     * and use WOOD for the post effect. */
    int outer_faces = (int)nx_out + (int)px_out + (int)nz_out + (int)pz_out;
    if (outer_faces >= 2) return BLK_WOOD;
    return BLK_PLANK;
}

/* T4: L-Gabled Cabin. 5×5 L-shape, 5 tall. Plank walls, wood
 * corner posts. Roof = a gable ridge along EACH wing, meeting at
 * the inner corner. Long wing's ridge runs N-S at dx=1; short
 * wing's ridge runs E-W at dz=1. Both ridges sit at dy=5; dy=4 is
 * a 3-wide slab along each wing. Visually busier than the hipped
 * variant — two peaks. */
static BlockId hut_block_l_gabled(int dx, int dz, int dy, int dir, int W, int H) {
    if (hut_l_outside(dx, dz)) return BLK_AIR;
    if (dy == 5) {
        /* Ridge over long wing (dx=1, all dz 0..4) and short wing
         * (dz=1, all dx 0..4) — they meet at (1, 1). */
        if (dx == 1 || dz == 1) return BLK_PLANK;
        return BLK_AIR;
    }
    if (dy == 4) {
        /* 3-wide slab along long wing (dx 0..2) and short wing
         * (dz 0..2). Constrained to inside-L cells. */
        bool long_slab  = (dx <= 2);
        bool short_slab = (dz <= 2);
        if ((long_slab || short_slab) && !hut_l_outside(dx, dz)) return BLK_PLANK;
        return BLK_AIR;
    }
    /* Same wall logic as l_hipped. */
    bool nx_out = (dx == 0) || hut_l_outside(dx - 1, dz);
    bool px_out = (dx == W - 1) || hut_l_outside(dx + 1, dz);
    bool nz_out = (dz == 0) || hut_l_outside(dx, dz - 1);
    bool pz_out = (dz == H - 1) || hut_l_outside(dx, dz + 1);
    bool on_perim = nx_out || px_out || nz_out || pz_out;
    if (!on_perim) return BLK_AIR;
    if (hut_is_door(dx, dz, dy, dir, W, H)) return BLK_AIR;
    int outer_faces = (int)nx_out + (int)px_out + (int)nz_out + (int)pz_out;
    if (outer_faces >= 2) return BLK_WOOD;
    return BLK_PLANK;
}

/* T5: Watchtower. 3×3×7. Stone shaft, COBBLE crenellated parapet
 * at the top (4 merlons at the corners, gaps between for the
 * arrow-slit look), a GLASS slit in the back wall at dy=3, and a
 * TORCH atop one corner merlon. */
static BlockId hut_block_tower(int dx, int dz, int dy, int dir, int W, int H) {
    /* Crenellated parapet at dy=6 — corners only (cobble merlons). */
    if (dy == 6) {
        if (hut_is_corner(dx, dz, W, H)) return BLK_COBBLE;
        return BLK_AIR;
    }
    /* Single torch flame sitting on the back-right merlon at dy=7. */
    if (dy == 7) {
        switch (dir) {
            case 0: if (dx == W - 1 && dz == H - 1) return BLK_TORCH; break;
            case 1: if (dx == W - 1 && dz == 0)     return BLK_TORCH; break;
            case 2: if (dx == 0     && dz == H - 1) return BLK_TORCH; break;
            case 3: if (dx == W - 1 && dz == H - 1) return BLK_TORCH; break;
        }
        return BLK_AIR;
    }
    /* Walls dy 1..5 — perimeter stone. */
    if (!hut_is_perim(dx, dz, W, H)) return BLK_AIR;
    if (hut_is_door(dx, dz, dy, dir, W, H)) return BLK_AIR;
    /* Single GLASS slit at dy=3, back-wall centre. */
    if (dy == 3 && hut_is_back_centre(dx, dz, dir, W, H)) return BLK_GLASS;
    return BLK_STONE;
}

/* T6: Church. 5×5×7. Stone walls with GLASS windows on the side
 * walls at dy=2 (two cells per side flanking the centre). Steep
 * plank gabled roof (3-wide slab + 1-wide ridge) on top, then a
 * 1-cell WOOD log steeple rising 2 cells above the back-centre
 * with a TORCH belfry at the very top. */
static BlockId hut_block_church(int dx, int dz, int dy, int dir, int W, int H) {
    bool z_axis = hut_door_on_z_wall(dir);
    /* Belfry torch at the very top of the steeple — back-wall centre. */
    if (dy == 7 && hut_is_back_centre(dx, dz, dir, W, H)) return BLK_TORCH;
    /* Steeple shaft at dy=6, sitting on the back-centre cell. */
    if (dy == 6 && hut_is_back_centre(dx, dz, dir, W, H)) return BLK_WOOD;
    if (dy >= 6) return BLK_AIR;
    /* Roof dy=5: 1-wide ridge along door-perpendicular axis.
     * The back-centre cell hosts the steeple base — stamp WOOD
     * there, plank everywhere else along the ridge. */
    if (dy == 5) {
        if (hut_is_back_centre(dx, dz, dir, W, H)) return BLK_WOOD;
        if (z_axis) { if (dx == W / 2) return BLK_PLANK; }
        else        { if (dz == H / 2) return BLK_PLANK; }
        return BLK_AIR;
    }
    /* Roof dy=4: 3-wide slab along the ridge axis. */
    if (dy == 4) {
        if (z_axis) { if (dx >= 1 && dx <= W - 2) return BLK_PLANK; }
        else        { if (dz >= 1 && dz <= H - 2) return BLK_PLANK; }
        return BLK_AIR;
    }
    /* Walls. */
    if (!hut_is_perim(dx, dz, W, H)) return BLK_AIR;
    if (hut_is_door(dx, dz, dy, dir, W, H)) return BLK_AIR;
    /* Arched-window GLASS pair on the LONG sides at dy=2 (the
     * non-door, non-back walls). For the church we want the windows
     * on the two side walls perpendicular to the door axis. */
    if (dy == 2) {
        bool side_window = false;
        switch (dir) {
            case 0: case 1:    /* door on z-wall → side walls are dx 0 / W-1 */
                side_window = (dx == 0 || dx == W - 1) &&
                              (dz == 1 || dz == H - 2);
                break;
            case 2: case 3:    /* door on x-wall → side walls are dz 0 / H-1 */
                side_window = (dz == 0 || dz == H - 1) &&
                              (dx == 1 || dx == W - 2);
                break;
        }
        if (side_window) return BLK_GLASS;
    }
    return BLK_STONE;
}

/* T7: Castle Keep. 7×7×6. Stone walls with COBBLE crenellated
 * battlements at dy=6: cobble on every other cell of the
 * perimeter (corner-and-alternating-merlon pattern). Wall has
 * GLASS arrow slits on the side walls at dy=3. Interior is open
 * (one big hall). Door is 2 cells tall like always. */
static BlockId hut_block_castle(int dx, int dz, int dy, int dir, int W, int H) {
    /* Crenellation at dy=6 — perimeter only, alternating cells. */
    if (dy == 6) {
        if (!hut_is_perim(dx, dz, W, H)) return BLK_AIR;
        /* Always include all 4 corners as merlons, then add merlons
         * at every-other cell along each edge. The pattern is
         * cobble when (dx+dz) is even on the perimeter — gives a
         * neat alternating crenellation. */
        if (((dx + dz) & 1) == 0) return BLK_COBBLE;
        return BLK_AIR;
    }
    /* Walls dy 1..5 — perimeter stone. */
    if (!hut_is_perim(dx, dz, W, H)) return BLK_AIR;
    if (hut_is_door(dx, dz, dy, dir, W, H)) return BLK_AIR;
    /* Arrow-slit GLASS on the non-door walls at dy=3, at the
     * centre of each side. */
    if (dy == 3) {
        bool slit =
            (dx == W / 2 && dz == 0)     ||
            (dx == W / 2 && dz == H - 1) ||
            (dz == H / 2 && dx == 0)     ||
            (dz == H / 2 && dx == W - 1);
        /* Don't put a glass slit in the door wall's centre cell — the
         * door is there. */
        if (slit && !hut_is_door(dx, dz, /*dy=*/2, dir, W, H)) return BLK_GLASS;
    }
    return BLK_STONE;
}

/* T10: Forest skeleton outpost. 9×9 footprint. A crenellated stone
 * compound wall (4 tall) around an open courtyard, with a 3×3 stone
 * keep in the (0,0) corner rising to dy=9 and its own cobble
 * battlement at dy=10. Hollow keep shaft (chest at its base), arrow
 * slits in both. No torches — it stays dark and grim. Skeletons are
 * spawned around it by the mob system, day and night. */
static BlockId hut_block_fort(int dx, int dz, int dy, int dir, int W, int H) {
    bool in_keep = (dx <= 2 && dz <= 2);          /* corner keep footprint */
    if (in_keep) {
        bool kperim = (dx == 0 || dx == 2 || dz == 0 || dz == 2);
        if (dy == 10) {                            /* keep battlement */
            return (kperim && ((dx + dz) & 1) == 0) ? BLK_COBBLE : BLK_AIR;
        }
        if (dy > 10) return BLK_AIR;
        if (!kperim) return BLK_AIR;               /* hollow shaft */
        /* Arrow slits on the keep's two outward faces. */
        if ((dy == 5 || dy == 8) &&
            ((dx == 1 && dz == 0) || (dx == 0 && dz == 1))) return BLK_GLASS;
        return BLK_STONE;
    }
    /* Outer compound wall — 4 tall, open courtyard inside. */
    if (dy >= 5) return BLK_AIR;
    if (!hut_is_perim(dx, dz, W, H)) return BLK_AIR;   /* courtyard */
    if (dy == 4) {                                 /* crenellation */
        return (((dx + dz) & 1) == 0) ? BLK_COBBLE : BLK_AIR;
    }
    if (hut_is_door(dx, dz, dy, dir, W, H)) return BLK_AIR;   /* gate */
    if (dy == 2) {                                 /* arrow slits mid-wall */
        bool slit = (dx == W / 2 && (dz == 0 || dz == H - 1)) ||
                    (dz == H / 2 && (dx == 0 || dx == W - 1));
        if (slit && !hut_is_door(dx, dz, 2, dir, W, H)) return BLK_GLASS;
    }
    return BLK_STONE;
}

/* --- Desert temples (types 8 / 9) --------------------------------
 *
 * Both share a 7×7×2 hollow treasure room inside a 9×9 footprint, a
 * baked redstone trap kit, and a centre chest (4,4) whose loot is
 * biased high in craft_gen_seed_hut_chest. They differ only in the
 * exterior shell wrapped around that room: TEMPLE is a stepped
 * pyramid, ZIGGURAT is a walled keep with a rooftop terrace.
 *
 * Trap kit (door-relative (along, depth); depth=0 is the door wall):
 *   - a real DOOR at the entrance (along 4, depth 0),
 *   - an OBSERVER watching that door → WIRE → DISPENSER that fires
 *     back across the threshold the instant the door is opened,
 *   - three PRESSURE_PADs around the centre chest, each backed by a
 *     directly-adjacent DISPENSER that fires inward when stepped on.
 * The chest sits at the dir-invariant centre (4,4), so the existing
 * single-chest detect/seed plumbing works without per-dir math. */

/* Map door-relative (along, depth) → local (dx, dz). The centre (4,4)
 * is invariant under all four door directions. */
static void temple_ad(int dir, int along, int depth, int *dx, int *dz) {
    switch (dir) {
        case 0: *dx = along;     *dz = depth;     break; /* door dz=0, +Z in */
        case 1: *dx = along;     *dz = 8 - depth; break; /* door dz=8, -Z in */
        case 2: *dx = 8 - depth; *dz = along;     break; /* door dx=8, -X in */
        default:*dx = depth;     *dz = along;     break; /* door dx=0, +X in */
    }
}
/* World face pointing back toward the door (the -depth direction). */
static int temple_face_to_door(int dir) {
    switch (dir) { case 0: return FACE_NZ; case 1: return FACE_PZ;
                   case 2: return FACE_PX; default: return FACE_NX; }
}
/* World faces for +along / -along (lateral across the door wall). */
static int temple_face_plus_along(int dir)  { return (dir < 2) ? FACE_PX : FACE_PZ; }
static int temple_face_minus_along(int dir) { return (dir < 2) ? FACE_NX : FACE_NZ; }

/* Face sentinels resolved per-dir at use. */
#define TF_NONE   (-1)
#define TF_DOOR   (-2)
#define TF_PALONG (-3)
#define TF_MALONG (-4)

typedef struct { int8_t along, depth, dy; uint8_t blk; int8_t face; } TempleCell;
static const TempleCell k_temple_cells[] = {
    /* entrance door (2 cells tall) */
    { 4, 0, 1, BLK_DOOR_OFF,      TF_DOOR  },
    { 4, 0, 2, BLK_DOOR_OFF,      TF_DOOR  },
    /* door-watch observer → wire → dispenser firing back at the entrant */
    { 4, 1, 2, BLK_OBSERVER,      TF_DOOR  },
    { 4, 2, 2, BLK_REDSTONE_WIRE, TF_NONE  },
    { 4, 2, 1, BLK_DISPENSER,     TF_DOOR  },
    /* treasure pads + inward dispensers ringing the centre chest (4,4) */
    { 3, 4, 1, BLK_PRESSURE_PAD,  TF_NONE  },
    { 2, 4, 1, BLK_DISPENSER,     TF_PALONG},
    { 5, 4, 1, BLK_PRESSURE_PAD,  TF_NONE  },
    { 6, 4, 1, BLK_DISPENSER,     TF_MALONG},
    { 4, 5, 1, BLK_PRESSURE_PAD,  TF_NONE  },
    { 4, 6, 1, BLK_DISPENSER,     TF_DOOR  },
    /* corner torches so the loot is visible */
    { 1, 1, 1, BLK_TORCH,         TF_NONE  },
    { 7, 7, 1, BLK_TORCH,         TF_NONE  },
};

static int temple_resolve_face(int dir, int f) {
    switch (f) {
        case TF_DOOR:   return temple_face_to_door(dir);
        case TF_PALONG: return temple_face_plus_along(dir);
        case TF_MALONG: return temple_face_minus_along(dir);
        default:        return -1;
    }
}
/* If local (dx,dz,dy) is a trap cell, set *blk + *face (face = -1 when
 * none) and return true. */
static bool temple_trap_at(int dx, int dz, int dy, int dir,
                           BlockId *out_blk, int *out_face) {
    for (unsigned i = 0; i < sizeof(k_temple_cells)/sizeof(k_temple_cells[0]); i++) {
        const TempleCell *c = &k_temple_cells[i];
        if (c->dy != dy) continue;
        int tdx, tdz;
        temple_ad(dir, c->along, c->depth, &tdx, &tdz);
        if (tdx == dx && tdz == dz) {
            *out_blk  = (BlockId)c->blk;
            *out_face = temple_resolve_face(dir, c->face);
            return true;
        }
    }
    return false;
}

/* Stepped sandstone pyramid shell: each pair of levels insets by one,
 * 9×9 base → 3×3 cap over seven courses. */
static BlockId temple_pyramid_shell(int dx, int dz, int dy) {
    int inset = (dy - 1) / 2;
    if (dx >= inset && dx <= 8 - inset && dz >= inset && dz <= 8 - inset)
        return BLK_SANDSTONE;
    return BLK_AIR;
}
/* Walled keep shell: solid perimeter walls dy 1..4, a roof slab over
 * the room at dy 3, an open rooftop terrace at dy 4, battlements dy 5. */
static BlockId temple_ziggurat_shell(int dx, int dz, int dy) {
    bool perim = (dx == 0 || dx == 8 || dz == 0 || dz == 8);
    if (dy <= 4) {
        if (perim)   return BLK_SANDSTONE;   /* walls */
        if (dy == 3) return BLK_SANDSTONE;   /* roof slab over the room */
        return BLK_AIR;                      /* dy4 terrace (dy1,2 room handled) */
    }
    if (dy == 5 && perim && ((dx + dz) & 1) == 0)
        return BLK_SANDSTONE;                /* crenellations */
    return BLK_AIR;
}

static BlockId hut_block_temple(int dx, int dz, int dy, int dir, bool zig) {
    BlockId tb; int tf;
    if (temple_trap_at(dx, dz, dy, dir, &tb, &tf)) return tb;
    /* Hollow 7×7×2 treasure room; its ceiling is the shell's dy=3 slab. */
    if (dy <= 2 && dx >= 1 && dx <= 7 && dz >= 1 && dz <= 7) return BLK_AIR;
    return zig ? temple_ziggurat_shell(dx, dz, dy)
               : temple_pyramid_shell(dx, dz, dy);
}

/* Dispatch hut-local cell to the per-type rule. dx, dz are clamped
 * to the type's actual footprint; dy is in [1, hut_top(type)]. */
static BlockId hut_block_local(int dx, int dz, int dy, int dir, int type) {
    int W = hut_w(type), H = hut_h(type), top = hut_top(type);
    if (dx < 0 || dx >= W) return BLK_AIR;
    if (dz < 0 || dz >= H) return BLK_AIR;
    if (dy <= 0 || dy > top) return BLK_AIR;

    /* Chest at this type's chest cell, floor level. Stamped before
     * the per-type rule so individual rules can ignore chests. */
    if (dy == HUT_CHEST_DY &&
        dx == hut_chest_dx(type) &&
        dz == hut_chest_dz(type)) {
        return BLK_CHEST;
    }

    switch (type) {
        case HUT_TYPE_AFRAME:    return hut_block_aframe   (dx, dz, dy, dir, W, H);
        case HUT_TYPE_HIPPED:    return hut_block_hipped   (dx, dz, dy, dir, W, H);
        case HUT_TYPE_LONGHOUSE: return hut_block_longhouse(dx, dz, dy, dir, W, H);
        case HUT_TYPE_L_HIPPED:  return hut_block_l_hipped (dx, dz, dy, dir, W, H);
        case HUT_TYPE_L_GABLED:  return hut_block_l_gabled (dx, dz, dy, dir, W, H);
        case HUT_TYPE_TOWER:     return hut_block_tower    (dx, dz, dy, dir, W, H);
        case HUT_TYPE_CHURCH:    return hut_block_church   (dx, dz, dy, dir, W, H);
        case HUT_TYPE_CASTLE:    return hut_block_castle   (dx, dz, dy, dir, W, H);
        case HUT_TYPE_TEMPLE:    return hut_block_temple   (dx, dz, dy, dir, false);
        case HUT_TYPE_ZIGGURAT:  return hut_block_temple   (dx, dz, dy, dir, true);
        case HUT_TYPE_FORT:      return hut_block_fort     (dx, dz, dy, dir, W, H);
    }
    return BLK_AIR;
}

/* If world cell (x, y, z) lies inside any hut footprint, set *covered
 * and return the hut block (which may be AIR for interior/door). The
 * 5×5 scan in (dx, dz) is small enough to run per-cell from
 * craft_gen_block_at without measurable cost. */
static BlockId hut_cell(int x, int y, int z, uint32_t seed, bool *covered) {
    *covered = false;
    /* Scan the maximum 7×7 bounding box so any building type can be
     * detected. Per-type W×H culling then drops candidates whose
     * actual footprint doesn't cover (x, z). */
    for (int dz = -(HUT_H - 1); dz <= 0; dz++) {
        for (int dx = -(HUT_W - 1); dx <= 0; dx++) {
            int hx = x + dx, hz = z + dz;
            if (!hut_origin_at(hx, hz, seed)) continue;
            int type = hut_type(hx, hz, seed);
            int W = hut_w(type), H = hut_h(type);
            int lx = -dx, lz = -dz;
            if (lx >= W || lz >= H) continue;        /* outside this type's footprint */
            int gy = hut_floor_y(hx, hz, seed);
            int dy = y - gy;
            if (dy <= 0 || dy > hut_top(type)) continue;
            *covered = true;
            int dir = hut_door_dir(hx, hz, seed);
            return hut_block_local(lx, lz, dy, dir, type);
        }
    }
    return BLK_AIR;
}

/* --- Underground roguelike dungeons ------------------------------
 *
 * Rooms of varied size and jittered position on a coarse grid, linked
 * by L-shaped corridors to their +X / +Z neighbours, all confined to
 * scattered "dungeon zones" near the lava band. Cobble floor + ceiling
 * + side walls wrap the carved air; some room centres hold a treasure
 * chest. Computed per COLUMN (one 3×3 room scan) then filled into the
 * fixed y-band. Skeleton/spider/slime are spawned in them by the mob
 * system. */
#define DUN_FLOOR   (CRAFT_LAVA_LEVEL + 3)      /* 13: floor course   */
#define DUN_CEIL    (DUN_FLOOR + 4)             /* 17: ceiling course */
#define DUN_PITCH   10                          /* room grid spacing  */

static inline int dun_fdiv(int a, int b) { return (a >= 0) ? a / b : -(((-a) + b - 1) / b); }
static inline int dun_imin(int a, int b) { return a < b ? a : b; }
static inline int dun_imax(int a, int b) { return a > b ? a : b; }

/* A 40-block region (4×4 grid cells) is a dungeon zone — ~1/3 of them. */
static inline bool dun_zone_cell(int gx, int gz, uint32_t seed) {
    return (hash3(gx >> 2, gz >> 2, seed ^ 0xD002047Eu) % 3u) == 0u;
}
/* Room for grid cell (gx,gz): jittered centre, varied half-size, ~1/8
 * carry a chest. Returns false where there's a gap (no room). */
static bool dun_room(int gx, int gz, uint32_t seed,
                     int *cx, int *cz, int *rw, int *rh, bool *chest) {
    if (!dun_zone_cell(gx, gz, seed)) return false;
    uint32_t h = hash3(gx, gz, seed ^ 0x0D006ED0u);
    if ((h & 3u) == 0u) return false;                   /* ~1/4 gaps */
    *cx    = gx * DUN_PITCH + 5 + (int)((h >> 2) & 3u) - 1;
    *cz    = gz * DUN_PITCH + 5 + (int)((h >> 4) & 3u) - 1;
    *rw    = 2 + (int)((h >> 6) % 3u);                  /* half-width 2..4 → 5-9 wide */
    *rh    = 2 + (int)((h >> 8) % 3u);
    *chest = ((h >> 10) & 7u) == 0u;                    /* ~1/8 rooms */
    return true;
}
/* L-shaped corridor A→(bx,az)→B of width w (1..3); kept narrower than
 * the rooms. The strip is [-(w/2) .. (w-1)/2] around the centreline. */
static bool dun_on_lpath(int wx, int wz, int ax, int az, int bx, int bz, int w) {
    int lo = -(w / 2), hi = (w - 1) / 2;
    int dz = wz - az;
    if (dz >= lo && dz <= hi && wx >= dun_imin(ax, bx) && wx <= dun_imax(ax, bx)) return true;
    int dx = wx - bx;
    if (dx >= lo && dx <= hi && wz >= dun_imin(az, bz) && wz <= dun_imax(az, bz)) return true;
    return false;
}
/* Is column (wx,wz) inside a dungeon room or corridor? */
static bool dun_interior(int wx, int wz, uint32_t seed) {
    int cgx = dun_fdiv(wx, DUN_PITCH), cgz = dun_fdiv(wz, DUN_PITCH);
    for (int dgz = -1; dgz <= 1; dgz++)
        for (int dgx = -1; dgx <= 1; dgx++) {
            int gx = cgx + dgx, gz = cgz + dgz;
            int rcx, rcz, rw, rh; bool ch;
            if (!dun_room(gx, gz, seed, &rcx, &rcz, &rw, &rh, &ch)) continue;
            if (abs_i(wx - rcx) <= rw && abs_i(wz - rcz) <= rh) return true;
            int nx, nz, nw, nh; bool nc;
            /* Per-corridor width 1..3 — a mix of tight tunnels and
             * wider passages between the chambers. */
            int cwx = 1 + (int)(hash3(gx, gz, seed ^ 0xC0440001u) % 3u);
            int cwz = 1 + (int)(hash3(gx, gz, seed ^ 0xC0440002u) % 3u);
            if (dun_room(gx + 1, gz, seed, &nx, &nz, &nw, &nh, &nc) &&
                dun_on_lpath(wx, wz, rcx, rcz, nx, nz, cwx)) return true;
            if (dun_room(gx, gz + 1, seed, &nx, &nz, &nw, &nh, &nc) &&
                dun_on_lpath(wx, wz, rcx, rcz, nx, nz, cwz)) return true;
        }
    return false;
}
/* Per-column dungeon class: 0 none, 1 interior, 2 wall. *chest set when
 * this column is a chest-room centre. */
static int dungeon_column(int wx, int wz, uint32_t seed, bool *chest) {
    *chest = false;
    if (dun_interior(wx, wz, seed)) {
        int cgx = dun_fdiv(wx, DUN_PITCH), cgz = dun_fdiv(wz, DUN_PITCH);
        int rcx, rcz, rw, rh; bool ch;
        if (dun_room(cgx, cgz, seed, &rcx, &rcz, &rw, &rh, &ch) && ch &&
            wx == rcx && wz == rcz) *chest = true;
        return 1;
    }
    if (dun_interior(wx + 1, wz, seed) || dun_interior(wx - 1, wz, seed) ||
        dun_interior(wx, wz + 1, seed) || dun_interior(wx, wz - 1, seed) ||
        dun_interior(wx + 1, wz + 1, seed) || dun_interior(wx - 1, wz - 1, seed) ||
        dun_interior(wx + 1, wz - 1, seed) || dun_interior(wx - 1, wz + 1, seed))
        return 2;
    return 0;
}
/* Per-cell variant (for craft_gen_block_at): 0 none, 1 air, 2 cobble,
 * 3 chest. */
static int dungeon_cell(int wx, int y, int wz, uint32_t seed) {
    if (y < DUN_FLOOR || y > DUN_CEIL) return 0;
    bool chest;
    int dk = dungeon_column(wx, wz, seed, &chest);
    if (dk == 0) return 0;
    if (dk == 2) return 2;                          /* side wall */
    if (y == DUN_FLOOR || y == DUN_CEIL) return 2;  /* floor / ceiling */
    if (chest && y == DUN_FLOOR + 1) return 3;      /* treasure */
    return 1;                                       /* air */
}
/* Some rooms (~1/4) open a 1-wide vertical shaft to the surface, capped
 * by a trapdoor in an 8-block stone surround — the dungeon's
 * discoverable hatch entrance/exit. Role of column (wx,wz): 0 none,
 * 1 = shaft centre (the trapdoor + the well below it), 2 = stone frame
 * (the 8 cells around the centre). */
static int dun_shaft_role(int wx, int wz, uint32_t seed) {
    int cgx = dun_fdiv(wx, DUN_PITCH), cgz = dun_fdiv(wz, DUN_PITCH);
    bool frame = false;
    for (int dgz = -1; dgz <= 1; dgz++)
        for (int dgx = -1; dgx <= 1; dgx++) {
            int gx = cgx + dgx, gz = cgz + dgz;
            int rcx, rcz, rw, rh; bool ch;
            if (!dun_room(gx, gz, seed, &rcx, &rcz, &rw, &rh, &ch)) continue;
            uint32_t h = hash3(gx, gz, seed ^ 0x0D006ED0u);
            if (((h >> 11) & 3u) != 0u) continue;       /* ~1/4 are entrances */
            if (wx == rcx && wz == rcz) return 1;       /* shaft centre */
            if (abs_i(wx - rcx) <= 1 && abs_i(wz - rcz) <= 1) frame = true;
        }
    return frame ? 2 : 0;
}

/* Is (wx,wy,wz) a dungeon treasure-chest cell? (for loot seeding) */
bool craft_gen_is_dungeon_chest(int wx, int wy, int wz, uint32_t seed) {
    if (wy != DUN_FLOOR + 1) return false;
    bool chest;
    dungeon_column(wx, wz, seed, &chest);
    return chest;
}

BlockId craft_gen_block_at(int x, int y, int z, uint32_t seed) {
    /* Only Y is bounded — X and Z extend infinitely. */
    if ((unsigned)y >= CRAFT_WORLD_Y) return BLK_AIR;
    (void)x;

    int h = height_at(x, z, seed);

    /* Dungeon entrance hatch — a trapdoor at the surface over a 1-wide
     * well down to the room, set in an 8-block stone surround. */
    if (DUN_CEIL < h - 3) {
        int role = dun_shaft_role(x, z, seed);
        if (role == 1) {                            /* centre: well + lid */
            if (y == h) return BLK_TRAPDOOR_OFF;
            if (y >= DUN_CEIL && y < h) return BLK_AIR;
        } else if (role == 2 && y == h) {           /* stone frame */
            return BLK_STONE;
        }
    }

    /* Underground / surface columns. Caves carve out the deep stone
     * layer only — never touches the topmost dirt or surface block,
     * so the silhouette of the world from above is unaffected. The
     * y>=2 floor keeps a stone "bedrock" layer below caves, and the
     * y<h-8 ceiling keeps the top 5 cells of stone solid so caves
     * stay genuinely subterranean (matches craft_gen_column). */
    if (y <  h - 3) {
        int dc = dungeon_cell(x, y, z, seed);
        if (dc == 1) return BLK_AIR;
        if (dc == 2) return BLK_COBBLE;
        if (dc == 3) return BLK_CHEST;
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
            int tv = (tt == TREE_PALM) ? palm_variant_at(tx, tz, seed)
                                       : tree_variant_at(tx, tz, seed);
            BlockId b = tree_block_at(tt, tv, -dx, -dz, y, th);
            if (b == BLK_LEAVES && tree_blossoms_at(tx, tz, seed))
                b = BLK_BLOSSOM_LEAVES;
            if (b != BLK_AIR) return b;
        }
    }
    return BLK_AIR;
}

void craft_gen_column(int wx, int wz, uint32_t seed,
                      uint8_t out[/* CRAFT_WORLD_Y */]) {
    /* Climate factors computed ONCE and shared with the height calc
     * (height_from_factors) and the biome classifier — the old code
     * recomputed mountain/temperature/humidity a second time inside
     * height_at, doubling the per-column 2D-noise cost. */
    float m      = mountain_factor(wx, wz, seed);
    float f      = flatland_factor(wx, wz, seed);
    float t_clim = temperature_at(wx, wz, seed);
    float hum    = humidity_at(wx, wz, seed);

    int h = height_from_factors(wx, wz, seed, m, f, t_clim, hum);
    if (h < 1) h = 1;
    if (h >= CRAFT_WORLD_Y - 4) h = CRAFT_WORLD_Y - 4;

    /* Ore placement chance — denser in mountain biome.
     * Iron is rarer than coal in both biomes. Test: (hash & mask)==0. */
    uint32_t coal_mask = (m > 0.5f) ? 0x0F : 0x3F;   /* ~1/16 vs 1/64 */
    uint32_t iron_mask = (m > 0.5f) ? 0x1F : 0x7F;   /* ~1/32 vs 1/128 */

    /* Terrain pass — stone (or coal ore) / dirt / surface / water / air. */
    int wl = CRAFT_WATER_LEVEL;
    CraftBiome biome = biome_classify(m, t_clim, hum);
    /* Record the biome for this column so the renderer can tint grass
     * + leaves. Only when the column is inside the resident window. */
    {
        int blx = wx - craft_world_origin_x;
        int blz = wz - craft_world_origin_z;
        if ((unsigned)blx < CRAFT_WORLD_X && (unsigned)blz < CRAFT_WORLD_Z)
            craft_world_biome[blz * CRAFT_WORLD_X + blx] = (uint8_t)biome;
    }
    /* Surface block by biome. Shorelines (h ≤ wl+1) are always sand
     * regardless of biome; mountain peaks above the tree line are
     * bare stone; deserts are sand all the way; everything else
     * (plains/forest/taiga/swamp) is grass. */
    BlockId surface;
    if (h <= wl + 1)                          /* shore */
        surface = (biome == CBIOME_TAIGA) ? BLK_SNOW   /* snowy beach */
                                          : BLK_SAND;
    else if (biome == CBIOME_MOUNTAINS) {
        /* Altitude-gated snow whose snow LINE drops with temperature:
         * a cold mountain (t≈0) is snow-capped from ~wl+6 — low enough
         * to blend into adjacent taiga/tundra snow — while a hot
         * mountain (t≈1) only caps above ~wl+36. Below the snow line
         * it's bare rock on the steeper slopes, grass on the flanks. */
        int snow_line = wl + 6 + (int)(t_clim * 30.0f);
        if      (h > snow_line) surface = BLK_SNOWY_ROCK;
        else if (h > wl + 16)   surface = BLK_STONE;
        else                    surface = BLK_GRASS;
    }
    else if (biome == CBIOME_DESERT)          surface = BLK_SAND;
    else if (biome == CBIOME_TAIGA)           surface = BLK_SNOW;   /* snowy cap */
    else                                      surface = BLK_GRASS;
    bool desert_sub = (biome == CBIOME_DESERT);
    /* Cave depth floor — caves only carve below (h - 8) so the top
     * 5 cells under any surface stay solid. Without this, hill
     * columns next to rivers expose 3-4 cells of cave mouths in
     * the cliff band (h-3..h-1 are dirt; cave carving runs up to
     * h-4, sometimes adjacent to surface). With the river-bank
     * smoothing the cliff is mostly gone, but keeping caves
     * genuinely subterranean is the right default anyway. */
    int cave_top = h - 8;
    /* Cave carve is evaluated per cell (was 2-tall-quantised for perf,
     * but the vertical blockiness was too visible — reverted). */
    for (int y = 0; y < h - 3; y++) {
        /* Cave carve before ore placement — caves remove a cell
         * entirely so ore doesn't get assigned to it. y<2 stays
         * solid as a "bedrock" floor. */
        if (y >= 2 && y < cave_top && is_cave(wx, y, wz, seed)) {
            /* Deep caverns pool lava; higher caves are open air. */
            out[y] = (y <= CRAFT_LAVA_LEVEL) ? BLK_LAVA : BLK_AIR;
            continue;
        }
        /* Lava pockets — ~4×4×4 magma blobs embedded in solid stone
         * ABOVE the deep lava line, so lava also turns up higher in the
         * rock (and far more often inside mountains). They sit sealed in
         * stone until a cave or the player's digging opens the hollow.
         * The >>2 quantised hash makes neighbouring cells share a value,
         * giving coherent blobs rather than single specks. */
        if (y > CRAFT_LAVA_LEVEL && y < h - 5) {
            uint32_t pk = hash3(wx >> 2, y >> 2, wz >> 2) ^ (seed * 0x9E3779B9u);
            uint32_t prate = (m > 0.5f) ? 0xFFu : 0x3FFu;   /* mountains 4× denser */
            if ((pk & prate) == 0) { out[y] = BLK_LAVA; continue; }
        }
        /* Gravel patches — ~2×2×2 blobs in the upper stone, exposed in
         * cliff faces and cave walls. Mining them sometimes yields flint. */
        if (y > h - 20) {
            uint32_t gk = hash3(wx >> 1, y >> 1, wz >> 1) ^ (seed * 0x85EBCA6Bu);
            if ((gk & 0x3Fu) == 0) { out[y] = BLK_GRAVEL; continue; }
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
    /* Carve dungeon rooms/corridors into the deep rock (overrides
     * cave/stone but stays subterranean). One room scan per column. */
    if (DUN_CEIL < h - 3) {
        bool dchest;
        int dk = dungeon_column(wx, wz, seed, &dchest);
        if (dk == 1) {                       /* room / corridor interior */
            out[DUN_FLOOR] = BLK_COBBLE;
            for (int y = DUN_FLOOR + 1; y < DUN_CEIL; y++) out[y] = BLK_AIR;
            out[DUN_CEIL] = BLK_COBBLE;
            if (dchest) out[DUN_FLOOR + 1] = BLK_CHEST;
        } else if (dk == 2) {                /* side wall */
            for (int y = DUN_FLOOR; y <= DUN_CEIL; y++) out[y] = BLK_COBBLE;
        }
    }
    bool mtn_sub = (biome == CBIOME_MOUNTAINS);
    for (int y = h - 3; y < h; y++) {
        /* Mountains: solid rock right under the surface (so snow/stone
         * caps sit on rock, not a mud band). Deserts: a sand layer
         * then sandstone. Otherwise dirt. */
        if (mtn_sub)         out[y] = BLK_STONE;
        else if (desert_sub) out[y] = (y == h - 1) ? BLK_SAND : BLK_SANDSTONE;
        else                 out[y] = BLK_DIRT;
    }
    out[h] = surface;
    /* Above-surface fill — fused single loop so GCC can't lower the
     * old two-loop form to a pair of memsets where a negative count
     * (h > wl) underflows to a huge unsigned and smashes the stack. */
    for (int y = h + 1; y < CRAFT_WORLD_Y; y++) {
        out[y] = (y <= wl) ? BLK_WATER : BLK_AIR;
    }
    /* Tundra freezes over: the topmost water cell becomes a walkable
     * sheet of ICE, with the water column preserved beneath it. */
    if (biome == CBIOME_TAIGA && wl < CRAFT_WORLD_Y && out[wl] == BLK_WATER)
        out[wl] = BLK_ICE;

    /* Column-local biome features — single-column so they need no
     * cross-column stamp. Reuse the biome already classified above. */
    if (biome == CBIOME_DESERT && h > wl + 1) {
        /* Cactus — sparse 1-3 tall column on the sand. */
        uint32_t cr = hash3(wx, wz, seed ^ 0x0CAC7005u);
        if ((cr & 0x3F) == 0) {                 /* ~1/64 desert columns */
            int ch = 1 + (int)((cr >> 8) % 3);
            for (int c = 1; c <= ch && h + c < CRAFT_WORLD_Y; c++)
                out[h + c] = BLK_CACTUS;
        }
    }

    /* Meadow ground cover — tall grass + flowers on grass surfaces.
     * Cross-sprite cutout plants placed in the cell above the surface.
     * Density by biome: plains lush (+flowers), forest patchier, swamp
     * grass-only; savanna + jungle are the warm grassy biomes (their
     * tufts render seedier via the variant weighting). Skipped on a
     * tree-trunk column so the trunk (which fills AIR only) isn't
     * blocked at its base. */
    if (surface == BLK_GRASS && h + 1 < CRAFT_WORLD_Y && out[h + 1] == BLK_AIR &&
        (biome == CBIOME_PLAINS || biome == CBIOME_FOREST || biome == CBIOME_SWAMP ||
         biome == CBIOME_SAVANNA || biome == CBIOME_JUNGLE)) {
        uint32_t dr = hash3(wx, wz, seed ^ 0x0F10E12Du);
        int roll = (int)(dr & 0xFF);
        BlockId deco = BLK_AIR;
        if (biome == CBIOME_PLAINS) {            /* ~8% flower, ~35% grass */
            if      (roll < 20)  deco = (dr & 0x100) ? BLK_FLOWER_RED : BLK_FLOWER_YELLOW;
            else if (roll < 110) deco = BLK_TALL_GRASS;
        } else if (biome == CBIOME_FOREST) {     /* ~2% flower, ~25% grass */
            if      (roll < 6)   deco = (dr & 0x100) ? BLK_FLOWER_RED : BLK_FLOWER_YELLOW;
            else if (roll < 70)  deco = BLK_TALL_GRASS;
        } else if (biome == CBIOME_SAVANNA) {    /* dry, seedy ~40% grass */
            if (roll < 100)      deco = BLK_TALL_GRASS;
        } else if (biome == CBIOME_JUNGLE) {     /* lush ~45% grass, no flowers */
            if (roll < 115)      deco = BLK_TALL_GRASS;
        } else {                                 /* swamp — ~23% grass */
            if (roll < 60)       deco = BLK_TALL_GRASS;
        }
        if (deco != BLK_AIR && !tree_at(wx, wz, seed)) out[h + 1] = deco;
    }

    /* Dungeon entrance hatch — carved LAST so it punches through the
     * surface just placed. Centre column: a 1-wide well from the room
     * ceiling up, capped by a trapdoor flush with the ground. The 8
     * surrounding columns get a stone frame so the hatch reads as built. */
    if (DUN_CEIL < h - 3) {
        int role = dun_shaft_role(wx, wz, seed);
        if (role == 1) {
            for (int y = DUN_CEIL; y < h && y < CRAFT_WORLD_Y; y++)
                out[y] = BLK_AIR;
            if (h < CRAFT_WORLD_Y)     out[h]     = BLK_TRAPDOOR_OFF;
            if (h + 1 < CRAFT_WORLD_Y) out[h + 1] = BLK_AIR;
        } else if (role == 2) {
            if (h < CRAFT_WORLD_Y)     out[h]     = BLK_STONE;
            if (h + 1 < CRAFT_WORLD_Y) out[h + 1] = BLK_AIR;
        }
    }

    /* Trees and huts are NOT stamped per-column any more — they're
     * applied as whole units by craft_gen_stamp_features() after the
     * window's terrain is laid down. That makes a tree/building appear
     * complete the moment its trunk/origin column is inside the loaded
     * window, instead of streaming canopy leaves into edge columns
     * before the trunk is in view. */
}

/* Maximum canopy reach (in cells from the trunk) per tree type, so
 * the stamp pass knows how far out to write. Keep in sync with the
 * tree_block_* functions. */
static int tree_radius(TreeType t) {
    switch (t) {
        case TREE_SWAMP_GIANT: return 7;   /* wide spreading canopy */
        case TREE_ACACIA:      return 5;   /* slant offset + flat disc */
        case TREE_PALM:        return 8;   /* lean offset 3 + frond reach 5 */
        case TREE_OAK_LARGE:   return 3;   /* branch tip + leaf ring */
        case TREE_JUNGLE:      return 2;
        case TREE_PINE:        return 2;
        case TREE_OAK:
        default:               return 2;
    }
}

/* Stamp all trees and buildings whose trunk / origin column lies in
 * the current resident window, writing the entire feature in one shot
 * (cross-column) directly into craft_world_blocks. Run once after a
 * window load and after every window shift.
 *
 *  - Trees fill AIR cells only and never overwrite player edits
 *    (mod-store hits are skipped), so a chopped tree stays chopped.
 *  - Huts run AFTER trees and overwrite, so walls + interior clear any
 *    tree blocks in the footprint (matching the old per-column order);
 *    they too skip modded cells.
 *
 * A feature whose trunk/origin is OUTSIDE the window contributes
 * nothing — its canopy will not bleed into the window edge. It pops in
 * as a complete unit once the player has walked far enough for its
 * trunk column to load. */
/* Core stamp over a TRUNK/ORIGIN local-coord rectangle [tlx0,tlx1) ×
 * [tlz0,tlz1). Canopy/footprint writes still spill outward from the
 * trunk and are clamped to the full window, so passing a sub-rect
 * bounded by the new strip + a CRAFT_GEN_MAX_TREE_RADIUS margin is
 * enough to fill freshly-exposed columns after a shift. */
static void stamp_region(uint32_t seed, int tlx0, int tlx1,
                         int tlz0, int tlz1) {
    int ox = craft_world_origin_x;
    int oz = craft_world_origin_z;

    /* --- Trees --- */
    for (int lz = tlz0; lz < tlz1; lz++) {
        for (int lx = tlx0; lx < tlx1; lx++) {
            int wx = lx + ox, wz = lz + oz;
            if (!tree_at(wx, wz, seed)) continue;
            int th        = height_at(wx, wz, seed);
            TreeType tt   = tree_type_at(wx, wz, seed);
            int tv        = (tt == TREE_PALM) ? palm_variant_at(wx, wz, seed)
                                              : tree_variant_at(wx, wz, seed);
            int R         = tree_radius(tt);
            for (int dz = -R; dz <= R; dz++) {
                for (int dx = -R; dx <= R; dx++) {
                    int cwx = wx + dx, cwz = wz + dz;
                    int clx = cwx - ox, clz = cwz - oz;
                    if ((unsigned)clx >= CRAFT_WORLD_X) continue;
                    if ((unsigned)clz >= CRAFT_WORLD_Z) continue;
                    for (int y = th + 1; y < CRAFT_WORLD_Y; y++) {
                        BlockId b = tree_block_at(tt, tv, dx, dz, y, th);
                        if (b == BLK_AIR) continue;
                        int idx = (y * CRAFT_WORLD_Z + clz) * CRAFT_WORLD_X + clx;
                        if (craft_world_blocks[idx] != BLK_AIR) continue;
                        if (craft_world_mod_get(cwx, y, cwz) >= 0) continue;
                        craft_world_blocks[idx] = (uint8_t)b;
                    }
                }
            }
        }
    }

    /* --- Huts --- */
    for (int lz = tlz0; lz < tlz1; lz++) {
        for (int lx = tlx0; lx < tlx1; lx++) {
            int hx = lx + ox, hz = lz + oz;
            if (!hut_origin_at(hx, hz, seed)) continue;
            int type = hut_type(hx, hz, seed);
            int W = hut_w(type), H = hut_h(type);
            int gy   = hut_floor_y(hx, hz, seed);
            int dir  = hut_door_dir(hx, hz, seed);
            int top  = hut_top(type);
            for (int hlz = 0; hlz < H; hlz++) {
                for (int hlx = 0; hlx < W; hlx++) {
                    int cwx = hx + hlx, cwz = hz + hlz;
                    int clx = cwx - ox, clz = cwz - oz;
                    if ((unsigned)clx >= CRAFT_WORLD_X) continue;
                    if ((unsigned)clz >= CRAFT_WORLD_Z) continue;
                    for (int dy = 1; dy <= top; dy++) {
                        int y = gy + dy;
                        if (y < 0 || y >= CRAFT_WORLD_Y) continue;
                        if (craft_world_mod_get(cwx, y, cwz) >= 0) continue;
                        BlockId hb = (BlockId)hut_block_local(hlx, hlz, dy, dir, type);
                        int idx = (y * CRAFT_WORLD_Z + clz) * CRAFT_WORLD_X + clx;
                        craft_world_blocks[idx] = (uint8_t)hb;
                        /* Bake the facing of temple trap blocks (dispenser
                         * fire direction, observer watch direction) into
                         * the orient table so the redstone tick aims them
                         * correctly. */
                        if (type == HUT_TYPE_TEMPLE || type == HUT_TYPE_ZIGGURAT) {
                            BlockId tb; int tf;
                            if (temple_trap_at(hlx, hlz, dy, dir, &tb, &tf) && tf >= 0)
                                craft_torches_record_orient(cwx, y, cwz, tf);
                        }
                    }
                }
            }
        }
    }
}

void craft_gen_stamp_features(uint32_t seed) {
    stamp_region(seed, 0, CRAFT_WORLD_X, 0, CRAFT_WORLD_Z);
}

void craft_gen_stamp_features_region(uint32_t seed,
                                     int tlx0, int tlx1,
                                     int tlz0, int tlz1) {
    if (tlx0 < 0) tlx0 = 0;
    if (tlz0 < 0) tlz0 = 0;
    if (tlx1 > CRAFT_WORLD_X) tlx1 = CRAFT_WORLD_X;
    if (tlz1 > CRAFT_WORLD_Z) tlz1 = CRAFT_WORLD_Z;
    if (tlx0 >= tlx1 || tlz0 >= tlz1) return;
    stamp_region(seed, tlx0, tlx1, tlz0, tlz1);
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

/* Blocks a spawn plumb-line passes straight through when seeking the
 * ground surface — air, tree canopy/trunk and ground plants are not
 * "solid ground" to stand on. */
static inline bool spawn_passthrough(BlockId b) {
    return b == BLK_AIR || b == BLK_LEAVES || b == BLK_WOOD ||
           b == BLK_VINE || b == BLK_FLOWER_VINE || b == BLK_BLOSSOM_LEAVES ||
           b == BLK_PALM_LEAF || b == BLK_TALL_GRASS ||
           b == BLK_FLOWER_RED || b == BLK_FLOWER_YELLOW;
}

Vec3 craft_gen_spawn(void) {
    /* Spawn at world origin (0,0): drop a plumb line from the top of the
     * world down to the first ground block (passing through canopy and
     * plants). If that surface is water/lava, the column is no good —
     * spiral outward to the nearest column whose surface is solid ground
     * with two clear cells of headroom, and stand on top of it. The
     * window is centred on (0,0), so the spiral stays in bounds. */
    for (int radius = 0; radius < CRAFT_WORLD_X / 2; radius++) {
        for (int dz = -radius; dz <= radius; dz++) {
            for (int dx = -radius; dx <= radius; dx++) {
                /* Ring only — interior cells were covered by smaller radii. */
                if (radius > 0 && abs_i(dx) != radius && abs_i(dz) != radius)
                    continue;
                int x = dx, z = dz;          /* world coords, centred on (0,0) */
                int gy = -1;
                for (int y = CRAFT_WORLD_Y - 2; y > 0; y--) {
                    BlockId blk = craft_world_get(x, y, z);
                    if (spawn_passthrough(blk)) continue;
                    /* First real surface block. Water/lava → reject this
                     * column (try a new location). */
                    if (!craft_is_water_id((uint8_t)blk) &&
                        !craft_is_lava_id((uint8_t)blk))
                        gy = y;
                    break;
                }
                if (gy < 0) continue;        /* empty, or water/lava surface */
                BlockId head1 = craft_world_get(x, gy + 1, z);
                BlockId head2 = craft_world_get(x, gy + 2, z);
                if (craft_block_solid(head1) || craft_block_solid(head2)) continue;
                if (craft_is_water_id((uint8_t)head1) ||
                    craft_is_water_id((uint8_t)head2)) continue;
                return v3((float)x + 0.5f, (float)gy + 1.0f + 1.6f,
                          (float)z + 0.5f);
            }
        }
    }
    /* Fallback (should never trigger): stand above the waterline at origin. */
    return v3(0.5f, (float)CRAFT_WATER_LEVEL + 2.0f, 0.5f);
}

bool craft_gen_is_hut_chest(int wx, int wy, int wz, uint32_t seed) {
    /* Walk back over every (hx, hz) origin whose footprint could cover
     * (wx, wz). For each that's actually a building, check whether
     * (wx, wy, wz) is that type's chest cell. */
    for (int dz = -(HUT_H - 1); dz <= 0; dz++) {
        for (int dx = -(HUT_W - 1); dx <= 0; dx++) {
            int hx = wx + dx, hz = wz + dz;
            if (!hut_origin_at(hx, hz, seed)) continue;
            int type = hut_type(hx, hz, seed);
            int W = hut_w(type), H = hut_h(type);
            int lx = -dx, lz = -dz;
            if (lx >= W || lz >= H) continue;
            int gy = hut_floor_y(hx, hz, seed);
            if (wy != gy + HUT_CHEST_DY) continue;
            if (lx != hut_chest_dx(type)) continue;
            if (lz != hut_chest_dz(type)) continue;
            return true;
        }
    }
    return false;
}

void craft_gen_seed_hut_chest(CraftChest *c, int wx, int wy, int wz,
                              uint32_t seed) {
    /* Each chest rolls one of four rarity tiers, weighted so the
     * jackpot is rare enough to feel rewarding. Building type is
     * independent — a plain plank cabin can hide a legendary chest
     * and a stone house can be near-empty. Keeps exploration
     * worthwhile regardless of which buildings the player passes.
     *
     *   T0 common     (50 %): basic crafting fodder only
     *   T1 uncommon   (30 %): adds iron / bow / wood pickaxe
     *   T2 rare       (15 %): stone tools / redstone / bigger stacks
     *   T3 legendary  ( 5 %): iron tools, gold, occasional diamond
     */
    uint32_t r = hash3(wx, wy, wz) ^ (seed * 0xA1B2C3D4u);
    int tier;
    uint32_t tier_roll = r & 0xFFu;
    if (tier_roll < 128)       tier = 0;
    else if (tier_roll < 205)  tier = 1;
    else if (tier_roll < 243)  tier = 2;
    else                       tier = 3;

    /* Temple chests (desert + jungle) are the landmark payoff for
     * braving the arrow traps — never below T2, ~40% legendary T3. The
     * chest sits at the dir-invariant centre (4,4) of a 9×9 temple, so
     * the origin is (wx-4, wz-4); confirm it's actually a temple there
     * (not a village) before applying the bias. */
    if (tier < 2) {
        int ox = wx - 4, oz = wz - 4;
        if (hut_origin_at(ox, oz, seed)) {
            int t = hut_type(ox, oz, seed);
            if (t == HUT_TYPE_TEMPLE || t == HUT_TYPE_ZIGGURAT)
                tier = ((r >> 28) & 3) == 0 ? 3 : 2;
        }
    }
    /* Dungeon chests — the payoff for braving the deep lair. Never
     * below T2, ~40% legendary T3 like temples. */
    if (tier < 2 && craft_gen_is_dungeon_chest(wx, wy, wz, seed))
        tier = ((r >> 24) & 3) == 0 ? 3 : 2;

    int slot = 0;

    /* Sticks + planks scale with tier — even T0 gets crafting fodder
     * so an empty plain chest still pays back the walk. */
    c->slots[slot].blk = BLK_STICK;
    c->slots[slot].n   = (uint8_t)(2 + ((r >> 8) & 3) + tier);
    slot++;
    c->slots[slot].blk = BLK_PLANK;
    c->slots[slot].n   = (uint8_t)(1 + ((r >> 10) & 3) + tier);
    slot++;

    /* Torches — always at T1+, 50/50 at T0 (so very early-game caves
     * aren't immediately lit by every plain chest). */
    bool torch_drop = (tier >= 1) || (((r >> 12) & 1) == 0);
    if (torch_drop) {
        c->slots[slot].blk = BLK_TORCH;
        c->slots[slot].n   = (uint8_t)(1 + ((r >> 13) & (1 + tier)));
        slot++;
    }

    /* Bow + arrows — T1+ guaranteed; ~25 % at T0. Arrow count scales. */
    bool bow_drop = (tier >= 1) || (((r >> 14) & 3) == 0);
    if (bow_drop) {
        c->slots[slot].blk = BLK_BOW;
        c->slots[slot].n   = 1;
        slot++;
        c->slots[slot].blk = BLK_ARROW;
        c->slots[slot].n   = (uint8_t)(4 + tier * 3 + ((r >> 16) & 3));
        slot++;
    }

    /* Iron ingots — T1+. Count scales with tier. */
    if (tier >= 1) {
        c->slots[slot].blk = BLK_IRON_INGOT;
        c->slots[slot].n   = (uint8_t)(tier + ((r >> 18) & 1));
        slot++;
    }

    /* Pickaxe — T1 wood, T2 stone, T3 iron. */
    if (tier >= 1) {
        BlockId pick;
        switch (tier) {
            case 1:  pick = BLK_PICKAXE_WOOD;  break;
            case 2:  pick = BLK_PICKAXE_STONE; break;
            default: pick = BLK_PICKAXE_IRON;  break;
        }
        c->slots[slot].blk = pick;
        c->slots[slot].n   = 1;
        slot++;
    }

    /* Sword — T2+ stone, T3 iron. */
    if (tier >= 2) {
        c->slots[slot].blk = (tier >= 3) ? BLK_SWORD_IRON : BLK_SWORD_STONE;
        c->slots[slot].n   = 1;
        slot++;
    }

    /* Redstone dust — T2+ in small amounts, T3 in bulk. Lets the
     * player start tinkering with circuits without first finding a
     * vein. */
    if (tier >= 2) {
        c->slots[slot].blk = BLK_REDSTONE;
        c->slots[slot].n   = (uint8_t)((tier == 2 ? 1 : 3) + ((r >> 20) & 1));
        slot++;
    }

    /* Legendary T3 extras: gold ingots, ~50 % diamond drop. */
    if (tier >= 3) {
        c->slots[slot].blk = BLK_GOLD_INGOT;
        c->slots[slot].n   = (uint8_t)(1 + ((r >> 22) & 1));
        slot++;
        if (((r >> 24) & 1) == 0) {
            c->slots[slot].blk = BLK_DIAMOND;
            c->slots[slot].n   = 1;
            slot++;
        }
    }
    (void)slot;
}

/* Find the nearest forest skeleton-fort origin to world column (px,pz)
 * within FORT_SCAN blocks. Returns its courtyard centre + floor y in
 * *ox/*oy/*oz. Used by the mob system to swarm skeletons around forts.
 * Cheap-gated on the fort hash before any heavier site validation. */
bool craft_gen_nearest_fort(int px, int pz, uint32_t seed,
                            int *ox, int *oy, int *oz) {
    const int FORT_SCAN = 40;
    int best = 1 << 30; bool found = false;
    for (int dz = -FORT_SCAN; dz <= FORT_SCAN; dz++) {
        for (int dx = -FORT_SCAN; dx <= FORT_SCAN; dx++) {
            int hx = px + dx, hz = pz + dz;
            if ((hash3(hx, hz, seed ^ 0x5CE1E70Eu) & 0x3FFu) != 0) continue;
            if (!hut_origin_at(hx, hz, seed)) continue;
            if (hut_type(hx, hz, seed) != HUT_TYPE_FORT) continue;
            int d = dx * dx + dz * dz;
            if (d < best) {
                best = d;
                *ox = hx + 4;                       /* 9×9 centre */
                *oz = hz + 4;
                *oy = hut_floor_y(hx, hz, seed);
                found = true;
            }
        }
    }
    return found;
}

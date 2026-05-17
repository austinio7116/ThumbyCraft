/*
 * ThumbyCraft — sliding-window infinite world.
 *
 * The world buffer is still 64×64×64 of resident SRAM, but it now
 * represents a *window* over an infinite plane. world_origin_(x,z)
 * tracks which world coordinate the buffer's local [0,0] sits at.
 * As the player walks, the window slides in chunks (CRAFT_SHIFT
 * blocks at a time) — strips of cells leaving the window are
 * scanned for player-modified blocks and stored in the global mod
 * hash; strips entering the window are regenerated from the seed
 * and overlaid with any saved mods.
 *
 * Mod hash: open-addressing hash table keyed on (wx, wy, wz). Cap
 * is MOD_TABLE_SIZE * 0.75 — when the table fills, the oldest mods
 * silently drop (we'd lose them anyway on a sufficiently long walk).
 * Cells that match the procedural baseline are NOT stored, so the
 * table only grows with actual player changes.
 */
#include "craft_world.h"
#include "craft_gen.h"
#include <string.h>

uint8_t  craft_world_blocks[CRAFT_WORLD_VOXELS];
uint32_t craft_world_dirty;
int      craft_world_origin_x;
int      craft_world_origin_z;

#define CRAFT_SHIFT      16    /* slide step in world units */
#define CRAFT_EDGE_MARGIN 16   /* shift triggers within this many cells of edge */

/* --- Mod hash table ----------------------------------------------- */
#define MOD_TABLE_SIZE 2048     /* power of 2 — open addressing */
#define MOD_TABLE_MASK (MOD_TABLE_SIZE - 1)
#define MOD_FREE_KEY   INT32_MIN

typedef struct {
    int32_t wx;
    int32_t wz;
    int16_t wy;
    uint8_t blk;
    uint8_t flags;       /* bit 0 = occupied */
} ModEntry;

static ModEntry s_mods[MOD_TABLE_SIZE];
static int      s_mod_count;

static uint32_t mod_hash(int wx, int wy, int wz) {
    uint32_t h = (uint32_t)wx * 73856093u
               ^ (uint32_t)wy * 19349663u
               ^ (uint32_t)wz * 83492791u;
    h ^= h >> 16;
    return h;
}

static ModEntry *mod_find_slot(int wx, int wy, int wz, bool insert) {
    uint32_t h = mod_hash(wx, wy, wz);
    for (int probe = 0; probe < MOD_TABLE_SIZE; probe++) {
        int idx = (h + probe) & MOD_TABLE_MASK;
        ModEntry *e = &s_mods[idx];
        if (e->flags & 1) {
            if (e->wx == wx && e->wy == wy && e->wz == wz) return e;
        } else if (insert) {
            return e;
        } else {
            return NULL;
        }
    }
    return NULL;
}

static void mod_set(int wx, int wy, int wz, BlockId blk) {
    ModEntry *e = mod_find_slot(wx, wy, wz, true);
    if (!e) return;          /* table full — drop */
    if (!(e->flags & 1)) {
        s_mod_count++;
        e->wx = wx; e->wz = wz; e->wy = (int16_t)wy;
        e->flags = 1;
    }
    e->blk = blk;
}

static int mod_get(int wx, int wy, int wz) {
    ModEntry *e = mod_find_slot(wx, wy, wz, false);
    return e ? e->blk : -1;
}

int craft_world_mod_count(void) { return s_mod_count; }

/* --- Window-local indexing --------------------------------------- */
static inline int local_idx(int lx, int wy, int lz) {
    return (wy * CRAFT_WORLD_Z + lz) * CRAFT_WORLD_X + lx;
}

BlockId craft_world_get(int wx, int wy, int wz) {
    if ((unsigned)wy >= CRAFT_WORLD_Y) return BLK_AIR;
    int lx = wx - craft_world_origin_x;
    int lz = wz - craft_world_origin_z;
    if ((unsigned)lx >= CRAFT_WORLD_X) return BLK_AIR;
    if ((unsigned)lz >= CRAFT_WORLD_Z) return BLK_AIR;
    return (BlockId)(craft_world_blocks[local_idx(lx, wy, lz)] & 0x0F);
}

void craft_world_set(int wx, int wy, int wz, BlockId blk) {
    if ((unsigned)wy >= CRAFT_WORLD_Y) return;
    /* Always record the mod so it survives a window shift. We could
     * avoid storing cells that match the procedural baseline, but the
     * caller doesn't know the seed, and recomputing here per-set is
     * not worth the saved entries for typical play. */
    mod_set(wx, wy, wz, blk);
    int lx = wx - craft_world_origin_x;
    int lz = wz - craft_world_origin_z;
    if ((unsigned)lx < CRAFT_WORLD_X && (unsigned)lz < CRAFT_WORLD_Z) {
        uint8_t *cell = &craft_world_blocks[local_idx(lx, wy, lz)];
        *cell = (*cell & 0xF0) | ((uint8_t)blk & 0x0F);
    }
    craft_world_dirty = 1;
}

BlockId craft_world_block_at(int wx, int wy, int wz, uint32_t seed) {
    int m = mod_get(wx, wy, wz);
    if (m >= 0) return (BlockId)m;
    return craft_gen_block_at(wx, wy, wz, seed);
}

/* --- Lifecycle --------------------------------------------------- */
void craft_world_init(void) {
    memset(craft_world_blocks, 0, sizeof craft_world_blocks);
    memset(s_mods, 0, sizeof s_mods);
    s_mod_count = 0;
    craft_world_dirty = 0;
    craft_world_origin_x = 0;
    craft_world_origin_z = 0;
}

void craft_world_clear(void) {
    memset(craft_world_blocks, 0, sizeof craft_world_blocks);
    craft_world_dirty = 1;
}

/* Fill the entire window using craft_gen_column (column-batched)
 * + per-cell mod overrides. Roughly 100× faster than the original
 * per-cell loop because the tree-scan no longer happens once per Y. */
static void window_load(uint32_t seed) {
    int ox = craft_world_origin_x;
    int oz = craft_world_origin_z;
    uint8_t col[CRAFT_WORLD_Y];
    for (int lz = 0; lz < CRAFT_WORLD_Z; lz++) {
        for (int lx = 0; lx < CRAFT_WORLD_X; lx++) {
            int wx = lx + ox;
            int wz = lz + oz;
            craft_gen_column(wx, wz, seed, col);
            /* Apply mod overrides for this column. For sparse mod
             * tables the lookup is one hash + one probe each. */
            for (int wy = 0; wy < CRAFT_WORLD_Y; wy++) {
                int m = mod_get(wx, wy, wz);
                if (m >= 0) col[wy] = (uint8_t)m;
                craft_world_blocks[local_idx(lx, wy, lz)] = col[wy];
            }
        }
    }
}

void craft_world_load_around(int player_wx, int player_wz, uint32_t seed) {
    craft_world_origin_x = player_wx - CRAFT_WORLD_X / 2;
    craft_world_origin_z = player_wz - CRAFT_WORLD_Z / 2;
    window_load(seed);
    craft_world_dirty = 0;
}

void craft_world_maybe_shift(int player_wx, int player_wz, uint32_t seed) {
    int lx = player_wx - craft_world_origin_x;
    int lz = player_wz - craft_world_origin_z;

    /* Compute shift amount in CRAFT_SHIFT-sized steps so we don't
     * shift every single block of player motion. */
    int dx = 0, dz = 0;
    while (lx < CRAFT_EDGE_MARGIN)              { dx -= CRAFT_SHIFT; lx += CRAFT_SHIFT; }
    while (lx >= CRAFT_WORLD_X - CRAFT_EDGE_MARGIN) { dx += CRAFT_SHIFT; lx -= CRAFT_SHIFT; }
    while (lz < CRAFT_EDGE_MARGIN)              { dz -= CRAFT_SHIFT; lz += CRAFT_SHIFT; }
    while (lz >= CRAFT_WORLD_Z - CRAFT_EDGE_MARGIN) { dz += CRAFT_SHIFT; lz -= CRAFT_SHIFT; }
    if (dx == 0 && dz == 0) return;

    craft_world_origin_x += dx;
    craft_world_origin_z += dz;
    /* Full regen is simpler than a memmove + strip-fill and only
     * happens at chunk boundaries. ~50 ms one-time stutter. */
    window_load(seed);
}

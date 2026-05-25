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
#include "craft_torches.h"
#include "craft_redstone.h"
#include "craft_chunk_store.h"
#include <string.h>

uint8_t  craft_world_blocks[CRAFT_WORLD_VOXELS];
uint32_t craft_world_dirty;
int      craft_world_origin_x;
int      craft_world_origin_z;
uint8_t  craft_world_lightmap[CRAFT_LIGHTMAP_BYTES];
uint8_t  craft_world_skyheight[CRAFT_WORLD_X * CRAFT_WORLD_Z];
uint8_t  craft_world_biome[CRAFT_WORLD_X * CRAFT_WORLD_Z];

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

/* Batch-rebuild deferral (see craft_world_begin/end_batch). */
static bool s_defer_rebuild  = false;
static bool s_pending_torch  = false;
static bool s_pending_light  = false;

/* --- Dirty-chunk queue ------------------------------------------ *
 * Every block edit marks its chunk dirty. Persist paths consult
 * this list instead of scanning the whole window, so unmodified
 * chunks pay zero flash cost. Background tick drains entries one
 * at a time to spread the ~60-75 ms flash erase+program across
 * frames. */
#define MAX_DIRTY_CHUNKS 32
typedef struct { int32_t cx, cz; } DirtyChunk;
static DirtyChunk s_dirty_q[MAX_DIRTY_CHUNKS];
static int        s_dirty_q_n = 0;

static inline int chunk_of(int w);              /* fwd — used below */
static void persist_chunk(int cx, int cz);      /* fwd — used below */

static int dirty_find(int cx, int cz) {
    for (int i = 0; i < s_dirty_q_n; i++) {
        if (s_dirty_q[i].cx == cx && s_dirty_q[i].cz == cz) return i;
    }
    return -1;
}
static void dirty_drop_at(int i) {
    for (int j = i + 1; j < s_dirty_q_n; j++) s_dirty_q[j - 1] = s_dirty_q[j];
    s_dirty_q_n--;
}
static void mark_chunk_dirty(int cx, int cz) {
    if (dirty_find(cx, cz) >= 0) return;
    if (s_dirty_q_n >= MAX_DIRTY_CHUNKS) {
        /* Overflow — drain the oldest synchronously to free a slot. */
        persist_chunk(s_dirty_q[0].cx, s_dirty_q[0].cz);
        dirty_drop_at(0);
    }
    s_dirty_q[s_dirty_q_n].cx = cx;
    s_dirty_q[s_dirty_q_n].cz = cz;
    s_dirty_q_n++;
}

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
    mark_chunk_dirty(chunk_of(wx), chunk_of(wz));
}

static int mod_get(int wx, int wy, int wz) {
    ModEntry *e = mod_find_slot(wx, wy, wz, false);
    return e ? e->blk : -1;
}

int craft_world_mod_count(void) { return s_mod_count; }

/* --- Flash chunk-store bridge ------------------------------------ */

static inline int chunk_of(int w) {
    if (w >= 0) return w / CHUNK_STORE_CHUNK_SIZE;
    return -((-w + CHUNK_STORE_CHUNK_SIZE - 1) / CHUNK_STORE_CHUNK_SIZE);
}
static inline int chunk_local(int w) {
    int m = w % CHUNK_STORE_CHUNK_SIZE;
    return m < 0 ? m + CHUNK_STORE_CHUNK_SIZE : m;
}

static void persist_chunk(int cx, int cz) {
    static ChunkMod buf[CHUNK_STORE_MAX_MODS_PER_CHUNK];
    int n = 0;
    for (int i = 0; i < MOD_TABLE_SIZE && n < CHUNK_STORE_MAX_MODS_PER_CHUNK; i++) {
        ModEntry *e = &s_mods[i];
        if (!(e->flags & 1)) continue;
        if (chunk_of(e->wx) != cx) continue;
        if (chunk_of(e->wz) != cz) continue;
        buf[n].lx  = (uint8_t)chunk_local(e->wx);
        buf[n].y   = (uint8_t)e->wy;
        buf[n].lz  = (uint8_t)chunk_local(e->wz);
        buf[n].blk = e->blk;
        n++;
    }
    craft_chunk_store_save(cx, cz, buf, n);
}

/* Translate any legacy-format cell byte stored before save v8 into
 * the current encoding. The cell byte IS the BlockId now — no
 * upper-bit packing — so the only translation we can safely apply
 * without misinterpreting a new-format byte is for old WATER cells
 * that had a non-zero level in bits 6-7.
 *
 *   0x87 → WATER_L2   (was id=7, level=2)
 *   0xC7 → WATER_L3   (was id=7, level=3)
 *
 * Old WATER level 0 was stored as 0x07 — same byte as new
 * BLK_WATER_L0, passes through unchanged. Old WATER level 1 was
 * stored as 0x47, which IS the new BLK_OBSERVER_ON byte — that
 * ambiguity resolves in favour of the new BlockId because new
 * saves are far more common than legacy v7 water-edge cells.
 *
 * Any other byte passes through unchanged. This includes the new
 * water levels (64..70), the new redstone _ON variants (71..75),
 * and any future BlockIds. */
static uint8_t migrate_legacy_byte(uint8_t b) {
    if (b == 0x87) return (uint8_t)BLK_WATER_L2;
    if (b == 0xC7) return (uint8_t)BLK_WATER_L3;
    return b;
}

static void restore_chunk(int cx, int cz) {
    /* If the chunk is still in the dirty queue, its newest mods are
     * already in the SRAM mod hash — flash has an older copy that
     * mod_set would overwrite. Skip the flash read in that case.
     * This is what lets chunks_persist_departing defer to
     * persist_tick without losing edits when the player walks back
     * over the deferred region. */
    if (dirty_find(cx, cz) >= 0) return;
    static ChunkMod buf[CHUNK_STORE_MAX_MODS_PER_CHUNK];
    int n = craft_chunk_store_load(cx, cz, buf, CHUNK_STORE_MAX_MODS_PER_CHUNK);
    for (int i = 0; i < n; i++) {
        int wx = cx * CHUNK_STORE_CHUNK_SIZE + buf[i].lx;
        int wz = cz * CHUNK_STORE_CHUNK_SIZE + buf[i].lz;
        uint8_t b = migrate_legacy_byte(buf[i].blk);
        mod_set(wx, buf[i].y, wz, (BlockId)b);
    }
}

static void window_chunk_range(int *cx0, int *cx1, int *cz0, int *cz1) {
    int x0 = craft_world_origin_x;
    int x1 = craft_world_origin_x + CRAFT_WORLD_X - 1;
    int z0 = craft_world_origin_z;
    int z1 = craft_world_origin_z + CRAFT_WORLD_Z - 1;
    *cx0 = chunk_of(x0);
    *cx1 = chunk_of(x1);
    *cz0 = chunk_of(z0);
    *cz1 = chunk_of(z1);
}

void craft_world_chunks_persist_window(void) {
    /* Drain dirty chunks inside the current window. Clean chunks
     * already match flash so we skip them — that's the difference
     * between a fresh-edited-chunk save (~70 ms) and a no-op (free). */
    int cx0, cx1, cz0, cz1;
    window_chunk_range(&cx0, &cx1, &cz0, &cz1);
    int i = 0;
    while (i < s_dirty_q_n) {
        int cx = s_dirty_q[i].cx;
        int cz = s_dirty_q[i].cz;
        if (cx >= cx0 && cx <= cx1 && cz >= cz0 && cz <= cz1) {
            persist_chunk(cx, cz);
            dirty_drop_at(i);   /* don't advance — entries shifted left */
        } else {
            i++;
        }
    }
}

/* Persist only chunks that are leaving the window after a shift.
 * The dirty queue is the source of truth — even leaving chunks that
 * weren't edited can be skipped. */
static void chunks_persist_departing(int old_x0, int old_x1, int old_z0, int old_z1,
                                     int new_x0, int new_x1, int new_z0, int new_z1) {
    /* No-op on the hot path. Chunks leaving the window remain in
     * the dirty queue and the SRAM mod hash; the background
     * persist_tick drains them one chunk per tick — that spreads
     * the per-write cost (~30 ms on FatFs, ~25 ms on raw flash)
     * across many frames instead of bursting all leaving chunks
     * at shift time, which is what caused the "major hitching
     * every chunk change" report.
     *
     * Correctness: restore_chunk skips chunks that are still in
     * the dirty queue, so re-entering a deferred chunk's region
     * reads from the in-RAM mods (newest) rather than the flash
     * copy (older). force_persist_window still drains synchronously
     * on save so the user-visible "save" hitch IS bounded by
     * what they've actually edited. */
    (void)old_x0; (void)old_x1; (void)old_z0; (void)old_z1;
    (void)new_x0; (void)new_x1; (void)new_z0; (void)new_z1;
}

void craft_world_persist_tick(void) {
    if (s_dirty_q_n == 0) return;
    /* Pop the oldest dirty chunk. One flash erase+program per call
     * (~60-75 ms hitch). Caller spaces invocations on a timer. */
    int cx = s_dirty_q[0].cx;
    int cz = s_dirty_q[0].cz;
    persist_chunk(cx, cz);
    dirty_drop_at(0);
}

void craft_world_chunks_force_persist_window(void) {
    /* Walk the mod hash and gather the set of (cx, cz) keys whose
     * mods sit inside the current window. Then persist each one
     * exactly once. This catches the case where a chunk's mods are
     * in the SRAM hash but the chunk is no longer in the dirty queue
     * (because a previous persist drained it) AND the on-flash copy
     * could be stale for some other reason. Save path uses this for
     * extra safety. */
    int cx0, cx1, cz0, cz1;
    window_chunk_range(&cx0, &cx1, &cz0, &cz1);
    /* Bitset of unique chunks (max 8x8 = 64 chunks in a 64x64 window). */
    int n_x = cx1 - cx0 + 1;
    int n_z = cz1 - cz0 + 1;
    /* Cap at 64 — bigger windows would need a different structure. */
    if (n_x * n_z > 64) return;
    bool present[64] = {0};
    for (int i = 0; i < MOD_TABLE_SIZE; i++) {
        ModEntry *e = &s_mods[i];
        if (!(e->flags & 1)) continue;
        int cx = chunk_of(e->wx);
        int cz = chunk_of(e->wz);
        if (cx < cx0 || cx > cx1 || cz < cz0 || cz > cz1) continue;
        int idx = (cz - cz0) * n_x + (cx - cx0);
        present[idx] = true;
    }
    for (int dz = 0; dz < n_z; dz++) {
        for (int dx = 0; dx < n_x; dx++) {
            if (!present[dz * n_x + dx]) continue;
            int cx = cx0 + dx;
            int cz = cz0 + dz;
            persist_chunk(cx, cz);
            int q = dirty_find(cx, cz);
            if (q >= 0) dirty_drop_at(q);
        }
    }
}

void craft_world_chunks_restore_window(void) {
    int cx0, cx1, cz0, cz1;
    window_chunk_range(&cx0, &cx1, &cz0, &cz1);
    for (int cz = cz0; cz <= cz1; cz++) {
        for (int cx = cx0; cx <= cx1; cx++) {
            restore_chunk(cx, cz);
        }
    }
}

/* --- Window-local indexing --------------------------------------- */
static inline int local_idx(int lx, int wy, int lz) {
    return (wy * CRAFT_WORLD_Z + lz) * CRAFT_WORLD_X + lx;
}

/* --- Lightmap --------------------------------------------------- */

/* 2 bits per cell — 4 cells per byte. Levels 0..CRAFT_LIGHT_MAX. */
static inline uint8_t light_get(int idx) {
    return (craft_world_lightmap[idx >> 2] >> ((idx & 3) * 2)) & 3;
}
/* Write only when the new level is brighter than what's already there
 * — lets the BFS naturally take the max across overlapping torches. */
static inline void light_set_max(int idx, uint8_t level) {
    int b = idx >> 2;
    int shift = (idx & 3) * 2;
    uint8_t cur = (craft_world_lightmap[b] >> shift) & 3;
    if (level > cur) {
        craft_world_lightmap[b] = (uint8_t)(
            (craft_world_lightmap[b] & ~(3u << shift)) | ((uint32_t)level << shift)
        );
    }
}

/* True if a torch's light can pass through this block. Air / water /
 * glass / torch as before, PLUS every sprite-rendered cell: the
 * sprite cuboid is small relative to its cell (ladder rails on one
 * wall, wire dust on the floor, a thin door panel etc.) and the
 * surrounding cell volume is empty, so the BFS should treat them
 * the same as air for light propagation. Without this, a wire on
 * the floor casts a "full cube" shadow that darkens cells behind
 * it as if the cell were solid. */
static inline bool light_transparent(BlockId b) {
    if (b == BLK_AIR   || craft_is_water_id((uint8_t)b) ||
        b == BLK_GLASS || b == BLK_TORCH) return true;
    if (b == BLK_LADDER        || b == BLK_PRESSURE_PAD) return true;
    if (b == BLK_REDSTONE_WIRE || b == BLK_REDSTONE_WIRE_ON) return true;
    if (b == BLK_DOOR_OFF      || b == BLK_DOOR_ON) return true;
    if (b == BLK_TRAPDOOR_OFF  || b == BLK_TRAPDOOR_ON) return true;
    if (b == BLK_LEVER_OFF     || b == BLK_LEVER_ON) return true;
    if (b == BLK_PISTON_OFF    || b == BLK_PISTON_ON ||
        b == BLK_STICKY_PISTON_OFF || b == BLK_STICKY_PISTON_ON ||
        b == BLK_PISTON_ARM) return true;
    if (b == BLK_VINE || b == BLK_LILY_PAD) return true;
    return false;
}

/* Map BFS hop distance → light level. With CRAFT_LIGHT_MAX=3 and
 * CRAFT_LIGHT_RADIUS=6: dist 0,1 → 3; 2,3 → 2; 4,5 → 1; ≥6 → 0.
 * Gives a four-step falloff over a 6-block radius. */
static inline int light_level_for_dist(int dist) {
    int level = CRAFT_LIGHT_MAX - (dist >> 1);
    return level < 0 ? 0 : level;
}

/* BFS flood from a torch at local (sx, sy, sz). Each visited cell is
 * marked with the *maximum* level it has seen so overlapping torches
 * compose by max(). Cells are re-enqueued only when a brighter level
 * arrives via a different path. */
#define LIGHT_BFS_MAX 1024
typedef struct __attribute__((packed)) {
    int16_t x, y, z;
    uint8_t dist;
} LightQNode;

static void light_flood_from(int sx, int sy, int sz) {
    if ((unsigned)sx >= CRAFT_WORLD_X) return;
    if ((unsigned)sy >= CRAFT_WORLD_Y) return;
    if ((unsigned)sz >= CRAFT_WORLD_Z) return;

    LightQNode q[LIGHT_BFS_MAX];
    int qh = 0, qt = 0;

    int s_idx = local_idx(sx, sy, sz);
    light_set_max(s_idx, (uint8_t)CRAFT_LIGHT_MAX);
    q[qt++] = (LightQNode){ (int16_t)sx, (int16_t)sy, (int16_t)sz, 0 };

    static const int8_t dxs[6] = { 1, -1, 0, 0, 0, 0 };
    static const int8_t dys[6] = { 0, 0, 1, -1, 0, 0 };
    static const int8_t dzs[6] = { 0, 0, 0, 0, 1, -1 };

    while (qh < qt) {
        LightQNode n = q[qh++];
        int next_level = light_level_for_dist(n.dist + 1);
        if (next_level == 0) continue;     /* nothing brighter to propagate */
        for (int i = 0; i < 6; i++) {
            int nx = n.x + dxs[i];
            int ny = n.y + dys[i];
            int nz = n.z + dzs[i];
            if ((unsigned)nx >= CRAFT_WORLD_X) continue;
            if ((unsigned)ny >= CRAFT_WORLD_Y) continue;
            if ((unsigned)nz >= CRAFT_WORLD_Z) continue;
            int n_idx = local_idx(nx, ny, nz);
            BlockId b = (BlockId)craft_world_blocks[n_idx];
            if (!light_transparent(b)) continue;
            if (light_get(n_idx) >= next_level) continue;  /* already as bright or brighter */
            light_set_max(n_idx, (uint8_t)next_level);
            if (qt < LIGHT_BFS_MAX) {
                q[qt++] = (LightQNode){
                    (int16_t)nx, (int16_t)ny, (int16_t)nz,
                    (uint8_t)(n.dist + 1)
                };
            }
        }
    }
}

/* --- Sky-height ------------------------------------------------- */
/* Counts as "blocks sky" anything that's not transparent. Same set
 * as light_transparent above so sprite cells (ladder / wire / pad /
 * door / trapdoor / piston / lever) don't shadow the column below
 * them. Glass lets sunlight through; water shouldn't (Minecraft
 * attenuates water but for cheapness we treat it as opaque to sky
 * here so deep ocean floors are dark, which is the right vibe). */
static inline bool blocks_sky(BlockId b) {
    if (b == BLK_AIR   || b == BLK_GLASS || b == BLK_TORCH) return false;
    if (b == BLK_LADDER        || b == BLK_PRESSURE_PAD) return false;
    if (b == BLK_REDSTONE_WIRE || b == BLK_REDSTONE_WIRE_ON) return false;
    if (b == BLK_DOOR_OFF      || b == BLK_DOOR_ON) return false;
    if (b == BLK_TRAPDOOR_OFF  || b == BLK_TRAPDOOR_ON) return false;
    if (b == BLK_LEVER_OFF     || b == BLK_LEVER_ON) return false;
    if (b == BLK_PISTON_OFF    || b == BLK_PISTON_ON ||
        b == BLK_STICKY_PISTON_OFF || b == BLK_STICKY_PISTON_ON ||
        b == BLK_PISTON_ARM) return false;
    if (b == BLK_VINE || b == BLK_LILY_PAD) return false;
    return true;
}

static void compute_skyheight_column(int lx, int lz) {
    int sh = 0;
    for (int wy = CRAFT_WORLD_Y - 1; wy >= 0; wy--) {
        BlockId b = (BlockId)craft_world_blocks[local_idx(lx, wy, lz)];
        if (blocks_sky(b)) { sh = wy; break; }
    }
    craft_world_skyheight[lz * CRAFT_WORLD_X + lx] = (uint8_t)sh;
}

static void compute_skyheight_all(void) {
    for (int lz = 0; lz < CRAFT_WORLD_Z; lz++) {
        for (int lx = 0; lx < CRAFT_WORLD_X; lx++) {
            compute_skyheight_column(lx, lz);
        }
    }
}

void craft_world_rebuild_lightmap(void) {
    memset(craft_world_lightmap, 0, sizeof craft_world_lightmap);
    for (int lz = 0; lz < CRAFT_WORLD_Z; lz++) {
        for (int lx = 0; lx < CRAFT_WORLD_X; lx++) {
            for (int wy = 0; wy < CRAFT_WORLD_Y; wy++) {
                uint8_t b = craft_world_blocks[local_idx(lx, wy, lz)];
                /* Torches, lava and lit portals all emit light. (These
                 * use a full-byte compare — their ids are above the
                 * legacy 6-bit mask that the torch check still relies on.) */
                if ((b & 0x3F) == BLK_TORCH || craft_is_lava_id(b) || b == BLK_PORTAL ||
                    b == BLK_LAMP_ON) {
                    light_flood_from(lx, wy, lz);
                }
            }
        }
    }
}

BlockId craft_world_get(int wx, int wy, int wz) {
    if ((unsigned)wy >= CRAFT_WORLD_Y) return BLK_AIR;
    int lx = wx - craft_world_origin_x;
    int lz = wz - craft_world_origin_z;
    if ((unsigned)lx >= CRAFT_WORLD_X) return BLK_AIR;
    if ((unsigned)lz >= CRAFT_WORLD_Z) return BLK_AIR;
    /* Low 6 bits = block id (BLK_COUNT fits in 6 bits). Top 2 bits
     * are repurposed as the water-level field used by the water
     * flow simulation. The mask hides those bits from every
     * consumer of the public block-id API. */
    return (BlockId)craft_world_blocks[local_idx(lx, wy, lz)];
}

uint8_t craft_world_get_byte(int wx, int wy, int wz) {
    if ((unsigned)wy >= CRAFT_WORLD_Y) return 0;
    int lx = wx - craft_world_origin_x;
    int lz = wz - craft_world_origin_z;
    if ((unsigned)lx >= CRAFT_WORLD_X) return 0;
    if ((unsigned)lz >= CRAFT_WORLD_Z) return 0;
    return craft_world_blocks[local_idx(lx, wy, lz)];
}

void craft_world_set_byte(int wx, int wy, int wz, uint8_t b) {
    if ((unsigned)wy >= CRAFT_WORLD_Y) return;
    int lx = wx - craft_world_origin_x;
    int lz = wz - craft_world_origin_z;
    if ((unsigned)lx >= CRAFT_WORLD_X) return;
    if ((unsigned)lz >= CRAFT_WORLD_Z) return;
    craft_world_blocks[local_idx(lx, wy, lz)] = b;
    /* No mod_set side effect — water flow changes are transient and
     * deliberately skip the player-edit chunk-store path. */
}

int craft_world_mod_get(int wx, int wy, int wz) {
    return mod_get(wx, wy, wz);
}

void craft_world_persist_byte(int wx, int wy, int wz, uint8_t b) {
    /* No SRAM write — caller is expected to either already have the
     * right value in SRAM (the "settled at MAX" pool case) or to be
     * pairing this with a set/set_byte (the evaporation case). */
    int prev = mod_get(wx, wy, wz);
    if (prev == (int)b) return;        /* idempotent — skip the dirty-mark */
    mod_set(wx, wy, wz, (BlockId)b);   /* full byte, upper bits preserved */
}

void craft_world_set(int wx, int wy, int wz, BlockId blk) {
    if ((unsigned)wy >= CRAFT_WORLD_Y) return;
    BlockId prev = craft_world_get(wx, wy, wz);
    craft_redstone_note_change(prev, blk);
    /* Skip the mod-store write when the new value is any water level.
     * The water tick handles persistence: settled pools (contained
     * MAX-level cells) get written to mod store explicitly via
     * craft_world_persist_byte, and orphaned flowing water evaporates
     * back to AIR which DOES go through mod_set so any stale entry
     * is purged. The intermediate transient flow stays in SRAM only.
     *
     * Without this gate, a player-placed water source — or natural
     * water that flowed through a player's dug channel — would land
     * in the chunk store as a "permanent player-edit" source and
     * restart spreading on every reload. */
    if (!craft_is_water_id((uint8_t)blk)) {
        mod_set(wx, wy, wz, blk);
    }
    int lx = wx - craft_world_origin_x;
    int lz = wz - craft_world_origin_z;
    if ((unsigned)lx < CRAFT_WORLD_X && (unsigned)lz < CRAFT_WORLD_Z) {
        craft_world_blocks[local_idx(lx, wy, lz)] = (uint8_t)blk;
    }
    craft_world_dirty = 1;

    /* Wall-break activation: if a player just removed a solid block
     * (prev was solid, new is air), any adjacent BLK_WATER_L0 cell
     * is a static natural-water cell that now has an exposed face.
     * Wake those neighbours up by converting to BLK_WATER_L1 so the
     * water tick picks them up and they flow into the new air space.
     * Water-tick evaporations also pass through here, but those have
     * prev == water and so don't trigger the activation. */
    if (blk == BLK_AIR && prev != BLK_AIR &&
        !craft_is_water_id((uint8_t)prev) && !craft_is_lava_id((uint8_t)prev)) {
        static const int dxw[6] = { 1, -1, 0,  0, 0,  0 };
        static const int dyw[6] = { 0,  0, 1, -1, 0,  0 };
        static const int dzw[6] = { 0,  0, 0,  0, 1, -1 };
        for (int d = 0; d < 6; d++) {
            int nx = wx + dxw[d];
            int ny = wy + dyw[d];
            int nz = wz + dzw[d];
            if ((unsigned)ny >= CRAFT_WORLD_Y) continue;
            int nlx = nx - craft_world_origin_x;
            int nlz = nz - craft_world_origin_z;
            if ((unsigned)nlx >= CRAFT_WORLD_X) continue;
            if ((unsigned)nlz >= CRAFT_WORLD_Z) continue;
            int nidx = local_idx(nlx, ny, nlz);
            /* Wake a static fluid source that just gained an exposed
             * face: water L0 → L1, lava source → L1. The flow sim then
             * spreads it into the new air space. */
            if (craft_world_blocks[nidx] == BLK_WATER_L0) {
                craft_world_blocks[nidx] = (uint8_t)BLK_WATER_L1;
            } else if (craft_world_blocks[nidx] == BLK_LAVA) {
                craft_world_blocks[nidx] = (uint8_t)BLK_LAVA_L1;
            }
        }
    }
    /* Torch place/remove or anything that changes solid→transparent
     * needs a lightmap rebuild. Cheap (~few ms) so just rebuild on
     * any structural change involving torches. */
    /* Sky-height column update: cheap, one column scan, always do it. */
    if ((unsigned)lx < CRAFT_WORLD_X && (unsigned)lz < CRAFT_WORLD_Z) {
        compute_skyheight_column(lx, lz);
    }

    /* Lightmap maintenance.
     *  - Torch place/break needs both the torch list and the lightmap
     *    rebuilt.
     *  - Any other transparency change (solid → air, or vice versa)
     *    affects how light propagates: breaking a wall might expose a
     *    torch-lit room beyond it; placing a wall blocks light. So
     *    rebuild the lightmap there too.
     *  - Pure same-transparency changes (e.g. dirt → grass, stone →
     *    cobble) don't touch propagation — skip the rebuild to keep
     *    the build/place loop fast. */
    bool torch_change   = (blk == BLK_TORCH || prev == BLK_TORCH);
    /* Wires and levers ride the torch render pipeline as small overlay
     * sprites — their list also needs to be rebuilt on placement /
     * removal / state transition (lever toggle, wire power flip). */
    /* Trigger torch-list rebuild on any block change that affects
     * sprite rendering OR wire connectivity. Pistons + TNT aren't
     * rendered as sprites themselves but adjacent wires should
     * connect to them visually — including them here makes the
     * wire connect mask refresh on those placements/changes. */
    #define IS_SPRITE(b) ((b) == BLK_REDSTONE_WIRE    ||     \
                          (b) == BLK_REDSTONE_WIRE_ON ||     \
                          (b) == BLK_LEVER_OFF        ||     \
                          (b) == BLK_LEVER_ON         ||     \
                          (b) == BLK_LADDER           ||     \
                          (b) == BLK_PRESSURE_PAD     ||     \
                          (b) == BLK_DOOR_OFF         ||     \
                          (b) == BLK_DOOR_ON          ||     \
                          (b) == BLK_TRAPDOOR_OFF     ||     \
                          (b) == BLK_TRAPDOOR_ON      ||     \
                          (b) == BLK_PISTON_OFF       ||     \
                          (b) == BLK_PISTON_ON        ||     \
                          (b) == BLK_STICKY_PISTON_OFF ||    \
                          (b) == BLK_STICKY_PISTON_ON ||     \
                          (b) == BLK_VINE             ||     \
                          (b) == BLK_LILY_PAD         ||     \
                          (b) == BLK_TNT              ||     \
                          (b) == BLK_TNT_FUSED        ||     \
                          (b) == BLK_REDSTONE_BLOCK)
    bool sprite_change  = torch_change || IS_SPRITE(blk) || IS_SPRITE(prev);
    #undef IS_SPRITE
    bool prev_blocks    = !light_transparent(prev);
    bool new_blocks     = !light_transparent(blk);
    bool transp_changed = (prev_blocks != new_blocks);
    /* A redstone lamp toggling on/off changes emitted light without
     * changing transparency, so it needs its own rebuild trigger. */
    bool lamp_change    = (blk == BLK_LAMP_ON || prev == BLK_LAMP_ON);
    if (sprite_change) {
        if (prev == BLK_TORCH && blk != BLK_TORCH) {
            craft_torches_forget_orient(wx, wy, wz);
        }
        /* Torches actually emit light, so a torch change forces a
         * lightmap rebuild. Wire/lever transitions don't affect
         * lightmap (wires are non-opaque both ways; levers are
         * solid both ways), so skip that work. */
        bool want_light = torch_change || transp_changed;
        if (s_defer_rebuild) {
            s_pending_torch = true;
            if (want_light) s_pending_light = true;
        } else {
            craft_torches_rebuild();
            if (want_light) craft_world_rebuild_lightmap();
        }
    } else if (transp_changed || lamp_change) {
        if (s_defer_rebuild) s_pending_light = true;
        else craft_world_rebuild_lightmap();
    }
}

void craft_world_begin_batch(void) {
    s_defer_rebuild = true;
    s_pending_torch = false;
    s_pending_light = false;
}

bool craft_world_end_batch(void) {
    s_defer_rebuild = false;
    bool torch = s_pending_torch;
    if (s_pending_light) craft_world_rebuild_lightmap();
    s_pending_torch = false;
    s_pending_light = false;
    return torch;
}

BlockId craft_world_block_at(int wx, int wy, int wz, uint32_t seed) {
    int m = mod_get(wx, wy, wz);
    if (m >= 0) return (BlockId)m;
    return craft_gen_block_at(wx, wy, wz, seed);
}

/* --- Lifecycle --------------------------------------------------- */
void craft_world_init(void) {
    memset(craft_world_blocks, 0, sizeof craft_world_blocks);
    memset(craft_world_lightmap, 0, sizeof craft_world_lightmap);
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

void craft_world_reset_mods(void) {
    memset(s_mods, 0, sizeof s_mods);
    s_mod_count = 0;
    s_dirty_q_n = 0;
}

/* Walk the mod hash and drop every water-id entry, marking each
 * touched chunk dirty so the flash store gets the cleaned set on
 * the next persist. Used as a one-shot wipe right after world load
 * to clear stray water sources persisted by older save logic — the
 * settled-pool persist path is the only thing allowed to keep water
 * in the mod store now. */
int craft_world_wipe_water_mods(void) {
    int wiped = 0;
    for (int i = 0; i < MOD_TABLE_SIZE; i++) {
        ModEntry *e = &s_mods[i];
        if (!(e->flags & 1)) continue;
        if (!craft_is_water_id(e->blk)) continue;
        mark_chunk_dirty(chunk_of(e->wx), chunk_of(e->wz));
        e->flags = 0;
        s_mod_count--;
        wiped++;
    }
    return wiped;
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
    /* Height cache is keyed on the window origin — drop it before
     * regenerating columns so the lazy refill writes valid data for
     * the new region. */
    craft_gen_invalidate_height_cache();
    /* Restore persisted mods for chunks in the new window BEFORE
     * regen — that way window_load's mod_get lookups see them and
     * the buffer comes out with the player's previous edits already
     * applied. */
    craft_world_chunks_restore_window();
    window_load(seed);
    /* Stamp trees + huts as whole units now the terrain is down and
     * mods are applied — features only appear where their trunk/origin
     * column is inside the window. */
    craft_gen_stamp_features(seed);
    compute_skyheight_all();
    craft_torches_rebuild();
    craft_redstone_rescan();
    craft_world_rebuild_lightmap();
    craft_world_dirty = 0;
}

/* Regenerate a single column's contents into the resident buffer.
 * `lx`, `lz` are local buffer indices; `wx`, `wz` are absolute world
 * coords (caller computes these consistent with origin). Applies mod
 * overrides on top of the procedural gen. */
static void regen_one_column(int lx, int lz, int wx, int wz, uint32_t seed) {
    uint8_t col[CRAFT_WORLD_Y];
    craft_gen_column(wx, wz, seed, col);
    for (int wy = 0; wy < CRAFT_WORLD_Y; wy++) {
        int m = mod_get(wx, wy, wz);
        if (m >= 0) col[wy] = (uint8_t)m;
        craft_world_blocks[local_idx(lx, wy, lz)] = col[wy];
    }
}

/* Slide the buffer by dx along X: memmove the cells already in
 * memory, then regenerate the freshly-exposed strip. Origin updates
 * to the new value. */
static void shift_x(int dx, uint32_t seed) {
    int adx = (dx > 0) ? dx : -dx;
    if (adx >= CRAFT_WORLD_X) {
        /* No overlap — caller should full-regen instead. */
        return;
    }
    /* X is the innermost (fastest-varying) buffer index, so each
     * Y/Z row is a contiguous run of CRAFT_WORLD_X bytes. */
    for (int wy = 0; wy < CRAFT_WORLD_Y; wy++) {
        for (int lz = 0; lz < CRAFT_WORLD_Z; lz++) {
            uint8_t *row = &craft_world_blocks[
                (wy * CRAFT_WORLD_Z + lz) * CRAFT_WORLD_X];
            if (dx > 0) {
                /* New origin > old: cells slide toward index 0. */
                memmove(row, row + dx, CRAFT_WORLD_X - dx);
            } else {
                memmove(row + (-dx), row, CRAFT_WORLD_X + dx);
            }
        }
    }
    /* Slide the per-column biome map in lockstep (one Z-row of X bytes
     * each) — the new strip's entries are rewritten by regen below. */
    for (int lz = 0; lz < CRAFT_WORLD_Z; lz++) {
        uint8_t *brow = &craft_world_biome[lz * CRAFT_WORLD_X];
        if (dx > 0) memmove(brow, brow + dx, CRAFT_WORLD_X - dx);
        else        memmove(brow + (-dx), brow, CRAFT_WORLD_X + dx);
    }
    craft_world_origin_x += dx;
    /* Height cache anchors on origin — drop and re-anchor before
     * any regen so the lazy fill writes against the new window. */
    craft_gen_invalidate_height_cache();

    int new_lx0 = (dx > 0) ? (CRAFT_WORLD_X - dx) : 0;
    int new_lx1 = (dx > 0) ? CRAFT_WORLD_X : -dx;
    for (int lx = new_lx0; lx < new_lx1; lx++) {
        int wx = lx + craft_world_origin_x;
        for (int lz = 0; lz < CRAFT_WORLD_Z; lz++) {
            int wz = lz + craft_world_origin_z;
            regen_one_column(lx, lz, wx, wz, seed);
        }
    }
}

/* Slide the buffer by dz along Z. Z spans whole rows of X cells, so
 * memmove operates on contiguous (W × dz) blocks per Y layer. */
static void shift_z(int dz, uint32_t seed) {
    int adz = (dz > 0) ? dz : -dz;
    if (adz >= CRAFT_WORLD_Z) return;
    for (int wy = 0; wy < CRAFT_WORLD_Y; wy++) {
        uint8_t *layer = &craft_world_blocks[
            wy * CRAFT_WORLD_Z * CRAFT_WORLD_X];
        size_t row_bytes = CRAFT_WORLD_X;
        if (dz > 0) {
            memmove(layer,
                    layer + dz * row_bytes,
                    (CRAFT_WORLD_Z - dz) * row_bytes);
        } else {
            memmove(layer + (-dz) * row_bytes,
                    layer,
                    (CRAFT_WORLD_Z + dz) * row_bytes);
        }
    }
    /* Slide the per-column biome map in Z (whole rows of X bytes). */
    if (dz > 0) {
        memmove(craft_world_biome,
                craft_world_biome + dz * CRAFT_WORLD_X,
                (size_t)(CRAFT_WORLD_Z - dz) * CRAFT_WORLD_X);
    } else {
        memmove(craft_world_biome + (-dz) * CRAFT_WORLD_X,
                craft_world_biome,
                (size_t)(CRAFT_WORLD_Z + dz) * CRAFT_WORLD_X);
    }
    craft_world_origin_z += dz;
    /* Re-anchor the height cache to the new origin before regen. */
    craft_gen_invalidate_height_cache();

    int new_lz0 = (dz > 0) ? (CRAFT_WORLD_Z - dz) : 0;
    int new_lz1 = (dz > 0) ? CRAFT_WORLD_Z : -dz;
    for (int lz = new_lz0; lz < new_lz1; lz++) {
        int wz = lz + craft_world_origin_z;
        for (int lx = 0; lx < CRAFT_WORLD_X; lx++) {
            int wx = lx + craft_world_origin_x;
            regen_one_column(lx, lz, wx, wz, seed);
        }
    }
}

void craft_world_maybe_shift(int player_wx, int player_wz, uint32_t seed) {
    int lx = player_wx - craft_world_origin_x;
    int lz = player_wz - craft_world_origin_z;

    int dx = 0, dz = 0;
    while (lx < CRAFT_EDGE_MARGIN)                  { dx -= CRAFT_SHIFT; lx += CRAFT_SHIFT; }
    while (lx >= CRAFT_WORLD_X - CRAFT_EDGE_MARGIN) { dx += CRAFT_SHIFT; lx -= CRAFT_SHIFT; }
    while (lz < CRAFT_EDGE_MARGIN)                  { dz -= CRAFT_SHIFT; lz += CRAFT_SHIFT; }
    while (lz >= CRAFT_WORLD_Z - CRAFT_EDGE_MARGIN) { dz += CRAFT_SHIFT; lz -= CRAFT_SHIFT; }
    if (dx == 0 && dz == 0) return;

    /* Before shifting: persist only chunks that are LEAVING the
     * window AND are dirty. The mod hash is keyed on world coords —
     * mods for chunks that stay in window keep living in the hash,
     * and the background persist_tick will drain them later. */
    int old_x0, old_x1, old_z0, old_z1;
    window_chunk_range(&old_x0, &old_x1, &old_z0, &old_z1);
    int new_origin_x = craft_world_origin_x + dx;
    int new_origin_z = craft_world_origin_z + dz;
    int new_x0 = chunk_of(new_origin_x);
    int new_x1 = chunk_of(new_origin_x + CRAFT_WORLD_X - 1);
    int new_z0 = chunk_of(new_origin_z);
    int new_z1 = chunk_of(new_origin_z + CRAFT_WORLD_Z - 1);
    chunks_persist_departing(old_x0, old_x1, old_z0, old_z1,
                             new_x0, new_x1, new_z0, new_z1);

    /* No overlap → full regen. Happens only on huge teleports. */
    int adx = (dx > 0) ? dx : -dx;
    int adz = (dz > 0) ? dz : -dz;
    if (adx >= CRAFT_WORLD_X || adz >= CRAFT_WORLD_Z) {
        craft_world_origin_x += dx;
        craft_world_origin_z += dz;
        craft_gen_invalidate_height_cache();
        craft_world_chunks_restore_window();
        window_load(seed);
        craft_gen_stamp_features(seed);
        compute_skyheight_all();
        craft_torches_rebuild();
        craft_world_rebuild_lightmap();
        return;
    }

    /* Partial regen: memmove the overlap, regen only the new strip.
     * X and Z handled independently. Each shift_* function updates
     * the origin and then refreshes the height cache before regen
     * so the lazy fill lands at the right coords. */
    if (dx != 0) shift_x(dx, seed);
    if (dz != 0) shift_z(dz, seed);

    /* After shift: pull in mods for chunks that newly overlap the
     * window AND back-stamp any in-hash mods onto the buffer (some
     * strips were regenerated before the restore added them, so
     * they'd otherwise be missing). The back-stamp loop scans the
     * 2K-entry hash — microseconds. */
    craft_world_chunks_restore_window();
    for (int i = 0; i < MOD_TABLE_SIZE; i++) {
        ModEntry *e = &s_mods[i];
        if (!(e->flags & 1)) continue;
        int lxi = e->wx - craft_world_origin_x;
        int lzi = e->wz - craft_world_origin_z;
        if ((unsigned)lxi >= CRAFT_WORLD_X) continue;
        if ((unsigned)lzi >= CRAFT_WORLD_Z) continue;
        if ((unsigned)e->wy >= CRAFT_WORLD_Y) continue;
        int idx = (e->wy * CRAFT_WORLD_Z + lzi) * CRAFT_WORLD_X + lxi;
        craft_world_blocks[idx] = e->blk;
    }

    /* Re-stamp features ONLY for the freshly-exposed strips, widened
     * by the max canopy radius so a trunk sitting just inside the
     * overlap whose canopy reaches the new strip still gets drawn.
     * Bounds the per-shift cost to strip-width + margin instead of
     * the whole window. Canopy that overhangs into the overlap is
     * idempotent (AIR-only writes / mod-skip), and the X/Z corner is
     * harmlessly stamped twice. */
    const int R = CRAFT_GEN_MAX_TREE_RADIUS;
    if (dx != 0) {
        int s0 = (dx > 0) ? (CRAFT_WORLD_X - dx) : 0;
        int s1 = (dx > 0) ? CRAFT_WORLD_X : -dx;
        craft_gen_stamp_features_region(seed, s0 - R, s1 + R, 0, CRAFT_WORLD_Z);
    }
    if (dz != 0) {
        int s0 = (dz > 0) ? (CRAFT_WORLD_Z - dz) : 0;
        int s1 = (dz > 0) ? CRAFT_WORLD_Z : -dz;
        craft_gen_stamp_features_region(seed, 0, CRAFT_WORLD_X, s0 - R, s1 + R);
    }

    /* Sky-height, lightmap, and torch list are all local-indexed —
     * rebuild from the contents of the new window. Few ms total. */
    compute_skyheight_all();
    craft_torches_rebuild();
    craft_redstone_rescan();   /* registry is local-indexed — rebuild for the new window */
    craft_world_rebuild_lightmap();
}

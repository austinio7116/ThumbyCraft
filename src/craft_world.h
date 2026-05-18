/*
 * ThumbyCraft — block storage.
 *
 * v1: monolithic byte-per-block array sized CRAFT_WORLD_VOXELS.
 * v2 will swap this for a chunk LRU paged from flash with the same
 * get/set API so callers don't change.
 */
#ifndef CRAFT_WORLD_H
#define CRAFT_WORLD_H

#include "craft_blocks.h"

/* Backing array — exposed so the save layer can take fast bulk
 * snapshots and walk diffs. Treat as opaque elsewhere. */
extern uint8_t  craft_world_blocks[CRAFT_WORLD_VOXELS];
extern uint32_t craft_world_dirty;

/* World is an infinite plane in X/Z; CRAFT_WORLD_X×CRAFT_WORLD_Z is
 * a sliding *window* of cells currently kept in SRAM. World coords
 * passed to get/set are absolute (signed) — the window origin tracks
 * where the buffer's local [0, 0] sits in world space. */
extern int craft_world_origin_x;
extern int craft_world_origin_z;

/* Lightmap — 2 bits per cell, 0..CRAFT_LIGHT_MAX. 0 means "no torch
 * light reaches this cell"; higher levels mean brighter. The renderer
 * looks at the air cell on the outside of each rendered face and uses
 * that cell's level to floor the face brightness, giving a smooth
 * radial falloff around torches.
 *
 * Size: 262144 cells × 2 bits = 64 KB. Funded by moving craft_textures
 * to flash (see craft_blocks.c). */
#define CRAFT_LIGHTMAP_BYTES (CRAFT_WORLD_VOXELS / 4)
#define CRAFT_LIGHT_MAX       3
#define CRAFT_LIGHT_RADIUS    6  /* BFS hop limit — also the dist at which level hits 0 */
extern uint8_t craft_world_lightmap[CRAFT_LIGHTMAP_BYTES];

/* Sky-height per column — Y of the topmost solid cell. Anything
 * above that cell is "sky-exposed" (gets sun/moon brightness);
 * anything at-or-below is in shadow (cave / under cover) and stays
 * dark independent of the day/night cycle. Updated whenever a
 * column's contents change. */
extern uint8_t craft_world_skyheight[CRAFT_WORLD_X * CRAFT_WORLD_Z];

static inline bool craft_world_sky_exposed(int wx, int wy, int wz) {
    int lx = wx - craft_world_origin_x;
    int lz = wz - craft_world_origin_z;
    if ((unsigned)lx >= CRAFT_WORLD_X) return true;  /* off-window = sky */
    if ((unsigned)lz >= CRAFT_WORLD_Z) return true;
    if ((unsigned)wy >= CRAFT_WORLD_Y) return true;
    return wy > craft_world_skyheight[lz * CRAFT_WORLD_X + lx];
}

static inline int craft_world_light_level(int wx, int wy, int wz) {
    if ((unsigned)wy >= CRAFT_WORLD_Y) return 0;
    int lx = wx - craft_world_origin_x;
    int lz = wz - craft_world_origin_z;
    if ((unsigned)lx >= CRAFT_WORLD_X) return 0;
    if ((unsigned)lz >= CRAFT_WORLD_Z) return 0;
    int idx = (wy * CRAFT_WORLD_Z + lz) * CRAFT_WORLD_X + lx;
    return (craft_world_lightmap[idx >> 2] >> ((idx & 3) * 2)) & 3;
}

/* Recompute the entire lightmap from torches in the current window.
 * Called automatically after window_load, window shifts, and any
 * craft_world_set that involves a torch. ~few ms even with many
 * torches in view. */
void craft_world_rebuild_lightmap(void);

/* Initialise the world to an empty buffer at origin (0, 0). */
void craft_world_init(void);

/* Wipe the resident buffer to all-AIR. The mod table is untouched —
 * call craft_world_load_around afterwards to repopulate from seed. */
void craft_world_clear(void);

/* Generate the window around (player_wx, player_wz) for `seed`. */
void craft_world_load_around(int player_wx, int player_wz, uint32_t seed);

/* Check if the player has walked close enough to a window edge that
 * the buffer should slide. If so, regenerate the new strips from
 * seed and re-apply any player modifications from the mod table.
 * Called every frame; cheap when no shift is needed. */
void craft_world_maybe_shift(int player_wx, int player_wz, uint32_t seed);

/* Number of mod entries in the table (for HUD diagnostics). */
int craft_world_mod_count(void);

/* Persist all currently-windowed chunks' mods to flash, and pull in
 * any flash-persisted mods for chunks in the window. The pair is the
 * bridge between the in-SRAM mod hash and the flash chunk store —
 * the world layer calls them automatically on load and around window
 * shifts; the save path can call persist explicitly for safety. */
void craft_world_chunks_persist_window(void);
void craft_world_chunks_restore_window(void);

/* Stronger variant for the Save path: persists EVERY in-window chunk
 * that has any mods in the SRAM hash, not just chunks marked dirty.
 * Slower than chunks_persist_window (one flash op per modified chunk
 * regardless of dirty state) but guarantees the chunk store reflects
 * the current in-memory state — belt-and-braces against any case
 * where the dirty queue might have got out of sync. */
void craft_world_chunks_force_persist_window(void);

/* Drain at most one dirty chunk from the in-SRAM queue to flash.
 * Cheap no-op when nothing is dirty. Call from the main loop on a
 * timer so flash hitches spread across seconds instead of bundling
 * into one stutter on window shift / Save. */
void craft_world_persist_tick(void);

/* Get/set use *absolute world coordinates*. Out-of-window get
 * returns BLK_AIR (the renderer treats that as sky-equivalent at
 * window edge); out-of-window set still records in the mod table
 * so the cell takes effect when its strip slides into view. */
BlockId craft_world_get(int wx, int wy, int wz);
void    craft_world_set(int wx, int wy, int wz, BlockId blk);

/* Mod-aware block lookup — used by the window shift path. Returns
 * the override block if (wx, wy, wz) is in the mod table, otherwise
 * the procedural value from craft_gen_block_at(seed). */
BlockId craft_world_block_at(int wx, int wy, int wz, uint32_t seed);

#endif

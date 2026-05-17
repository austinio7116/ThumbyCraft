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

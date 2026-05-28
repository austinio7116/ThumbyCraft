/*
 * ThumbyCraft — terrain generator.
 *
 * Deterministic from a 32-bit seed. Fills the world with a heightmap-
 * based landscape: stone interior, dirt + grass surface, sand at
 * water level, water plane at WATER_LEVEL, scattered trees.
 *
 * Single-shot operation — called from craft_world_init when no save
 * is present (or when the user starts a new world).
 */
#ifndef CRAFT_GEN_H
#define CRAFT_GEN_H

#include "craft_types.h"
#include "craft_blocks.h"
#include "craft_chests.h"

#define CRAFT_WATER_LEVEL 28
/* Lava floods carved cave cells at or below this Y, forming lava lakes
 * in the deep caverns (well under the water table). */
#define CRAFT_LAVA_LEVEL  10

/* Suggested player spawn point — picks a grass tile near world centre
 * a few blocks above the ground. Call after the world is generated. */
Vec3 craft_gen_spawn(void);

/* Nearest forest skeleton-fort origin to (px,pz) within scan range —
 * fills courtyard centre + floor y. Returns false if none near. */
bool craft_gen_nearest_fort(int px, int pz, uint32_t seed,
                            int *ox, int *oy, int *oz);

/* Fast column generator — fills `out[CRAFT_WORLD_Y]` with the gen
 * values for column (wx, wz). Used by craft_world's window_load to
 * batch terrain + tree expansion per-column instead of per-cell —
 * about 100× faster on a window-wide regen. */
void craft_gen_column(int wx, int wz, uint32_t seed,
                      uint8_t out[/* CRAFT_WORLD_Y */]);

/* Stamp trees + buildings whose trunk/origin column is inside the
 * current resident window directly into craft_world_blocks, as whole
 * units (cross-column). Run after craft_gen_column has laid the
 * window's terrain (window load) and after every window shift. Fills
 * AIR for trees / overwrites for huts, and never clobbers player
 * mod-store edits. */
void craft_gen_stamp_features(uint32_t seed);

/* Largest distance (in cells) a feature's blocks can sit from its
 * trunk/origin column — the giant swamp tree's canopy. Callers that
 * stamp only a sub-region (e.g. a freshly shifted strip) must widen
 * the TRUNK search by this margin so a trunk just outside the strip
 * whose canopy reaches in still gets stamped. */
#define CRAFT_GEN_MAX_TREE_RADIUS 7

/* Stamp features whose trunk/origin column is inside the local-coord
 * rectangle [tlx0,tlx1) × [tlz0,tlz1). Bounds are clamped to the
 * window. Use after a shift with the new strip widened by
 * CRAFT_GEN_MAX_TREE_RADIUS. */
void craft_gen_stamp_features_region(uint32_t seed,
                                     int tlx0, int tlx1,
                                     int tlz0, int tlz1);

/* Drop the column-height cache. The generator memoises height_at into
 * a window-aligned table to amortise the 7×7-neighbour tree scan;
 * call after every window shift (origin change) so the next column
 * regens repopulate cleanly. Idempotent. */
void craft_gen_invalidate_height_cache(void);

/* True if world cell (wx, wy, wz) sits on the chest cell of some hut
 * for this seed. Used at chest-open time so the loot table fires
 * exactly once per hut. */
bool craft_gen_is_hut_chest(int wx, int wy, int wz, uint32_t seed);

/* True if (wx,wy,wz) is an underground dungeon treasure-chest cell. */
bool craft_gen_is_dungeon_chest(int wx, int wy, int wz, uint32_t seed);

/* Populate `c` (assumed freshly-created and zeroed) with deterministic
 * random loot for the hut at (wx, wy, wz). Same seed + coords always
 * yield the same contents. */
void craft_gen_seed_hut_chest(CraftChest *c, int wx, int wy, int wz,
                              uint32_t seed);

#endif

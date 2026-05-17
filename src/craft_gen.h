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

#define CRAFT_WATER_LEVEL 28

/* Fill the world buffer with freshly-generated terrain from `seed`. */
void craft_gen_world(uint32_t seed);

/* Suggested player spawn point — picks a grass tile near world centre
 * a few blocks above the ground. Call after craft_gen_world. */
Vec3 craft_gen_spawn(void);

/* Pure function — what *would* block (x, y, z) be in a fresh world
 * for `seed`? Used by the save layer to diff the current world
 * against the base without holding a second world buffer in SRAM. */
BlockId craft_gen_block_at(int x, int y, int z, uint32_t seed);

/* Fast column generator — fills `out[CRAFT_WORLD_Y]` with the gen
 * values for column (wx, wz). Used by craft_world's window_load to
 * batch terrain + tree expansion per-column instead of per-cell —
 * about 100× faster on a window-wide regen. */
void craft_gen_column(int wx, int wz, uint32_t seed,
                      uint8_t out[/* CRAFT_WORLD_Y */]);

#endif

/*
 * ThumbyCraft — water flow simulation.
 *
 * Cellular automaton tick (5 Hz) that spreads source water into
 * adjacent air cells and decays flowing water that loses its
 * supplier. Source cells = BLK_WATER at y ≤ CRAFT_WATER_LEVEL
 * (natural lakes/oceans); these never decay. Flowing cells carry
 * a 3-bit distance field stored in the top bits of the block byte
 * (visible only to this module and the world-byte API).
 *
 * Memory: ~2 KB static event queue, no per-cell state outside the
 * existing world buffer.
 * CPU:    one window scan per tick (5 Hz) — ~3-5 ms / tick, well
 *         under 1 frame at 30 fps, smoothed out by gating.
 *
 * Use:
 *   craft_water_init();          // once at startup
 *   craft_water_tick(dt);        // every frame; internal accum
 *                                // gates the actual sim work
 */
#ifndef CRAFT_WATER_H
#define CRAFT_WATER_H

#include <stdbool.h>

void craft_water_init(void);
void craft_water_tick(float dt);

#endif

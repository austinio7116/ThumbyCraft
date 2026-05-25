/*
 * ThumbyCraft — lava flow simulation.
 *
 * Mirrors the water flow automaton (craft_water.c) almost exactly, but
 * ticks far more slowly so lava oozes rather than rushes. Source cells
 * are BLK_LAVA (static, never decay); flowing cells carry levels
 * BLK_LAVA_L1..L3 and spread/decay one ring per (slow) tick.
 *
 * Lava that touches water petrifies into obsidian — the symmetric half
 * of the rule the water sim already applies to lava neighbours.
 *
 * Use:
 *   craft_lava_init();           // once at startup
 *   craft_lava_tick(dt);         // every frame; internal accum gates
 *                                // the actual sim work (~1.1 Hz)
 */
#ifndef CRAFT_LAVA_H
#define CRAFT_LAVA_H

#include <stdbool.h>

void craft_lava_init(void);
void craft_lava_tick(float dt);

#endif

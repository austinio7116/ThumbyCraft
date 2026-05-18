/*
 * ThumbyCraft — redstone power propagation.
 *
 * Tiny 5 Hz sim that walks the resident world window once per tick to
 * propagate power from BLK_LEVER_ON cells through BLK_REDSTONE_WIRE
 * cells (in either powered/unpowered state), 6-direction adjacency.
 *
 * Side effects:
 *   - Wire cells transition between WIRE / WIRE_ON based on whether
 *     they were reached by the BFS.
 *   - When a BLK_DIAMOND_BLOCK ends up adjacent to a powered cell
 *     (lever_on or wire_on), it activates ONCE — the redstone module
 *     calls craft_mobs_spawn_boss_at() at the block + (0,1,0) and
 *     records the activation in s_activated[] so re-energising the
 *     same block doesn't spawn another boss.
 */
#ifndef CRAFT_REDSTONE_H
#define CRAFT_REDSTONE_H

#include "craft_blocks.h"

void craft_redstone_init(void);
void craft_redstone_tick(float dt);

/* Full-window recount of active sources (LEVER_ON + WIRE_ON). Cheap
 * — direct byte scan — but only worth doing on window load / new
 * world; per-block edits use note_change below. */
void craft_redstone_rescan(void);

/* Incremental update for a single block transition. Called from
 * craft_world_set so the tick can short-circuit when nothing is
 * powered. Pass the previous block id and the new one. */
void craft_redstone_note_change(BlockId prev_blk, BlockId new_blk);

#endif

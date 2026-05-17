/*
 * ThumbyCraft — dropped item entities.
 *
 * Small pool of "item floating in the world" entities. A drop has a
 * world position, a block / item id, a lifetime, and a small Y bob
 * so it stays visually distinct from the terrain.
 *
 * Sources:
 *   - Mob death (skeleton → bow + arrows)
 *   - Arrow miss recovery (an arrow that hits a block or expires
 *     becomes a collectable arrow drop)
 *   - Future: more mob loot, broken-block items if we add the "drop
 *     what you mined" feature
 *
 * Pickup: any drop within CRAFT_DROP_PICKUP_DIST of the player gets
 * collected into their inventory[blk_id] and despawns.
 *
 * Rendering: each drop renders as a small spinning cube using the
 * existing block atlas — cheap to project, just one cuboid per drop.
 */
#ifndef CRAFT_DROPS_H
#define CRAFT_DROPS_H

#include "craft_types.h"
#include "craft_blocks.h"
#include "craft_player.h"
#include "craft_render.h"

#define CRAFT_MAX_DROPS         12
#define CRAFT_DROP_PICKUP_DIST  1.5f
#define CRAFT_DROP_LIFETIME     90.0f   /* sec before despawn */

typedef struct {
    bool    alive;
    BlockId blk;
    Vec3    pos;        /* world position, centre of the floating cube */
    float   age;
    float   spin;       /* radians, advanced each tick for the spin animation */
} CraftDrop;

extern CraftDrop craft_drops[CRAFT_MAX_DROPS];

void craft_drops_init(void);

/* Spawn a drop of `blk` at world `pos`. Silently no-ops if the pool is
 * full — fail-soft so a creeper blast or skeleton dying mid-frame
 * doesn't crash. */
void craft_drops_spawn(BlockId blk, Vec3 pos);

/* Advance ages, collect drops near the player, and despawn expired
 * ones. Call once per game-step from the platform. */
void craft_drops_tick(float dt, CraftPlayer *p);

/* Render all live drops as small spinning cubes. */
void craft_drops_render(const CraftCamera *cam, uint16_t *fb);

#endif

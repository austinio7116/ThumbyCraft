/*
 * ThumbyCraft — particle system (Phase 15: break particles).
 *
 * Tiny ballistic dust puffs spawned when blocks break, sampled in
 * colour from the broken block's texture. Each particle is a 1-2px
 * sprite with gravity + fade-out. Z-tested against craft_zbuf so
 * particles don't punch through walls.
 *
 * Capacity is small (32) — typical break emits 8 particles, so we
 * can have a few breaks overlapping without recycling artifacts.
 */
#ifndef CRAFT_PARTICLES_H
#define CRAFT_PARTICLES_H

#include "craft_types.h"
#include "craft_render.h"
#include "craft_blocks.h"

void craft_particles_init(void);

/* Emit a burst of particles from world-space `centre` coloured by
 * sampling the broken block's side texture. Call right after the
 * block is set to BLK_AIR. */
void craft_particles_emit_break(Vec3 centre, BlockId broken);

/* Big radial fireball — used by creeper detonation. ~24 particles
 * with extended lifetime (0.9-1.5 s) launched outward in fire colors. */
void craft_particles_emit_explosion(Vec3 centre);

/* Small rising flame puff — 1-2 particles per call, no gravity, ~0.4 s
 * lifetime. Call once per frame from any continuously-burning source. */
void craft_particles_emit_flame(Vec3 centre);

/* Advance physics + age by dt seconds. */
void craft_particles_tick(float dt);

/* Render all live particles. Z-tested against craft_zbuf. */
void craft_particles_render(const CraftCamera *cam, uint16_t *fb);

#endif

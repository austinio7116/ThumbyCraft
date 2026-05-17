/*
 * ThumbyCraft — passive mobs (Phase 27).
 *
 * Up to CRAFT_MAX_MOBS billboarded sprites with simple wandering AI.
 * Three types ship in v1: sheep, pig, chicken. All walk a slow random
 * walk on the ground, snap to terrain via gravity, and don't collide
 * with each other (overkill for this scale).
 *
 * Rendering is a billboard sprite pass that runs once per frame after
 * the world raycaster. Z-test against craft_zbuf so mobs occlude
 * correctly behind trees and hills.
 */
#ifndef CRAFT_MOBS_H
#define CRAFT_MOBS_H

#include "craft_types.h"
#include "craft_render.h"
#include "craft_player.h"

typedef enum {
    MOB_SHEEP = 0,
    MOB_PIG,
    MOB_CHICKEN,
    MOB_SLIME,       /* hostile — chases and contact-damages the player */
    MOB_TYPE_COUNT
} MobType;

typedef struct {
    bool    alive;
    MobType type;
    Vec3    pos;          /* feet position (bottom of mob) */
    float   yaw;
    Vec3    vel;
    float   ai_timer;     /* sec until next decision */
    int     hp;           /* set on spawn from mob type table */
    float   hurt_flash;   /* sec — non-zero shows red tint */
} CraftMob;

/* Damage a mob by `amt`. If hp drops to 0, mob dies (alive=false) and
 * the function returns true. */
bool craft_mob_damage(int mob_index, int amt);

/* Project the player's pick ray and find the closest live mob hit
 * within max_dist. Returns mob index or -1. */
int  craft_mobs_pick(const CraftCamera *cam, float max_dist);

/* Update day/night spawn timer. When night, spawn slimes at low rate
 * until cap. Day, gradually despawn slimes. Pass current sun_y from
 * craft_render_sun_y. */
void craft_mobs_day_night_tick(float dt, float sun_y, CraftPlayer *p);

#define CRAFT_MAX_MOBS 6

extern CraftMob craft_mobs[CRAFT_MAX_MOBS];

/* Build the sprite atlas (call once at startup). */
void craft_mobs_build_sprites(void);

/* Spawn fresh mobs around the given centre. Called when a new world
 * is generated or loaded. Clears existing mobs first. */
void craft_mobs_spawn_around(Vec3 centre, uint32_t seed);

/* Advance AI + physics by dt seconds. Pass the player pointer so
 * hostile mobs can chase and call craft_player_take_damage on contact. */
void craft_mobs_tick(float dt, CraftPlayer *p);

/* Spawn n hostile mobs (slimes) at random valid ground tiles near
 * the player. Called when entering survival mode. */
void craft_mobs_spawn_hostile(CraftPlayer *p, int n);

/* Draw all alive mobs onto fb, z-tested against craft_zbuf. */
void craft_mobs_render(const CraftCamera *cam, uint16_t *fb);

#endif

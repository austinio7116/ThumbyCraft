/*
 * ThumbyCraft — passive mobs (Phase 27, 3D rebuild).
 *
 * Each mob is composed of several axis-aligned cuboid parts (body,
 * head, legs, snout, etc) defined in mob-local space. At render time
 * we project the mob's world AABB to find the screen bounding box,
 * then per-pixel cast a ray into the mob's local frame and test
 * against each cuboid using slab intersection. The nearest hit
 * defines the colour + face for shading.
 *
 * This integrates with the existing raycaster's z-buffer so mobs
 * occlude correctly against blocks AND each other — no billboards,
 * no facing-camera cheats. The mob rotates around its yaw axis with
 * proper depth on every side, which is what "blocky 3D" means.
 *
 * Cost: only the pixels inside each mob's screen bbox are walked.
 * For ~6 mobs at typical distances that's a few hundred extra pixel
 * rays per frame, maybe 5% of one core.
 */
#include "craft_mobs.h"
#include "craft_world.h"
#include "craft_blocks.h"
#include "craft_player.h"

#include <string.h>

CraftMob craft_mobs[CRAFT_MAX_MOBS];

/* --- Model description ------------------------------------------- */

typedef struct {
    float    cx, cy, cz;       /* part centre in local frame */
    float    hx, hy, hz;       /* half-sizes */
    uint16_t color;
} CuboidPart;

#define MAX_PARTS 8
typedef struct {
    int             n_parts;
    CuboidPart      parts[MAX_PARTS];
    float           radius;    /* horizontal yaw-invariant radius */
    float           height;    /* world height (feet to top) */
} MobModel;

static MobModel s_models[MOB_TYPE_COUNT];

static const float mob_speed[MOB_TYPE_COUNT] = {
    1.0f,    /* sheep */
    1.3f,    /* pig   */
    1.6f,    /* chicken */
    2.2f,    /* slime */
};
static const int mob_hp_table[MOB_TYPE_COUNT] = {
    3, 3, 1, 2,
};
/* Aggro range — slime starts chasing when player within this distance. */
#define SLIME_AGGRO_DIST   12.0f
#define SLIME_CONTACT_DIST 0.9f
#define DAY_NIGHT_CAP      6      /* max alive hostile mobs */
#define NIGHT_SPAWN_GAP    5.0f   /* sec between spawn attempts at night */

static float s_day_night_t;

/* --- RNG ---------------------------------------------------------- */
static uint32_t s_rng = 0xC0FFEE;
static uint32_t xs(void) {
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}
static float frand(void) { return (xs() & 0xFFFF) / 65535.0f; }

/* --- Build the cuboid models ------------------------------------- */
/* Models look "down +Z" in local space (so head is at +Z). The mob's
 * yaw rotates the model around the Y axis into world space at render
 * time. All centres + sizes are in world-unit fractions; legs sit on
 * the ground plane (local y=0 is feet level). */

void craft_mobs_build_sprites(void) {
    /* SHEEP — cream body, grey legs. */
    MobModel *m = &s_models[MOB_SHEEP];
    uint16_t WOOL = rgb565(240, 235, 220);
    uint16_t LEG  = rgb565(110, 100, 90);
    m->parts[0] = (CuboidPart){  0.00f, 0.55f,  0.00f,  0.32f, 0.28f, 0.50f, WOOL };  /* body */
    m->parts[1] = (CuboidPart){  0.00f, 0.65f,  0.55f,  0.22f, 0.22f, 0.22f, WOOL };  /* head */
    m->parts[2] = (CuboidPart){ -0.20f, 0.13f,  0.28f,  0.08f, 0.13f, 0.08f, LEG  };  /* FL leg */
    m->parts[3] = (CuboidPart){  0.20f, 0.13f,  0.28f,  0.08f, 0.13f, 0.08f, LEG  };  /* FR leg */
    m->parts[4] = (CuboidPart){ -0.20f, 0.13f, -0.28f,  0.08f, 0.13f, 0.08f, LEG  };  /* BL leg */
    m->parts[5] = (CuboidPart){  0.20f, 0.13f, -0.28f,  0.08f, 0.13f, 0.08f, LEG  };  /* BR leg */
    m->n_parts = 6;
    m->radius  = 0.78f;
    m->height  = 1.25f;

    /* PIG — pink body, darker snout, pink legs. */
    m = &s_models[MOB_PIG];
    uint16_t PIG  = rgb565(240, 160, 180);
    uint16_t SNOUT= rgb565(200, 110, 140);
    m->parts[0] = (CuboidPart){  0.00f, 0.45f,  0.00f,  0.28f, 0.22f, 0.45f, PIG   };  /* body */
    m->parts[1] = (CuboidPart){  0.00f, 0.48f,  0.50f,  0.20f, 0.18f, 0.15f, PIG   };  /* head */
    m->parts[2] = (CuboidPart){  0.00f, 0.42f,  0.66f,  0.10f, 0.08f, 0.05f, SNOUT };  /* snout */
    m->parts[3] = (CuboidPart){ -0.18f, 0.12f,  0.25f,  0.08f, 0.12f, 0.08f, PIG   };
    m->parts[4] = (CuboidPart){  0.18f, 0.12f,  0.25f,  0.08f, 0.12f, 0.08f, PIG   };
    m->parts[5] = (CuboidPart){ -0.18f, 0.12f, -0.25f,  0.08f, 0.12f, 0.08f, PIG   };
    m->parts[6] = (CuboidPart){  0.18f, 0.12f, -0.25f,  0.08f, 0.12f, 0.08f, PIG   };
    m->n_parts = 7;
    m->radius  = 0.62f;
    m->height  = 1.0f;

    /* CHICKEN — small white body, red comb, yellow beak + legs. */
    m = &s_models[MOB_CHICKEN];
    uint16_t WHT  = rgb565(240, 240, 240);
    uint16_t YEL  = rgb565(240, 220, 60);
    uint16_t RED  = rgb565(220, 50, 50);
    m->parts[0] = (CuboidPart){  0.00f, 0.25f,  0.00f,  0.14f, 0.18f, 0.18f, WHT };
    m->parts[1] = (CuboidPart){  0.00f, 0.48f,  0.10f,  0.11f, 0.10f, 0.10f, WHT };
    m->parts[2] = (CuboidPart){  0.00f, 0.46f,  0.22f,  0.04f, 0.03f, 0.05f, YEL };
    m->parts[3] = (CuboidPart){  0.00f, 0.59f,  0.06f,  0.05f, 0.04f, 0.06f, RED };
    m->parts[4] = (CuboidPart){ -0.07f, 0.07f,  0.00f,  0.04f, 0.07f, 0.04f, YEL };
    m->parts[5] = (CuboidPart){  0.07f, 0.07f,  0.00f,  0.04f, 0.07f, 0.04f, YEL };
    m->n_parts = 6;
    m->radius  = 0.32f;
    m->height  = 0.70f;

    /* SLIME — single bright-green chunky cube + dark spots for eyes. */
    m = &s_models[MOB_SLIME];
    uint16_t GREEN = rgb565(80, 220, 70);
    uint16_t DARK  = rgb565(30,  60, 30);
    m->parts[0] = (CuboidPart){  0.00f, 0.35f,  0.00f,  0.35f, 0.35f, 0.35f, GREEN };  /* body */
    m->parts[1] = (CuboidPart){ -0.15f, 0.45f,  0.34f,  0.05f, 0.04f, 0.02f, DARK  };  /* L eye */
    m->parts[2] = (CuboidPart){  0.15f, 0.45f,  0.34f,  0.05f, 0.04f, 0.02f, DARK  };  /* R eye */
    m->parts[3] = (CuboidPart){  0.00f, 0.20f,  0.34f,  0.18f, 0.02f, 0.02f, DARK  };  /* mouth */
    m->n_parts = 4;
    m->radius  = 0.55f;
    m->height  = 0.70f;
}

static bool mob_is_hostile(MobType t) { return t == MOB_SLIME; }

/* --- Find a grass/sand/dirt block under (x, z) ------------------- */
static int find_ground(int x, int z) {
    /* World is infinite — bound check is now only Y, handled by
     * craft_world_get returning BLK_AIR for out-of-window coords. */
    for (int y = CRAFT_WORLD_Y - 2; y > 0; y--) {
        BlockId b = craft_world_get(x, y, z);
        if (b == BLK_GRASS || b == BLK_SAND || b == BLK_DIRT) return y;
    }
    return -1;
}

void craft_mobs_spawn_around(Vec3 centre, uint32_t seed) {
    s_rng ^= seed;
    for (int i = 0; i < CRAFT_MAX_MOBS; i++) craft_mobs[i].alive = false;
    int placed = 0;
    for (int tries = 0; tries < 60 && placed < CRAFT_MAX_MOBS; tries++) {
        int dx = (int)(xs() & 0x1F) - 16;
        int dz = (int)(xs() & 0x1F) - 16;
        int x  = (int)centre.x + dx;
        int z  = (int)centre.z + dz;
        int y  = find_ground(x, z);
        if (y < 0) continue;
        CraftMob *m = &craft_mobs[placed];
        m->alive    = true;
        m->type     = (MobType)(xs() % MOB_SLIME);
        m->pos      = v3((float)x + 0.5f, (float)(y + 1), (float)z + 0.5f);
        m->yaw      = frand() * 6.2831853f;
        m->vel      = v3(0, 0, 0);
        m->ai_timer = 0.5f + frand() * 2.0f;
        m->hp       = mob_hp_table[m->type];
        m->hurt_flash = 0.0f;
        placed++;
    }
    s_day_night_t = 0.0f;
}


void craft_mobs_spawn_hostile(CraftPlayer *p, int n) {
    int placed = 0;
    for (int tries = 0; tries < 80 && placed < n; tries++) {
        /* Spawn beyond visible range so they emerge instead of popping
         * in. 12-20 blocks from the player. */
        float angle = frand() * 6.2831853f;
        float dist  = 12.0f + frand() * 8.0f;
        int x = (int)(p->cam.pos.x + cosf(angle) * dist);
        int z = (int)(p->cam.pos.z + sinf(angle) * dist);
        int y = find_ground(x, z);
        if (y < 0) continue;
        /* Find a free slot. */
        for (int i = 0; i < CRAFT_MAX_MOBS; i++) {
            if (craft_mobs[i].alive) continue;
            CraftMob *m = &craft_mobs[i];
            m->alive    = true;
            m->type     = MOB_SLIME;
            m->pos      = v3((float)x + 0.5f, (float)(y + 1), (float)z + 0.5f);
            m->yaw      = frand() * 6.2831853f;
            m->vel      = v3(0, 0, 0);
            m->ai_timer = 0.5f;
            m->hp       = mob_hp_table[MOB_SLIME];
            m->hurt_flash = 0.0f;
            placed++;
            break;
        }
    }
}

bool craft_mob_damage(int mob_index, int amt) {
    if (mob_index < 0 || mob_index >= CRAFT_MAX_MOBS) return false;
    CraftMob *m = &craft_mobs[mob_index];
    if (!m->alive) return false;
    m->hp -= amt;
    m->hurt_flash = 0.25f;
    /* Knock-back away from current heading. */
    m->vel.y = 4.0f;
    if (m->hp <= 0) {
        m->alive = false;
        return true;
    }
    return false;
}

/* Ray-vs-mob picking. Returns index of closest hit mob within max_dist
 * or -1 if none. Uses the same yaw-invariant AABB the renderer uses. */
int craft_mobs_pick(const CraftCamera *cam, float max_dist) {
    float cy = cosf(cam->yaw),  sye = sinf(cam->yaw);
    float cp = cosf(cam->pitch), sp = sinf(cam->pitch);
    float dx = sye * cp, dy = sp, dz = cy * cp;

    int best = -1;
    float best_t = max_dist;
    for (int i = 0; i < CRAFT_MAX_MOBS; i++) {
        CraftMob *m = &craft_mobs[i];
        if (!m->alive) continue;
        const MobModel *model = &s_models[m->type];
        float bminx = m->pos.x - model->radius;
        float bmaxx = m->pos.x + model->radius;
        float bminy = m->pos.y;
        float bmaxy = m->pos.y + model->height;
        float bminz = m->pos.z - model->radius;
        float bmaxz = m->pos.z + model->radius;
        /* Slab intersect against world-space AABB. */
        float t_near = 0.0f, t_far = best_t;
        float inv;
        bool fail = false;
#define SLAB(o, d, mn, mx)                                          \
        do {                                                        \
            if (d > -1e-6f && d < 1e-6f) {                          \
                if (o < mn || o > mx) { fail = true; break; }       \
            } else {                                                \
                inv = 1.0f / d;                                     \
                float t1 = (mn - o) * inv;                          \
                float t2 = (mx - o) * inv;                          \
                if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; } \
                if (t1 > t_near) t_near = t1;                       \
                if (t2 < t_far)  t_far  = t2;                       \
                if (t_near > t_far) { fail = true; break; }         \
            }                                                       \
        } while (0)
        SLAB(cam->pos.x, dx, bminx, bmaxx);
        if (!fail) SLAB(cam->pos.y, dy, bminy, bmaxy);
        if (!fail) SLAB(cam->pos.z, dz, bminz, bmaxz);
#undef SLAB
        if (fail) continue;
        if (t_near < 0.0f) continue;
        if (t_near < best_t) {
            best_t = t_near;
            best = i;
        }
    }
    return best;
}

void craft_mobs_day_night_tick(float dt, float sun_y, CraftPlayer *p) {
    if (p->mode != CRAFT_MODE_SURVIVAL) return;
    s_day_night_t += dt;
    /* Night = sun below horizon by more than a touch. */
    bool night = sun_y < -0.10f;
    if (night) {
        if (s_day_night_t < NIGHT_SPAWN_GAP) return;
        s_day_night_t = 0.0f;
        /* Count live hostiles. */
        int alive_h = 0;
        for (int i = 0; i < CRAFT_MAX_MOBS; i++)
            if (craft_mobs[i].alive && craft_mobs[i].type == MOB_SLIME) alive_h++;
        if (alive_h < DAY_NIGHT_CAP) craft_mobs_spawn_hostile(p, 1);
    } else if (sun_y > 0.15f) {
        /* Daylight despawn — pop one slime per tick at low chance. */
        if (s_day_night_t < 8.0f) return;
        s_day_night_t = 0.0f;
        for (int i = 0; i < CRAFT_MAX_MOBS; i++) {
            if (craft_mobs[i].alive && craft_mobs[i].type == MOB_SLIME) {
                craft_mobs[i].alive = false;
                break;
            }
        }
    }
}

/* --- AI ----------------------------------------------------------- */
static void ai_decide(CraftMob *m) {
    float roll = frand();
    if (roll < 0.35f) {
        m->vel = v3(0, m->vel.y, 0);
        m->ai_timer = 1.5f + frand() * 2.5f;
    } else if (roll < 0.55f) {
        m->yaw += (frand() - 0.5f) * 2.0f;
        m->vel = v3(0, m->vel.y, 0);
        m->ai_timer = 0.5f + frand() * 1.0f;
    } else {
        m->yaw += (frand() - 0.5f) * 1.2f;
        float spd = mob_speed[m->type];
        m->vel.x = sinf(m->yaw) * spd;
        m->vel.z = cosf(m->yaw) * spd;
        m->ai_timer = 1.5f + frand() * 3.0f;
    }
}

/* Slime AI override: chase player when in range, contact-damage on
 * proximity. Falls back to wandering when player is far. */
static void slime_ai(CraftMob *m, CraftPlayer *p, float dt) {
    (void)dt;
    float dx = p->cam.pos.x - m->pos.x;
    float dz = p->cam.pos.z - m->pos.z;
    float dist = sqrtf(dx * dx + dz * dz);
    if (dist > SLIME_AGGRO_DIST) {
        /* Out of range — wander like a passive mob. */
        if (m->ai_timer <= 0.0f) ai_decide(m);
        return;
    }
    /* In range — chase. */
    if (dist > 0.001f) {
        m->yaw = atan2f(dx, dz);
        float spd = mob_speed[MOB_SLIME];
        m->vel.x = (dx / dist) * spd;
        m->vel.z = (dz / dist) * spd;
        m->ai_timer = 0.5f;
    }
    /* Contact damage. */
    if (dist < SLIME_CONTACT_DIST) {
        craft_player_take_damage(p, 1);
    }
}

static bool foot_solid(float fx, float fy, float fz) {
    int bx = (int)floorf(fx);
    int by = (int)floorf(fy - 0.05f);
    int bz = (int)floorf(fz);
    return craft_block_solid(craft_world_get(bx, by, bz));
}
static bool ahead_solid(float fx, float fy, float fz) {
    int bx = (int)floorf(fx);
    int by = (int)floorf(fy + 0.4f);
    int bz = (int)floorf(fz);
    return craft_block_solid(craft_world_get(bx, by, bz));
}

void craft_mobs_tick(float dt, CraftPlayer *p) {
    if (dt > 0.1f) dt = 0.1f;
    for (int i = 0; i < CRAFT_MAX_MOBS; i++) {
        CraftMob *m = &craft_mobs[i];
        if (!m->alive) continue;
        if (m->hurt_flash > 0.0f) m->hurt_flash -= dt;
        m->ai_timer -= dt;
        if (mob_is_hostile(m->type)) {
            slime_ai(m, p, dt);
        } else if (m->ai_timer <= 0.0f) {
            ai_decide(m);
        }

        m->vel.y -= 22.0f * dt;
        if (m->vel.y < -16.0f) m->vel.y = -16.0f;

        float nx = m->pos.x + m->vel.x * dt;
        float nz = m->pos.z + m->vel.z * dt;

        /* Despawn when too far from the player (infinite world). */
        float dxp = nx - p->cam.pos.x;
        float dzp = nz - p->cam.pos.z;
        if (dxp * dxp + dzp * dzp > 60.0f * 60.0f) {
            m->alive = false;
            continue;
        }

        if (ahead_solid(nx, m->pos.y, nz)) {
            m->vel.x = 0; m->vel.z = 0;
            m->ai_timer = 0.0f;
        } else {
            m->pos.x = nx; m->pos.z = nz;
        }

        m->pos.y += m->vel.y * dt;
        if (m->pos.y < 1.0f) m->pos.y = 1.0f;
        if (foot_solid(m->pos.x, m->pos.y, m->pos.z)) {
            float gy = floorf(m->pos.y - 0.05f) + 1.0f;
            if (m->pos.y < gy) m->pos.y = gy;
            m->vel.y = 0;
        }
    }
}

/* --- 3D rendering -------------------------------------------------- */

/* Ray vs AABB slab intersection. Returns true if the ray hits the box
 * at positive t. t_out is the entry t, face_out identifies which face:
 *   0 = +X (east), 1 = -X (west),
 *   2 = +Y (top),  3 = -Y (bottom),
 *   4 = +Z (front), 5 = -Z (back).
 */
static inline bool ray_aabb(float ox, float oy, float oz,
                            float dx, float dy, float dz,
                            float bminx, float bminy, float bminz,
                            float bmaxx, float bmaxy, float bmaxz,
                            float *t_out, int *face_out) {
    float t_near = -1e30f, t_far = 1e30f;
    int   nf = -1;

    /* X slab */
    if (dx > -1e-6f && dx < 1e-6f) {
        if (ox < bminx || ox > bmaxx) return false;
    } else {
        float inv = 1.0f / dx;
        float t1  = (bminx - ox) * inv;
        float t2  = (bmaxx - ox) * inv;
        int near_face = (dx > 0) ? 1 : 0;
        if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; near_face ^= 1; }
        if (t1 > t_near) { t_near = t1; nf = near_face; }
        if (t2 < t_far)  t_far  = t2;
        if (t_near > t_far) return false;
    }
    /* Y slab */
    if (dy > -1e-6f && dy < 1e-6f) {
        if (oy < bminy || oy > bmaxy) return false;
    } else {
        float inv = 1.0f / dy;
        float t1  = (bminy - oy) * inv;
        float t2  = (bmaxy - oy) * inv;
        int near_face = (dy > 0) ? 3 : 2;
        if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; near_face ^= 1; }
        if (t1 > t_near) { t_near = t1; nf = near_face; }
        if (t2 < t_far)  t_far  = t2;
        if (t_near > t_far) return false;
    }
    /* Z slab */
    if (dz > -1e-6f && dz < 1e-6f) {
        if (oz < bminz || oz > bmaxz) return false;
    } else {
        float inv = 1.0f / dz;
        float t1  = (bminz - oz) * inv;
        float t2  = (bmaxz - oz) * inv;
        int near_face = (dz > 0) ? 5 : 4;
        if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; near_face ^= 1; }
        if (t1 > t_near) { t_near = t1; nf = near_face; }
        if (t2 < t_far)  t_far  = t2;
        if (t_near > t_far) return false;
    }
    if (t_near < 0.0f) return false;
    *t_out = t_near;
    *face_out = nf;
    return true;
}

/* Face shading — analog to block face shade. */
static const uint16_t mob_face_shade[6] = {
    220, 220, 256, 150, 200, 170
};

static inline uint16_t shade565(uint16_t c, int m) {
    int r = ((c >> 11) & 0x1F) * m >> 8;
    int g = ((c >>  5) & 0x3F) * m >> 8;
    int b = ( c        & 0x1F) * m >> 8;
    if (r > 31) r = 31;
    if (g > 63) g = 63;
    if (b > 31) b = 31;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

void craft_mobs_render(const CraftCamera *cam, uint16_t *fb) {
    float cy_c = cosf(cam->yaw),  sy_c = sinf(cam->yaw);
    float cp_c = cosf(cam->pitch), sp_c = sinf(cam->pitch);
    Vec3 fwd   = v3(sy_c * cp_c, sp_c, cy_c * cp_c);
    Vec3 right = v3(cy_c, 0.0f, -sy_c);
    Vec3 up    = v3(
        fwd.y * right.z - fwd.z * right.y,
        fwd.z * right.x - fwd.x * right.z,
        fwd.x * right.y - fwd.y * right.x);
    float tan_h  = tanf(cam->fov * 0.5f);
    float focal_h = (CRAFT_FB_W * 0.5f) / tan_h;
    float focal_v = (CRAFT_FB_H * 0.5f) / tan_h;

    for (int i = 0; i < CRAFT_MAX_MOBS; i++) {
        CraftMob *m = &craft_mobs[i];
        if (!m->alive) continue;
        const MobModel *model = &s_models[m->type];

        /* Yaw-invariant world AABB for screen-bbox computation. */
        float wbmin_x = m->pos.x - model->radius;
        float wbmin_y = m->pos.y;
        float wbmin_z = m->pos.z - model->radius;
        float wbmax_x = m->pos.x + model->radius;
        float wbmax_y = m->pos.y + model->height;
        float wbmax_z = m->pos.z + model->radius;

        /* Project 8 AABB corners → screen bbox. */
        int sx_min = CRAFT_FB_W, sx_max = -1;
        int sy_min = CRAFT_FB_H, sy_max = -1;
        bool any_in_front = false;
        for (int corner = 0; corner < 8; corner++) {
            float cx = (corner & 1) ? wbmax_x : wbmin_x;
            float cyw = (corner & 2) ? wbmax_y : wbmin_y;
            float cz = (corner & 4) ? wbmax_z : wbmin_z;
            float rx = cx - cam->pos.x;
            float ry = cyw - cam->pos.y;
            float rz = cz - cam->pos.z;
            float zf = rx * fwd.x + ry * fwd.y + rz * fwd.z;
            if (zf <= 0.05f) continue;
            any_in_front = true;
            float xs = (rx * right.x + ry * right.y + rz * right.z) / zf;
            float ys = (rx * up.x    + ry * up.y    + rz * up.z   ) / zf;
            int   sx = (int)(CRAFT_FB_W * 0.5f + xs * focal_h);
            int   sy = (int)(CRAFT_FB_H * 0.5f - ys * focal_v);
            if (sx < sx_min) sx_min = sx;
            if (sx > sx_max) sx_max = sx;
            if (sy < sy_min) sy_min = sy;
            if (sy > sy_max) sy_max = sy;
        }
        if (!any_in_front) continue;
        /* Clip + expand by 1 px for safety. */
        sx_min--; sy_min--; sx_max++; sy_max++;
        if (sx_min < 0)            sx_min = 0;
        if (sy_min < 0)            sy_min = 0;
        if (sx_max >= CRAFT_FB_W)  sx_max = CRAFT_FB_W - 1;
        if (sy_max >= CRAFT_FB_H)  sy_max = CRAFT_FB_H - 1;
        if (sx_min > sx_max || sy_min > sy_max) continue;

        /* Pre-transform camera position into mob-local frame. The mob
         * faces +Z in local space; world yaw rotates that around Y.
         * Inverse rotation: -yaw around Y. */
        float my_c = cosf(-m->yaw), my_s = sinf(-m->yaw);
        float rel_x = cam->pos.x - m->pos.x;
        float rel_y = cam->pos.y - m->pos.y;
        float rel_z = cam->pos.z - m->pos.z;
        float lo_x  = rel_x * my_c - rel_z * my_s;
        float lo_y  = rel_y;
        float lo_z  = rel_x * my_s + rel_z * my_c;

        /* Iterate screen-bbox pixels. */
        for (int sy = sy_min; sy <= sy_max; sy++) {
            float ndc_y = -((float)(sy * 2 - CRAFT_FB_H + 1) / (float)CRAFT_FB_H);
            float vy    = ndc_y * tan_h;
            for (int sx = sx_min; sx <= sx_max; sx++) {
                float ndc_x = ((float)(sx * 2 - CRAFT_FB_W + 1) / (float)CRAFT_FB_W);
                float vx    = ndc_x * tan_h;

                /* World ray dir. */
                float wdx = fwd.x + right.x * vx + up.x * vy;
                float wdy = fwd.y + right.y * vx + up.y * vy;
                float wdz = fwd.z + right.z * vx + up.z * vy;
                /* Rotate into local frame. */
                float ldx = wdx * my_c - wdz * my_s;
                float ldy = wdy;
                float ldz = wdx * my_s + wdz * my_c;

                /* Test against every cuboid part — keep nearest hit. */
                float best_t = 1e30f;
                int   best_face = 0;
                uint16_t best_color = 0;
                for (int p = 0; p < model->n_parts; p++) {
                    const CuboidPart *part = &model->parts[p];
                    float bminx = part->cx - part->hx;
                    float bmaxx = part->cx + part->hx;
                    float bminy = part->cy - part->hy;
                    float bmaxy = part->cy + part->hy;
                    float bminz = part->cz - part->hz;
                    float bmaxz = part->cz + part->hz;
                    float t; int face;
                    if (ray_aabb(lo_x, lo_y, lo_z, ldx, ldy, ldz,
                                 bminx, bminy, bminz,
                                 bmaxx, bmaxy, bmaxz, &t, &face)) {
                        if (t < best_t) {
                            best_t = t;
                            best_face = face;
                            best_color = part->color;
                        }
                    }
                }
                if (best_t >= 1e29f) continue;

                /* Hurt-flash tint — momentary red wash. */
                if (m->hurt_flash > 0.0f) {
                    int r = ((best_color >> 11) & 0x1F);
                    int g = ((best_color >>  5) & 0x3F);
                    int b = ( best_color        & 0x1F);
                    r = (r + 31) / 2;
                    g = g / 3;
                    b = b / 3;
                    best_color = (uint16_t)((r << 11) | (g << 5) | b);
                }

                /* World distance = t * |world_dir| (rotation preserves
                 * length, so |local_dir| = |world_dir|). */
                float dl2 = wdx*wdx + wdy*wdy + wdz*wdz;
                float dl  = (dl2 > 1.0001f) ? sqrtf(dl2) : 1.0f;
                float world_dist = best_t * dl;
                int q = (int)(world_dist * 255.0f / CRAFT_MAX_DIST_FOR_ZBUF);
                if (q < 0)   q = 0;
                if (q > 254) q = 254;
                int idx = sy * CRAFT_FB_W + sx;
                if (craft_zbuf[idx] > (uint8_t)q) {
                    fb[idx] = shade565(best_color, mob_face_shade[best_face]);
                    craft_zbuf[idx] = (uint8_t)q;
                }
            }
        }
    }
}

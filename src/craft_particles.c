/*
 * ThumbyCraft — break-block dust particles.
 *
 * Particle pool is fixed-size. Each emit_break walks the pool for
 * empty slots and writes up to 8 particles with randomised launch
 * velocity. Particles age over ~1.2 s with gravity dragging them
 * down; colour comes from random samples of the broken block's
 * side texture so a stone break looks grey, grass green, etc.
 */
#include "craft_particles.h"
#include <string.h>

/* Pool bumped to 48 — enough for an explosion (24) + 2 block-breaks
 * (16) + a couple of burn flames overlapping. Not 64; the extra 16
 * slots would push past the SRAM ceiling once mob model parts and
 * the chunk store buffer also expanded. */
#define MAX_PARTICLES 48
#define PARTICLE_LIFE 1.2f
#define PARTICLES_PER_BREAK 8
/* Explosion burst — big radial cloud of fire/smoke debris. */
#define PARTICLES_PER_EXPLOSION 24

typedef struct {
    bool   alive;
    Vec3   pos;
    Vec3   vel;
    uint16_t color;
    float  age;
    float  life;       /* override per-particle when ≠ 0; 0 = default PARTICLE_LIFE */
    uint8_t gravity;   /* 0 = no gravity (flames rise), 1 = full gravity (dust falls) */
} Particle;

static Particle s_pool[MAX_PARTICLES];
static uint32_t s_rng = 0xBEEF1234u;

static uint32_t xs(void) {
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}
static float frand(void) { return (xs() & 0xFFFF) / 65535.0f; }

void craft_particles_init(void) {
    memset(s_pool, 0, sizeof s_pool);
}

void craft_particles_emit_break(Vec3 centre, BlockId broken) {
    /* Side texture as colour source. Reuse the same atlas we draw
     * blocks from — guarantees the dust matches the block visually. */
    const uint16_t *tex = craft_block_texture(broken, FACE_PZ);

    int spawned = 0;
    for (int i = 0; i < MAX_PARTICLES && spawned < PARTICLES_PER_BREAK; i++) {
        if (s_pool[i].alive) continue;
        Particle *p = &s_pool[i];
        p->alive = true;
        p->age   = 0.0f;
        p->life  = 0.0f;    /* default lifetime */
        p->gravity = 1;
        p->pos.x = centre.x + (frand() - 0.5f) * 0.4f;
        p->pos.y = centre.y + (frand() - 0.5f) * 0.4f;
        p->pos.z = centre.z + (frand() - 0.5f) * 0.4f;
        /* Launch outward + upward. */
        p->vel.x = (frand() - 0.5f) * 3.0f;
        p->vel.y =  frand() * 3.0f + 0.5f;
        p->vel.z = (frand() - 0.5f) * 3.0f;
        int idx = (int)(xs() & (CRAFT_TEX_PIXELS - 1));
        p->color = tex[idx];
        spawned++;
    }
}

/* Big radial fireball — orange/red/grey palette, longer lifetime,
 * higher launch velocity. Used by creeper detonation. */
void craft_particles_emit_explosion(Vec3 centre) {
    static const uint16_t PAL[6] = {
        /* bright core → cooler smoke */
        0xFD60,  /* bright yellow */
        0xFB00,  /* deep orange */
        0xD8E0,  /* red-orange */
        0xA000,  /* dark red */
        0x4208,  /* dim grey */
        0x2104,  /* dark grey */
    };
    int spawned = 0;
    for (int i = 0; i < MAX_PARTICLES && spawned < PARTICLES_PER_EXPLOSION; i++) {
        if (s_pool[i].alive) continue;
        Particle *p = &s_pool[i];
        p->alive   = true;
        p->age     = 0.0f;
        p->life    = 0.9f + frand() * 0.6f;   /* 0.9 - 1.5 s */
        p->gravity = (xs() & 3) == 0 ? 1 : 0; /* most rise; ~25 % fall */
        p->pos.x = centre.x + (frand() - 0.5f) * 0.3f;
        p->pos.y = centre.y + (frand() - 0.5f) * 0.3f;
        p->pos.z = centre.z + (frand() - 0.5f) * 0.3f;
        /* Radial launch — much faster than dust. */
        float vx = (frand() - 0.5f) * 8.0f;
        float vy =  frand() * 6.0f + 1.0f;
        float vz = (frand() - 0.5f) * 8.0f;
        p->vel.x = vx; p->vel.y = vy; p->vel.z = vz;
        p->color = PAL[xs() % 6];
        spawned++;
    }
}

/* Small flame puff — yellow/orange, no gravity, short lifetime. Emit
 * 1-2 per call from a burning mob each frame. */
void craft_particles_emit_flame(Vec3 centre) {
    static const uint16_t PAL[4] = {
        0xFFE0,  /* near-white yellow tip */
        0xFD20,  /* bright orange */
        0xFA00,  /* deep orange */
        0xC800,  /* red base */
    };
    int spawned = 0;
    for (int i = 0; i < MAX_PARTICLES && spawned < 2; i++) {
        if (s_pool[i].alive) continue;
        Particle *p = &s_pool[i];
        p->alive   = true;
        p->age     = 0.0f;
        p->life    = 0.35f + frand() * 0.25f;
        p->gravity = 0;       /* flames rise */
        p->pos.x = centre.x + (frand() - 0.5f) * 0.30f;
        p->pos.y = centre.y + frand() * 0.20f;
        p->pos.z = centre.z + (frand() - 0.5f) * 0.30f;
        p->vel.x = (frand() - 0.5f) * 0.6f;
        p->vel.y =  frand() * 1.2f + 0.8f;
        p->vel.z = (frand() - 0.5f) * 0.6f;
        p->color = PAL[xs() & 3];
        spawned++;
    }
}

void craft_particles_tick(float dt) {
    if (dt > 0.1f) dt = 0.1f;
    for (int i = 0; i < MAX_PARTICLES; i++) {
        Particle *p = &s_pool[i];
        if (!p->alive) continue;
        p->age += dt;
        float lifetime = p->life > 0.0f ? p->life : PARTICLE_LIFE;
        if (p->age >= lifetime) { p->alive = false; continue; }
        if (p->gravity) p->vel.y -= 14.0f * dt;
        else            p->vel.y += 0.6f * dt;   /* slight buoyancy for flames */
        p->pos.x += p->vel.x * dt;
        p->pos.y += p->vel.y * dt;
        p->pos.z += p->vel.z * dt;
    }
}

void craft_particles_render(const CraftCamera *cam, uint16_t *fb) {
    for (int i = 0; i < MAX_PARTICLES; i++) {
        Particle *p = &s_pool[i];
        if (!p->alive) continue;
        int sx, sy;
        uint8_t depth;
        float dist;
        if (!craft_render_project(cam, p->pos, &sx, &sy, &depth, &dist)) continue;
        if (dist > CRAFT_MAX_DIST_FOR_ZBUF) continue;
        if ((unsigned)sx >= CRAFT_FB_W || (unsigned)sy >= CRAFT_FB_H) continue;
        int idx = sy * CRAFT_FB_W + sx;
        if (craft_zbuf[idx] <= depth) continue;

        /* Fade alpha over life. */
        float a = 1.0f - (p->age / PARTICLE_LIFE);
        if (a <= 0.0f) continue;
        int af = (int)(a * 256.0f);
        uint16_t c = p->color;
        int r = ((c >> 11) & 0x1F) * af >> 8;
        int g = ((c >>  5) & 0x3F) * af >> 8;
        int b = ( c        & 0x1F) * af >> 8;
        fb[idx] = (uint16_t)((r << 11) | (g << 5) | b);
        craft_zbuf[idx] = depth;
    }
}

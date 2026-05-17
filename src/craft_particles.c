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

#define MAX_PARTICLES 32
#define PARTICLE_LIFE 1.2f
#define PARTICLES_PER_BREAK 8

typedef struct {
    bool   alive;
    Vec3   pos;
    Vec3   vel;
    uint16_t color;
    float  age;
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

void craft_particles_tick(float dt) {
    if (dt > 0.1f) dt = 0.1f;
    for (int i = 0; i < MAX_PARTICLES; i++) {
        Particle *p = &s_pool[i];
        if (!p->alive) continue;
        p->age += dt;
        if (p->age >= PARTICLE_LIFE) { p->alive = false; continue; }
        p->vel.y -= 14.0f * dt;
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

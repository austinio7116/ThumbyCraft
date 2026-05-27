/*
 * Headless frame-phase profiler. For each of several representative
 * scenes it times the individual draw-phase functions the game runs per
 * frame (render_begin, render_strip, stars, celestials, torches) and
 * reports ms + % of the timed total, plus DDA steps/ray. Host CPU is far
 * faster than the RP2350, so absolute ms are only meaningful relatively
 * — the per-phase proportions and steps/ray transfer to the device.
 *
 *   ./build_host/craft_profile
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "craft_render.h"
#include "craft_world.h"
#include "craft_blocks.h"
#include "craft_torches.h"

void craft_main_init(uint16_t *fb, uint32_t seed);
void craft_render_set_time(float t);
void craft_render_stars(const CraftCamera *, uint16_t *);
void craft_render_celestials(const CraftCamera *, uint16_t *);
extern int craft_gen_force_biome;
extern unsigned long long craft_prof_rays, craft_prof_steps, craft_prof_maxed, craft_prof_air;
uint32_t craft_platform_rand32(void) { return (uint32_t)rand(); }
int craft_save_slot_used(int s) { (void)s; return 0; }
const uint16_t *craft_save_slot_thumb(int s) { (void)s; return NULL; }

static uint16_t g_fb[CRAFT_FB_W * CRAFT_FB_H];
static double now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec * 1e-6;
}

#define ITERS 400
#define TIMEIT(stmt) ({ double _t = now_ms(); \
    for (int _i = 0; _i < ITERS; _i++) { stmt; } (now_ms() - _t) / ITERS; })

typedef struct { const char *name; int biome; uint32_t seed; float dy; float pitch; float yaw; } Scene;

int main(void) {
    Scene sc[] = {
        { "plains-level",  0, 4242, 2.0f, -0.05f, 0.60f },
        { "plains-down",   0, 4242, 3.0f, -0.55f, 0.60f },
        { "plains-up",     0, 4242, 2.0f,  0.20f, 0.60f },
        { "forest",        1, 4242, 2.0f, -0.10f, 0.60f },
        { "jungle",        6, 4242, 2.0f, -0.10f, 0.60f },
        { "desert-beach",  2,   55, 2.0f, -0.10f, 0.70f },
        { "mountains",     5, 4242, 7.0f, -0.30f, 0.70f },
    };
    int n = (int)(sizeof sc / sizeof sc[0]);

    printf("%-13s %6s %6s %6s %6s %6s | %7s %6s %5s %6s\n",
           "scene", "begin", "strip", "torch", "stars", "celes",
           "total", "stp/ry", "air%", "maxed%");
    for (int s = 0; s < n; s++) {
        craft_gen_force_biome = sc[s].biome;
        craft_main_init(g_fb, sc[s].seed);
        craft_render_set_time(90.0f);
        int cx = craft_world_origin_x + CRAFT_WORLD_X / 2;
        int cz = craft_world_origin_z + CRAFT_WORLD_Z / 2;
        int gy = craft_world_skyheight[(cz - craft_world_origin_z) * CRAFT_WORLD_X
                                       + (cx - craft_world_origin_x)];
        CraftCamera cam = {
            .pos = { (float)cx + 0.5f, (float)gy + sc[s].dy, (float)cz + 0.5f },
            .yaw = sc[s].yaw, .pitch = sc[s].pitch, .fov = 1.4f
        };
        /* Warm up + populate sprite list / caches. */
        craft_render_begin(&cam);
        craft_render_strip(&cam, g_fb, 0, CRAFT_FB_H);
        craft_torches_render(&cam, g_fb);

        double t_begin = TIMEIT(craft_render_begin(&cam));
        craft_render_begin(&cam);            /* one begin, then time strip */
        craft_prof_rays = craft_prof_steps = craft_prof_maxed = craft_prof_air = 0;
        craft_render_strip(&cam, g_fb, 0, CRAFT_FB_H);   /* one pass for counters */
        unsigned long long rays = craft_prof_rays, steps = craft_prof_steps,
                           maxed = craft_prof_maxed, air = craft_prof_air;
        double t_strip = TIMEIT(craft_render_strip(&cam, g_fb, 0, CRAFT_FB_H));
        double t_torch = TIMEIT(craft_torches_render(&cam, g_fb));
        double t_stars = TIMEIT(craft_render_stars(&cam, g_fb));
        double t_celes = TIMEIT(craft_render_celestials(&cam, g_fb));
        double total = t_begin + t_strip + t_torch + t_stars + t_celes;
        double spr = rays ? (double)steps / (double)rays : 0.0;
        double airp = steps ? 100.0 * (double)air / (double)steps : 0.0;
        double maxp = rays ? 100.0 * (double)maxed / (double)rays : 0.0;

        printf("%-13s %6.3f %6.3f %6.3f %6.3f %6.3f | %7.3f %6.1f %4.0f%% %5.0f%%\n",
               sc[s].name, t_begin, t_strip, t_torch, t_stars, t_celes,
               total, spr, airp, maxp);
    }
    return 0;
}

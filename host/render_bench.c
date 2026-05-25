/*
 * Headless render profiler.
 *
 * Times craft_render_strip over a set of representative camera scenarios
 * and reports ms/frame plus DDA-step statistics (CRAFT_PROFILE counters),
 * so we can see where the raycaster's cost concentrates — long open
 * views vs caves vs sky — before optimising. Also reports the effect of
 * the existing perf toggles (far-LOD).
 *
 * Host CPU is far faster than the RP2350, so absolute fps is meaningless;
 * the RELATIVE cost between scenarios and the steps/ray figures are what
 * transfer to the device.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include "craft_render.h"
#include "craft_gen.h"
#include "craft_world.h"

void craft_main_init(uint16_t *fb, uint32_t seed);
void craft_render_set_far_lod(bool on);
void craft_render_set_coarse_skip(bool on);

extern unsigned long long craft_prof_rays, craft_prof_steps,
                          craft_prof_maxed, craft_prof_hits, craft_prof_air;

/* Platform hooks normally supplied by host_main.c. */
uint32_t craft_platform_rand32(void) { return (uint32_t)rand(); }
int craft_save_slot_used(int slot) { (void)slot; return 0; }
const uint16_t *craft_save_slot_thumb(int slot) { (void)slot; return NULL; }

static uint16_t g_fb[CRAFT_FB_W * CRAFT_FB_H];

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec * 1e-6;
}

typedef struct { const char *name; Vec3 pos; float yaw, pitch; } Scene;

static double time_scene(const Scene *s, int iters, bool skip) {
    CraftCamera cam = { .pos = s->pos, .yaw = s->yaw, .pitch = s->pitch,
                        .fov = 1.4f };
    craft_render_set_coarse_skip(skip);
    craft_render_begin(&cam);
    craft_render_strip(&cam, g_fb, 0, CRAFT_FB_H);   /* warm up */
    craft_prof_rays = craft_prof_steps = 0;
    double t0 = now_ms();
    for (int i = 0; i < iters; i++) {
        craft_render_begin(&cam);
        craft_render_strip(&cam, g_fb, 0, CRAFT_FB_H);
    }
    return (now_ms() - t0) / iters;
}

static void bench_scene(const Scene *s, int iters) {
    double off = time_scene(s, iters, false);
    double on  = time_scene(s, iters, true);
    double spr = craft_prof_rays > 0 ? (double)craft_prof_steps / craft_prof_rays : 0;
    printf("  %-14s no-skip %6.3f ms -> skip %6.3f ms  (%.0f%% faster)  steps/ray=%4.1f\n",
           s->name, off, on, 100.0 * (off - on) / off, spr);
}

int main(int argc, char **argv) {
    uint32_t seed = (argc > 1) ? (uint32_t)atoi(argv[1]) : 1234u;
    int iters = (argc > 2) ? atoi(argv[2]) : 400;
    craft_main_init(g_fb, seed);

    Vec3 sp = craft_gen_spawn();
    printf("[render_bench] seed=%u iters=%d  spawn=(%.1f,%.1f,%.1f)\n",
           seed, iters, sp.x, sp.y, sp.z);

    /* Realistic gameplay views: eye height on the ground, looking level
     * and slightly down/up — the actual way the game is played. Plus an
     * elevated vantage for the long open case. */
    Vec3 high = { sp.x, sp.y + 12.0f, sp.z };

    Scene scenes[] = {
        { "eye-level",    sp,   0.0f,    0.0f   },  /* stand, look forward */
        { "eye-diag",     sp,   0.785f,  0.0f   },  /* stand, 45° */
        { "eye-down",     sp,   0.4f,   -0.5f   },  /* walking gaze */
        { "eye-up",       sp,   0.0f,    0.6f   },  /* glance up */
        { "elevated",     high, 0.0f,    0.0f   },  /* long open view */
    };
    int n = (int)(sizeof scenes / sizeof scenes[0]);

    /* Exact-equivalence check: the empty-space skip must produce a
     * pixel-identical frame to the brute-force every-cell trace. */
    printf("--- equivalence (skip vs no-skip, must be 0 diffs) ---\n");
    static uint16_t ref[CRAFT_FB_W * CRAFT_FB_H];
    static uint8_t  refz[CRAFT_FB_W * CRAFT_FB_H];
    extern uint8_t craft_zbuf[CRAFT_FB_W * CRAFT_FB_H];
    int total_diff = 0;
    for (int i = 0; i < n; i++) {
        CraftCamera cam = { .pos = scenes[i].pos, .yaw = scenes[i].yaw,
                            .pitch = scenes[i].pitch, .fov = 1.4f };
        craft_render_set_coarse_skip(false);
        craft_render_begin(&cam);
        craft_render_strip(&cam, ref, 0, CRAFT_FB_H);
        memcpy(refz, craft_zbuf, sizeof refz);
        craft_render_set_coarse_skip(true);
        craft_render_begin(&cam);
        craft_render_strip(&cam, g_fb, 0, CRAFT_FB_H);
        int d = 0, phantom = 0, missed = 0, fp = -1;
        for (int p = 0; p < CRAFT_FB_W * CRAFT_FB_H; p++) {
            if (ref[p] != g_fb[p]) {
                d++;
                /* z 255 = sky/no-hit. ref sky + skip solid = phantom hit;
                 * ref solid + skip sky = missed hit. */
                if (refz[p] == 255 && craft_zbuf[p] != 255) { phantom++; if (fp<0) fp=p; }
                else if (refz[p] != 255 && craft_zbuf[p] == 255) { missed++; if (fp<0) fp=p; }
            }
        }
        total_diff += d;
        printf("  %-14s pixel_diff=%d  phantom=%d missed=%d\n",
               scenes[i].name, d, phantom, missed);
    }
    craft_render_set_coarse_skip(true);

    printf("--- timing (no-skip vs skip) ---\n");
    for (int i = 0; i < n; i++) bench_scene(&scenes[i], iters);
    return 0;
}

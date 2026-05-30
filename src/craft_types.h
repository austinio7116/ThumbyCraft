/*
 * ThumbyCraft — shared types and small math helpers.
 *
 * No platform headers here — this file is shared by host and device
 * builds, and by every engine module.
 */
#ifndef CRAFT_TYPES_H
#define CRAFT_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <math.h>

/* Framebuffer size. Square 128x128 on the RP2350; a more powerful host
 * (Android) can override at compile time — e.g. a wide 256x128 to fill a
 * phone screen. Keep CRAFT_FB_H at 128 so the HUD layout is unchanged. */
#ifndef CRAFT_FB_W
#define CRAFT_FB_W 128
#endif
#ifndef CRAFT_FB_H
#define CRAFT_FB_H 128
#endif

/* CRAFT_HOT — function attribute that places the function in SRAM
 * on device so XIP flash latency doesn't stall the inner loop. On
 * host it's a no-op. PICO_ON_DEVICE is defined by the Pico SDK when
 * the target is the RP2350. */
#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE
#  define CRAFT_HOT  __attribute__((section(".time_critical.craft")))
#else
#  define CRAFT_HOT
#endif

/* In-memory world window (a sliding window over the infinite X/Z plane).
 * 64^3 = 256 KB fits the RP2350; a host with more RAM (Android) can widen
 * X/Z to hold more blocks so a longer draw distance has world to show.
 * Indexing is by symbolic multiply (not bit-masks), so these need NOT be
 * powers of two. Keep the X/Z:draw-distance ratio ~ the default (64:60). */
#ifndef CRAFT_WORLD_X
#define CRAFT_WORLD_X 64
#endif
#ifndef CRAFT_WORLD_Y
#define CRAFT_WORLD_Y 64
#endif
#ifndef CRAFT_WORLD_Z
#define CRAFT_WORLD_Z 64
#endif
#define CRAFT_WORLD_VOXELS (CRAFT_WORLD_X * CRAFT_WORLD_Y * CRAFT_WORLD_Z)

typedef struct { float x, y, z; } Vec3;

static inline Vec3 v3(float x, float y, float z) {
    Vec3 v = { x, y, z }; return v;
}
static inline Vec3 v3_add(Vec3 a, Vec3 b) { return v3(a.x+b.x, a.y+b.y, a.z+b.z); }
static inline Vec3 v3_sub(Vec3 a, Vec3 b) { return v3(a.x-b.x, a.y-b.y, a.z-b.z); }
static inline Vec3 v3_scl(Vec3 a, float s) { return v3(a.x*s, a.y*s, a.z*s); }
static inline float v3_dot(Vec3 a, Vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static inline float v3_len(Vec3 a) { return sqrtf(v3_dot(a, a)); }
static inline Vec3 v3_norm(Vec3 a) {
    float l = v3_len(a);
    if (l <= 1e-6f) return v3(0, 0, 0);
    return v3_scl(a, 1.0f / l);
}

/* Pack 5-6-5 RGB565 from 0..255 channels. */
static inline uint16_t rgb565(int r, int g, int b) {
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    if (g < 0) g = 0;
    if (g > 255) g = 255;
    if (b < 0) b = 0;
    if (b > 255) b = 255;
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

#endif

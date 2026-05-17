/*
 * ThumbyCraft — DDA voxel raycaster.
 *
 * Hot path is one DDA traversal per pixel. Optimisations baked in
 * (post-perf-pass — see PERFORMANCE.md):
 *  - per-frame camera basis precomputed
 *  - per-column horizontal ray basis precomputed once per strip
 *    (saves 9 fmul + 3 fadd per pixel)
 *  - rays are NOT renormalised per pixel — DDA works fine on
 *    unnormalised dirs; fog uses t-with-magnitude-correction at the
 *    end (saves one sqrt + 1 div + 3 fmul per pixel)
 *  - trace_ray + sky/shade/fog helpers force-inlined so the inner
 *    loop is one big function
 *  - CRAFT_HOT places the rendering functions in SRAM on device so
 *    XIP flash fetch latency doesn't dominate the inner loop
 *
 * Math stays in floats — the RP2350 Cortex-M33 has a hardware FPU.
 */
#include "craft_render.h"
#include "craft_world.h"
#include "craft_blocks.h"
#include <string.h>

#define CRAFT_MAX_STEPS  64
#define CRAFT_MAX_DIST   60.0f

#define INLINE_HOT static inline __attribute__((always_inline))

static bool  s_fog_enabled = true;
static float s_sun_y = 1.0f;          /* sin(sun_angle): +1 noon, -1 midnight */
static int   s_brightness_q8 = 256;   /* 0..256, applied to face_shade */
static int   s_sky_top_r, s_sky_top_g, s_sky_top_b;       /* RGB565 components */
static int   s_sky_horizon_r, s_sky_horizon_g, s_sky_horizon_b;

void craft_render_set_fog(bool on) { s_fog_enabled = on; }
float craft_render_sun_y(void) { return s_sun_y; }

/* Recompute the sky / brightness lookup for the new sun position.
 * Called once per frame from render_begin. */
void craft_render_set_time(float world_time) {
    const float DAY_LENGTH = 240.0f;
    float t = world_time / DAY_LENGTH;
    t -= (float)((int)t);
    if (t < 0) t += 1.0f;
    {
        extern float s_sun_cos;
        float angle = t * 6.2831853f;
        s_sun_y   = sinf(angle);
        s_sun_cos = cosf(angle);
    }

    /* Brightness ramps from 0.15 (deep night) to 1.0 (noon). */
    float b = s_sun_y * 0.55f + 0.55f;
    if (b < 0.18f) b = 0.18f;
    if (b > 1.0f)  b = 1.0f;
    s_brightness_q8 = (int)(b * 256.0f);

    /* Horizon glow window — orange tint between sun_y in [-0.2, 0.4]
     * peaking at 0.0 (true sunrise/sunset). */
    float glow = 1.0f - (s_sun_y > 0.0f ? s_sun_y * 2.5f : -s_sun_y * 5.0f);
    if (glow < 0) glow = 0;
    if (glow > 1) glow = 1;

    /* Top-of-sky base — black at night through deep-blue at noon. */
    s_sky_top_r = (int)(30  * b);
    s_sky_top_g = (int)(75  * b);
    s_sky_top_b = (int)(190 * b);

    /* Horizon base. */
    int h_r = (int)(150 * b);
    int h_g = (int)(190 * b);
    int h_b = (int)(220 * b);
    /* Blend toward orange when sun near horizon. */
    s_sky_horizon_r = (int)(h_r * (1.0f - glow) + 240.0f * glow);
    s_sky_horizon_g = (int)(h_g * (1.0f - glow) + 120.0f * glow);
    s_sky_horizon_b = (int)(h_b * (1.0f - glow) +  60.0f * glow);
}

INLINE_HOT uint16_t sky_at(int py) {
    /* Vertical lerp top→horizon. Stars at night in the upper half:
     * cheap deterministic xorshift on (px,py) → sparse white pixels
     * whose brightness scales with -sun_y. Computed inline in the
     * pixel loop instead of here so we have access to px. */
    int t = py;
    int r = s_sky_top_r + ((s_sky_horizon_r - s_sky_top_r) * t) / (CRAFT_FB_H - 1);
    int g = s_sky_top_g + ((s_sky_horizon_g - s_sky_top_g) * t) / (CRAFT_FB_H - 1);
    int b = s_sky_top_b + ((s_sky_horizon_b - s_sky_top_b) * t) / (CRAFT_FB_H - 1);
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return rgb565(r, g, b);
}

/* Legacy screen-space star sprinkle — kept for reference. Real stars
 * now render as a fixed celestial-sphere pass (craft_render_stars).
 * The renderer's sky path no longer calls this. */
INLINE_HOT uint16_t star_dust(int px, int py, uint16_t base) {
    (void)px; (void)py;
    return base;
}

INLINE_HOT uint16_t fog_mix(uint16_t c, int t, int py) {
    if (!s_fog_enabled || t <= 0) return c;
    if (t > 255) t = 255;
    uint16_t fc = sky_at(py);
    int r1 = (c  >> 11) & 0x1F, g1 = (c  >> 5) & 0x3F, b1 = c  & 0x1F;
    int r2 = (fc >> 11) & 0x1F, g2 = (fc >> 5) & 0x3F, b2 = fc & 0x1F;
    int rr = r1 + ((r2 - r1) * t >> 8);
    int gg = g1 + ((g2 - g1) * t >> 8);
    int bb = b1 + ((b2 - b1) * t >> 8);
    return (uint16_t)((rr << 11) | (gg << 5) | bb);
}

INLINE_HOT uint16_t shade(uint16_t c, int m) {
    int r = ((c >> 11) & 0x1F) * m >> 8;
    int g = ((c >>  5) & 0x3F) * m >> 8;
    int b = ( c        & 0x1F) * m >> 8;
    if (r > 31) r = 31;
    if (g > 63) g = 63;
    if (b > 31) b = 31;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

/* Base per-face shading. Final value = (face_shade * brightness) >> 8
 * so the world dims smoothly with the day/night cycle. */
static const uint16_t face_shade[6] = {
    220, 220, 256, 150, 180, 180
};
static uint16_t s_face_shade_lit[6];   /* recomputed per frame */

/* Phase 26 — z-buffer, written by render_strip for every pixel. */
uint8_t craft_zbuf[CRAFT_FB_W * CRAFT_FB_H];

typedef struct {
    bool   hit;
    int    bx, by, bz;
    int    fx, fy, fz;
    int    face;
    float  u, v;
    float  t;
    BlockId blk;
    bool   passed_water;
} TraceHit;

INLINE_HOT TraceHit trace_ray(Vec3 origin, Vec3 dir, bool stop_at_water) {
    TraceHit h = (TraceHit){0};

    int vx = (int)origin.x;
    int vy = (int)origin.y;
    int vz = (int)origin.z;
    if (origin.x < 0) vx--;
    if (origin.y < 0) vy--;
    if (origin.z < 0) vz--;

    int sx = (dir.x > 0) ? 1 : (dir.x < 0 ? -1 : 0);
    int sy = (dir.y > 0) ? 1 : (dir.y < 0 ? -1 : 0);
    int sz = (dir.z > 0) ? 1 : (dir.z < 0 ? -1 : 0);

    /* 1/0 -> a huge number; subsequent comparisons exclude that axis. */
    float inv_x = (dir.x != 0.0f) ? 1.0f / dir.x : 1e30f;
    float inv_y = (dir.y != 0.0f) ? 1.0f / dir.y : 1e30f;
    float inv_z = (dir.z != 0.0f) ? 1.0f / dir.z : 1e30f;

    float t_delta_x = (sx != 0) ? (float)sx * inv_x : 1e30f;
    float t_delta_y = (sy != 0) ? (float)sy * inv_y : 1e30f;
    float t_delta_z = (sz != 0) ? (float)sz * inv_z : 1e30f;

    float t_max_x = (sx > 0)
        ? ((float)(vx + 1) - origin.x) * inv_x
        : (sx < 0 ? ((float)vx - origin.x) * inv_x : 1e30f);
    float t_max_y = (sy > 0)
        ? ((float)(vy + 1) - origin.y) * inv_y
        : (sy < 0 ? ((float)vy - origin.y) * inv_y : 1e30f);
    float t_max_z = (sz > 0)
        ? ((float)(vz + 1) - origin.z) * inv_z
        : (sz < 0 ? ((float)vz - origin.z) * inv_z : 1e30f);

    int face = -1;
    int prev_vx = vx, prev_vy = vy, prev_vz = vz;
    float t = 0.0f;

    for (int step = 0; step < CRAFT_MAX_STEPS; step++) {
        prev_vx = vx; prev_vy = vy; prev_vz = vz;
        if (t_max_x < t_max_y && t_max_x < t_max_z) {
            vx += sx;
            t = t_max_x;
            t_max_x += t_delta_x;
            face = (sx > 0) ? FACE_NX : FACE_PX;
        } else if (t_max_y < t_max_z) {
            vy += sy;
            t = t_max_y;
            t_max_y += t_delta_y;
            face = (sy > 0) ? FACE_NY : FACE_PY;
        } else {
            vz += sz;
            t = t_max_z;
            t_max_z += t_delta_z;
            face = (sz > 0) ? FACE_NZ : FACE_PZ;
        }

        if (t > CRAFT_MAX_DIST) break;
        /* Terminate the ray when it exits the loaded WINDOW (not the
         * old fixed-world bounds 0..63). The window slides with the
         * player so the visible terrain follows them in infinite
         * worlds. */
        if ((unsigned)(vx - craft_world_origin_x) >= CRAFT_WORLD_X ||
            (unsigned)vy >= CRAFT_WORLD_Y ||
            (unsigned)(vz - craft_world_origin_z) >= CRAFT_WORLD_Z) break;

        BlockId blk = craft_world_get(vx, vy, vz);
        if (blk == BLK_AIR) continue;
        if (blk == BLK_WATER) {
            if (!stop_at_water) {
                h.passed_water = true;
                continue;
            }
        }

        h.hit = true;
        h.bx = vx; h.by = vy; h.bz = vz;
        h.fx = prev_vx; h.fy = prev_vy; h.fz = prev_vz;
        h.face = face;
        h.blk = blk;
        h.t = t;

        float hx = origin.x + dir.x * t;
        float hy = origin.y + dir.y * t;
        float hz = origin.z + dir.z * t;
        /* UV is the fractional part within the hit cell. Use floorf
         * (one VRINTM instruction on M33) so negative hit positions
         * — which happen once the player wanders into negative world
         * coords — still produce a [0, 1) fractional part. (int) cast
         * is truncation-toward-zero and would give a negative result
         * for negative inputs, clamping every face pixel to texture
         * column 0 and rendering each face as a single stretched
         * line of colour. */
        switch (face) {
            case FACE_PX: case FACE_NX:
                h.u = hz - floorf(hz);
                h.v = 1.0f - (hy - floorf(hy));
                break;
            case FACE_PY: case FACE_NY:
                h.u = hx - floorf(hx);
                h.v = hz - floorf(hz);
                break;
            case FACE_PZ: case FACE_NZ:
                h.u = hx - floorf(hx);
                h.v = 1.0f - (hy - floorf(hy));
                break;
        }
        return h;
    }
    return h;
}

CraftRayHit craft_render_pick(const CraftCamera *cam) {
    float cy = cosf(cam->yaw),  sy = sinf(cam->yaw);
    float cp = cosf(cam->pitch), sp = sinf(cam->pitch);
    Vec3 fwd = v3(sy * cp, sp, cy * cp);
    TraceHit h = trace_ray(cam->pos, fwd, true);
    CraftRayHit r = (CraftRayHit){0};
    r.hit = h.hit;
    r.bx = h.bx; r.by = h.by; r.bz = h.bz;
    r.fx = h.fx; r.fy = h.fy; r.fz = h.fz;
    r.face = h.face;
    r.distance = h.t;
    return r;
}

/* Per-frame camera basis. */
static Vec3  s_fwd, s_right, s_up;
static float s_fov_tan_v;
static float s_fov_tan_h;

/* Per-column basis: col_basis[px] = fwd + right * vx[px]. Recomputed
 * each strip render — both strips of one frame share the same basis. */
static Vec3  s_col_basis[CRAFT_FB_W];
static bool  s_col_basis_valid;

static void update_basis(const CraftCamera *cam) {
    float cy = cosf(cam->yaw),  sy = sinf(cam->yaw);
    float cp = cosf(cam->pitch), sp = sinf(cam->pitch);
    s_fwd   = v3(sy * cp, sp, cy * cp);
    s_right = v3(cy, 0.0f, -sy);
    s_up    = v3(
        s_fwd.y * s_right.z - s_fwd.z * s_right.y,
        s_fwd.z * s_right.x - s_fwd.x * s_right.z,
        s_fwd.x * s_right.y - s_fwd.y * s_right.x);
    s_fov_tan_v = tanf(cam->fov * 0.5f);
    s_fov_tan_h = s_fov_tan_v;   /* square framebuffer */

    /* Precompute per-column horizontal ray. */
    for (int px = 0; px < CRAFT_FB_W; px++) {
        float ndc_x = ((float)(px * 2 - CRAFT_FB_W + 1) / (float)CRAFT_FB_W);
        float vx = ndc_x * s_fov_tan_h;
        s_col_basis[px].x = s_fwd.x + s_right.x * vx;
        s_col_basis[px].y = s_fwd.y + s_right.y * vx;
        s_col_basis[px].z = s_fwd.z + s_right.z * vx;
    }
    s_col_basis_valid = true;
}

void craft_render_begin(const CraftCamera *cam) {
    update_basis(cam);
    /* Light each face's base shade by current brightness. */
    for (int i = 0; i < 6; i++) {
        int v = (int)face_shade[i] * s_brightness_q8 >> 8;
        if (v > 256) v = 256;
        s_face_shade_lit[i] = (uint16_t)v;
    }
}

CRAFT_HOT
void craft_render_strip(const CraftCamera *cam, uint16_t *fb,
                        int y_start, int y_end) {
    bool underwater = false;
    {
        int ix = (int)cam->pos.x;
        int iy = (int)cam->pos.y;
        int iz = (int)cam->pos.z;
        underwater = (craft_world_get(ix, iy, iz) == BLK_WATER);
    }

    if (y_start < 0) y_start = 0;
    if (y_end > CRAFT_FB_H) y_end = CRAFT_FB_H;

    for (int py = y_start; py < y_end; py++) {
        float ndc_y = -((float)(py * 2 - CRAFT_FB_H + 1) / (float)CRAFT_FB_H);
        float vy = ndc_y * s_fov_tan_v;
        Vec3 up_vy = v3(s_up.x * vy, s_up.y * vy, s_up.z * vy);
        uint16_t *row = &fb[py * CRAFT_FB_W];

        for (int px = 0; px < CRAFT_FB_W; px++) {
            Vec3 dir = v3(
                s_col_basis[px].x + up_vy.x,
                s_col_basis[px].y + up_vy.y,
                s_col_basis[px].z + up_vy.z);

            /* No per-pixel renormalisation — DDA works on any dir;
             * fog handled with a magnitude correction below. */
            TraceHit h = trace_ray(cam->pos, dir, false);

            uint16_t out;
            int fog_t = 0;
            uint8_t z_q = 255;
            if (!h.hit) {
                out = sky_at(py);
                /* zbuf sky = far sentinel (255 default). */
            } else {
                const uint16_t *tex = craft_block_texture(h.blk, h.face);
                int tu = (int)(h.u * CRAFT_TEX_SIZE);
                int tv = (int)(h.v * CRAFT_TEX_SIZE);
                if (tu < 0) tu = 0; else if (tu >= CRAFT_TEX_SIZE) tu = CRAFT_TEX_SIZE - 1;
                if (tv < 0) tv = 0; else if (tv >= CRAFT_TEX_SIZE) tv = CRAFT_TEX_SIZE - 1;
                uint16_t c = tex[tv * CRAFT_TEX_SIZE + tu];
                c = shade(c, s_face_shade_lit[h.face]);

                if (h.passed_water) {
                    int r1 = (c >> 11) & 0x1F, g1 = (c >> 5) & 0x3F, b1 = c & 0x1F;
                    int r2 = 5, g2 = 11, b2 = 20;
                    r1 = (r1 + r2 * 2) / 3;
                    g1 = (g1 + g2 * 2) / 3;
                    b1 = (b1 + b2 * 2) / 3;
                    c = (uint16_t)((r1 << 11) | (g1 << 5) | b1);
                }

                /* Compute world distance once: needed for zbuf and
                 * (conditionally) fog. One sqrt per pixel costs us
                 * ~2% vs the previous fog-only sqrt. */
                float dxh = dir.x, dyh = dir.y, dzh = dir.z;
                float len2 = dxh * dxh + dyh * dyh + dzh * dzh;
                float dl   = (len2 > 1.0001f) ? sqrtf(len2) : 1.0f;
                float t_world = h.t * dl;

                int q = (int)(t_world * 255.0f / CRAFT_MAX_DIST_FOR_ZBUF);
                if (q > 254) q = 254;
                if (q < 0)   q = 0;
                z_q = (uint8_t)q;

                float fog_start = CRAFT_MAX_DIST * 0.45f;
                if (t_world > fog_start) {
                    float k = (t_world - fog_start) / (CRAFT_MAX_DIST - fog_start);
                    if (k > 1.0f) k = 1.0f;
                    fog_t = (int)(k * 255.0f);
                }
                out = fog_mix(c, fog_t, py);
            }
            craft_zbuf[py * CRAFT_FB_W + px] = z_q;

            if (underwater) {
                int r1 = (out >> 11) & 0x1F, g1 = (out >> 5) & 0x3F, b1 = out & 0x1F;
                r1 = r1 / 3;
                g1 = (g1 + 20) / 2;
                b1 = (b1 + 28) / 2;
                if (b1 > 31) b1 = 31;
                out = (uint16_t)((r1 << 11) | (g1 << 5) | b1);
            }
            row[px] = out;
        }
    }
}

void craft_render_frame(const CraftCamera *cam, uint16_t *fb) {
    craft_render_begin(cam);
    craft_render_strip(cam, fb, 0, CRAFT_FB_H);
    craft_render_celestials(cam, fb);
}

/* --- Sun + moon billboards ----------------------------------------
 * Sun travels in an arc — sin(angle) for altitude, cos(angle) for
 * east/west position. Moon is the antipode. World convention: +X
 * east, +Y up, +Z south.
 */
static void draw_disc(uint16_t *fb, int cx, int cy, int radius,
                      uint16_t core, uint16_t halo) {
    int r2 = radius * radius;
    int h2 = (radius + 1) * (radius + 1);
    for (int dy = -radius - 1; dy <= radius + 1; dy++) {
        int y = cy + dy;
        if ((unsigned)y >= CRAFT_FB_H) continue;
        for (int dx = -radius - 1; dx <= radius + 1; dx++) {
            int x = cx + dx;
            if ((unsigned)x >= CRAFT_FB_W) continue;
            int idx = y * CRAFT_FB_W + x;
            /* Sun/moon live at infinity — only paint over sky.
             * Anything closer (block, tree, mob) occludes them. */
            if (craft_zbuf[idx] < 254) continue;
            int d2 = dx * dx + dy * dy;
            if (d2 <= r2)        fb[idx] = core;
            else if (d2 <= h2)   fb[idx] = halo;
        }
    }
}

void craft_render_celestials(const CraftCamera *cam, uint16_t *fb) {
    /* Use the world clock that fed render_set_time. Re-derive the
     * angle from s_sun_y plus a sign bit — simpler: store the angle
     * too. Since set_time computed s_sun_y = sin(angle) and we want
     * cos for the east/west component, recover via sqrt of the
     * complement. Sign comes from cycle phase: we need the original
     * angle to know whether sun is east-rising or west-setting. */

    /* Keep it simple: read s_sun_y, compute a believable cos with a
     * fixed phase relationship — we'll stash cos at set_time. */

    extern float s_sun_cos;     /* defined below — populated by set_time */
    float sy = s_sun_y;
    float sc = s_sun_cos;

    float cy = cosf(cam->yaw), sye = sinf(cam->yaw);
    float cp = cosf(cam->pitch), sp = sinf(cam->pitch);
    Vec3 fwd   = v3(sye * cp, sp, cy * cp);
    Vec3 right = v3(cy, 0.0f, -sye);
    Vec3 up    = v3(
        fwd.y * right.z - fwd.z * right.y,
        fwd.z * right.x - fwd.x * right.z,
        fwd.x * right.y - fwd.y * right.x);
    float tan_h = tanf(cam->fov * 0.5f);

    /* Helper to project a unit world direction onto screen. */
    /* Sun first (only if above horizon). */
    if (sy > -0.05f) {
        Vec3 sun_dir = v3(sc, sy, 0.2f);
        float l = sqrtf(sun_dir.x*sun_dir.x + sun_dir.y*sun_dir.y + sun_dir.z*sun_dir.z);
        if (l > 0.001f) { sun_dir.x/=l; sun_dir.y/=l; sun_dir.z/=l; }
        float zf = v3_dot(sun_dir, fwd);
        if (zf > 0.05f) {
            float xs = v3_dot(sun_dir, right) / zf;
            float ys = v3_dot(sun_dir, up)    / zf;
            int   px = (int)(CRAFT_FB_W * 0.5f + xs * CRAFT_FB_W * 0.5f / tan_h);
            int   py = (int)(CRAFT_FB_H * 0.5f - ys * CRAFT_FB_H * 0.5f / tan_h);
            /* Bright yellow with faint glow halo. */
            uint16_t core = rgb565(255, 230, 140);
            uint16_t halo = rgb565(220, 180,  80);
            draw_disc(fb, px, py, 5, core, halo);
        }
    }

    /* Moon — opposite direction, only visible when sun is set. */
    if (sy < 0.10f) {
        Vec3 moon_dir = v3(-sc, -sy, -0.2f);
        float l = sqrtf(moon_dir.x*moon_dir.x + moon_dir.y*moon_dir.y + moon_dir.z*moon_dir.z);
        if (l > 0.001f) { moon_dir.x/=l; moon_dir.y/=l; moon_dir.z/=l; }
        float zf = v3_dot(moon_dir, fwd);
        if (zf > 0.05f) {
            float xs = v3_dot(moon_dir, right) / zf;
            float ys = v3_dot(moon_dir, up)    / zf;
            int   px = (int)(CRAFT_FB_W * 0.5f + xs * CRAFT_FB_W * 0.5f / tan_h);
            int   py = (int)(CRAFT_FB_H * 0.5f - ys * CRAFT_FB_H * 0.5f / tan_h);
            /* Cool pale white with darker halo. */
            uint16_t core = rgb565(230, 230, 240);
            uint16_t halo = rgb565(120, 120, 150);
            draw_disc(fb, px, py, 4, core, halo);
        }
    }
}

/* Companion to s_sun_y — populated by craft_render_set_time. */
float s_sun_cos = 1.0f;

/* --- Starfield ----------------------------------------------------
 * STAR_COUNT fixed positions on the upper celestial hemisphere.
 * Each is a unit direction in world space — projected through the
 * camera every frame. Lazy-initialised on first render call. */
#define STAR_COUNT 96

static Vec3     s_star_dirs[STAR_COUNT];
static uint16_t s_star_colors[STAR_COUNT];
static bool     s_stars_ready;

static uint32_t star_xs(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x; return x;
}
static float star_frand(uint32_t *s) {
    return (star_xs(s) & 0xFFFF) / 65535.0f;
}

static void stars_init(void) {
    uint32_t rng = 0xCAFE00BAu;
    for (int i = 0; i < STAR_COUNT; i++) {
        /* Upper hemisphere only. y in [0.05, 0.95] avoids the
         * pole-only crowding and the just-on-the-horizon cluster. */
        float v = star_frand(&rng) * 0.90f + 0.05f;
        float u = star_frand(&rng);
        float sin_phi = sqrtf(1.0f - v * v);
        float theta   = u * 6.2831853f;
        s_star_dirs[i] = v3(sin_phi * cosf(theta), v, sin_phi * sinf(theta));
        /* Slightly varied pale-blue-white colours. */
        int rr = 180 + (int)(star_xs(&rng) & 0x3F);
        int gg = 180 + (int)(star_xs(&rng) & 0x3F);
        int bb = 200 + (int)(star_xs(&rng) & 0x3F);
        if (rr > 240) rr = 240;
        if (gg > 240) gg = 240;
        if (bb > 255) bb = 255;
        s_star_colors[i] = rgb565(rr, gg, bb);
    }
    s_stars_ready = true;
}

/* --- Bresenham line draw — used by the pick outline -------------- */
static void draw_line(uint16_t *fb, int x0, int y0, int x1, int y1, uint16_t c) {
    int dx =  (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int dy = -((y1 > y0) ? (y1 - y0) : (y0 - y1));
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        if ((unsigned)x0 < CRAFT_FB_W && (unsigned)y0 < CRAFT_FB_H)
            fb[y0 * CRAFT_FB_W + x0] = c;
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void craft_render_pick_outline(const CraftCamera *cam, uint16_t *fb) {
    CraftRayHit h = craft_render_pick(cam);
    if (!h.hit || h.distance > 8.0f) return;

    /* 8 corners of the targeted unit cube. Corner i = (i&1, (i>>1)&1, (i>>2)&1). */
    int  sx[8], sy[8];
    bool in_front[8];
    for (int i = 0; i < 8; i++) {
        Vec3 c = v3((float)(h.bx + ((i >> 0) & 1)),
                    (float)(h.by + ((i >> 1) & 1)),
                    (float)(h.bz + ((i >> 2) & 1)));
        uint8_t depth;
        in_front[i] = craft_render_project(cam, c, &sx[i], &sy[i], &depth, NULL);
    }

    /* Pass 1 — 12 cube edges in dark grey: "A breaks this block". */
    static const uint8_t edges[12][2] = {
        {0,1},{2,3},{4,5},{6,7},
        {0,2},{1,3},{4,6},{5,7},
        {0,4},{1,5},{2,6},{3,7},
    };
    uint16_t break_col = rgb565(30, 30, 30);
    for (int i = 0; i < 12; i++) {
        int a = edges[i][0], b = edges[i][1];
        if (!in_front[a] || !in_front[b]) continue;
        draw_line(fb, sx[a], sy[a], sx[b], sy[b], break_col);
    }

    /* Pass 2 — 4 perimeter edges of the hit face in bright white:
     * "B places a new block adhering to this face". The hit face is
     * the one the ray struck (from craft_render_pick), which is
     * exactly the face a placed block would attach to. */
    static const uint8_t face_corners[6][4] = {
        {1, 3, 7, 5},   /* FACE_PX — x = 1 */
        {0, 2, 6, 4},   /* FACE_NX — x = 0 */
        {2, 3, 7, 6},   /* FACE_PY — y = 1 */
        {0, 1, 5, 4},   /* FACE_NY — y = 0 */
        {4, 5, 7, 6},   /* FACE_PZ — z = 1 */
        {0, 1, 3, 2},   /* FACE_NZ — z = 0 */
    };
    if ((unsigned)h.face < 6) {
        const uint8_t *fc = face_corners[h.face];
        uint16_t place_col = rgb565(240, 240, 200);   /* warm white */
        for (int e = 0; e < 4; e++) {
            int a = fc[e];
            int b = fc[(e + 1) & 3];
            if (!in_front[a] || !in_front[b]) continue;
            draw_line(fb, sx[a], sy[a], sx[b], sy[b], place_col);
        }
    }
}

void craft_render_stars(const CraftCamera *cam, uint16_t *fb) {
    /* Fade in as sun drops below the horizon, full strength after
     * sun_y < -0.4. */
    if (s_sun_y > -0.05f) return;
    float fade = (-s_sun_y - 0.05f) / 0.35f;
    if (fade <= 0.0f) return;
    if (fade > 1.0f)  fade = 1.0f;
    int alpha = (int)(fade * 256.0f);
    if (alpha < 32) return;

    if (!s_stars_ready) stars_init();

    /* Camera basis (matches craft_render_project / celestials). */
    float cy = cosf(cam->yaw),  sye = sinf(cam->yaw);
    float cp = cosf(cam->pitch), sp = sinf(cam->pitch);
    Vec3 fwd   = v3(sye * cp, sp, cy * cp);
    Vec3 right = v3(cy, 0.0f, -sye);
    Vec3 up    = v3(
        fwd.y * right.z - fwd.z * right.y,
        fwd.z * right.x - fwd.x * right.z,
        fwd.x * right.y - fwd.y * right.x);
    float tan_h   = tanf(cam->fov * 0.5f);
    float focal_h = (CRAFT_FB_W * 0.5f) / tan_h;
    float focal_v = (CRAFT_FB_H * 0.5f) / tan_h;

    for (int i = 0; i < STAR_COUNT; i++) {
        Vec3 d = s_star_dirs[i];
        float zf = d.x * fwd.x + d.y * fwd.y + d.z * fwd.z;
        if (zf <= 0.05f) continue;
        float xs = (d.x * right.x + d.y * right.y + d.z * right.z) / zf;
        float ys = (d.x * up.x    + d.y * up.y    + d.z * up.z   ) / zf;
        int   px = (int)(CRAFT_FB_W * 0.5f + xs * focal_h);
        int   py = (int)(CRAFT_FB_H * 0.5f - ys * focal_v);
        if ((unsigned)px >= CRAFT_FB_W) continue;
        if ((unsigned)py >= CRAFT_FB_H) continue;
        int idx = py * CRAFT_FB_W + px;
        /* Only paint over sky (zbuf at far-sentinel). */
        if (craft_zbuf[idx] < 254) continue;
        uint16_t c = s_star_colors[i];
        int r = ((c >> 11) & 0x1F) * alpha >> 8;
        int g = ((c >>  5) & 0x3F) * alpha >> 8;
        int b = ( c        & 0x1F) * alpha >> 8;
        fb[idx] = (uint16_t)((r << 11) | (g << 5) | b);
    }
}

bool craft_render_project(const CraftCamera *cam, Vec3 world_pos,
                          int *out_sx, int *out_sy, uint8_t *out_depth,
                          float *out_dist) {
    /* Camera basis. */
    float cy = cosf(cam->yaw), sye = sinf(cam->yaw);
    float cp = cosf(cam->pitch), sp = sinf(cam->pitch);
    Vec3 fwd   = v3(sye * cp, sp, cy * cp);
    Vec3 right = v3(cy, 0.0f, -sye);
    Vec3 up    = v3(
        fwd.y * right.z - fwd.z * right.y,
        fwd.z * right.x - fwd.x * right.z,
        fwd.x * right.y - fwd.y * right.x);
    float tan_h = tanf(cam->fov * 0.5f);

    Vec3 rel = v3_sub(world_pos, cam->pos);
    float zf = v3_dot(rel, fwd);
    if (zf <= 0.05f) return false;       /* behind / at camera plane */
    float xs = v3_dot(rel, right) / zf;
    float ys = v3_dot(rel, up)    / zf;
    int sx = (int)(CRAFT_FB_W * 0.5f + xs * CRAFT_FB_W * 0.5f / tan_h);
    int sy = (int)(CRAFT_FB_H * 0.5f - ys * CRAFT_FB_H * 0.5f / tan_h);
    float dist = sqrtf(rel.x*rel.x + rel.y*rel.y + rel.z*rel.z);
    int q = (int)(dist * 255.0f / CRAFT_MAX_DIST_FOR_ZBUF);
    if (q > 254) q = 254;
    if (q < 0)   q = 0;
    if (out_sx) *out_sx = sx;
    if (out_sy) *out_sy = sy;
    if (out_depth) *out_depth = (uint8_t)q;
    if (out_dist) *out_dist = dist;
    return true;
}

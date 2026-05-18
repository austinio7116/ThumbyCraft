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
#include "craft_tool_models.h"
#include <string.h>

#define CRAFT_MAX_STEPS  64
#define CRAFT_MAX_DIST   60.0f

#define INLINE_HOT static inline __attribute__((always_inline))

static bool  s_fog_enabled = true;
static float s_sun_y = 1.0f;          /* sin(sun_angle): +1 noon, -1 midnight */
static float s_cloud_drift = 0.0f;    /* world units of east drift since boot */
static int   s_brightness_q8 = 256;   /* 0..256, applied to face_shade */
static int   s_sky_top_r, s_sky_top_g, s_sky_top_b;       /* RGB565 components */
static int   s_sky_horizon_r, s_sky_horizon_g, s_sky_horizon_b;

void craft_render_set_fog(bool on) { s_fog_enabled = on; }
float craft_render_sun_y(void) { return s_sun_y; }

/* Recompute the sky / brightness lookup for the new sun position.
 * Called once per frame from render_begin. */
void craft_render_set_time(float world_time) {
    const float DAY_LENGTH = 240.0f;
    /* Cloud drift — slow east scroll, ~0.5 world blocks per second.
     * Wraps every 16384 units which is way beyond a play session. */
    s_cloud_drift = world_time * 0.5f;
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

    /* Maintain `idx` = local-buffer index = (vy*WORLD_Z + lz)*WORLD_X + lx
     * incrementally as the DDA steps. Saves a function call + bounds
     * compare + multiply on every step (up to 64 steps per pixel ×
     * 16 384 pixels per frame).
     *
     * Validity: trace_ray is only called from camera-position rays
     * (render strip, player pick), and the camera lives inside the
     * window via maybe_shift. So origin is always in-window and the
     * initial idx is in-range. The DDA signs are fixed for a ray so
     * once vx/vy/vz step outside the window they stay outside — we
     * break before the next read, so idx is never used while
     * out-of-range. */
    int lx0 = vx - craft_world_origin_x;
    int lz0 = vz - craft_world_origin_z;
    int idx = (vy * CRAFT_WORLD_Z + lz0) * CRAFT_WORLD_X + lx0;
    const int idx_dy = CRAFT_WORLD_X * CRAFT_WORLD_Z;
    const int idx_dz = CRAFT_WORLD_X;

    for (int step = 0; step < CRAFT_MAX_STEPS; step++) {
        prev_vx = vx; prev_vy = vy; prev_vz = vz;
        if (t_max_x < t_max_y && t_max_x < t_max_z) {
            vx += sx;
            idx += sx;
            t = t_max_x;
            t_max_x += t_delta_x;
            face = (sx > 0) ? FACE_NX : FACE_PX;
        } else if (t_max_y < t_max_z) {
            vy += sy;
            idx += sy * idx_dy;
            t = t_max_y;
            t_max_y += t_delta_y;
            face = (sy > 0) ? FACE_NY : FACE_PY;
        } else {
            vz += sz;
            idx += sz * idx_dz;
            t = t_max_z;
            t_max_z += t_delta_z;
            face = (sz > 0) ? FACE_NZ : FACE_PZ;
        }

        if (t > CRAFT_MAX_DIST) break;
        /* Terminate the ray when it exits the loaded WINDOW. */
        if ((unsigned)(vx - craft_world_origin_x) >= CRAFT_WORLD_X ||
            (unsigned)vy >= CRAFT_WORLD_Y ||
            (unsigned)(vz - craft_world_origin_z) >= CRAFT_WORLD_Z) break;

        /* Direct buffer read — bounds already checked above, idx is
         * maintained incrementally so this is one load. Mask off the
         * top 3 bits, which carry the water-flow level field. */
        BlockId blk = (BlockId)(craft_world_blocks[idx] & 0x1F);
        if (blk == BLK_AIR) continue;
        /* Torches are smaller-than-cube objects rendered in a
         * separate 3D post-pass. The raycaster sees the cell as
         * empty so the player can see through it and the torch
         * cuboid sits properly inside it. */
        if (blk == BLK_TORCH) continue;
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
/* --- Procedural cloud volume ------------------------------------
 *
 * Vanilla Minecraft clouds are flat-bottomed 3D prisms ~4 blocks
 * thick and ~12×12 blocks wide, built from individual square
 * pieces. We mimic that with a 4-block slab between Y=CLOUD_Y_BOT
 * and Y=CLOUD_Y_TOP, populated by a hash bitmap on an 8-block
 * grid (~40% coverage).
 *
 * Per upward ray: compute the two plane intersections t_bot/t_top,
 * then sample the hash at 3 evenly-spaced points between them.
 * If any sample hits a cloud cell, paint the pixel — the same
 * sampling renders the side walls of the boxes from below, which
 * is what gives clouds their characteristic Minecraft silhouette.
 *
 * Cost is ~40-50 cycles per sky pixel that survives the gating
 * (upward + below-cloud + not-deep-night).
 */
#define CLOUD_Y_BOT       110.0f
#define CLOUD_Y_TOP       114.0f
#define CLOUD_FINE         16.0f       /* fine cell — visible square edge */
#define CLOUD_COARSE       64.0f       /* coarse region — clump scale */
#define CLOUD_INV_FINE     (1.0f / CLOUD_FINE)
#define CLOUD_INV_COARSE   (1.0f / CLOUD_COARSE)

INLINE_HOT uint32_t cloud_hash(int x, int z) {
    uint32_t h = (uint32_t)x * 374761393u ^ (uint32_t)z * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

/* Sample the cloud volume at world position (wx, wz).
 *
 * Two-scale clumping so clouds form connected patches instead of
 * white-noise speckle:
 *   1. Coarse 64-block region passes ~55 % of the time → roughly
 *      half the sky is "cloudy zones", the rest is open sky.
 *   2. Within a cloudy zone, the 16-block fine cell passes ~70 % →
 *      bulky connected blobs with the occasional internal gap that
 *      reads as classic Minecraft cloud silhouette.
 * Net coverage ≈ 38 %, organised into 64-block patches. */
INLINE_HOT bool cloud_at(float wx, float wz) {
    int gcx = (int)floorf(wx * CLOUD_INV_COARSE);
    int gcz = (int)floorf(wz * CLOUD_INV_COARSE);
    uint32_t gh = cloud_hash(gcx + 0x9E37, gcz - 0x517C);
    if ((gh & 0xFF) < 0x73) return false;   /* ~55 % regions are cloudy */

    int cx = (int)floorf(wx * CLOUD_INV_FINE);
    int cz = (int)floorf(wz * CLOUD_INV_FINE);
    uint32_t h = cloud_hash(cx, cz);
    return (h & 0xFF) < 0xB3;               /* ~70 % fine cells solid */
}

#define CLOUD_MAX_T       200.0f      /* fog horizon for cloud rays */

INLINE_HOT uint16_t cloud_overlay(uint16_t sky_c, Vec3 origin, Vec3 dir) {
    /* Cheap rejects first. Stronger up-tilt cutoff (0.08) drops the
     * pixel count by skipping rays close to horizontal — they hit
     * the slab so far away it's invisible anyway. */
    if (dir.y <= 0.08f) return sky_c;
    if (origin.y >= CLOUD_Y_BOT - 0.5f) return sky_c;
    if (s_sun_y < -0.30f) return sky_c;

    float inv_dy = 1.0f / dir.y;
    float t_bot = (CLOUD_Y_BOT - origin.y) * inv_dy;
    if (t_bot > CLOUD_MAX_T) return sky_c;

    /* Sample the bottom face first — that's what a player below the
     * cloud layer actually sees. Cloudy pixels stop right here (one
     * hash pair) and never pay for the side-wall lookup. */
    float wx0 = origin.x + dir.x * t_bot + s_cloud_drift;
    float wz0 = origin.z + dir.z * t_bot;
    bool hit_bottom = cloud_at(wx0, wz0);
    float t_hit;
    bool side_face;

    if (hit_bottom) {
        t_hit = t_bot;
        side_face = false;
    } else {
        /* Mid-slab sample picks up side walls — rays that miss the
         * bottom of a cell but clip its vertical face on the way
         * up. Single sample is enough for a 4-block slab. */
        float t_top = (CLOUD_Y_TOP - origin.y) * inv_dy;
        float t_mid = (t_bot + t_top) * 0.5f;
        float wx1 = origin.x + dir.x * t_mid + s_cloud_drift;
        float wz1 = origin.z + dir.z * t_mid;
        if (!cloud_at(wx1, wz1)) return sky_c;
        t_hit = t_mid;
        side_face = true;
    }

    /* Distance fade — clouds soften toward the horizon so the slab
     * doesn't show a hard ring. */
    float dist_fade = 1.0f - (t_hit * (1.0f / CLOUD_MAX_T));
    if (dist_fade < 0.10f) return sky_c;
    if (dist_fade > 1.0f) dist_fade = 1.0f;

    /* Cloud colour: light grey by default, warm-orange at twilight. */
    int cr = 230, cg = 230, cb = 240;
    if (s_sun_y < 0.3f && s_sun_y > -0.3f) {
        float glow = 1.0f - fabsf(s_sun_y) / 0.3f;
        cr = (int)(cr + (255 - cr) * glow);
        cg = (int)(cg + (150 - cg) * glow);
        cb = (int)(cb + ( 90 - cb) * glow);
    }
    /* Side walls render slightly darker than the bottom face so the
     * 3D shape reads from below. */
    if (side_face) {
        cr = (cr * 6) >> 3;
        cg = (cg * 6) >> 3;
        cb = (cb * 6) >> 3;
    }

    int sr = (sky_c >> 11) & 0x1F;
    int sg = (sky_c >>  5) & 0x3F;
    int sb =  sky_c        & 0x1F;
    int alpha = (int)(dist_fade * 220.0f);
    int r = sr + (((cr >> 3) - sr) * alpha) / 256;
    int g = sg + (((cg >> 2) - sg) * alpha) / 256;
    int b = sb + (((cb >> 3) - sb) * alpha) / 256;
    if (r > 31) r = 31;
    if (g > 63) g = 63;
    if (b > 31) b = 31;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

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
                out = cloud_overlay(out, cam->pos, dir);
                /* zbuf sky = far sentinel (255 default). */
            } else {
                const uint16_t *tex = craft_block_texture(h.blk, h.face);
                int tu = (int)(h.u * CRAFT_TEX_SIZE);
                int tv = (int)(h.v * CRAFT_TEX_SIZE);
                if (tu < 0) tu = 0; else if (tu >= CRAFT_TEX_SIZE) tu = CRAFT_TEX_SIZE - 1;
                if (tv < 0) tv = 0; else if (tv >= CRAFT_TEX_SIZE) tv = CRAFT_TEX_SIZE - 1;
                uint16_t c = tex[tv * CRAFT_TEX_SIZE + tu];
                /* Sky vs cave brightness, with torch overlay.
                 *  - Air cell adjacent to face is sky-exposed → use
                 *    the day/night-dimmed s_face_shade_lit table so
                 *    the surface tracks the sun.
                 *  - Air cell is buried under at least one solid
                 *    cell (a cave or under a roof) → use a permanent
                 *    "cave dim" base independent of time of day.
                 *  - If a torch reaches the air cell, floor the
                 *    final brightness so the torch is visible. */
                /* Depth below sky uses the cell's OWN column —
                 * preserves tree shadow shape (the previous 3×3
                 * neighbour min pulled shadow edges into full sky
                 * and made shadows look narrow). The horizontal
                 * neighbour scan still applies but only as a "cave
                 * mouth lift": when our column is well below sky
                 * (depth ≥ 4 = inside a cave) AND a neighbour
                 * column has open sky at our Y, lift to depth 2
                 * (shallow-shadow tier). This brightens cave
                 * entrances without affecting tree shadows whose
                 * own column is only slightly buried. */
                int face_shade_v;
                int lx = h.fx - craft_world_origin_x;
                int lz = h.fz - craft_world_origin_z;
                int eff_depth;
                if ((unsigned)lx >= CRAFT_WORLD_X || (unsigned)lz >= CRAFT_WORLD_Z) {
                    eff_depth = -1;  /* off-window = sky */
                } else {
                    int own_sh = craft_world_skyheight[lz * CRAFT_WORLD_X + lx];
                    eff_depth = own_sh - h.fy;
                    if (eff_depth >= 4) {
                        /* Look for a sky-open neighbour at our Y to
                         * lift cave-mouth darkness. Capped at 2 so
                         * we never get full sky brightness from a
                         * neighbour — only relief, not exposure. */
                        for (int dz = -1; dz <= 1; dz++) {
                            int nlz = lz + dz;
                            if ((unsigned)nlz >= CRAFT_WORLD_Z) continue;
                            for (int dx = -1; dx <= 1; dx++) {
                                int nlx = lx + dx;
                                if ((unsigned)nlx >= CRAFT_WORLD_X) continue;
                                int n_sh = craft_world_skyheight[nlz * CRAFT_WORLD_X + nlx];
                                if (n_sh < h.fy && eff_depth > 2) eff_depth = 2;
                            }
                        }
                    }
                }
                if (eff_depth <= 0) {
                    face_shade_v = s_face_shade_lit[h.face];
                } else {
                    /* Shadow falloff. Each tier is a fraction of
                     * current sky brightness so shadows fade with the
                     * sun (under a tree at midnight is still dark).
                     * Tiers extended so trees with tall canopies
                     * (ground 4-6 below leaves) stay in the bright
                     * shadow band rather than dropping to cave. */
                    int shade_factor;
                    if      (eff_depth <= 2) shade_factor = 180;  /* tree leaf shadow */
                    else if (eff_depth <= 5) shade_factor = 130;  /* under canopy floor */
                    else if (eff_depth <= 9) shade_factor = 70;   /* upper cave / deep shade */
                    else                     shade_factor = 0;    /* deep cave */
                    int shaded = (int)s_face_shade_lit[h.face] * shade_factor >> 8;
                    /* Deep-cave floor: constant ~16 % so caves never
                     * go fully black during daylight and stay only
                     * mildly darker at night. */
                    int deep_cave = ((int)face_shade[h.face] * 40) >> 8;
                    face_shade_v = shaded > deep_cave ? shaded : deep_cave;
                }
                /* Torch light — gradient floor. Level 0 = no torch
                 * reaches here; 1/2/3 give progressively brighter
                 * minimums. The cave/sky base above still wins if
                 * it's already brighter than the torch floor. */
                int torch_level = craft_world_light_level(h.fx, h.fy, h.fz);
                if (torch_level > 0) {
                    static const int TORCH_FLOOR[4] = { 0, 110, 165, 220 };
                    int floor_v = TORCH_FLOOR[torch_level];
                    if (face_shade_v < floor_v) face_shade_v = floor_v;
                }
                c = shade(c, face_shade_v);

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
        /* Light grey — still noticeably brighter than the dark-grey
         * break-edges (30,30,30) so the face reads as "active for
         * placement", but no longer the eye-grabbing near-white that
         * the earlier 240,240,200 was. */
        uint16_t place_col = rgb565(140, 140, 150);
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

/* --- Held-item viewport ------------------------------------------- *
 *
 * Renders the player's currently-held item into a small fixed
 * viewport at the bottom-right of the framebuffer using the same
 * per-pixel multi-cuboid pipeline as craft_mobs_render — only with a
 * virtual near-camera locked to the item's local frame instead of a
 * projected world AABB.
 *
 * No z-buffer interaction: the held item always overdraws whatever
 * was there. ~50 × 40 px × ~10 cuboid ray tests = a few thousand FMA
 * per frame, single-digit % of one core.
 */

#define HELD_VP_W      70
#define HELD_VP_H      56
#define HELD_VP_X0     (CRAFT_FB_W - HELD_VP_W)   /* 58 */
#define HELD_VP_Y0     (CRAFT_FB_H - HELD_VP_H)   /* 72 */
/* Camera sits this far in -Z from the model origin; the near-camera
 * FOV is wide enough that the model's ~0.5 m envelope fills the
 * viewport without clipping at idle. */
#define HELD_CAM_BACK  0.48f

/* Local ray-vs-AABB slab intersect — independent copy from
 * craft_mobs.c (which keeps its own static for the mob renderer) so
 * we don't have to expose either as global. Face id matches the mob
 * renderer's convention so we can reuse held_face_shade[]. */
static inline bool held_ray_aabb(float ox, float oy, float oz,
                                 float dx, float dy, float dz,
                                 float bminx, float bminy, float bminz,
                                 float bmaxx, float bmaxy, float bmaxz,
                                 float *t_out, int *face_out) {
    float t_near = -1e30f, t_far = 1e30f;
    int   nf = -1;
    if (dx > -1e-6f && dx < 1e-6f) {
        if (ox < bminx || ox > bmaxx) return false;
    } else {
        float inv = 1.0f / dx;
        float t1 = (bminx - ox) * inv;
        float t2 = (bmaxx - ox) * inv;
        int near_face = (dx > 0) ? 1 : 0;
        if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; near_face ^= 1; }
        if (t1 > t_near) { t_near = t1; nf = near_face; }
        if (t2 < t_far)  t_far  = t2;
        if (t_near > t_far) return false;
    }
    if (dy > -1e-6f && dy < 1e-6f) {
        if (oy < bminy || oy > bmaxy) return false;
    } else {
        float inv = 1.0f / dy;
        float t1 = (bminy - oy) * inv;
        float t2 = (bmaxy - oy) * inv;
        int near_face = (dy > 0) ? 3 : 2;
        if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; near_face ^= 1; }
        if (t1 > t_near) { t_near = t1; nf = near_face; }
        if (t2 < t_far)  t_far  = t2;
        if (t_near > t_far) return false;
    }
    if (dz > -1e-6f && dz < 1e-6f) {
        if (oz < bminz || oz > bmaxz) return false;
    } else {
        float inv = 1.0f / dz;
        float t1 = (bminz - oz) * inv;
        float t2 = (bmaxz - oz) * inv;
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

/* Face shading for the held item — same convention as mobs:
 *   0=+X, 1=-X, 2=+Y (top, lit), 3=-Y (bottom, dark),
 *   4=+Z (back, dim), 5=-Z (front, bright-ish). */
static const uint16_t held_face_shade[6] = {
    220, 220, 256, 150, 200, 240
};

static inline uint16_t held_shade565(uint16_t c, int m) {
    int r = ((c >> 11) & 0x1F) * m >> 8;
    int g = ((c >>  5) & 0x3F) * m >> 8;
    int b = ( c        & 0x1F) * m >> 8;
    if (r > 31) r = 31;
    if (g > 63) g = 63;
    if (b > 31) b = 31;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

/* Centre-pixel sample of a block face texture — used to give the
 * held-cube path a representative colour per face without a full UV
 * sampler. */
static inline uint16_t held_face_color(BlockId blk, Face face) {
    const uint16_t *tex = craft_block_texture(blk, face);
    /* CRAFT_TEX_SIZE is 16; centre = (8,8) → index 8*16+8 = 136. */
    return tex[(CRAFT_TEX_SIZE / 2) * CRAFT_TEX_SIZE + (CRAFT_TEX_SIZE / 2)];
}

void craft_render_held_item(BlockId held, uint16_t *fb, float swing_t) {
    if (held == BLK_AIR) return;
    if (swing_t < 0.0f) swing_t = 0.0f;
    if (swing_t > 1.0f) swing_t = 1.0f;

    /* Resolve the model. Placeable blocks render as a 6-face tilted
     * cube; tools/weapons/bow/arrow render from the tool model
     * table. Non-handheld items (sticks, ingots) currently have no
     * model — skip them. */
    /* Tool model wins if one exists — even for placeables like BLK_TORCH
     * that would otherwise render as a flat-coloured cube. The block
     * cube path is the fallback for ordinary placeable blocks (dirt,
     * stone, planks etc.) that have no dedicated model. */
    CraftToolModel tm = craft_tool_model(held);
    bool is_block = (tm.n_parts == 0) && craft_block_placeable(held);
    int n_parts = 0;
    if (tm.n_parts > 0) {
        n_parts = tm.n_parts;
    } else if (is_block) {
        n_parts = 1;     /* one virtual cuboid, face colour resolved per pixel */
    } else {
        return;          /* non-handheld item (stick, ingot) — nothing to draw */
    }

    /* Block "cube" pseudo-model — single 0.36 m cube centred at the
     * origin. Per-face colour lookup happens after the slab test
     * (cheaper than 6 cuboid checks). */
    const float CUBE_HX = 0.18f, CUBE_HY = 0.18f, CUBE_HZ = 0.18f;

    /* Idle pose + swing pose. Even at idle we apply a small fixed
     * yaw + pitch so the item shows three faces instead of looking
     * like a flat sticker — exactly the same trick a vanilla
     * Minecraft hand-render uses. The swing then adds an extra
     * forward tilt and a downward dip on top of that.
     *
     * Conventions: + tilt around X tips the model's top toward the
     * camera; + yaw around Y swings the model's right side toward
     * the camera. We apply the inverse rotation to the ray so each
     * cuboid part stays axis-aligned for the slab test (the part
     * positions/sizes never change). */
    /* Idle pose — Minecraft-style hand: tool tip points up-left,
     * handle visible bottom-right. Yaw shows the right face, mild
     * positive pitch keeps the tip in screen-space view without
     * tipping the top toward the camera (that was the "stabbing
     * yourself" look). */
    const float IDLE_YAW   =  0.4200f;  /* ~24 deg — show right face */
    const float IDLE_PITCH =  0.1800f;  /* ~+10 deg — tip slightly up-out */
    /* Swing arc: yaw sweeps inward across the screen and pitch
     * pushes the tip DOWN+OUTWARD as if striking the world. Both
     * INCREASE on swing — the old code drove pitch the wrong way,
     * which made the tool tip whip back toward the player's face. */
    float yaw_rad   = IDLE_YAW   - 0.6000f * swing_t;
    float pitch_rad = IDLE_PITCH + 0.7000f * swing_t;
    float dip       =  0.05f * swing_t;
    float cos_p = cosf(pitch_rad), sin_p = sinf(pitch_rad);
    float cos_y = cosf(yaw_rad),   sin_y = sinf(yaw_rad);

    /* Virtual near-camera looking toward +Z from -HELD_CAM_BACK so
     * the model's -Z front faces the viewer. Wide FOV (~75 deg) so
     * the model fills the 50×40 viewport. */
    const float vp_tan_h = 0.85f;   /* tan(half horizontal fov) */
    const float vp_tan_v = vp_tan_h * (float)HELD_VP_H / (float)HELD_VP_W;
    const float ox = 0.0f, oy = 0.0f, oz = -HELD_CAM_BACK;

    for (int sy = 0; sy < HELD_VP_H; sy++) {
        int   py = HELD_VP_Y0 + sy;
        float ndc_y = -((float)(sy * 2 - HELD_VP_H + 1) / (float)HELD_VP_H);
        float vy = ndc_y * vp_tan_v;
        for (int sx = 0; sx < HELD_VP_W; sx++) {
            int   px = HELD_VP_X0 + sx;
            float ndc_x = ((float)(sx * 2 - HELD_VP_W + 1) / (float)HELD_VP_W);
            float vx = ndc_x * vp_tan_h;

            /* Ray dir from camera through pixel into model frame
             * (+Z forward). */
            float wdx = vx;
            float wdy = vy;
            float wdz = 1.0f;

            /* Apply inverse model transform to ray origin + dir. The
             * model is rotated by +yaw about Y, then +pitch about X,
             * then translated by +dip in Y. The inverse on the
             * camera ray is the reverse order with negated angles:
             *   1. translate ray origin by -dip in Y
             *   2. rotate by -pitch about X
             *   3. rotate by -yaw about Y
             * With everything zero this collapses to identity. */
            /* Step 1: undo translation (only origin shifts; dirs
             * are unaffected by translation). */
            float ax = ox;
            float ay = oy - dip;
            float az = oz;
            float dax = wdx, day = wdy, daz = wdz;
            /* Step 2: rotate by -pitch about X. With angle -p,
             *   y' =  cos·y + sin·z
             *   z' = -sin·y + cos·z   (using -p so sin flips sign) */
            float bx = ax;
            float by =  cos_p * ay + sin_p * az;
            float bz = -sin_p * ay + cos_p * az;
            float dbx = dax;
            float dby =  cos_p * day + sin_p * daz;
            float dbz = -sin_p * day + cos_p * daz;
            /* Step 3: rotate by -yaw about Y.
             *   x' =  cos·x - sin·z
             *   z' =  sin·x + cos·z */
            float lox =  cos_y * bx - sin_y * bz;
            float loy =  by;
            float loz =  sin_y * bx + cos_y * bz;
            float ldx =  cos_y * dbx - sin_y * dbz;
            float ldy =  dby;
            float ldz =  sin_y * dbx + cos_y * dbz;

            float    best_t = 1e30f;
            int      best_face = 0;
            uint16_t best_color = 0;

            if (is_block) {
                float t; int face;
                if (held_ray_aabb(lox, loy, loz, ldx, ldy, ldz,
                                  -CUBE_HX, -CUBE_HY, -CUBE_HZ,
                                   CUBE_HX,  CUBE_HY,  CUBE_HZ,
                                   &t, &face)) {
                    best_t = t;
                    best_face = face;
                    /* Map the 6 cuboid faces to a representative
                     * block texture face. The tilted view shows the
                     * front (-Z, FACE_NZ), the top (+Y, FACE_PY), and
                     * the right (+X, FACE_PX) by default. */
                    Face bf;
                    switch (face) {
                        case 2: bf = FACE_PY; break;   /* top */
                        case 3: bf = FACE_NY; break;   /* bottom */
                        case 0: bf = FACE_PX; break;   /* right */
                        case 1: bf = FACE_NX; break;   /* left */
                        case 4: bf = FACE_PZ; break;   /* back */
                        default: bf = FACE_NZ; break;  /* front */
                    }
                    best_color = held_face_color(held, bf);
                }
            } else {
                for (int p = 0; p < n_parts; p++) {
                    const CraftToolPart *part = &tm.parts[p];
                    float t; int face;
                    if (held_ray_aabb(lox, loy, loz, ldx, ldy, ldz,
                                      part->cx - part->hx,
                                      part->cy - part->hy,
                                      part->cz - part->hz,
                                      part->cx + part->hx,
                                      part->cy + part->hy,
                                      part->cz + part->hz,
                                      &t, &face)) {
                        if (t < best_t) {
                            best_t = t;
                            best_face = face;
                            best_color = part->color;
                        }
                    }
                }
            }

            if (best_t >= 1e29f) continue;
            uint16_t out = held_shade565(best_color, held_face_shade[best_face]);
            fb[py * CRAFT_FB_W + px] = out;
        }
    }
}

/*
 * ThumbyCraft — 3D torch render + orientation tracking.
 *
 * Each torch is two axis-aligned cuboids: a small brown stick and a
 * bright flame on top. Floor torches sit centred at the bottom of
 * the cell; wall torches are offset toward whichever face they were
 * mounted on. The cuboids are placed in mob-style local coords and
 * rendered via the same ray-AABB slab intersection the mob system
 * uses, so depth-test against craft_zbuf is "free".
 *
 * The orientation hash persists torch-mount face across window
 * shifts (the world cell only carries BLK_TORCH; orient comes from
 * here). Capacity is small — 256 slots — because torches are sparse
 * in typical play.
 */
#include "craft_torches.h"
#include "craft_world.h"
#include "craft_blocks.h"

#include <string.h>

CraftTorch craft_torches[CRAFT_MAX_TORCHES];
static int s_torch_count;

/* --- Orientation hash ------------------------------------------- */
#define ORIENT_HASH_SIZE 256
#define ORIENT_HASH_MASK (ORIENT_HASH_SIZE - 1)
typedef struct {
    int32_t wx, wz;
    int16_t wy;
    uint8_t orient;
    uint8_t flags;      /* bit 0 = occupied */
} OrientEntry;

static OrientEntry s_orient[ORIENT_HASH_SIZE];

static uint32_t orient_hash(int wx, int wy, int wz) {
    uint32_t h = (uint32_t)wx * 73856093u
               ^ (uint32_t)wy * 19349663u
               ^ (uint32_t)wz * 83492791u;
    h ^= h >> 16;
    return h;
}

static OrientEntry *orient_find(int wx, int wy, int wz, bool insert) {
    uint32_t h = orient_hash(wx, wy, wz);
    for (int probe = 0; probe < ORIENT_HASH_SIZE; probe++) {
        int idx = (h + probe) & ORIENT_HASH_MASK;
        OrientEntry *e = &s_orient[idx];
        if (e->flags & 1) {
            if (e->wx == wx && e->wy == wy && e->wz == wz) return e;
        } else if (insert) {
            return e;
        } else {
            return NULL;
        }
    }
    return NULL;
}

void craft_torches_record_orient(int wx, int wy, int wz, int face) {
    OrientEntry *e = orient_find(wx, wy, wz, true);
    if (!e) return;
    if (!(e->flags & 1)) {
        e->wx = wx; e->wz = wz; e->wy = (int16_t)wy;
        e->flags = 1;
    }
    e->orient = (uint8_t)face;
}

void craft_torches_forget_orient(int wx, int wy, int wz) {
    OrientEntry *e = orient_find(wx, wy, wz, false);
    if (e) {
        memset(e, 0, sizeof *e);
    }
}

static uint8_t orient_lookup(int wx, int wy, int wz) {
    OrientEntry *e = orient_find(wx, wy, wz, false);
    return e ? e->orient : FACE_PY;   /* default: floor torch */
}

/* --- Window scan ------------------------------------------------ */

void craft_torches_rebuild(void) {
    s_torch_count = 0;
    for (int i = 0; i < CRAFT_MAX_TORCHES; i++) craft_torches[i].alive = false;

    int ox = craft_world_origin_x;
    int oz = craft_world_origin_z;
    for (int lz = 0; lz < CRAFT_WORLD_Z; lz++) {
        for (int lx = 0; lx < CRAFT_WORLD_X; lx++) {
            for (int wy = 0; wy < CRAFT_WORLD_Y; wy++) {
                int idx = (wy * CRAFT_WORLD_Z + lz) * CRAFT_WORLD_X + lx;
                if (craft_world_blocks[idx] != BLK_TORCH) continue;
                if (s_torch_count >= CRAFT_MAX_TORCHES) goto done;
                CraftTorch *t = &craft_torches[s_torch_count++];
                t->alive  = true;
                t->wx     = lx + ox;
                t->wz     = lz + oz;
                t->wy     = (int16_t)wy;
                t->orient = orient_lookup(t->wx, t->wy, t->wz);
            }
        }
    }
done:
    return;
}

/* --- Cuboid model + transforms ----------------------------------- */
/* Local coords inside the torch cell (range 0..1 on each axis):
 *   floor torch (FACE_PY): centred stick at the bottom
 *   wall torch on face F:  stick offset to the wall, slightly above
 *                          mid-height; flame above stick
 *
 * Both cuboids share local Y axis. For wall mounts we just shift X/Z
 * toward the wall — no actual rotation — which reads cleanly at 128 px. */

typedef struct {
    float cx, cy, cz;   /* centre in cell-local coords (0..1) */
    float hx, hy, hz;   /* half-sizes */
    uint16_t color;
} TorchCuboid;

static void torch_parts(int orient, TorchCuboid out[2]) {
    /* Defaults — floor torch. */
    float sx = 0.5f, sz = 0.5f;
    float fx = 0.5f, fz = 0.5f;
    float sy = 0.25f, fy = 0.55f;     /* stick centre, flame centre */
    float sh = 0.25f;                  /* stick half-height */

    /* Wall-mount offsets: push everything toward the wall.
     * The orient value is the face of the *parent* block the player
     * aimed at; the torch attaches to that wall. */
    switch (orient) {
        case FACE_PX:   /* attached to a +X face → torch on -X wall of cell */
            sx = fx = 0.18f; sy = fy - 0.1f; break;
        case FACE_NX:
            sx = fx = 0.82f; sy = fy - 0.1f; break;
        case FACE_PZ:
            sz = fz = 0.18f; sy = fy - 0.1f; break;
        case FACE_NZ:
            sz = fz = 0.82f; sy = fy - 0.1f; break;
        case FACE_NY:   /* ceiling — render as floor for v1 */
        case FACE_PY:
        default:
            break;
    }

    /* Stick — brown. */
    out[0].cx = sx; out[0].cy = sy; out[0].cz = sz;
    out[0].hx = 0.06f; out[0].hy = sh; out[0].hz = 0.06f;
    out[0].color = rgb565(140, 95, 45);
    /* Flame — bright orange. Slightly bigger than stick. */
    out[1].cx = fx; out[1].cy = fy; out[1].cz = fz;
    out[1].hx = 0.09f; out[1].hy = 0.10f; out[1].hz = 0.09f;
    out[1].color = rgb565(255, 200, 60);
}

/* --- Picking ---------------------------------------------------- */

/* Ray-vs-AABB slab — same form as the mob picker. Returns nearest t
 * (in world-dir units) on hit. */
static bool ray_aabb(float ox, float oy, float oz,
                     float dx, float dy, float dz,
                     float bminx, float bminy, float bminz,
                     float bmaxx, float bmaxy, float bmaxz,
                     float *t_out) {
    float t_near = 0.0f, t_far = 1e30f;
    bool fail = false;
    float inv;
#define SLAB(o, d, mn, mx)                                          \
    do {                                                            \
        if (d > -1e-6f && d < 1e-6f) {                              \
            if (o < mn || o > mx) { fail = true; break; }           \
        } else {                                                    \
            inv = 1.0f / d;                                         \
            float t1 = (mn - o) * inv;                              \
            float t2 = (mx - o) * inv;                              \
            if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; }     \
            if (t1 > t_near) t_near = t1;                           \
            if (t2 < t_far)  t_far  = t2;                           \
            if (t_near > t_far) { fail = true; break; }             \
        }                                                           \
    } while (0)
    SLAB(ox, dx, bminx, bmaxx);
    if (!fail) SLAB(oy, dy, bminy, bmaxy);
    if (!fail) SLAB(oz, dz, bminz, bmaxz);
#undef SLAB
    if (fail) return false;
    if (t_near < 0.0f) return false;
    *t_out = t_near;
    return true;
}

int craft_torches_pick(const CraftCamera *cam, float max_dist) {
    float cy = cosf(cam->yaw),  sy = sinf(cam->yaw);
    float cp = cosf(cam->pitch), sp = sinf(cam->pitch);
    float dx = sy * cp, dy = sp, dz = cy * cp;

    int best = -1;
    float best_t = max_dist;
    for (int i = 0; i < CRAFT_MAX_TORCHES; i++) {
        CraftTorch *t = &craft_torches[i];
        if (!t->alive) continue;
        TorchCuboid parts[2];
        torch_parts(t->orient, parts);
        for (int p = 0; p < 2; p++) {
            float bminx = (float)t->wx + parts[p].cx - parts[p].hx;
            float bminy = (float)t->wy + parts[p].cy - parts[p].hy;
            float bminz = (float)t->wz + parts[p].cz - parts[p].hz;
            float bmaxx = (float)t->wx + parts[p].cx + parts[p].hx;
            float bmaxy = (float)t->wy + parts[p].cy + parts[p].hy;
            float bmaxz = (float)t->wz + parts[p].cz + parts[p].hz;
            float th;
            if (ray_aabb(cam->pos.x, cam->pos.y, cam->pos.z,
                         dx, dy, dz,
                         bminx, bminy, bminz,
                         bmaxx, bmaxy, bmaxz, &th)) {
                if (th < best_t) { best_t = th; best = i; }
            }
        }
    }
    return best;
}

/* --- Render ----------------------------------------------------- */

static inline uint16_t shade565(uint16_t c, int m) {
    int r = ((c >> 11) & 0x1F) * m >> 8;
    int g = ((c >>  5) & 0x3F) * m >> 8;
    int b = ( c        & 0x1F) * m >> 8;
    if (r > 31) r = 31;
    if (g > 63) g = 63;
    if (b > 31) b = 31;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

void craft_torches_render(const CraftCamera *cam, uint16_t *fb) {
    if (s_torch_count == 0) return;

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

    for (int i = 0; i < CRAFT_MAX_TORCHES; i++) {
        CraftTorch *t = &craft_torches[i];
        if (!t->alive) continue;

        /* Cull torches outside the visible distance. */
        float dxp = (float)t->wx + 0.5f - cam->pos.x;
        float dyp = (float)t->wy + 0.5f - cam->pos.y;
        float dzp = (float)t->wz + 0.5f - cam->pos.z;
        float zf = dxp * fwd.x + dyp * fwd.y + dzp * fwd.z;
        if (zf <= 0.05f) continue;
        float dist2 = dxp*dxp + dyp*dyp + dzp*dzp;
        if (dist2 > 50.0f * 50.0f) continue;

        TorchCuboid parts[2];
        torch_parts(t->orient, parts);

        /* Compute the screen bbox containing all 8 corners of both
         * parts' world-space AABBs. */
        float bmin_x = 1.0f, bmin_y = 1.0f, bmin_z = 1.0f;
        float bmax_x = 0.0f, bmax_y = 0.0f, bmax_z = 0.0f;
        for (int p = 0; p < 2; p++) {
            float lo_x = parts[p].cx - parts[p].hx;
            float lo_y = parts[p].cy - parts[p].hy;
            float lo_z = parts[p].cz - parts[p].hz;
            float hi_x = parts[p].cx + parts[p].hx;
            float hi_y = parts[p].cy + parts[p].hy;
            float hi_z = parts[p].cz + parts[p].hz;
            if (p == 0 || lo_x < bmin_x) bmin_x = lo_x;
            if (p == 0 || lo_y < bmin_y) bmin_y = lo_y;
            if (p == 0 || lo_z < bmin_z) bmin_z = lo_z;
            if (p == 0 || hi_x > bmax_x) bmax_x = hi_x;
            if (p == 0 || hi_y > bmax_y) bmax_y = hi_y;
            if (p == 0 || hi_z > bmax_z) bmax_z = hi_z;
        }

        int sx_min = CRAFT_FB_W, sx_max = -1;
        int sy_min = CRAFT_FB_H, sy_max = -1;
        for (int corner = 0; corner < 8; corner++) {
            float cx_off = (corner & 1) ? bmax_x : bmin_x;
            float cy_off = (corner & 2) ? bmax_y : bmin_y;
            float cz_off = (corner & 4) ? bmax_z : bmin_z;
            float cw_x = (float)t->wx + cx_off;
            float cw_y = (float)t->wy + cy_off;
            float cw_z = (float)t->wz + cz_off;
            float rx = cw_x - cam->pos.x;
            float ry = cw_y - cam->pos.y;
            float rz = cw_z - cam->pos.z;
            float zfc = rx * fwd.x + ry * fwd.y + rz * fwd.z;
            if (zfc <= 0.05f) continue;
            float xs = (rx * right.x + ry * right.y + rz * right.z) / zfc;
            float ys = (rx * up.x    + ry * up.y    + rz * up.z   ) / zfc;
            int sx = (int)(CRAFT_FB_W * 0.5f + xs * focal_h);
            int sy = (int)(CRAFT_FB_H * 0.5f - ys * focal_v);
            if (sx < sx_min) sx_min = sx;
            if (sx > sx_max) sx_max = sx;
            if (sy < sy_min) sy_min = sy;
            if (sy > sy_max) sy_max = sy;
        }
        sx_min--; sy_min--; sx_max++; sy_max++;
        if (sx_min < 0)            sx_min = 0;
        if (sy_min < 0)            sy_min = 0;
        if (sx_max >= CRAFT_FB_W)  sx_max = CRAFT_FB_W - 1;
        if (sy_max >= CRAFT_FB_H)  sy_max = CRAFT_FB_H - 1;
        if (sx_min > sx_max || sy_min > sy_max) continue;

        /* Camera position in torch-local frame (no rotation, just
         * subtract torch cell origin). */
        float lo_x = cam->pos.x - (float)t->wx;
        float lo_y = cam->pos.y - (float)t->wy;
        float lo_z = cam->pos.z - (float)t->wz;

        for (int sy = sy_min; sy <= sy_max; sy++) {
            float ndc_y = -((float)(sy * 2 - CRAFT_FB_H + 1) / (float)CRAFT_FB_H);
            float vy    = ndc_y * tan_h;
            for (int sx = sx_min; sx <= sx_max; sx++) {
                float ndc_x = ((float)(sx * 2 - CRAFT_FB_W + 1) / (float)CRAFT_FB_W);
                float vx    = ndc_x * tan_h;
                float wdx = fwd.x + right.x * vx + up.x * vy;
                float wdy = fwd.y + right.y * vx + up.y * vy;
                float wdz = fwd.z + right.z * vx + up.z * vy;

                float best_t = 1e30f;
                uint16_t best_color = 0;
                bool best_is_flame = false;
                for (int p = 0; p < 2; p++) {
                    float bminx = parts[p].cx - parts[p].hx;
                    float bmaxx = parts[p].cx + parts[p].hx;
                    float bminy = parts[p].cy - parts[p].hy;
                    float bmaxy = parts[p].cy + parts[p].hy;
                    float bminz = parts[p].cz - parts[p].hz;
                    float bmaxz = parts[p].cz + parts[p].hz;
                    float th;
                    if (ray_aabb(lo_x, lo_y, lo_z, wdx, wdy, wdz,
                                 bminx, bminy, bminz,
                                 bmaxx, bmaxy, bmaxz, &th)) {
                        if (th < best_t) {
                            best_t = th;
                            best_color = parts[p].color;
                            best_is_flame = (p == 1);
                        }
                    }
                }
                if (best_t >= 1e29f) continue;

                /* Convert hit t to world distance for z-test. */
                float dl2 = wdx*wdx + wdy*wdy + wdz*wdz;
                float dl  = (dl2 > 1.0001f) ? sqrtf(dl2) : 1.0f;
                float wdist = best_t * dl;
                int q = (int)(wdist * 255.0f / CRAFT_MAX_DIST_FOR_ZBUF);
                if (q < 0)   q = 0;
                if (q > 254) q = 254;
                int idx = sy * CRAFT_FB_W + sx;
                if (craft_zbuf[idx] <= (uint8_t)q) continue;

                /* Flame stays bright regardless of time of day; stick
                 * uses normal shading. */
                if (!best_is_flame) {
                    best_color = shade565(best_color, 220);
                }
                fb[idx] = best_color;
                craft_zbuf[idx] = (uint8_t)q;
            }
        }
    }
}

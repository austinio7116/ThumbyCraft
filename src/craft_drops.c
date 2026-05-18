/*
 * ThumbyCraft — dropped item entities (impl).
 *
 * Each drop renders as a small spinning cube using a per-drop yaw
 * applied to a single-cuboid model. Texture is the side face of the
 * dropped block sampled at the centre pixel (cheap colour).
 */
#include "craft_drops.h"
#include "craft_world.h"
#include "craft_audio.h"
#include "craft_tool_models.h"

#include <math.h>
#include <string.h>

CraftDrop craft_drops[CRAFT_MAX_DROPS];

void craft_drops_init(void) {
    memset(craft_drops, 0, sizeof craft_drops);
}

int craft_drops_live_count(void) {
    int n = 0;
    for (int i = 0; i < CRAFT_MAX_DROPS; i++) if (craft_drops[i].alive) n++;
    return n;
}

bool craft_drops_first_pos(Vec3 *out) {
    for (int i = 0; i < CRAFT_MAX_DROPS; i++) {
        if (craft_drops[i].alive) {
            if (out) *out = craft_drops[i].pos;
            return true;
        }
    }
    return false;
}

void craft_drops_spawn(BlockId blk, Vec3 pos) {
    int free_idx = -1;
    int oldest_idx = 0;
    float oldest_age = -1.0f;
    for (int i = 0; i < CRAFT_MAX_DROPS; i++) {
        if (!craft_drops[i].alive) {
            if (free_idx < 0) free_idx = i;
            continue;
        }
        if (craft_drops[i].age > oldest_age) {
            oldest_age = craft_drops[i].age;
            oldest_idx = i;
        }
    }
    /* Prefer a free slot. If the pool is full, evict the oldest drop
     * — otherwise a skeleton's bow/arrow can vanish into a pool
     * already saturated with stale missed-arrow drops. */
    int idx = (free_idx >= 0) ? free_idx : oldest_idx;
    CraftDrop *d = &craft_drops[idx];
    d->alive = true;
    d->blk   = blk;
    d->pos   = pos;
    d->age   = 0.0f;
    d->spin  = 0.0f;
}

void craft_drops_tick(float dt, CraftPlayer *p) {
    if (dt > 0.1f) dt = 0.1f;
    for (int i = 0; i < CRAFT_MAX_DROPS; i++) {
        CraftDrop *d = &craft_drops[i];
        if (!d->alive) continue;
        d->age  += dt;
        d->spin += dt * 2.5f;
        if (d->spin > 6.2831853f) d->spin -= 6.2831853f;
        /* Despawn after lifetime. */
        if (d->age >= CRAFT_DROP_LIFETIME) {
            d->alive = false;
            continue;
        }
        /* Pickup check — player AABB centre to drop centre. */
        float dx = p->cam.pos.x - d->pos.x;
        float dy = (p->cam.pos.y - 0.8f) - d->pos.y;   /* feet-to-drop */
        float dz = p->cam.pos.z - d->pos.z;
        float dist2 = dx*dx + dy*dy + dz*dz;
        if (dist2 < CRAFT_DROP_PICKUP_DIST * CRAFT_DROP_PICKUP_DIST) {
            /* Auto-add to hotbar if there's an empty slot AND the
             * player doesn't already have it slotted. Always credit
             * inventory. */
            p->inventory[d->blk]++;
            bool present = false;
            for (int s = 0; s < CRAFT_HOTBAR_SLOTS; s++)
                if (p->hotbar[s] == d->blk) { present = true; break; }
            if (!present) {
                for (int s = 0; s < CRAFT_HOTBAR_SLOTS; s++) {
                    if (p->hotbar[s] == BLK_AIR) {
                        p->hotbar[s] = d->blk;
                        break;
                    }
                }
            }
            craft_audio_pickaxe_ting();
            d->alive = false;
        }
    }
}

/* Per-block representative colour for the drop sprite. Centre-pixel
 * texture sampling is unreliable — sparse-silhouette blocks like
 * the bow have a dark background colour at (8, 8) that vanishes
 * against grass. Hard-coded bright tones are a tiny table that
 * always reads. */
static inline uint16_t drop_colour(BlockId blk) {
    switch (blk) {
        case BLK_BOW:         return 0xFCC0;   /* warm wood */
        case BLK_ARROW:       return 0xDEDA;   /* shaft silver */
        case BLK_STICK:       return 0xAAA9;   /* light brown */
        case BLK_IRON_INGOT:  return 0xEF7D;   /* iron grey */
        case BLK_COBBLE:      return 0x8410;   /* mid grey */
        case BLK_DIRT:        return 0x8AA3;   /* dirt brown */
        case BLK_GRASS:       return 0x6606;   /* leaf green */
        case BLK_STONE:       return 0xAD55;   /* stone grey */
        case BLK_WOOD:        return 0x8AA3;   /* trunk */
        case BLK_PLANK:       return 0xCC8A;   /* plank tan */
        case BLK_COAL_ORE:    return 0x4208;   /* dark grey */
        case BLK_IRON_ORE:    return 0xC638;   /* warm grey */
        case BLK_SAND:        return 0xEF1A;   /* sand */
        default:              return 0xFFFF;   /* bright fallback */
    }
}

/* Render a single drop as either:
 *   - For tools/items with a tool model: a billboard stack of each
 *     cuboid part, projected separately and painted in the part's
 *     colour. Back-to-front order so foreground parts overdraw
 *     background ones. Gives a recognisable 3D silhouette without a
 *     full per-pixel raycast.
 *   - For plain blocks: a small filled square in the block's
 *     representative colour with a darker right + bottom edge for
 *     fake depth.
 *
 * Spin animation: yaw rotation about Y so the drop visibly turns
 * over time, identical to the held-item idle pose convention.
 */
static void render_drop_tool(const CraftCamera *cam, uint16_t *fb,
                             const CraftDrop *d, CraftToolModel m) {
    /* Drop centre with bob. */
    float bob = sinf(d->age * 4.0f) * 0.06f;
    float cs = cosf(d->spin), sn = sinf(d->spin);
    /* Distance to drop centre (for screen-size scaling). */
    float ddx = d->pos.x - cam->pos.x;
    float ddy = (d->pos.y + 0.35f + bob) - cam->pos.y;
    float ddz = d->pos.z - cam->pos.z;
    float drop_dist = sqrtf(ddx*ddx + ddy*ddy + ddz*ddz);
    if (drop_dist > CRAFT_MAX_DIST_FOR_ZBUF) return;
    /* Items in flight are ~0.5 m envelope — scale to ~0.35 m drop
     * size by shrinking the model around its origin. */
    const float SCALE = 0.7f;

    /* Project each part, gather visible ones. */
    typedef struct { uint16_t col; int sx, sy; int half; uint8_t depth; float dist; } Part;
    Part parts[16];
    int n_visible = 0;
    int n_parts = m.n_parts;
    if (n_parts > 16) n_parts = 16;
    for (int i = 0; i < n_parts; i++) {
        const CraftToolPart *pp = &m.parts[i];
        Vec3 wp = {
            d->pos.x + (cs * pp->cx - sn * pp->cz) * SCALE,
            d->pos.y + 0.35f + bob + pp->cy * SCALE,
            d->pos.z + (sn * pp->cx + cs * pp->cz) * SCALE,
        };
        int sx, sy; uint8_t depth; float dist;
        if (!craft_render_project(cam, wp, &sx, &sy, &depth, &dist)) continue;
        if (dist > CRAFT_MAX_DIST_FOR_ZBUF) continue;
        if (sx < -8 || sx >= CRAFT_FB_W + 8) continue;
        if (sy < -8 || sy >= CRAFT_FB_H + 8) continue;
        /* Screen half = world half × (pixels per metre at this dist).
         * Pixels per metre ≈ FB_W / (2 * tan(fov/2) * dist). */
        float half_m = fmaxf(fmaxf(pp->hx, pp->hy), pp->hz) * SCALE;
        float screen_half = half_m * (CRAFT_FB_W * 0.5f) / (0.7f * (dist + 0.01f));
        int half = (int)(screen_half + 0.5f);
        if (half < 1) half = 1;
        if (half > 8) half = 8;
        parts[n_visible].col   = pp->color;
        parts[n_visible].sx    = sx;
        parts[n_visible].sy    = sy;
        parts[n_visible].half  = half;
        parts[n_visible].depth = depth;
        parts[n_visible].dist  = dist;
        n_visible++;
    }
    /* Insertion sort: far parts first so near parts overdraw. */
    for (int i = 1; i < n_visible; i++) {
        Part v = parts[i]; int j = i;
        while (j > 0 && parts[j-1].dist < v.dist) { parts[j] = parts[j-1]; j--; }
        parts[j] = v;
    }
    /* Paint each part as a filled rect, z-tested against terrain. */
    for (int i = 0; i < n_visible; i++) {
        Part *pr = &parts[i];
        int sxa = pr->sx - pr->half, sxb = pr->sx + pr->half;
        int sya = pr->sy - pr->half, syb = pr->sy + pr->half;
        if (sxa < 0) sxa = 0;
        if (sya < 0) sya = 0;
        if (sxb >= CRAFT_FB_W) sxb = CRAFT_FB_W - 1;
        if (syb >= CRAFT_FB_H) syb = CRAFT_FB_H - 1;
        for (int yy = sya; yy <= syb; yy++) {
            for (int xx = sxa; xx <= sxb; xx++) {
                int idx = yy * CRAFT_FB_W + xx;
                if (craft_zbuf[idx] <= pr->depth) continue;
                fb[idx] = pr->col;
                craft_zbuf[idx] = pr->depth;
            }
        }
    }
}

static void render_drop_block(const CraftCamera *cam, uint16_t *fb,
                              const CraftDrop *d) {
    float bob = sinf(d->age * 4.0f) * 0.06f;
    Vec3 p = (Vec3){ d->pos.x, d->pos.y + 0.35f + bob, d->pos.z };
    int sx, sy;
    uint8_t depth;
    float dist;
    if (!craft_render_project(cam, p, &sx, &sy, &depth, &dist)) return;
    if (dist > CRAFT_MAX_DIST_FOR_ZBUF) return;
    if (sx < 0 || sx >= CRAFT_FB_W) return;
    if (sy < 0 || sy >= CRAFT_FB_H) return;
    int half = (int)(40.0f / (dist + 0.5f));
    if (half < 2) half = 2;
    if (half > 6) half = 6;
    uint16_t c = drop_colour(d->blk);
    /* Pre-compute a darker shade for the cube's "shadowed" right +
     * bottom edges so the player reads the square as a cube. */
    int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
    uint16_t c_dark = (uint16_t)(((r * 5 / 8) << 11) | ((g * 5 / 8) << 5) | (b * 5 / 8));
    int sxa = sx - half, sxb = sx + half;
    int sya = sy - half, syb = sy + half;
    if (sxa < 0) sxa = 0;
    if (sya < 0) sya = 0;
    if (sxb >= CRAFT_FB_W) sxb = CRAFT_FB_W - 1;
    if (syb >= CRAFT_FB_H) syb = CRAFT_FB_H - 1;
    for (int yy = sya; yy <= syb; yy++) {
        for (int xx = sxa; xx <= sxb; xx++) {
            int idx = yy * CRAFT_FB_W + xx;
            if (craft_zbuf[idx] <= depth) continue;
            /* Right + bottom edge get dark shade; rest gets base. */
            bool edge = (xx >= sxb - 1) || (yy >= syb - 1);
            fb[idx] = edge ? c_dark : c;
            craft_zbuf[idx] = depth;
        }
    }
}

void craft_drops_render(const CraftCamera *cam, uint16_t *fb) {
    for (int i = 0; i < CRAFT_MAX_DROPS; i++) {
        CraftDrop *d = &craft_drops[i];
        if (!d->alive) continue;
        CraftToolModel m = craft_tool_model(d->blk);
        if (m.n_parts > 0) {
            render_drop_tool(cam, fb, d, m);
        } else {
            render_drop_block(cam, fb, d);
        }
    }
}

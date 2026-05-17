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

#include <math.h>
#include <string.h>

CraftDrop craft_drops[CRAFT_MAX_DROPS];

void craft_drops_init(void) {
    memset(craft_drops, 0, sizeof craft_drops);
}

void craft_drops_spawn(BlockId blk, Vec3 pos) {
    for (int i = 0; i < CRAFT_MAX_DROPS; i++) {
        if (craft_drops[i].alive) continue;
        CraftDrop *d = &craft_drops[i];
        d->alive = true;
        d->blk   = blk;
        d->pos   = pos;
        d->age   = 0.0f;
        d->spin  = 0.0f;
        return;
    }
    /* Pool full — silently drop. */
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
            d->alive = false;
        }
    }
}

/* Cheap render — for each drop, project its centre + a small spin
 * offset to screen space and stamp a small textured square sampled
 * from the block's side texture. Not a full cuboid raycast (the
 * drops are tiny on screen and a full per-pixel cuboid test would
 * be wasteful). */
void craft_drops_render(const CraftCamera *cam, uint16_t *fb) {
    for (int i = 0; i < CRAFT_MAX_DROPS; i++) {
        CraftDrop *d = &craft_drops[i];
        if (!d->alive) continue;
        /* Y bob — sin wave over 1.5s. */
        float bob = sinf(d->age * 4.0f) * 0.06f;
        Vec3 p = (Vec3){ d->pos.x, d->pos.y + 0.35f + bob, d->pos.z };
        int sx, sy;
        uint8_t depth;
        float dist;
        if (!craft_render_project(cam, p, &sx, &sy, &depth, &dist)) continue;
        if (dist > CRAFT_MAX_DIST_FOR_ZBUF) continue;
        /* Approximate screen radius from world size 0.25 m. */
        int half = (int)(60.0f / (dist + 0.5f));   /* heuristic shrink */
        if (half < 1) half = 1;
        if (half > 6) half = 6;
        const uint16_t *tex = craft_block_texture(d->blk, FACE_PZ);
        /* Sample a representative colour from the centre of the side
         * texture. */
        uint16_t c = tex[(CRAFT_TEX_SIZE / 2) * CRAFT_TEX_SIZE + (CRAFT_TEX_SIZE / 2)];
        /* Faux 3D: draw a small filled square with a darker right + top
         * edge so the player reads it as a cube even at low res. */
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
                fb[idx] = c;
                craft_zbuf[idx] = depth;
            }
        }
        /* Dark outline so the drop pops against terrain. */
        uint16_t outline = 0x18C3;   /* dim grey */
        for (int xx = sxa; xx <= sxb; xx++) {
            int top_idx = sya * CRAFT_FB_W + xx;
            int bot_idx = syb * CRAFT_FB_W + xx;
            if (craft_zbuf[top_idx] > depth) fb[top_idx] = outline;
            if (craft_zbuf[bot_idx] > depth) fb[bot_idx] = outline;
        }
        for (int yy = sya; yy <= syb; yy++) {
            int lt_idx = yy * CRAFT_FB_W + sxa;
            int rt_idx = yy * CRAFT_FB_W + sxb;
            if (craft_zbuf[lt_idx] > depth) fb[lt_idx] = outline;
            if (craft_zbuf[rt_idx] > depth) fb[rt_idx] = outline;
        }
    }
}

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
#include "craft_render.h"        /* craft_render_brightness_q8 for sprite lighting */
#include "craft_tool_models.h"   /* CraftToolPart — shared cuboid struct */

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

int craft_torches_lookup_orient(int wx, int wy, int wz) {
    OrientEntry *e = orient_find(wx, wy, wz, false);
    return e ? (int)e->orient : (int)FACE_PY;
}

/* --- Save persistence -------------------------------------------- */

static void put_u16_le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t) v;
    p[1] = (uint8_t)(v >> 8);
}
static uint16_t get_u16_le(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static void put_i32_le(uint8_t *p, int32_t v) {
    p[0] = (uint8_t) v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
static int32_t get_i32_le(const uint8_t *p) {
    return (int32_t)((uint32_t)p[0]
                   | ((uint32_t)p[1] <<  8)
                   | ((uint32_t)p[2] << 16)
                   | ((uint32_t)p[3] << 24));
}

size_t craft_torches_orient_serialise(uint8_t *out, size_t out_cap) {
    /* Count occupied entries first so we can stamp the header. */
    uint16_t n = 0;
    for (int i = 0; i < ORIENT_HASH_SIZE; i++) {
        if (s_orient[i].flags & 1) n++;
    }
    size_t need = 2 + (size_t)n * CRAFT_ORIENTS_BLOB_PER_ENTRY;
    if (need > out_cap) return 0;
    put_u16_le(out, n);
    uint8_t *p = out + 2;
    for (int i = 0; i < ORIENT_HASH_SIZE; i++) {
        const OrientEntry *e = &s_orient[i];
        if (!(e->flags & 1)) continue;
        put_i32_le(p + 0, e->wx);
        put_i32_le(p + 4, e->wz);
        p[8] = (uint8_t)(e->wy & 0xFF);
        p[9] = e->orient;
        p += CRAFT_ORIENTS_BLOB_PER_ENTRY;
    }
    return need;
}

bool craft_torches_orient_deserialise(const uint8_t *in, size_t in_n) {
    if (in_n < 2) return false;
    memset(s_orient, 0, sizeof s_orient);
    uint16_t n = get_u16_le(in);
    if (n > ORIENT_HASH_SIZE) n = ORIENT_HASH_SIZE;
    if (in_n < (size_t)(2 + n * CRAFT_ORIENTS_BLOB_PER_ENTRY)) return false;
    const uint8_t *p = in + 2;
    for (uint16_t i = 0; i < n; i++) {
        int32_t wx = get_i32_le(p + 0);
        int32_t wz = get_i32_le(p + 4);
        /* wy was serialised as a signed byte truncation of int16; it
         * always fits in [0, CRAFT_WORLD_Y), so unsigned widen. */
        int wy = (int)p[8];
        uint8_t orient = p[9];
        OrientEntry *e = orient_find((int)wx, wy, (int)wz, true);
        if (e) {
            e->wx = wx; e->wz = wz; e->wy = (int16_t)wy;
            e->orient = orient;
            e->flags  = 1;
        }
        p += CRAFT_ORIENTS_BLOB_PER_ENTRY;
    }
    return true;
}

static uint8_t orient_lookup(int wx, int wy, int wz) {
    OrientEntry *e = orient_find(wx, wy, wz, false);
    return e ? e->orient : FACE_PY;   /* default: floor torch */
}

/* --- Window scan ------------------------------------------------ */

/* Block-id → sprite kind (+1, so 0 = "not a sprite cell"). Replaces a
 * per-cell 18-way if/else over all 64³ cells, which dominated the
 * window-shift cost — almost every cell is air/stone and fell through
 * every comparison. One array read per cell instead. */
static const uint8_t s_sprite_kind1[256] = {
    [BLK_TORCH]             = TORCH_KIND_TORCH + 1,
    [BLK_LILY_PAD]          = TORCH_KIND_LILY_PAD + 1,
    /* Vine, ladder, pressure pad, redstone wire, doors + trapdoors are
     * now drawn by the DDA cutout paths (craft_render.c CROSS/PANEL),
     * not this post-pass — intentionally NOT registered here. Orient /
     * wire-connect are still read back via craft_torches_*. */
    [BLK_PISTON_OFF]        = TORCH_KIND_PISTON_OFF + 1,
    [BLK_PISTON_ON]         = TORCH_KIND_PISTON_ON + 1,
    [BLK_STICKY_PISTON_OFF] = TORCH_KIND_PISTON_OFF + 1,
    [BLK_STICKY_PISTON_ON]  = TORCH_KIND_PISTON_ON + 1,
    [BLK_PISTON_ARM]        = TORCH_KIND_PISTON_ARM + 1,
    [BLK_LEVER_OFF]         = TORCH_KIND_LEVER_OFF + 1,
    [BLK_LEVER_ON]          = TORCH_KIND_LEVER_ON + 1,
};

/* Wire-connection mask for a wire at local (lx,wy,lz). */
static uint8_t wire_connect_mask(int lx, int wy, int lz) {
    static const int ndx[4] = { 1, -1, 0,  0 };
    static const int ndz[4] = { 0,  0, 1, -1 };
    uint8_t connect = 0;
    for (int d = 0; d < 4; d++) {
        int nlx = lx + ndx[d], nlz = lz + ndz[d];
        if ((unsigned)nlx >= CRAFT_WORLD_X) continue;
        if ((unsigned)nlz >= CRAFT_WORLD_Z) continue;
        BlockId nb = (BlockId)craft_world_blocks[(wy * CRAFT_WORLD_Z + nlz) * CRAFT_WORLD_X + nlx];
        if (nb == BLK_REDSTONE_WIRE || nb == BLK_REDSTONE_WIRE_ON ||
            nb == BLK_LEVER_OFF || nb == BLK_LEVER_ON ||
            nb == BLK_DOOR_OFF || nb == BLK_DOOR_ON ||
            nb == BLK_TRAPDOOR_OFF || nb == BLK_TRAPDOOR_ON ||
            nb == BLK_PISTON_OFF || nb == BLK_PISTON_ON ||
            nb == BLK_STICKY_PISTON_OFF || nb == BLK_STICKY_PISTON_ON ||
            nb == BLK_DISPENSER || nb == BLK_DISPENSER_ON ||
            nb == BLK_TARGET || nb == BLK_TARGET_ON ||
            nb == BLK_TNT || nb == BLK_TNT_FUSED ||
            nb == BLK_PRESSURE_PAD || nb == BLK_REDSTONE_BLOCK)
            connect |= (1u << d);
    }
    return connect;
}

/* World-coord wrapper so the DDA wire renderer can query the same
 * connection mask. Bits: 0=+X 1=-X 2=+Z 3=-Z. */
uint8_t craft_torches_wire_connect_at(int wx, int wy, int wz) {
    return wire_connect_mask(wx - craft_world_origin_x, wy, wz - craft_world_origin_z);
}

static void add_sprite_cell(int lx, int wy, int lz, uint8_t kind) {
    if (s_torch_count >= CRAFT_MAX_TORCHES) return;
    CraftTorch *t = &craft_torches[s_torch_count++];
    t->alive   = true;
    t->wx      = lx + craft_world_origin_x;
    t->wz      = lz + craft_world_origin_z;
    t->wy      = (int16_t)wy;
    t->kind    = kind;
    t->connect = 0;
    if (kind == TORCH_KIND_TORCH || kind == TORCH_KIND_LADDER ||
        kind == TORCH_KIND_VINE ||
        kind == TORCH_KIND_DOOR_CLOSED  || kind == TORCH_KIND_DOOR_OPEN ||
        kind == TORCH_KIND_TRAPDOOR_CLOSED || kind == TORCH_KIND_TRAPDOOR_OPEN ||
        kind == TORCH_KIND_PISTON_OFF || kind == TORCH_KIND_PISTON_ON ||
        kind == TORCH_KIND_PISTON_ARM ||
        kind == TORCH_KIND_LEVER_OFF || kind == TORCH_KIND_LEVER_ON) {
        t->orient = orient_lookup(t->wx, t->wy, t->wz);
    } else {
        t->orient = (uint8_t)FACE_PY;
    }
    if (kind == TORCH_KIND_WIRE || kind == TORCH_KIND_WIRE_ON)
        t->connect = wire_connect_mask(lx, wy, lz);
}

/* Scan a local region, appending its sprite cells. pass 0 = functional
 * only, 1 = decorative only — preserving the overflow priority split. */
static void scan_sprites_region(int lx0, int lx1, int lz0, int lz1, int pass) {
    if (lx0 < 0) lx0 = 0; if (lx1 > CRAFT_WORLD_X) lx1 = CRAFT_WORLD_X;
    if (lz0 < 0) lz0 = 0; if (lz1 > CRAFT_WORLD_Z) lz1 = CRAFT_WORLD_Z;
    for (int wy = 0; wy < CRAFT_WORLD_Y; wy++)
        for (int lz = lz0; lz < lz1; lz++) {
            int base = (wy * CRAFT_WORLD_Z + lz) * CRAFT_WORLD_X;
            for (int lx = lx0; lx < lx1; lx++) {
                uint8_t k1 = s_sprite_kind1[craft_world_blocks[base + lx]];
                if (!k1) continue;
                uint8_t kind = (uint8_t)(k1 - 1);
                bool decorative = (kind == TORCH_KIND_VINE || kind == TORCH_KIND_LILY_PAD);
                if ((pass == 0) == decorative) continue;
                add_sprite_cell(lx, wy, lz, kind);
            }
        }
}

void craft_torches_rebuild(void) {
    s_torch_count = 0;
    for (int i = 0; i < CRAFT_MAX_TORCHES; i++) craft_torches[i].alive = false;
    scan_sprites_region(0, CRAFT_WORLD_X, 0, CRAFT_WORLD_Z, 0);
    scan_sprites_region(0, CRAFT_WORLD_X, 0, CRAFT_WORLD_Z, 1);
}

/* --- Incremental update for streaming shifts ---------------------- */
void craft_torches_drop_outside(void) {
    int ox = craft_world_origin_x, oz = craft_world_origin_z;
    int w = 0;
    for (int i = 0; i < s_torch_count; i++) {
        int lx = craft_torches[i].wx - ox, lz = craft_torches[i].wz - oz;
        if ((unsigned)lx < CRAFT_WORLD_X && (unsigned)lz < CRAFT_WORLD_Z) {
            if (w != i) craft_torches[w] = craft_torches[i];
            w++;
        }
    }
    for (int i = w; i < s_torch_count; i++) craft_torches[i].alive = false;
    s_torch_count = w;
}

void craft_torches_add_region(int lx0, int lx1, int lz0, int lz1) {
    scan_sprites_region(lx0, lx1, lz0, lz1, 0);
    scan_sprites_region(lx0, lx1, lz0, lz1, 1);
}

/* Remove list entries whose local cell falls in the region — paired
 * with add_region to cleanly REBUILD a strip's sprites (e.g. after a
 * streamed shift re-stamps features there) without duplicating the
 * ones already listed. */
void craft_torches_drop_region(int lx0, int lx1, int lz0, int lz1) {
    int ox = craft_world_origin_x, oz = craft_world_origin_z;
    int w = 0;
    for (int i = 0; i < s_torch_count; i++) {
        int lx = craft_torches[i].wx - ox, lz = craft_torches[i].wz - oz;
        if (lx >= lx0 && lx < lx1 && lz >= lz0 && lz < lz1) continue;  /* drop */
        if (w != i) craft_torches[w] = craft_torches[i];
        w++;
    }
    for (int i = w; i < s_torch_count; i++) craft_torches[i].alive = false;
    s_torch_count = w;
}

void craft_torches_refresh_connect(int lx0, int lx1, int lz0, int lz1) {
    int ox = craft_world_origin_x, oz = craft_world_origin_z;
    for (int i = 0; i < s_torch_count; i++) {
        if (craft_torches[i].kind != TORCH_KIND_WIRE &&
            craft_torches[i].kind != TORCH_KIND_WIRE_ON) continue;
        int lx = craft_torches[i].wx - ox, lz = craft_torches[i].wz - oz;
        if (lx >= lx0 && lx < lx1 && lz >= lz0 && lz < lz1)
            craft_torches[i].connect = wire_connect_mask(lx, craft_torches[i].wy, lz);
    }
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

static int wire_parts_n(bool powered, uint8_t connect, TorchCuboid *out) {
    /* Lifted ~10 cm above the floor of the cell so it reads as wire
     * sitting on TOP of the supporting block, not painted into the
     * surface. The thin slabs render via the post-pass so anything
     * below shows through cleanly. */
    uint16_t c = powered ? rgb565(255, 70, 50)
                          : rgb565(130, 35, 30);
    const float Y  = 0.10f;
    const float HY = 0.03f;
    const float W  = 0.06f;   /* half-width of the strip */
    const float CENTER_HALF = 0.06f;
    int n = 0;
    /* Always render a tiny central pad so isolated dust still
     * shows. */
    out[n].cx = 0.5f; out[n].cy = Y; out[n].cz = 0.5f;
    out[n].hx = CENTER_HALF; out[n].hy = HY; out[n].hz = CENTER_HALF;
    out[n].color = c;
    n++;
    /* Arm toward each connected neighbour. Half-length stops at
     * the central pad so adjacent arms join cleanly. */
    bool px = (connect & 0x1) != 0;
    bool nx = (connect & 0x2) != 0;
    bool pz = (connect & 0x4) != 0;
    bool nz = (connect & 0x8) != 0;
    if (px) {
        out[n].cx = 0.78f; out[n].cy = Y; out[n].cz = 0.5f;
        out[n].hx = 0.22f; out[n].hy = HY; out[n].hz = W;
        out[n].color = c; n++;
    }
    if (nx) {
        out[n].cx = 0.22f; out[n].cy = Y; out[n].cz = 0.5f;
        out[n].hx = 0.22f; out[n].hy = HY; out[n].hz = W;
        out[n].color = c; n++;
    }
    if (pz) {
        out[n].cx = 0.5f; out[n].cy = Y; out[n].cz = 0.78f;
        out[n].hx = W; out[n].hy = HY; out[n].hz = 0.22f;
        out[n].color = c; n++;
    }
    if (nz) {
        out[n].cx = 0.5f; out[n].cy = Y; out[n].cz = 0.22f;
        out[n].hx = W; out[n].hy = HY; out[n].hz = 0.22f;
        out[n].color = c; n++;
    }
    return n;
}

#define MAX_SPRITE_PARTS 8

static int ladder_parts_n(int orient, TorchCuboid *out) {
    /* Two vertical thin slabs forming a rail+rung silhouette pinned
     * to one face of the cell. The face the player aimed at is
     * stored via craft_torches_record_orient. */
    /* High contrast so rails vs rungs are obvious at 128 px. Rails
     * use a deep wood tone; rungs a bright light tone. */
    uint16_t rail = rgb565(80, 50, 25);
    uint16_t rung = rgb565(220, 160, 90);
    /* Defaults — laid against -Z wall (cz = 0.08), rails running L/R
     * along X at x=0.25 / 0.75. Override per face below. */
    /* Cuboids sit ~0.18 in from the wall so they read as projecting
     * forward from the surface, not painted onto it. */
    float a_cx = 0.25f, a_cz = 0.18f, a_hx = 0.04f, a_hz = 0.04f;
    float b_cx = 0.75f, b_cz = 0.18f, b_hx = 0.04f, b_hz = 0.04f;
    switch (orient) {
        case FACE_PZ:   /* aimed at +Z face of parent → ladder on -Z wall of cell */
            /* defaults */
            break;
        case FACE_NZ:   /* aimed at -Z face → ladder on +Z wall */
            a_cz = 0.82f; b_cz = 0.82f;
            break;
        case FACE_PX:   /* aimed at +X face → ladder on -X wall */
            a_cx = 0.18f; b_cx = 0.18f;
            a_cz = 0.25f; b_cz = 0.75f;
            break;
        case FACE_NX:   /* aimed at -X face → ladder on +X wall */
            a_cx = 0.82f; b_cx = 0.82f;
            a_cz = 0.25f; b_cz = 0.75f;
            break;
        default:
            break;
    }
    /* Two vertical rails — beefier (5 cm thick each way) so they're
     * visible across the cell at 128 px. */
    out[0].cx = a_cx; out[0].cy = 0.5f; out[0].cz = a_cz;
    out[0].hx = 0.05f; out[0].hy = 0.48f; out[0].hz = 0.05f;
    out[0].color = rail;
    out[1].cx = b_cx; out[1].cy = 0.5f; out[1].cz = b_cz;
    out[1].hx = 0.05f; out[1].hy = 0.48f; out[1].hz = 0.05f;
    out[1].color = rail;
    /* Horizontal rungs — 4 spaced along Y. Each is a thin slab
     * spanning the rail-to-rail axis (the player-perpendicular axis
     * for the chosen wall). */
    bool x_axis = (orient == FACE_PX || orient == FACE_NX);
    float ry[4] = { 0.18f, 0.40f, 0.62f, 0.84f };
    for (int i = 0; i < 4; i++) {
        TorchCuboid *r = &out[2 + i];
        if (x_axis) {
            /* Wall is on X axis — rungs span Z. */
            r->cx = a_cx; r->cy = ry[i]; r->cz = 0.5f;
            r->hx = 0.03f; r->hy = 0.025f; r->hz = 0.30f;
        } else {
            /* Wall is on Z axis — rungs span X. */
            r->cx = 0.5f; r->cy = ry[i]; r->cz = a_cz;
            r->hx = 0.30f; r->hy = 0.025f; r->hz = 0.03f;
        }
        r->color = rung;
    }
    return 6;
}

static void pad_parts(TorchCuboid out[2]) {
    /* Raised slab on the cell floor — sits ~10 cm above the
     * supporting block so it's visibly raised. Active-dip would
     * need per-cell stand-on plumbing through the torch struct;
     * left as a follow-up. */
    uint16_t pad = rgb565(170, 170, 180);
    uint16_t edge = rgb565(110, 110, 125);
    out[0].cx = 0.5f; out[0].cy = 0.10f; out[0].cz = 0.5f;
    out[0].hx = 0.42f; out[0].hy = 0.05f; out[0].hz = 0.42f;
    out[0].color = pad;
    /* Darker rim to differentiate from the slab surface. */
    out[1].cx = 0.5f; out[1].cy = 0.08f; out[1].cz = 0.5f;
    out[1].hx = 0.45f; out[1].hy = 0.025f; out[1].hz = 0.45f;
    out[1].color = edge;
}

/* VINE — crossed X-quad: two perpendicular planes through the cell
 * centre. In-world vines render via the DDA CROSS path now; this model
 * is only used for the held-item 3D preview, so flat green is enough.
 * `orient` is unused (the cross is symmetric). */
static int vine_parts_n(int orient, TorchCuboid *out) {
    (void)orient;
    const float T  = 0.04f;    /* half-thickness of each plane */
    const float HW = 0.46f;    /* half-width across the cell */
    const float HH = 0.48f;    /* half-height (nearly full cell) */
    out[0].cx = 0.5f; out[0].cy = 0.5f; out[0].cz = 0.5f;
    out[0].hx = HW;   out[0].hy = HH;   out[0].hz = T;
    out[0].color = rgb565(55, 120, 45);
    out[1].cx = 0.5f; out[1].cy = 0.5f; out[1].cz = 0.5f;
    out[1].hx = T;    out[1].hy = HH;   out[1].hz = HW;
    out[1].color = rgb565(48, 110, 40);
    return 2;
}

/* LILY PAD — reuse the pressure pad's flat slab, recoloured green and
 * dropped flush to the water surface. */
static void lily_parts(TorchCuboid out[2]) {
    pad_parts(out);
    out[0].color = rgb565(75, 155, 75);
    out[1].color = rgb565(45, 110, 45);
    /* Sit clearly proud of the water surface so the flat pad doesn't
     * z-fight the water plane and vanish up close. */
    out[0].cy = 0.22f; out[0].hy = 0.035f;
    out[1].cy = 0.20f; out[1].hy = 0.025f;
}

/* Door slab — thin vertical panel spanning the doorway when closed,
 * rotated 90° against an adjacent wall when open. `orient` is the
 * face of the parent wall the door was placed against, which tells
 * us which axis the doorway runs along. */
static int door_parts_n(bool open, int orient, TorchCuboid *out) {
    /* High-contrast wood + iron so the door reads at 128 px scale. */
    uint16_t plank   = rgb565(195, 130, 60);
    uint16_t plank_d = rgb565(80, 50, 25);
    uint16_t iron    = rgb565(70, 70, 85);
    uint16_t knob    = rgb565(255, 215, 90);

    /* Doorway axis (which way the door blocks when CLOSED):
     *   FACE_PZ / FACE_NZ  → player faced ±Z when placing → doorway
     *     spans X (the closed panel runs east-west).
     *   FACE_PX / FACE_NX  → doorway spans Z. */
    bool span_x = (orient == FACE_PZ || orient == FACE_NZ);

    /* Hinge sits at the LOW corner of the doorway axis (cx=0 for
     * span_x, cz=0 for span_z). The open variant rotates the panel
     * 90° around that hinge edge, so the open panel hugs the LOW
     * wall on the perpendicular axis. */
    /* Geometry rule: panel slab is thin (hz=0.025) so background
     * blocks show past the door on both sides. Grain veins + iron
     * straps + knob are made slightly THICKER than the panel so they
     * visibly protrude through it — visible from BOTH faces, not
     * just the one face the offset used to favour. */
    const float PANEL_T  = 0.025f;
    const float DETAIL_T = 0.040f;   /* > panel so it pokes out both sides */
    const float KNOB_T   = 0.055f;

    if (!open) {
        if (span_x) {
            /* Closed panel: spans X, thin Z, centred at cz=0.5. */
            out[0].cx = 0.5f;  out[0].cy = 0.5f; out[0].cz = 0.5f;
            out[0].hx = 0.45f; out[0].hy = 0.48f; out[0].hz = PANEL_T;
            out[0].color = plank;
            float vein_cx[3] = { 0.22f, 0.50f, 0.78f };
            for (int i = 0; i < 3; i++) {
                out[1 + i].cx = vein_cx[i];
                out[1 + i].cy = 0.5f;
                out[1 + i].cz = 0.5f;
                out[1 + i].hx = 0.018f;
                out[1 + i].hy = 0.46f;
                out[1 + i].hz = DETAIL_T;
                out[1 + i].color = plank_d;
            }
            out[4].cx = 0.5f;  out[4].cy = 0.84f; out[4].cz = 0.5f;
            out[4].hx = 0.41f; out[4].hy = 0.04f; out[4].hz = DETAIL_T;
            out[4].color = iron;
            out[5].cx = 0.5f;  out[5].cy = 0.16f; out[5].cz = 0.5f;
            out[5].hx = 0.41f; out[5].hy = 0.04f; out[5].hz = DETAIL_T;
            out[5].color = iron;
            out[6].cx = 0.85f; out[6].cy = 0.50f; out[6].cz = 0.5f;
            out[6].hx = 0.035f; out[6].hy = 0.04f; out[6].hz = KNOB_T;
            out[6].color = knob;
        } else {
            out[0].cx = 0.5f;  out[0].cy = 0.5f; out[0].cz = 0.5f;
            out[0].hx = PANEL_T; out[0].hy = 0.48f; out[0].hz = 0.45f;
            out[0].color = plank;
            float vein_cz[3] = { 0.22f, 0.50f, 0.78f };
            for (int i = 0; i < 3; i++) {
                out[1 + i].cx = 0.5f;
                out[1 + i].cy = 0.5f;
                out[1 + i].cz = vein_cz[i];
                out[1 + i].hx = DETAIL_T;
                out[1 + i].hy = 0.46f;
                out[1 + i].hz = 0.018f;
                out[1 + i].color = plank_d;
            }
            out[4].cx = 0.5f;  out[4].cy = 0.84f; out[4].cz = 0.5f;
            out[4].hx = DETAIL_T; out[4].hy = 0.04f; out[4].hz = 0.41f;
            out[4].color = iron;
            out[5].cx = 0.5f;  out[5].cy = 0.16f; out[5].cz = 0.5f;
            out[5].hx = DETAIL_T; out[5].hy = 0.04f; out[5].hz = 0.41f;
            out[5].color = iron;
            out[6].cx = 0.5f;  out[6].cy = 0.50f; out[6].cz = 0.85f;
            out[6].hx = KNOB_T; out[6].hy = 0.04f; out[6].hz = 0.035f;
            out[6].color = knob;
        }
        return 7;
    } else {
        /* OPEN — panel rotated 90° around the LOW-edge hinge. */
        if (span_x) {
            out[0].cx = 0.05f; out[0].cy = 0.5f; out[0].cz = 0.5f;
            out[0].hx = PANEL_T; out[0].hy = 0.48f; out[0].hz = 0.45f;
            out[0].color = plank;
            float vein_cz[3] = { 0.22f, 0.50f, 0.78f };
            for (int i = 0; i < 3; i++) {
                out[1 + i].cx = 0.05f;
                out[1 + i].cy = 0.5f;
                out[1 + i].cz = vein_cz[i];
                out[1 + i].hx = DETAIL_T;
                out[1 + i].hy = 0.46f;
                out[1 + i].hz = 0.018f;
                out[1 + i].color = plank_d;
            }
            out[4].cx = 0.05f; out[4].cy = 0.84f; out[4].cz = 0.5f;
            out[4].hx = DETAIL_T; out[4].hy = 0.04f; out[4].hz = 0.41f;
            out[4].color = iron;
            out[5].cx = 0.05f; out[5].cy = 0.16f; out[5].cz = 0.5f;
            out[5].hx = DETAIL_T; out[5].hy = 0.04f; out[5].hz = 0.41f;
            out[5].color = iron;
            out[6].cx = 0.05f; out[6].cy = 0.50f; out[6].cz = 0.85f;
            out[6].hx = KNOB_T; out[6].hy = 0.04f; out[6].hz = 0.035f;
            out[6].color = knob;
        } else {
            out[0].cx = 0.5f;  out[0].cy = 0.5f; out[0].cz = 0.05f;
            out[0].hx = 0.45f; out[0].hy = 0.48f; out[0].hz = PANEL_T;
            out[0].color = plank;
            float vein_cx[3] = { 0.22f, 0.50f, 0.78f };
            for (int i = 0; i < 3; i++) {
                out[1 + i].cx = vein_cx[i];
                out[1 + i].cy = 0.5f;
                out[1 + i].cz = 0.05f;
                out[1 + i].hx = 0.018f;
                out[1 + i].hy = 0.46f;
                out[1 + i].hz = DETAIL_T;
                out[1 + i].color = plank_d;
            }
            out[4].cx = 0.5f;  out[4].cy = 0.84f; out[4].cz = 0.05f;
            out[4].hx = 0.41f; out[4].hy = 0.04f; out[4].hz = DETAIL_T;
            out[4].color = iron;
            out[5].cx = 0.5f;  out[5].cy = 0.16f; out[5].cz = 0.05f;
            out[5].hx = 0.41f; out[5].hy = 0.04f; out[5].hz = DETAIL_T;
            out[5].color = iron;
            out[6].cx = 0.85f; out[6].cy = 0.50f; out[6].cz = 0.05f;
            out[6].hx = 0.035f; out[6].hy = 0.04f; out[6].hz = KNOB_T;
            out[6].color = knob;
        }
        return 7;
    }
}

/* Trapdoor — thin horizontal slab at floor level when closed, swung
 * up against the hinge wall when open. */
static int trapdoor_parts_n(bool open, int orient, TorchCuboid *out) {
    uint16_t plank   = rgb565(195, 130, 60);
    uint16_t plank_d = rgb565(80, 50, 25);
    uint16_t iron    = rgb565(70, 70, 85);
    if (!open) {
        /* CLOSED — thin slab at the TOP of the cell (the ceiling
         * surface), so it reads as a hatch you stand on top of.
         * 3 plank strips along the long axis + 2 iron straps + 2
         * grain veins crossing perpendicular = wood-grain feel. */
        out[0].cx = 0.5f;  out[0].cy = 0.94f; out[0].cz = 0.5f;
        out[0].hx = 0.46f; out[0].hy = 0.05f; out[0].hz = 0.46f;
        out[0].color = plank;
        /* Dark grain veins along Z. */
        float vein[3] = { 0.22f, 0.50f, 0.78f };
        for (int i = 0; i < 3; i++) {
            out[1 + i].cx = vein[i];
            out[1 + i].cy = 0.95f;
            out[1 + i].cz = 0.5f;
            out[1 + i].hx = 0.018f;
            out[1 + i].hy = 0.052f;
            out[1 + i].hz = 0.46f;
            out[1 + i].color = plank_d;
        }
        /* Iron straps along the short edges. */
        out[4].cx = 0.5f;  out[4].cy = 0.95f; out[4].cz = 0.10f;
        out[4].hx = 0.41f; out[4].hy = 0.055f; out[4].hz = 0.04f;
        out[4].color = iron;
        out[5].cx = 0.5f;  out[5].cy = 0.95f; out[5].cz = 0.90f;
        out[5].hx = 0.41f; out[5].hy = 0.055f; out[5].hz = 0.04f;
        out[5].color = iron;
        (void)orient;
        return 6;
    } else {
        /* OPEN — slab swung vertical, hinged on the side closest to
         * the player's facing direction. */
        bool on_x = (orient == FACE_PX || orient == FACE_NX);
        if (on_x) {
            float wall_axis = (orient == FACE_PX) ? 0.05f : 0.95f;
            float vein_off  = (orient == FACE_PX) ? -0.008f : 0.008f;
            out[0].cx = wall_axis; out[0].cy = 0.5f;  out[0].cz = 0.5f;
            out[0].hx = 0.05f;     out[0].hy = 0.48f; out[0].hz = 0.46f;
            out[0].color = plank;
            /* Dark grain veins along Z. */
            float vein[3] = { 0.22f, 0.50f, 0.78f };
            for (int i = 0; i < 3; i++) {
                out[1 + i].cx = wall_axis + vein_off;
                out[1 + i].cy = 0.5f;
                out[1 + i].cz = vein[i];
                out[1 + i].hx = 0.052f;
                out[1 + i].hy = 0.46f;
                out[1 + i].hz = 0.018f;
                out[1 + i].color = plank_d;
            }
            out[4].cx = wall_axis + vein_off; out[4].cy = 0.85f; out[4].cz = 0.5f;
            out[4].hx = 0.058f; out[4].hy = 0.035f; out[4].hz = 0.41f;
            out[4].color = iron;
            out[5].cx = wall_axis + vein_off; out[5].cy = 0.15f; out[5].cz = 0.5f;
            out[5].hx = 0.058f; out[5].hy = 0.035f; out[5].hz = 0.41f;
            out[5].color = iron;
        } else {
            float wall_axis = (orient == FACE_PZ) ? 0.05f : 0.95f;
            float vein_off  = (orient == FACE_PZ) ? -0.008f : 0.008f;
            out[0].cx = 0.5f;  out[0].cy = 0.5f;  out[0].cz = wall_axis;
            out[0].hx = 0.46f; out[0].hy = 0.48f; out[0].hz = 0.05f;
            out[0].color = plank;
            float vein[3] = { 0.22f, 0.50f, 0.78f };
            for (int i = 0; i < 3; i++) {
                out[1 + i].cx = vein[i];
                out[1 + i].cy = 0.5f;
                out[1 + i].cz = wall_axis + vein_off;
                out[1 + i].hx = 0.018f;
                out[1 + i].hy = 0.46f;
                out[1 + i].hz = 0.052f;
                out[1 + i].color = plank_d;
            }
            out[4].cx = 0.5f;  out[4].cy = 0.85f; out[4].cz = wall_axis + vein_off;
            out[4].hx = 0.41f; out[4].hy = 0.035f; out[4].hz = 0.058f;
            out[4].color = iron;
            out[5].cx = 0.5f;  out[5].cy = 0.15f; out[5].cz = wall_axis + vein_off;
            out[5].hx = 0.41f; out[5].hy = 0.035f; out[5].hz = 0.058f;
            out[5].color = iron;
        }
        return 6;
    }
}

/* Piston model — emits 3 sub-parts (base / shaft / head) positioned
 * along the orient axis. Variants:
 *   PISTON_OFF: base 0..0.45, shaft 0.45..0.7, head 0.7..1.0 (fits in cell)
 *   PISTON_ON : base 0..0.45, shaft 0.45..1.0 (head is in the next cell)
 *   PISTON_ARM: shaft 0..0.7, head 0.7..1.0   (continues from PISTON_ON cell)
 * `axis` is the orient (Face enum). Each part is built in axis-
 * neutral coords then rotated by remapping fields. */
static int piston_parts_n(int kind, int orient, bool sticky, TorchCuboid *out) {
    uint16_t base_l = rgb565(135, 135, 145);
    uint16_t base_d = rgb565(85, 85, 100);
    uint16_t shaft  = rgb565(180, 180, 190);
    /* Sticky pistons wear a green slime cap; regular pistons a wooden
     * brown one — the Minecraft tell, and what makes the two read
     * differently both in-world and in the held-item viewport. */
    uint16_t head_l = sticky ? rgb565(120, 200, 90) : rgb565(180, 130, 65);
    uint16_t head_d = sticky ? rgb565(70, 140, 55)  : rgb565(115, 75, 35);

    /* Build along +Y, then rotate to the actual orient. Local Y
     * is the "shaft axis". */
    /* Pick axis offsets for the chosen face. */
    int ax_axis = 0;     /* 0=X, 1=Y, 2=Z */
    float dir = 1.0f;    /* +1 = toward higher coord */
    switch (orient) {
        case 0: ax_axis = 0; dir =  1.0f; break;   /* FACE_PX */
        case 1: ax_axis = 0; dir = -1.0f; break;   /* FACE_NX */
        case 2: ax_axis = 1; dir =  1.0f; break;   /* FACE_PY */
        case 3: ax_axis = 1; dir = -1.0f; break;   /* FACE_NY */
        case 4: ax_axis = 2; dir =  1.0f; break;   /* FACE_PZ */
        default: ax_axis = 2; dir = -1.0f; break;  /* FACE_NZ */
    }

    /* Helper: writes a cuboid with `t` running along the shaft axis
     * (0..1 in cell-local coords with shaft-direction sign applied). */
    #define PISTON_PART(idx, t_lo, t_hi, perp_half, COL) do {            \
        TorchCuboid *p = &out[(idx)];                                    \
        p->color = (COL);                                                \
        float t_centre = 0.5f * ((t_lo) + (t_hi));                        \
        float t_half   = 0.5f * ((t_hi) - (t_lo));                        \
        /* If dir is -1, mirror around 0.5 along the shaft axis. */      \
        float ax_centre = (dir > 0) ? t_centre : (1.0f - t_centre);      \
        if (ax_axis == 0) {                                              \
            p->cx = ax_centre; p->cy = 0.5f;     p->cz = 0.5f;           \
            p->hx = t_half;    p->hy = perp_half; p->hz = perp_half;     \
        } else if (ax_axis == 1) {                                       \
            p->cx = 0.5f;      p->cy = ax_centre; p->cz = 0.5f;          \
            p->hx = perp_half; p->hy = t_half;    p->hz = perp_half;     \
        } else {                                                         \
            p->cx = 0.5f;      p->cy = 0.5f;     p->cz = ax_centre;      \
            p->hx = perp_half; p->hy = perp_half; p->hz = t_half;        \
        }                                                                \
    } while (0)

    int n = 0;
    if (kind == TORCH_KIND_PISTON_OFF) {
        /* base 0..0.68 (full-width), tiny gap, head 0.70..1.0 (30%) */
        PISTON_PART(n, 0.00f, 0.68f, 0.48f, base_l); n++;
        /* darker inset band on the base — read as a panel detail */
        PISTON_PART(n, 0.04f, 0.64f, 0.35f, base_d); n++;
        /* head — full perp size to read as a cap */
        PISTON_PART(n, 0.70f, 1.00f, 0.48f, head_l); n++;
        PISTON_PART(n, 0.74f, 0.96f, 0.36f, head_d); n++;
    } else if (kind == TORCH_KIND_PISTON_ON) {
        /* Extended base — base in this cell, shaft pokes out fully */
        PISTON_PART(n, 0.00f, 0.68f, 0.48f, base_l); n++;
        PISTON_PART(n, 0.04f, 0.64f, 0.35f, base_d); n++;
        PISTON_PART(n, 0.68f, 1.00f, 0.08f, shaft);  n++;
    } else if (kind == TORCH_KIND_PISTON_ARM) {
        /* Arm cell — shaft enters from near side, head at far end */
        PISTON_PART(n, 0.00f, 0.70f, 0.08f, shaft);  n++;
        PISTON_PART(n, 0.70f, 1.00f, 0.48f, head_l); n++;
        PISTON_PART(n, 0.74f, 0.96f, 0.36f, head_d); n++;
    }
    #undef PISTON_PART
    return n;
}

/* Lever — 3D mounted switch. A stone base plate sits flat against
 * the placement face; a wood handle tilts off the centre toward
 * one side (flips when toggled), tipped with a small ball. */
static int lever_parts_n(bool on, int orient, TorchCuboid *out) {
    uint16_t plate    = rgb565(150, 150, 165);
    uint16_t plate_d  = rgb565(95, 95, 110);
    uint16_t handle   = rgb565(160, 110, 55);
    uint16_t handle_d = rgb565(95, 65, 30);
    uint16_t ball     = on ? rgb565(255, 80, 60) : rgb565(225, 225, 235);

    /* Position the base plate flat against the orient face. Use
     * local 0..1 cell coords. The plate is 0.55×0.45×0.55 thin,
     * the handle is a stalk + ball poking outward from the plate.
     * Tilt offset: +0.18 when ON, -0.18 when OFF — flips sides. */
    float tilt = on ? 0.18f : -0.18f;

    /* Helper macro to set a part using axis-relative coords:
     *   `t` runs OUTWARD from the wall (0 = at wall, 1 = far face)
     *   `u, v` are perpendicular axes (each 0..1 across the wall)
     *   `tilt_uv` shifts the part along U when not zero (handle tilt)
     *
     * `orient` is the Face enum of the PARENT block the player aimed
     * at. The lever sits on the placement cell's wall touching the
     * parent — which is OPPOSITE the orient direction in cell-local
     * coords. So orient=FACE_PX (parent's +X face) ⇒ wall is at the
     * placement cell's LOW X (touching parent on its east side). */
    #define LEVER_PART(idx, t_c, t_h, u_c, u_h, v_c, v_h, COL) do {       \
        TorchCuboid *p = &out[(idx)];                                     \
        p->color = (COL);                                                 \
        float tc_signed = (t_c);                                          \
        switch (orient) {                                                 \
            case 0: /* FACE_PX → wall is at LOW X of placement cell */    \
                p->cx = tc_signed;        p->cy = (v_c); p->cz = (u_c);   \
                p->hx = (t_h);            p->hy = (v_h); p->hz = (u_h);   \
                break;                                                    \
            case 1: /* FACE_NX → wall is at HIGH X */                     \
                p->cx = 1.0f - tc_signed; p->cy = (v_c); p->cz = (u_c);   \
                p->hx = (t_h);            p->hy = (v_h); p->hz = (u_h);   \
                break;                                                    \
            case 2: /* FACE_PY → wall is the floor (low Y) */             \
                p->cx = (u_c); p->cy = tc_signed;        p->cz = (v_c);   \
                p->hx = (u_h); p->hy = (t_h);            p->hz = (v_h);   \
                break;                                                    \
            case 3: /* FACE_NY → wall is the ceiling (high Y) */          \
                p->cx = (u_c); p->cy = 1.0f - tc_signed; p->cz = (v_c);   \
                p->hx = (u_h); p->hy = (t_h);            p->hz = (v_h);   \
                break;                                                    \
            case 4: /* FACE_PZ → wall at LOW Z */                         \
                p->cx = (u_c); p->cy = (v_c); p->cz = tc_signed;          \
                p->hx = (u_h); p->hy = (v_h); p->hz = (t_h);              \
                break;                                                    \
            default: /* FACE_NZ → wall at HIGH Z */                       \
                p->cx = (u_c); p->cy = (v_c); p->cz = 1.0f - tc_signed;   \
                p->hx = (u_h); p->hy = (v_h); p->hz = (t_h);              \
                break;                                                    \
        }                                                                 \
    } while (0)

    int n = 0;
    /* Base plate — flat against the wall, sticking out 0.08 from
     * the face, covering the central 0.55×0.55 of the wall. */
    LEVER_PART(n,
        0.08f, 0.08f,           /* t centre / half — close to wall */
        0.5f,  0.28f,           /* u centre / half */
        0.5f,  0.28f,           /* v centre / half */
        plate);
    n++;
    /* Plate inset (darker) — sit inside the base for visual depth. */
    LEVER_PART(n,
        0.10f, 0.06f,
        0.5f,  0.20f,
        0.5f,  0.20f,
        plate_d);
    n++;
    /* Handle stalk — tilted along U axis. Length 0.30, thick 0.05. */
    LEVER_PART(n,
        0.30f, 0.20f,           /* protrudes 0.10..0.50 from wall */
        0.5f + tilt * 0.5f, 0.05f,
        0.5f,               0.05f,
        handle);
    n++;
    /* Darker accent on the stalk. */
    LEVER_PART(n,
        0.30f, 0.18f,
        0.5f + tilt * 0.5f, 0.03f,
        0.5f,               0.03f,
        handle_d);
    n++;
    /* Ball tip at the far end. */
    LEVER_PART(n,
        0.55f, 0.08f,
        0.5f + tilt, 0.08f,
        0.5f,        0.08f,
        ball);
    n++;
    #undef LEVER_PART
    return n;
}

/* Returns the number of parts written (1..MAX_SPRITE_PARTS).
 * Callers should pass an array at least MAX_SPRITE_PARTS long. */
/* Same as torch_parts but accepts a connection mask for wires. The
 * wire-only API at the top of the file calls this with mask=0xF
 * (all four arms) for held-item previews; the world render passes
 * the per-cell connect mask so only real connections draw. */
static int torch_parts_full(int kind, int orient, uint8_t connect,
                            bool sticky, TorchCuboid *out) {
    if (kind == TORCH_KIND_WIRE)              { return wire_parts_n(false, connect, out); }
    if (kind == TORCH_KIND_WIRE_ON)           { return wire_parts_n(true,  connect, out); }
    if (kind == TORCH_KIND_LADDER)            { return ladder_parts_n(orient, out); }
    if (kind == TORCH_KIND_VINE)              { return vine_parts_n(orient, out); }
    if (kind == TORCH_KIND_LILY_PAD)          { lily_parts(out); return 2; }
    if (kind == TORCH_KIND_PRESSURE_PAD)      { pad_parts(out); return 2; }
    if (kind == TORCH_KIND_DOOR_CLOSED)       { return door_parts_n(false, orient, out); }
    if (kind == TORCH_KIND_DOOR_OPEN)         { return door_parts_n(true,  orient, out); }
    if (kind == TORCH_KIND_TRAPDOOR_CLOSED)   { return trapdoor_parts_n(false, orient, out); }
    if (kind == TORCH_KIND_TRAPDOOR_OPEN)     { return trapdoor_parts_n(true,  orient, out); }
    if (kind == TORCH_KIND_PISTON_OFF ||
        kind == TORCH_KIND_PISTON_ON  ||
        kind == TORCH_KIND_PISTON_ARM)        { return piston_parts_n(kind, orient, sticky, out); }
    if (kind == TORCH_KIND_LEVER_OFF)         { return lever_parts_n(false, orient, out); }
    if (kind == TORCH_KIND_LEVER_ON)          { return lever_parts_n(true,  orient, out); }

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
    return 2;
}

/* Back-compat wrapper for callers that don't have a per-cell
 * connect mask (held-item previews use 0xF = "all four arms"). */
static int torch_parts(int kind, int orient, TorchCuboid *out) {
    return torch_parts_full(kind, orient, 0xF, false, out);
}

/* A piston sprite cell is "sticky" when the live world block at its
 * coords is a sticky-piston id — read at render time so we don't have to
 * widen the sprite list with a sticky kind. */
static bool torch_cell_sticky(const CraftTorch *t) {
    BlockId b = craft_world_get(t->wx, (int)t->wy, t->wz);
    return b == BLK_STICKY_PISTON_OFF || b == BLK_STICKY_PISTON_ON;
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
        TorchCuboid parts[MAX_SPRITE_PARTS] = {0};   /* tex defaults NULL */
        int n_parts = torch_parts_full(t->kind, t->orient,
                                       t->connect, torch_cell_sticky(t), parts);
        for (int p = 0; p < n_parts; p++) {
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

/* Highlight cell — set by the picker each frame so the sprite for
 * the picked cell renders in a brighter tint. */
static int  s_hl_wx, s_hl_wy, s_hl_wz;
static bool s_hl_enabled;

void craft_torches_set_highlight(int wx, int wy, int wz, bool enabled) {
    s_hl_wx = wx;
    s_hl_wy = wy;
    s_hl_wz = wz;
    s_hl_enabled = enabled;
}

/* Mix `c` toward white by `t` in [0..255]. Cheap per-channel blend
 * for the highlight tint. */
static uint16_t blend_to_white(uint16_t c, int t) {
    int r = (c >> 11) & 0x1F;
    int g = (c >>  5) & 0x3F;
    int b =  c        & 0x1F;
    r = r + ((0x1F - r) * t >> 8);
    g = g + ((0x3F - g) * t >> 8);
    b = b + ((0x1F - b) * t >> 8);
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
    float tan_h  = tanf(cam->fov * 0.5f);     /* fov is vertical → tan_v */
    /* Match the world raycaster's aspect-widened horizontal FOV so the cuboid
     * lines up with the terrain. Square device (FB_W==FB_H) → aspect 1, no-op. */
    float aspect = (float)CRAFT_FB_W / (float)CRAFT_FB_H;
    float focal_h = (CRAFT_FB_H * 0.5f) / tan_h;
    float focal_v = (CRAFT_FB_H * 0.5f) / tan_h;

    for (int i = 0; i < CRAFT_MAX_TORCHES; i++) {
        CraftTorch *t = &craft_torches[i];
        if (!t->alive) continue;
        bool highlight = s_hl_enabled &&
                         t->wx == s_hl_wx &&
                         t->wy == s_hl_wy &&
                         t->wz == s_hl_wz;

        /* Cull torches outside the visible distance. */
        float dxp = (float)t->wx + 0.5f - cam->pos.x;
        float dyp = (float)t->wy + 0.5f - cam->pos.y;
        float dzp = (float)t->wz + 0.5f - cam->pos.z;
        float zf = dxp * fwd.x + dyp * fwd.y + dzp * fwd.z;
        if (zf <= 0.05f) continue;
        float dist2 = dxp*dxp + dyp*dyp + dzp*dzp;
        if (dist2 > 50.0f * 50.0f) continue;

        TorchCuboid parts[MAX_SPRITE_PARTS] = {0};   /* tex defaults NULL */
        int n_parts = torch_parts_full(t->kind, t->orient,
                                       t->connect, torch_cell_sticky(t), parts);

        /* Compute the screen bbox containing all 8 corners of the
         * union of every part's world-space AABB. */
        float bmin_x = 1.0f, bmin_y = 1.0f, bmin_z = 1.0f;
        float bmax_x = 0.0f, bmax_y = 0.0f, bmax_z = 0.0f;
        for (int p = 0; p < n_parts; p++) {
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
                float vx    = ndc_x * tan_h * aspect;
                float wdx = fwd.x + right.x * vx + up.x * vy;
                float wdy = fwd.y + right.y * vx + up.y * vy;
                float wdz = fwd.z + right.z * vx + up.z * vy;

                float best_t = 1e30f;
                uint16_t best_color = 0;
                bool best_is_flame = false;
                for (int p = 0; p < n_parts; p++) {
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
                            /* p==1 is "flame" only for the torch
                             * model; for other sprites the second
                             * cuboid is just a detail accent. */
                            best_is_flame = (t->kind == TORCH_KIND_TORCH && p == 1);
                        }
                    }
                }
                if (best_t >= 1e29f) continue;

                /* Convert hit t to world distance for z-test. */
                float dl2 = wdx*wdx + wdy*wdy + wdz*wdz;
                float dl  = (dl2 > 1.0001f) ? sqrtf(dl2) : 1.0f;
                float wdist = best_t * dl;
                int q = (int)(wdist * 255.0f / CRAFT_MAX_DIST_FOR_ZBUF);
                /* Lily pads lie in the water-surface plane and would
                 * lose the depth tie to the water (drawn first) when
                 * viewed up close — pull them a few buckets toward the
                 * camera so they always win that coplanar tie. */
                if (t->kind == TORCH_KIND_LILY_PAD) q -= 4;
                if (q < 0)   q = 0;
                if (q > 254) q = 254;
                int idx = sy * CRAFT_FB_W + sx;
                if (craft_zbuf[idx] <= (uint8_t)q) continue;

                /* Lighting. True light sources (the torch flame, and
                 * powered redstone dust) glow at a fixed brightness
                 * regardless of time of day. EVERY other sprite cuboid
                 * — ladder, door, trapdoor, piston, lever, wire-off,
                 * vine, lily pad — is lit PER-CELL exactly like the
                 * world blocks: sky-exposed cells take the day/night
                 * ambient, buried/shadowed cells go dim, torch light
                 * floors it. (Previously most of these rendered at raw
                 * cuboid brightness and glowed at night / in caves.) */
                if (best_is_flame ||
                    t->kind == TORCH_KIND_TORCH ||
                    t->kind == TORCH_KIND_WIRE_ON) {
                    best_color = shade565(best_color, 220);
                } else {
                    int m = craft_world_sky_exposed(t->wx, t->wy, t->wz)
                              ? craft_render_brightness_q8() : 48;
                    int tl = craft_world_light_level(t->wx, t->wy, t->wz);
                    if (tl > 0) { int fl = 70 + tl * 55; if (m < fl) m = fl; }
                    best_color = shade565(best_color, m);
                }
                /* Picker highlight — tint toward white on the
                 * selected cell. */
                if (highlight) best_color = blend_to_white(best_color, 110);
                fb[idx] = best_color;
                craft_zbuf[idx] = (uint8_t)q;
            }
        }
    }
}

/* Map a sprite-block id to the torch system's "kind" + fill the
 * cuboid parts so the held-item viewport can render a mini 3D
 * preview instead of a flat painted cube. */
int craft_torches_block_model(uint8_t b, int orient,
                              CraftToolPart *out, int max_n) {
    int kind;
    switch (b) {
        case BLK_LADDER:           kind = TORCH_KIND_LADDER; break;
        case BLK_VINE:             kind = TORCH_KIND_VINE; break;
        case BLK_LILY_PAD:         kind = TORCH_KIND_LILY_PAD; break;
        case BLK_REDSTONE_WIRE:    kind = TORCH_KIND_WIRE; break;
        case BLK_REDSTONE_WIRE_ON: kind = TORCH_KIND_WIRE_ON; break;
        case BLK_PRESSURE_PAD:     kind = TORCH_KIND_PRESSURE_PAD; break;
        case BLK_DOOR_OFF:         kind = TORCH_KIND_DOOR_CLOSED; break;
        case BLK_DOOR_ON:          kind = TORCH_KIND_DOOR_OPEN; break;
        case BLK_TRAPDOOR_OFF:     kind = TORCH_KIND_TRAPDOOR_CLOSED; break;
        case BLK_TRAPDOOR_ON:      kind = TORCH_KIND_TRAPDOOR_OPEN; break;
        case BLK_TORCH:            kind = TORCH_KIND_TORCH; break;
        case BLK_LEVER_OFF:
        case BLK_LEVER_ON:
            /* Levers don't have a sprite-system model — they're
             * just cubes with directional textures. Return 0. */
            return 0;
        default:
            return 0;
    }
    TorchCuboid local[MAX_SPRITE_PARTS] = {0};
    int n = torch_parts(kind, orient, local);
    if (n > max_n) n = max_n;
    /* CraftToolPart and TorchCuboid share the layout — direct copy. */
    for (int i = 0; i < n; i++) {
        CraftToolPart *o = &out[i];
        o->cx = local[i].cx; o->cy = local[i].cy; o->cz = local[i].cz;
        o->hx = local[i].hx; o->hy = local[i].hy; o->hz = local[i].hz;
        o->color = local[i].color;
    }
    return n;
}

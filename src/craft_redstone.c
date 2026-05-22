/*
 * ThumbyCraft — redstone power propagation (impl).
 */
#include "craft_redstone.h"
#include "craft_world.h"
#include "craft_blocks.h"
#include "craft_mobs.h"
#include "craft_torches.h"
#include "craft_audio.h"

/* Convenience helper used by the piston extend/retract paths above.
 * Reads the player-recorded orient hash, defaults to +Y. */
int craft_redstone_piston_orient(int wx, int wy, int wz) {
    return craft_torches_lookup_orient(wx, wy, wz);
}

#include <stdbool.h>
#include <string.h>

#define REDSTONE_TICK_PERIOD 0.2f   /* 5 Hz */
#define WIRE_MAX             64     /* cap on wire cells we track per tick */

static float s_accum;

/* Count of "active" redstone sources currently in the resident window.
 * Active = LEVER_ON or REDSTONE_WIRE_ON. While this is zero there are
 * no powered cells anywhere, so the tick can early-exit before doing
 * the full window scan. Incrementally maintained via note_change on
 * every craft_world_set, plus full rescan on window load. */
static int s_active;

/* Bounded set of activated diamond blocks. Entries are kept across
 * frames so re-powering the same block doesn't respawn the boss. */
#define ACTIVATED_MAX 8
typedef struct { int wx, wy, wz; } ActivatedDiamond;
static ActivatedDiamond s_activated[ACTIVATED_MAX];
static int              s_activated_n;

static bool activated_seen(int wx, int wy, int wz) {
    for (int i = 0; i < s_activated_n; i++) {
        if (s_activated[i].wx == wx &&
            s_activated[i].wy == wy &&
            s_activated[i].wz == wz) return true;
    }
    return false;
}
static void activated_add(int wx, int wy, int wz) {
    if (activated_seen(wx, wy, wz)) return;
    if (s_activated_n >= ACTIVATED_MAX) return;
    s_activated[s_activated_n++] = (ActivatedDiamond){ wx, wy, wz };
}

void craft_redstone_init(void) {
    s_accum       = 0.0f;
    s_activated_n = 0;
    s_active      = 0;
}

void craft_redstone_rescan(void) {
    int n = 0;
    for (int i = 0; i < CRAFT_WORLD_VOXELS; i++) {
        BlockId b = (BlockId)craft_world_blocks[i];
        if (b == BLK_LEVER_ON || b == BLK_REDSTONE_WIRE_ON ||
            b == BLK_REDSTONE_BLOCK ||
            b == BLK_NOT_GATE || b == BLK_NOT_GATE_ON ||
            b == BLK_DELAY    || b == BLK_DELAY_ON    ||
            b == BLK_LAMP     || b == BLK_LAMP_ON     ||
            b == BLK_NOTE_BLOCK || b == BLK_NOTE_BLOCK_ON ||
            b == BLK_OBSERVER || b == BLK_OBSERVER_ON) n++;
    }
    s_active = n;
}

void craft_redstone_note_change(BlockId prev, BlockId blk) {
    int delta = 0;
    /* Sources count toward s_active so the tick can early-exit when
     * the window has no powered cells. Solid redstone blocks act as
     * permanent ON sources (vanilla rule). The new gates and sinks
     * (NOT/DELAY/OBSERVER/LAMP/NOTE_BLOCK) also count — gates emit
     * synthetic power, sinks need their bit refreshed each tick. */
    if (prev == BLK_LEVER_ON || prev == BLK_REDSTONE_WIRE_ON ||
        prev == BLK_REDSTONE_BLOCK ||
        prev == BLK_NOT_GATE || prev == BLK_NOT_GATE_ON ||
        prev == BLK_DELAY    || prev == BLK_DELAY_ON    ||
        prev == BLK_LAMP     || prev == BLK_LAMP_ON     ||
        prev == BLK_NOTE_BLOCK || prev == BLK_NOTE_BLOCK_ON ||
        prev == BLK_OBSERVER || prev == BLK_OBSERVER_ON) delta--;
    if (blk  == BLK_LEVER_ON || blk  == BLK_REDSTONE_WIRE_ON ||
        blk  == BLK_REDSTONE_BLOCK ||
        blk  == BLK_NOT_GATE || blk  == BLK_NOT_GATE_ON ||
        blk  == BLK_DELAY    || blk  == BLK_DELAY_ON    ||
        blk  == BLK_LAMP     || blk  == BLK_LAMP_ON     ||
        blk  == BLK_NOTE_BLOCK || blk  == BLK_NOTE_BLOCK_ON ||
        blk  == BLK_OBSERVER || blk  == BLK_OBSERVER_ON) delta++;
    s_active += delta;
    if (s_active < 0) s_active = 0;   /* clamp against drift */
}

/* --- Pressure pad latch + TNT fuse list -------------------------- *
 * Player tick reports its current pressure-pad cell each frame; the
 * redstone tick treats that as a power source. Single-cell latch is
 * enough for player demos. */
static int s_pad_wx, s_pad_wy, s_pad_wz;
static bool s_pad_held;
void craft_redstone_note_pressure(int wx, int wy, int wz) {
    if (wy < 0) { s_pad_held = false; return; }
    s_pad_wx = wx; s_pad_wy = wy; s_pad_wz = wz;
    s_pad_held = true;
}

#define FUSE_MAX 8
typedef struct { int wx, wy, wz; float t; } Fuse;
static Fuse s_fuses[FUSE_MAX];
static int  s_fuse_n;

static void fuse_add(int wx, int wy, int wz) {
    for (int i = 0; i < s_fuse_n; i++) {
        if (s_fuses[i].wx == wx && s_fuses[i].wy == wy &&
            s_fuses[i].wz == wz) return;
    }
    if (s_fuse_n >= FUSE_MAX) return;
    s_fuses[s_fuse_n++] = (Fuse){ wx, wy, wz, 3.0f };
    craft_audio_fuse();
}
static void fuse_remove_at(int i) {
    s_fuses[i] = s_fuses[--s_fuse_n];
}

static void tnt_explode(int cx, int cy, int cz) {
    craft_audio_explode();
    /* Clear a 3-block-radius spherical chunk of breakable cells. */
    for (int dy = -3; dy <= 3; dy++) {
        for (int dz = -3; dz <= 3; dz++) {
            for (int dx = -3; dx <= 3; dx++) {
                if (dx*dx + dy*dy + dz*dz > 3*3) continue;
                int x = cx + dx, y = cy + dy, z = cz + dz;
                if (y <= 0) continue;   /* spare bedrock */
                BlockId b = craft_world_get(x, y, z);
                if (b == BLK_AIR) continue;
                /* Chain-fire any adjacent TNT into a fused state
                 * with a short stagger. */
                if (b == BLK_TNT) {
                    craft_world_set(x, y, z, BLK_TNT_FUSED);
                    fuse_add(x, y, z);
                    s_fuses[s_fuse_n - 1].t = 0.5f + (float)(dx + dz) * 0.05f;
                    continue;
                }
                craft_world_set(x, y, z, BLK_AIR);
            }
        }
    }
}

void craft_redstone_tick_fuses(float dt) {
    if (s_fuse_n == 0) return;
    for (int i = 0; i < s_fuse_n; ) {
        s_fuses[i].t -= dt;
        if (s_fuses[i].t <= 0.0f) {
            int x = s_fuses[i].wx, y = s_fuses[i].wy, z = s_fuses[i].wz;
            BlockId here = craft_world_get(x, y, z);
            if (here == BLK_TNT_FUSED) {
                craft_world_set(x, y, z, BLK_AIR);
                tnt_explode(x, y, z);
            }
            fuse_remove_at(i);
        } else {
            i++;
        }
    }
}

/* Scratch BFS frontier (local coords, 3 bytes per cell). 256 entries
 * comfortably handles a wire network that fills a third of the
 * window's underground volume — plenty. */
#define BFS_MAX WIRE_MAX
typedef struct { uint8_t lx, wy, lz; } RsCell;
static RsCell s_frontier[BFS_MAX];

/* Bitmap of cells visited this BFS round. One bit per window cell.
 * 64*64*64 = 256K cells / 8 = 32 KB. Heavier than I'd like for one
 * feature, so use a smaller hash + linear-probe instead: tracking
 * only the cells we actually touched. Cap WIRE_MAX cells. */
static uint32_t s_visited_keys[WIRE_MAX];
static int      s_visited_n;

static inline uint32_t key_of(int lx, int wy, int lz) {
    return ((uint32_t)(lx & 0x3F) << 16) |
           ((uint32_t)(wy & 0x3F) << 8)  |
            (uint32_t)(lz & 0x3F);
}
static bool visited_seen(uint32_t k) {
    for (int i = 0; i < s_visited_n; i++)
        if (s_visited_keys[i] == k) return true;
    return false;
}
static bool visited_add(uint32_t k) {
    if (s_visited_n >= WIRE_MAX) return false;
    s_visited_keys[s_visited_n++] = k;
    return true;
}

void craft_redstone_tick(float dt) {
    s_accum += dt;
    if (s_accum < REDSTONE_TICK_PERIOD) return;
    s_accum -= REDSTONE_TICK_PERIOD;

    /* Fast path — no powered cells anywhere in the window means no
     * BFS sources and no WIRE_ON cells needing reset, so skip the
     * 256K scan entirely. The counter is kept in sync by
     * note_change (on craft_world_set) and by the writes below.
     * Pressure pads bypass this — they can be the sole power
     * source in a circuit. Driven-block scan also bypasses so
     * removing power correctly resets doors/pistons. */
    if (s_active == 0 && !s_pad_held) return;

    int ox = craft_world_origin_x;
    int oz = craft_world_origin_z;

    /* Phase 1 — collect lever_on positions and seed BFS. Also reset
     * any existing wire_on cells to wire (we'll re-mark below). */
    s_visited_n = 0;
    int frontier_n = 0;
    bool state_changed = false;

    /* Phase 0.5 — gate seeding. NOT_GATE / DELAY blocks contribute
     * synthetic "source" emissions into the BFS frontier based on
     * the gate's current cell-ID state. NOT_GATE_ON / DELAY_ON emit;
     * their OFF variants don't. The state transition (input→next
     * tick's output) happens in the post-BFS pass below — that gives
     * gates the one-tick lag that's wired into vanilla redstone. */
    for (int wy = 0; wy < CRAFT_WORLD_Y; wy++) {
        for (int lz = 0; lz < CRAFT_WORLD_Z; lz++) {
            for (int lx = 0; lx < CRAFT_WORLD_X; lx++) {
                int idx = (wy * CRAFT_WORLD_Z + lz) * CRAFT_WORLD_X + lx;
                BlockId b = (BlockId)craft_world_blocks[idx];
                if (b != BLK_NOT_GATE_ON && b != BLK_DELAY_ON) continue;

                int face = craft_torches_lookup_orient(lx + ox, wy, lz + oz);
                int odx = 0, ody = 0, odz = 0;
                switch (face) {
                    case FACE_PX: odx =  1; break;
                    case FACE_NX: odx = -1; break;
                    case FACE_PY: ody =  1; break;
                    case FACE_NY: ody = -1; break;
                    case FACE_PZ: odz =  1; break;
                    default:      odz = -1; break;
                }
                int olx = lx + odx, owy = wy + ody, olz = lz + odz;
                if ((unsigned)olx >= CRAFT_WORLD_X ||
                    (unsigned)olz >= CRAFT_WORLD_Z ||
                    (unsigned)owy >= CRAFT_WORLD_Y) continue;
                int oidx = (owy * CRAFT_WORLD_Z + olz) * CRAFT_WORLD_X + olx;
                BlockId ob = (BlockId)craft_world_blocks[oidx];
                if (ob == BLK_REDSTONE_WIRE || ob == BLK_REDSTONE_WIRE_ON) {
                    uint32_t k = key_of(olx, owy, olz);
                    if (visited_add(k) && frontier_n < BFS_MAX) {
                        s_frontier[frontier_n++] = (RsCell){
                            (uint8_t)olx, (uint8_t)owy, (uint8_t)olz
                        };
                    }
                }
            }
        }
    }

    for (int wy = 0; wy < CRAFT_WORLD_Y; wy++) {
        for (int lz = 0; lz < CRAFT_WORLD_Z; lz++) {
            for (int lx = 0; lx < CRAFT_WORLD_X; lx++) {
                /* Direct backing-array read avoids craft_world_get's
                 * bounds checks. The whole window-scan otherwise eats
                 * ~14 ms of CPU per tick at 280 MHz, which would land
                 * as a periodic frame-stutter at 5 Hz. */
                int idx = (wy * CRAFT_WORLD_Z + lz) * CRAFT_WORLD_X + lx;
                BlockId b = (BlockId)craft_world_blocks[idx];
                /* Both LEVER_ON cells and solid REDSTONE_BLOCKs act
                 * as power sources — seed adjacent wires from each. */
                if (b == BLK_LEVER_ON || b == BLK_REDSTONE_BLOCK) {
                    static const int dx[6] = { 1,-1, 0, 0, 0, 0 };
                    static const int dy[6] = { 0, 0, 1,-1, 0, 0 };
                    static const int dz[6] = { 0, 0, 0, 0, 1,-1 };
                    for (int d = 0; d < 6; d++) {
                        int nlx = lx + dx[d];
                        int nwy = wy + dy[d];
                        int nlz = lz + dz[d];
                        if ((unsigned)nlx >= CRAFT_WORLD_X) continue;
                        if ((unsigned)nlz >= CRAFT_WORLD_Z) continue;
                        if ((unsigned)nwy >= CRAFT_WORLD_Y) continue;
                        BlockId nb = craft_world_get(nlx + ox, nwy, nlz + oz);
                        if (nb == BLK_REDSTONE_WIRE ||
                            nb == BLK_REDSTONE_WIRE_ON) {
                            uint32_t k = key_of(nlx, nwy, nlz);
                            if (visited_add(k) && frontier_n < BFS_MAX) {
                                s_frontier[frontier_n++] = (RsCell){
                                    (uint8_t)nlx, (uint8_t)nwy, (uint8_t)nlz
                                };
                            }
                        } else if (nb == BLK_DIAMOND_BLOCK) {
                            int dwx = nlx + ox, dwz = nlz + oz;
                            if (!activated_seen(dwx, nwy, dwz)) {
                                activated_add(dwx, nwy, dwz);
                                craft_mobs_spawn_boss(dwx, nwy + 1, dwz);
                            }
                        }
                    }
                } else if (b == BLK_REDSTONE_WIRE_ON) {
                    /* Reset to unpowered; BFS may re-power below. */
                    craft_world_set_byte(lx + ox, wy, lz + oz,
                                         (uint8_t)BLK_REDSTONE_WIRE);
                    s_active--;
                    state_changed = true;
                }
            }
        }
    }

    /* Pressure pad as a source — if the player reported standing on
     * one, seed its 6-adjacent cells just like a lever_on would. */
    if (s_pad_held) {
        int plx = s_pad_wx - ox;
        int plz = s_pad_wz - oz;
        if ((unsigned)plx < CRAFT_WORLD_X && (unsigned)plz < CRAFT_WORLD_Z &&
            (unsigned)s_pad_wy < CRAFT_WORLD_Y) {
            static const int dx[6] = { 1,-1, 0, 0, 0, 0 };
            static const int dy[6] = { 0, 0, 1,-1, 0, 0 };
            static const int dz[6] = { 0, 0, 0, 0, 1,-1 };
            for (int d = 0; d < 6; d++) {
                int nlx = plx + dx[d];
                int nwy = s_pad_wy + dy[d];
                int nlz = plz + dz[d];
                if ((unsigned)nlx >= CRAFT_WORLD_X) continue;
                if ((unsigned)nlz >= CRAFT_WORLD_Z) continue;
                if ((unsigned)nwy >= CRAFT_WORLD_Y) continue;
                BlockId nb = craft_world_get(nlx + ox, nwy, nlz + oz);
                if (nb == BLK_REDSTONE_WIRE || nb == BLK_REDSTONE_WIRE_ON) {
                    uint32_t k = key_of(nlx, nwy, nlz);
                    if (visited_add(k) && frontier_n < BFS_MAX) {
                        s_frontier[frontier_n++] = (RsCell){
                            (uint8_t)nlx, (uint8_t)nwy, (uint8_t)nlz
                        };
                    }
                }
            }
        }
    }

    /* Phase 2 — BFS through the wire mesh, painting each visited
     * cell to WIRE_ON and checking for adjacent diamond blocks. */
    int head = 0;
    while (head < frontier_n) {
        RsCell c = s_frontier[head++];
        craft_world_set_byte(c.lx + ox, c.wy, c.lz + oz,
                             (uint8_t)BLK_REDSTONE_WIRE_ON);
        s_active++;
        state_changed = true;
        static const int dx[6] = { 1,-1, 0, 0, 0, 0 };
        static const int dy[6] = { 0, 0, 1,-1, 0, 0 };
        static const int dz[6] = { 0, 0, 0, 0, 1,-1 };
        for (int d = 0; d < 6; d++) {
            int nlx = c.lx + dx[d];
            int nwy = c.wy + dy[d];
            int nlz = c.lz + dz[d];
            if ((unsigned)nlx >= CRAFT_WORLD_X) continue;
            if ((unsigned)nlz >= CRAFT_WORLD_Z) continue;
            if ((unsigned)nwy >= CRAFT_WORLD_Y) continue;
            BlockId nb = craft_world_get(nlx + ox, nwy, nlz + oz);
            if (nb == BLK_REDSTONE_WIRE || nb == BLK_REDSTONE_WIRE_ON) {
                uint32_t k = key_of(nlx, nwy, nlz);
                if (!visited_seen(k)) {
                    if (visited_add(k) && frontier_n < BFS_MAX) {
                        s_frontier[frontier_n++] = (RsCell){
                            (uint8_t)nlx, (uint8_t)nwy, (uint8_t)nlz
                        };
                    }
                }
            } else if (nb == BLK_DIAMOND_BLOCK) {
                int dwx = nlx + ox, dwz = nlz + oz;
                if (!activated_seen(dwx, nwy, dwz)) {
                    activated_add(dwx, nwy, dwz);
                    craft_mobs_spawn_boss(dwx, nwy + 1, dwz);
                }
            }
        }
    }

    /* Phase 3 — drive trapdoors / doors / pistons / TNT based on
     * adjacent power. We scan the window once more (~3 ms direct
     * byte reads at 280 MHz) instead of maintaining a parallel
     * list. For each candidate block, check 6-neighbour cells for
     * a power source (LEVER_ON / WIRE_ON / the active pressure
     * pad cell). */
    for (int wy = 0; wy < CRAFT_WORLD_Y; wy++) {
        for (int lz = 0; lz < CRAFT_WORLD_Z; lz++) {
            for (int lx = 0; lx < CRAFT_WORLD_X; lx++) {
                int idx = (wy * CRAFT_WORLD_Z + lz) * CRAFT_WORLD_X + lx;
                BlockId b = (BlockId)craft_world_blocks[idx];
                bool is_driven =
                    b == BLK_TRAPDOOR_OFF || b == BLK_TRAPDOOR_ON ||
                    b == BLK_DOOR_OFF     || b == BLK_DOOR_ON     ||
                    b == BLK_PISTON_OFF   || b == BLK_PISTON_ON   ||
                    b == BLK_TNT;
                if (!is_driven) continue;
                /* Scan adjacent cells once: is any of them a power
                 * source AND is any of them part of a redstone
                 * circuit at all? Doors/trapdoors without ANY
                 * adjacent redstone gear are left alone so the
                 * player's B-toggle stays put. */
                bool powered = false;
                bool redstone_adj = false;
                static const int dx6[6] = { 1,-1, 0, 0, 0, 0 };
                static const int dy6[6] = { 0, 0, 1,-1, 0, 0 };
                static const int dz6[6] = { 0, 0, 0, 0, 1,-1 };
                for (int d = 0; d < 6; d++) {
                    int nlx = lx + dx6[d];
                    int nwy = wy + dy6[d];
                    int nlz = lz + dz6[d];
                    if ((unsigned)nlx >= CRAFT_WORLD_X) continue;
                    if ((unsigned)nlz >= CRAFT_WORLD_Z) continue;
                    if ((unsigned)nwy >= CRAFT_WORLD_Y) continue;
                    int nidx = (nwy * CRAFT_WORLD_Z + nlz) * CRAFT_WORLD_X + nlx;
                    BlockId nb = (BlockId)craft_world_blocks[nidx];
                    if (nb == BLK_LEVER_OFF || nb == BLK_LEVER_ON ||
                        nb == BLK_REDSTONE_WIRE || nb == BLK_REDSTONE_WIRE_ON ||
                        nb == BLK_PRESSURE_PAD || nb == BLK_REDSTONE_BLOCK) {
                        redstone_adj = true;
                    }
                    if (nb == BLK_LEVER_ON || nb == BLK_REDSTONE_WIRE_ON ||
                        nb == BLK_REDSTONE_BLOCK) {
                        powered = true;
                    }
                }
                /* Pressure-pad direct neighbour check. */
                if (s_pad_held) {
                    int adwx = (lx + ox) - s_pad_wx;
                    int adwy = wy       - s_pad_wy;
                    int adwz = (lz + oz) - s_pad_wz;
                    if (adwx*adwx + adwy*adwy + adwz*adwz == 1) {
                        powered = true;
                        redstone_adj = true;
                    }
                }
                int wx = lx + ox, wz = lz + oz;
                /* Doors / trapdoors: only drive if there's adjacent
                 * redstone gear at all — manual toggle persists when
                 * no circuit is attached. */
                bool is_door_or_trap = (b == BLK_DOOR_OFF || b == BLK_DOOR_ON ||
                                        b == BLK_TRAPDOOR_OFF || b == BLK_TRAPDOOR_ON);
                if (is_door_or_trap && !redstone_adj) continue;
                /* Doors are 2 cells tall — when the redstone signal
                 * flips one half, sync the matching neighbour cell
                 * so both halves open/close together regardless of
                 * which one is adjacent to the wire. */
                #define DOOR_SYNC_NEIGHBOUR(wx_, wy_, wz_, want_) do {           \
                    if (craft_world_get((wx_), (wy_) + 1, (wz_)) == BLK_DOOR_OFF \
                     || craft_world_get((wx_), (wy_) + 1, (wz_)) == BLK_DOOR_ON) \
                        craft_world_set((wx_), (wy_) + 1, (wz_), (want_));       \
                    if (craft_world_get((wx_), (wy_) - 1, (wz_)) == BLK_DOOR_OFF \
                     || craft_world_get((wx_), (wy_) - 1, (wz_)) == BLK_DOOR_ON) \
                        craft_world_set((wx_), (wy_) - 1, (wz_), (want_));       \
                } while (0)
                if (powered) {
                    if (b == BLK_TRAPDOOR_OFF)
                        craft_world_set(wx, wy, wz, BLK_TRAPDOOR_ON);
                    else if (b == BLK_DOOR_OFF) {
                        craft_world_set(wx, wy, wz, BLK_DOOR_ON);
                        DOOR_SYNC_NEIGHBOUR(wx, wy, wz, BLK_DOOR_ON);
                    }
                    else if (b == BLK_PISTON_OFF) {
                        /* Extend along the piston's orient axis. The
                         * orient stored at placement is a Face enum
                         * (0..5) indicating which way the shaft + head
                         * point. v1 sticky pistons: push the cell
                         * directly in front (if air → arm goes there;
                         * if solid → shove it one further along, place
                         * arm at the front cell). */
                        extern int craft_redstone_piston_orient(int wx, int wy, int wz);
                        int face = craft_redstone_piston_orient(wx, wy, wz);
                        int fdx = 0, fdy = 0, fdz = 0;
                        switch (face) {
                            case 0: fdx =  1; break;   /* FACE_PX */
                            case 1: fdx = -1; break;   /* FACE_NX */
                            case 2: fdy =  1; break;   /* FACE_PY */
                            case 3: fdy = -1; break;   /* FACE_NY */
                            case 4: fdz =  1; break;   /* FACE_PZ */
                            default: fdz = -1; break;  /* FACE_NZ */
                        }
                        int ax = wx + fdx, ay = wy + fdy, az = wz + fdz;
                        BlockId ahead = craft_world_get(ax, ay, az);
                        if (ahead == BLK_AIR) {
                            craft_world_set(wx, wy, wz, BLK_PISTON_ON);
                            craft_world_set(ax, ay, az, BLK_PISTON_ARM);
                            craft_torches_record_orient(ax, ay, az, face);
                        } else if (ahead != BLK_PISTON_ARM) {
                            int bx = ax + fdx, by = ay + fdy, bz = az + fdz;
                            BlockId beyond = craft_world_get(bx, by, bz);
                            if (beyond == BLK_AIR) {
                                craft_world_set(bx, by, bz, ahead);
                                craft_world_set(ax, ay, az, BLK_PISTON_ARM);
                                craft_torches_record_orient(ax, ay, az, face);
                                craft_world_set(wx, wy, wz, BLK_PISTON_ON);
                            }
                        }
                    }
                    else if (b == BLK_TNT) {
                        craft_world_set(wx, wy, wz, BLK_TNT_FUSED);
                        fuse_add(wx, wy, wz);
                    }
                } else {
                    if (b == BLK_TRAPDOOR_ON)
                        craft_world_set(wx, wy, wz, BLK_TRAPDOOR_OFF);
                    else if (b == BLK_DOOR_ON) {
                        craft_world_set(wx, wy, wz, BLK_DOOR_OFF);
                        DOOR_SYNC_NEIGHBOUR(wx, wy, wz, BLK_DOOR_OFF);
                    }
                    else if (b == BLK_PISTON_ON) {
                        /* Sticky retract — pull back any block that
                         * was pushed past the arm. The arm sits at
                         * piston+face; if a block sits at piston+2×face,
                         * drag it back to the arm cell. Then clear
                         * the cell beyond. */
                        extern int craft_redstone_piston_orient(int wx, int wy, int wz);
                        int face = craft_redstone_piston_orient(wx, wy, wz);
                        int fdx = 0, fdy = 0, fdz = 0;
                        switch (face) {
                            case 0: fdx =  1; break;
                            case 1: fdx = -1; break;
                            case 2: fdy =  1; break;
                            case 3: fdy = -1; break;
                            case 4: fdz =  1; break;
                            default: fdz = -1; break;
                        }
                        int ax = wx + fdx, ay = wy + fdy, az = wz + fdz;
                        int bx = ax + fdx, by = ay + fdy, bz = az + fdz;
                        if (craft_world_get(ax, ay, az) == BLK_PISTON_ARM) {
                            BlockId beyond = craft_world_get(bx, by, bz);
                            if (beyond != BLK_AIR && beyond != BLK_PISTON_ARM) {
                                /* Drag pushed block back. */
                                craft_world_set(ax, ay, az, beyond);
                                craft_world_set(bx, by, bz, BLK_AIR);
                            } else {
                                craft_world_set(ax, ay, az, BLK_AIR);
                            }
                        }
                        craft_world_set(wx, wy, wz, BLK_PISTON_OFF);
                    }
                }
            }
        }
    }

    /* Phase 4 — passive sinks (LAMP, NOTE_BLOCK). After BFS has
     * resolved wire_on states, walk the window once more and update
     * the cell's upper-bit "lit / was-powered" flag. Rising-edge on
     * a NOTE_BLOCK fires a tone. */
    {
        static const int dx6[6] = { 1,-1, 0, 0, 0, 0 };
        static const int dy6[6] = { 0, 0, 1,-1, 0, 0 };
        static const int dz6[6] = { 0, 0, 0, 0, 1,-1 };
        for (int wy = 0; wy < CRAFT_WORLD_Y; wy++) {
            for (int lz = 0; lz < CRAFT_WORLD_Z; lz++) {
                for (int lx = 0; lx < CRAFT_WORLD_X; lx++) {
                    int idx = (wy * CRAFT_WORLD_Z + lz) * CRAFT_WORLD_X + lx;
                    BlockId b = (BlockId)craft_world_blocks[idx];
                    bool is_lamp = (b == BLK_LAMP || b == BLK_LAMP_ON);
                    bool is_note = (b == BLK_NOTE_BLOCK || b == BLK_NOTE_BLOCK_ON);
                    bool is_not  = (b == BLK_NOT_GATE  || b == BLK_NOT_GATE_ON);
                    bool is_dly  = (b == BLK_DELAY     || b == BLK_DELAY_ON);
                    if (!is_lamp && !is_note && !is_not && !is_dly) continue;

                    /* Compute the cell's "input is powered" condition.
                     * Gates (NOT/DELAY) only care about their input
                     * face; sinks (LAMP/NOTE_BLOCK) accept power from
                     * any 6-neighbour. */
                    bool input_powered = false;
                    if (is_not || is_dly) {
                        int face = craft_torches_lookup_orient(lx + ox, wy, lz + oz);
                        int odx = 0, ody = 0, odz = 0;
                        switch (face) {
                            case FACE_PX: odx =  1; break;
                            case FACE_NX: odx = -1; break;
                            case FACE_PY: ody =  1; break;
                            case FACE_NY: ody = -1; break;
                            case FACE_PZ: odz =  1; break;
                            default:      odz = -1; break;
                        }
                        int ilx = lx - odx, iwy = wy - ody, ilz = lz - odz;
                        if ((unsigned)ilx < CRAFT_WORLD_X &&
                            (unsigned)ilz < CRAFT_WORLD_Z &&
                            (unsigned)iwy < CRAFT_WORLD_Y) {
                            int iidx = (iwy * CRAFT_WORLD_Z + ilz) * CRAFT_WORLD_X + ilx;
                            BlockId ib = (BlockId)craft_world_blocks[iidx];
                            if (ib == BLK_LEVER_ON || ib == BLK_REDSTONE_WIRE_ON ||
                                ib == BLK_REDSTONE_BLOCK ||
                                ib == BLK_NOT_GATE_ON || ib == BLK_DELAY_ON) {
                                input_powered = true;
                            }
                        }
                    } else {
                        for (int d = 0; d < 6 && !input_powered; d++) {
                            int nlx = lx + dx6[d];
                            int nwy = wy + dy6[d];
                            int nlz = lz + dz6[d];
                            if ((unsigned)nlx >= CRAFT_WORLD_X) continue;
                            if ((unsigned)nlz >= CRAFT_WORLD_Z) continue;
                            if ((unsigned)nwy >= CRAFT_WORLD_Y) continue;
                            int nidx = (nwy * CRAFT_WORLD_Z + nlz) * CRAFT_WORLD_X + nlx;
                            BlockId nb = (BlockId)craft_world_blocks[nidx];
                            if (nb == BLK_LEVER_ON || nb == BLK_REDSTONE_WIRE_ON ||
                                nb == BLK_REDSTONE_BLOCK ||
                                nb == BLK_NOT_GATE_ON || nb == BLK_DELAY_ON) {
                                input_powered = true;
                            }
                        }
                        if (!input_powered && s_pad_held) {
                            int dwx = (lx + ox) - s_pad_wx;
                            int dwy = wy - s_pad_wy;
                            int dwz = (lz + oz) - s_pad_wz;
                            if (dwx*dwx + dwy*dwy + dwz*dwz == 1) input_powered = true;
                        }
                    }

                    /* Compute next-tick state and the cell ID we want
                     * to land on. Each block type maps "input
                     * powered" → next-tick output:
                     *   NOT_GATE:    inverter → emit when input off
                     *   DELAY:       buffer   → emit when input on
                     *   LAMP:        sink     → lit when input on
                     *   NOTE_BLOCK:  sink + rising-edge tone */
                    BlockId next;
                    if (is_not)       next = input_powered ? BLK_NOT_GATE     : BLK_NOT_GATE_ON;
                    else if (is_dly)  next = input_powered ? BLK_DELAY_ON     : BLK_DELAY;
                    else if (is_lamp) next = input_powered ? BLK_LAMP_ON      : BLK_LAMP;
                    else              next = input_powered ? BLK_NOTE_BLOCK_ON : BLK_NOTE_BLOCK;

                    if (is_note && next == BLK_NOTE_BLOCK_ON && b == BLK_NOTE_BLOCK) {
                        /* Rising edge — play a tone. Pitch tuning per
                         * block (B-cycle) is a follow-up; mid-range
                         * default for now. */
                        craft_audio_note(12);
                    }
                    if (next != b) {
                        /* Persisting the state transition via
                         * craft_world_set funnels through mod_set, so
                         * the gate/sink state survives saves + window
                         * reloads instead of resetting every restore. */
                        craft_world_set(lx + ox, wy, lz + oz, next);
                        state_changed = true;
                    }
                }
            }
        }
    }

    /* Batched torch-list rebuild — covers all the wire-state writes
     * above with one window scan instead of N (one per craft_world_set
     * call) that would happen if we used the player-place path. */
    if (state_changed) craft_torches_rebuild();
}

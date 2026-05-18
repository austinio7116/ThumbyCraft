/*
 * ThumbyCraft — redstone power propagation (impl).
 */
#include "craft_redstone.h"
#include "craft_world.h"
#include "craft_blocks.h"
#include "craft_mobs.h"
#include "craft_torches.h"

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
        BlockId b = (BlockId)(craft_world_blocks[i] & 0x3F);
        if (b == BLK_LEVER_ON || b == BLK_REDSTONE_WIRE_ON) n++;
    }
    s_active = n;
}

void craft_redstone_note_change(BlockId prev, BlockId blk) {
    int delta = 0;
    if (prev == BLK_LEVER_ON || prev == BLK_REDSTONE_WIRE_ON) delta--;
    if (blk  == BLK_LEVER_ON || blk  == BLK_REDSTONE_WIRE_ON) delta++;
    s_active += delta;
    if (s_active < 0) s_active = 0;   /* clamp against drift */
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
     * note_change (on craft_world_set) and by the writes below. */
    if (s_active == 0) return;

    int ox = craft_world_origin_x;
    int oz = craft_world_origin_z;

    /* Phase 1 — collect lever_on positions and seed BFS. Also reset
     * any existing wire_on cells to wire (we'll re-mark below). */
    s_visited_n = 0;
    int frontier_n = 0;
    bool state_changed = false;

    for (int wy = 0; wy < CRAFT_WORLD_Y; wy++) {
        for (int lz = 0; lz < CRAFT_WORLD_Z; lz++) {
            for (int lx = 0; lx < CRAFT_WORLD_X; lx++) {
                /* Direct backing-array read avoids craft_world_get's
                 * bounds checks. The whole window-scan otherwise eats
                 * ~14 ms of CPU per tick at 280 MHz, which would land
                 * as a periodic frame-stutter at 5 Hz. */
                int idx = (wy * CRAFT_WORLD_Z + lz) * CRAFT_WORLD_X + lx;
                BlockId b = (BlockId)(craft_world_blocks[idx] & 0x3F);
                if (b == BLK_LEVER_ON) {
                    /* Lever is the source — seed adjacent cells, not
                     * the lever itself (lever stays as LEVER_ON). */
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

    /* Batched torch-list rebuild — covers all the wire-state writes
     * above with one window scan instead of N (one per craft_world_set
     * call) that would happen if we used the player-place path. */
    if (state_changed) craft_torches_rebuild();
}

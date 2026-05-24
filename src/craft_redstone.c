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

/* --- Transient per-cell meta store -------------------------------- *
 * A small open-addressed hash mapping a world cell to one byte of
 * scratch state. Used by OBSERVER (last-seen block id of its watched
 * face) and DELAY (remaining countdown ticks). Deliberately NOT
 * persisted — observers re-latch and repeaters reset their countdown
 * on reload, which is harmless. Keyed in world coords so it survives
 * window slides. */
#define META_HASH_SIZE 128
#define META_HASH_MASK (META_HASH_SIZE - 1)
typedef struct {
    int32_t wx, wz;
    int16_t wy;
    uint8_t val;
    uint8_t flags;          /* bit 0 = occupied */
} MetaEntry;
static MetaEntry s_meta[META_HASH_SIZE];

static MetaEntry *meta_find(int wx, int wy, int wz, bool insert) {
    uint32_t h = (uint32_t)wx * 73856093u
               ^ (uint32_t)wy * 19349663u
               ^ (uint32_t)wz * 83492791u;
    h ^= h >> 16;
    for (int probe = 0; probe < META_HASH_SIZE; probe++) {
        MetaEntry *e = &s_meta[(h + probe) & META_HASH_MASK];
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
static int meta_get(int wx, int wy, int wz, int def) {
    MetaEntry *e = meta_find(wx, wy, wz, false);
    return e ? (int)e->val : def;
}
static void meta_set(int wx, int wy, int wz, uint8_t val) {
    MetaEntry *e = meta_find(wx, wy, wz, true);
    if (!e) return;
    if (!(e->flags & 1)) {
        e->wx = wx; e->wz = wz; e->wy = (int16_t)wy; e->flags = 1;
    }
    e->val = val;
}

/* --- Redstone cell registry --------------------------------------
 *
 * The tick used to scan the entire 64^3 window FIVE times per tick
 * (observer / gate-seed / source / driven / sink passes). Any
 * always-evaluated block (observer, gate, lamp, note block) keeps the
 * fast-path from firing, so that 5x full scan ran at 5 Hz and showed
 * up as a steady framerate hit — the old code was only cheap with
 * levers/wire/pistons because those go fully idle when unpowered.
 *
 * Instead we keep a flat list of every redstone-interacting cell in
 * the window and iterate THAT. Cost now scales with how much redstone
 * you've placed, not world size. The list is rebuilt by a full scan
 * only on window load/shift (craft_redstone_rescan) and on the first
 * tick after a redstone block is placed/removed (s_rs_dirty). Each
 * phase re-reads the block at every listed cell and type-checks it, so
 * a stale entry (cell since changed) is simply skipped — harmless. */
#define RS_MAX 1024
static uint8_t s_rs_lx[RS_MAX], s_rs_wy[RS_MAX], s_rs_lz[RS_MAX];
static int     s_rs_n;
static bool    s_rs_dirty;
static bool    s_in_tick;

/* A cell belongs in the registry if it is any redstone-interacting
 * block — sources, wire, gates, sinks, observers, and driven blocks. */
static inline bool rs_relevant(BlockId b) {
    switch (b) {
        case BLK_REDSTONE_WIRE:     case BLK_REDSTONE_WIRE_ON:
        case BLK_LEVER_OFF:         case BLK_LEVER_ON:
        case BLK_REDSTONE_BLOCK:
        case BLK_NOT_GATE:          case BLK_NOT_GATE_ON:
        case BLK_DELAY:             case BLK_DELAY_ON:
        case BLK_LAMP:              case BLK_LAMP_ON:
        case BLK_NOTE_BLOCK:        case BLK_NOTE_BLOCK_ON:
        case BLK_OBSERVER:          case BLK_OBSERVER_ON:
        case BLK_DISPENSER:         case BLK_DISPENSER_ON:
        case BLK_TARGET:            case BLK_TARGET_ON:
        case BLK_PISTON_OFF:        case BLK_PISTON_ON:
        case BLK_STICKY_PISTON_OFF: case BLK_STICKY_PISTON_ON:
        case BLK_DOOR_OFF:          case BLK_DOOR_ON:
        case BLK_TRAPDOOR_OFF:      case BLK_TRAPDOOR_ON:
        case BLK_TNT:               case BLK_TNT_FUSED:
            return true;
        default:
            return false;
    }
}

/* True if a cell counts toward s_active — the "something is live"
 * fast-path gate. Sources + always-evaluated gates/sinks/observers;
 * the purely reactive DISPENSER/TARGET only count in their _ON state
 * (so an idle one doesn't keep the sim awake). */
static inline bool rs_active_block(BlockId b) {
    return b == BLK_LEVER_ON || b == BLK_REDSTONE_WIRE_ON ||
           b == BLK_REDSTONE_BLOCK ||
           b == BLK_NOT_GATE   || b == BLK_NOT_GATE_ON   ||
           b == BLK_DELAY      || b == BLK_DELAY_ON      ||
           b == BLK_LAMP       || b == BLK_LAMP_ON       ||
           b == BLK_NOTE_BLOCK || b == BLK_NOTE_BLOCK_ON ||
           b == BLK_OBSERVER   || b == BLK_OBSERVER_ON   ||
           b == BLK_DISPENSER_ON || b == BLK_TARGET_ON;
}

/* One full-window scan: rebuild the cell registry and recompute
 * s_active together. Runs on load/shift and when the list is dirty. */
static void rs_full_scan(void) {
    s_rs_n = 0;
    int active = 0;
    for (int wy = 0; wy < CRAFT_WORLD_Y; wy++) {
        for (int lz = 0; lz < CRAFT_WORLD_Z; lz++) {
            for (int lx = 0; lx < CRAFT_WORLD_X; lx++) {
                BlockId b = (BlockId)craft_world_blocks[
                    (wy * CRAFT_WORLD_Z + lz) * CRAFT_WORLD_X + lx];
                if (rs_active_block(b)) active++;
                if (!rs_relevant(b)) continue;
                if (s_rs_n < RS_MAX) {
                    s_rs_lx[s_rs_n] = (uint8_t)lx;
                    s_rs_wy[s_rs_n] = (uint8_t)wy;
                    s_rs_lz[s_rs_n] = (uint8_t)lz;
                    s_rs_n++;
                }
            }
        }
    }
    s_active   = active;
    s_rs_dirty = false;
}

void craft_redstone_init(void) {
    s_accum       = 0.0f;
    s_activated_n = 0;
    s_active      = 0;
    s_rs_n        = 0;
    s_rs_dirty    = false;
    s_in_tick     = false;
    memset(s_meta, 0, sizeof s_meta);
}

void craft_redstone_rescan(void) {
    rs_full_scan();
}

void craft_redstone_note_change(BlockId prev, BlockId blk) {
    /* Keep s_active (the fast-path "something is live" counter) in sync.
     * DISPENSER / TARGET are purely reactive, so only their _ON states
     * count — see rs_active_block. */
    if (rs_active_block(prev)) s_active--;
    if (rs_active_block(blk))  s_active++;
    if (s_active < 0) s_active = 0;   /* clamp against drift */

    /* Registry membership changed (a redstone block was placed or
     * removed) → flag a one-shot rebuild on the next tick. Pure state
     * toggles (both sides relevant) and the tick's own writes
     * (s_in_tick) leave the cell list unchanged, so they don't dirty. */
    if (!s_in_tick && rs_relevant(prev) != rs_relevant(blk))
        s_rs_dirty = true;
}

/* --- Pressure pad latch + TNT fuse list -------------------------- *
 * Player tick reports its current pressure-pad cell each frame; the
 * redstone tick treats that as a power source. Single-cell latch is
 * enough for player demos. */
/* Pressed-pad set — rebuilt every frame by craft_redstone_pads_clear()
 * + per-entity note_pressure. Holds the player's pad plus any pads
 * mobs are standing on, so several pads can be active at once. */
#define PAD_MAX 16
static int s_pad_wx[PAD_MAX], s_pad_wy[PAD_MAX], s_pad_wz[PAD_MAX];
static int s_pad_n;
static int s_pad_n_prev;   /* pad count at the previous tick — see fast path */

void craft_redstone_pads_clear(void) { s_pad_n = 0; }

void craft_redstone_note_pressure(int wx, int wy, int wz) {
    if (wy < 0) return;                 /* legacy "not on a pad" — no-op */
    for (int i = 0; i < s_pad_n; i++)   /* dedup: two entities on one pad */
        if (s_pad_wx[i] == wx && s_pad_wy[i] == wy && s_pad_wz[i] == wz) return;
    if (s_pad_n >= PAD_MAX) return;
    s_pad_wx[s_pad_n] = wx; s_pad_wy[s_pad_n] = wy; s_pad_wz[s_pad_n] = wz;
    s_pad_n++;
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
     * source in a circuit. We also run one extra "settle" tick after
     * the last pad releases (s_pad_n_prev) so pad-driven dispensers /
     * doors that latched _ON without any wire get reverted instead of
     * staying stuck on. */
    if (s_active == 0 && s_pad_n == 0 && s_pad_n_prev == 0) return;

    int ox = craft_world_origin_x;
    int oz = craft_world_origin_z;

    /* Guard note_change against re-dirtying the list from our own
     * writes, and rebuild the cell registry once if a redstone block
     * was placed/removed since the last tick. Every phase below then
     * iterates s_rs (the redstone cells) instead of the full window. */
    s_in_tick = true;
    if (s_rs_dirty) rs_full_scan();

    /* Phase 1 — collect lever_on positions and seed BFS. Also reset
     * any existing wire_on cells to wire (we'll re-mark below). */
    s_visited_n = 0;
    int frontier_n = 0;
    bool state_changed = false;

    /* Snapshot the lit-wire set. A wire that stays powered is reset to
     * WIRE then re-set to WIRE_ON every tick with no net change — only a
     * genuine change to which wires are lit should trigger the (full-
     * window) torch-list rebuild, otherwise a single lit wire rebuilds
     * every tick and tanks the framerate. */
    uint32_t wire_sum_before = 0;
    for (int ri = 0; ri < s_rs_n; ri++) {
        int wi = (s_rs_wy[ri] * CRAFT_WORLD_Z + s_rs_lz[ri]) * CRAFT_WORLD_X + s_rs_lx[ri];
        if (craft_world_blocks[wi] == BLK_REDSTONE_WIRE_ON)
            wire_sum_before ^= (uint32_t)wi * 2654435761u;
    }

    /* Phase 0.4 — observer edge detection. An OBSERVER watches the
     * block one cell along its orient face; when that block's id
     * changes it fires a 1-tick pulse from its BACK face. We model
     * the pulse as the OBSERVER_ON state lasting exactly one tick:
     *   - If already OBSERVER_ON, the pulse just ended → revert.
     *   - Else compare the watched block to the cached last-seen
     *     value; on mismatch latch the new value and go ON. */
    for (int ri = 0; ri < s_rs_n; ri++) {
        int lx = s_rs_lx[ri], wy = s_rs_wy[ri], lz = s_rs_lz[ri];
                int idx = (wy * CRAFT_WORLD_Z + lz) * CRAFT_WORLD_X + lx;
                BlockId b = (BlockId)craft_world_blocks[idx];
                if (b == BLK_OBSERVER_ON) {
                    craft_world_set(lx + ox, wy, lz + oz, BLK_OBSERVER);
                    state_changed = true;
                    continue;
                }
                if (b != BLK_OBSERVER) continue;
                int face = craft_torches_lookup_orient(lx + ox, wy, lz + oz) & 0x07;
                int odx = 0, ody = 0, odz = 0;
                switch (face) {
                    case FACE_PX: odx =  1; break;
                    case FACE_NX: odx = -1; break;
                    case FACE_PY: ody =  1; break;
                    case FACE_NY: ody = -1; break;
                    case FACE_PZ: odz =  1; break;
                    default:      odz = -1; break;
                }
                int wlx = lx + odx, wwy = wy + ody, wlz = lz + odz;
                uint8_t watched = BLK_AIR;
                if ((unsigned)wlx < CRAFT_WORLD_X &&
                    (unsigned)wlz < CRAFT_WORLD_Z &&
                    (unsigned)wwy < CRAFT_WORLD_Y) {
                    watched = craft_world_blocks[
                        (wwy * CRAFT_WORLD_Z + wlz) * CRAFT_WORLD_X + wlx];
                }
                int last = meta_get(lx + ox, wy, lz + oz, (int)watched);
                if ((int)watched != last) {
                    meta_set(lx + ox, wy, lz + oz, watched);
                    craft_world_set(lx + ox, wy, lz + oz, BLK_OBSERVER_ON);
                    state_changed = true;
                } else {
                    /* Keep the cache fresh even on the first sighting
                     * so a freshly placed observer doesn't fire. */
                    meta_set(lx + ox, wy, lz + oz, watched);
                }
    }

    /* Phase 0.5 — gate seeding. NOT_GATE / DELAY emit from their
     * orient (output) face; OBSERVER_ON emits from its BACK face
     * (opposite orient). Their OFF variants don't. The NOT/DELAY
     * state transition happens in the post-BFS pass below — that
     * gives gates the one-tick lag wired into vanilla redstone. */
    for (int ri = 0; ri < s_rs_n; ri++) {
        int lx = s_rs_lx[ri], wy = s_rs_wy[ri], lz = s_rs_lz[ri];
                int idx = (wy * CRAFT_WORLD_Z + lz) * CRAFT_WORLD_X + lx;
                BlockId b = (BlockId)craft_world_blocks[idx];
                if (b != BLK_NOT_GATE_ON && b != BLK_DELAY_ON &&
                    b != BLK_OBSERVER_ON) continue;

                int face = craft_torches_lookup_orient(lx + ox, wy, lz + oz) & 0x07;
                int odx = 0, ody = 0, odz = 0;
                switch (face) {
                    case FACE_PX: odx =  1; break;
                    case FACE_NX: odx = -1; break;
                    case FACE_PY: ody =  1; break;
                    case FACE_NY: ody = -1; break;
                    case FACE_PZ: odz =  1; break;
                    default:      odz = -1; break;
                }
                /* Observer outputs from the back — invert direction. */
                if (b == BLK_OBSERVER_ON) { odx = -odx; ody = -ody; odz = -odz; }
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

    for (int ri = 0; ri < s_rs_n; ri++) {
        int lx = s_rs_lx[ri], wy = s_rs_wy[ri], lz = s_rs_lz[ri];
                /* Direct backing-array read avoids craft_world_get's
                 * bounds checks. The whole window-scan otherwise eats
                 * ~14 ms of CPU per tick at 280 MHz, which would land
                 * as a periodic frame-stutter at 5 Hz. */
                int idx = (wy * CRAFT_WORLD_Z + lz) * CRAFT_WORLD_X + lx;
                BlockId b = (BlockId)craft_world_blocks[idx];
                /* LEVER_ON, solid REDSTONE_BLOCK and a TARGET_ON
                 * (arrow-struck this tick) all act as omnidirectional
                 * power sources — seed adjacent wires on every face. */
                if (b == BLK_LEVER_ON || b == BLK_REDSTONE_BLOCK ||
                    b == BLK_TARGET_ON) {
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
                    /* Reset to unpowered; BFS may re-power below. This
                     * reset+repower churns every tick a wire stays lit,
                     * so it deliberately does NOT set state_changed — the
                     * wire-set checksum decides if a real change happened. */
                    craft_world_set_byte(lx + ox, wy, lz + oz,
                                         (uint8_t)BLK_REDSTONE_WIRE);
                    s_active--;
                }
    }

    /* Pressure pads as sources — each pressed pad (player or any mob)
     * seeds its 6-adjacent wire cells just like a lever_on would. */
    for (int pi = 0; pi < s_pad_n; pi++) {
        int plx = s_pad_wx[pi] - ox;
        int plz = s_pad_wz[pi] - oz;
        int pwy = s_pad_wy[pi];
        if ((unsigned)plx >= CRAFT_WORLD_X || (unsigned)plz >= CRAFT_WORLD_Z ||
            (unsigned)pwy >= CRAFT_WORLD_Y) continue;
        static const int dx[6] = { 1,-1, 0, 0, 0, 0 };
        static const int dy[6] = { 0, 0, 1,-1, 0, 0 };
        static const int dz[6] = { 0, 0, 0, 0, 1,-1 };
        for (int d = 0; d < 6; d++) {
            int nlx = plx + dx[d];
            int nwy = pwy + dy[d];
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

    /* Phase 2 — BFS through the wire mesh, painting each visited
     * cell to WIRE_ON and checking for adjacent diamond blocks. */
    int head = 0;
    while (head < frontier_n) {
        RsCell c = s_frontier[head++];
        craft_world_set_byte(c.lx + ox, c.wy, c.lz + oz,
                             (uint8_t)BLK_REDSTONE_WIRE_ON);
        s_active++;
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

    /* Phase 3 — drive trapdoors / doors / pistons / dispensers / TNT
     * from adjacent power. For each candidate block, check its
     * 6-neighbour cells for a power source (LEVER_ON / WIRE_ON / the
     * active pressure-pad cell). */
    for (int ri = 0; ri < s_rs_n; ri++) {
        int lx = s_rs_lx[ri], wy = s_rs_wy[ri], lz = s_rs_lz[ri];
                int idx = (wy * CRAFT_WORLD_Z + lz) * CRAFT_WORLD_X + lx;
                BlockId b = (BlockId)craft_world_blocks[idx];
                bool is_driven =
                    b == BLK_TRAPDOOR_OFF || b == BLK_TRAPDOOR_ON ||
                    b == BLK_DOOR_OFF     || b == BLK_DOOR_ON     ||
                    b == BLK_PISTON_OFF   || b == BLK_PISTON_ON   ||
                    b == BLK_STICKY_PISTON_OFF || b == BLK_STICKY_PISTON_ON ||
                    b == BLK_DISPENSER    || b == BLK_DISPENSER_ON ||
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
                /* Pressure-pad direct neighbour check — any pressed pad. */
                for (int pi = 0; pi < s_pad_n; pi++) {
                    int adwx = (lx + ox) - s_pad_wx[pi];
                    int adwy = wy        - s_pad_wy[pi];
                    int adwz = (lz + oz) - s_pad_wz[pi];
                    if (adwx*adwx + adwy*adwy + adwz*adwz == 1) {
                        powered = true;
                        redstone_adj = true;
                        break;
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
                    else if (b == BLK_PISTON_OFF || b == BLK_STICKY_PISTON_OFF) {
                        /* Extend along the piston's orient axis. Push
                         * the cell directly in front (if air → arm goes
                         * there; if solid → shove it one further along
                         * and place the arm at the front cell). Regular
                         * and sticky extend identically — only retract
                         * differs. */
                        bool sticky = (b == BLK_STICKY_PISTON_OFF);
                        BlockId on_id = sticky ? BLK_STICKY_PISTON_ON
                                               : BLK_PISTON_ON;
                        extern int craft_redstone_piston_orient(int wx, int wy, int wz);
                        int face = craft_redstone_piston_orient(wx, wy, wz) & 0x07;
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
                            craft_world_set(wx, wy, wz, on_id);
                            craft_world_set(ax, ay, az, BLK_PISTON_ARM);
                            craft_torches_record_orient(ax, ay, az, face);
                        } else if (ahead != BLK_PISTON_ARM) {
                            int bx = ax + fdx, by = ay + fdy, bz = az + fdz;
                            BlockId beyond = craft_world_get(bx, by, bz);
                            if (beyond == BLK_AIR) {
                                craft_world_set(bx, by, bz, ahead);
                                craft_world_set(ax, ay, az, BLK_PISTON_ARM);
                                craft_torches_record_orient(ax, ay, az, face);
                                craft_world_set(wx, wy, wz, on_id);
                            }
                        }
                    }
                    else if (b == BLK_TNT) {
                        craft_world_set(wx, wy, wz, BLK_TNT_FUSED);
                        fuse_add(wx, wy, wz);
                    }
                    else if (b == BLK_DISPENSER) {
                        /* Rising edge — fire one arrow straight along the
                         * orient face, then latch to DISPENSER_ON so a
                         * held signal only fires once. The arrow damages
                         * whatever it hits (player or mob). Ammo is
                         * intentionally infinite in v1 — no per-block
                         * inventory plumbing yet. */
                        int face = craft_torches_lookup_orient(wx, wy, wz) & 0x07;
                        float fdx = 0, fdy = 0, fdz = 0;
                        switch (face) {
                            case FACE_PX: fdx =  1; break;
                            case FACE_NX: fdx = -1; break;
                            case FACE_PY: fdy =  1; break;
                            case FACE_NY: fdy = -1; break;
                            case FACE_PZ: fdz =  1; break;
                            default:      fdz = -1; break;
                        }
                        Vec3 apos = { wx + 0.5f + fdx * 0.6f,
                                      wy + 0.5f + fdy * 0.6f,
                                      wz + 0.5f + fdz * 0.6f };
                        Vec3 avel = { fdx * 14.0f, fdy * 14.0f, fdz * 14.0f };
                        craft_arrows_spawn(apos, avel);
                        craft_audio_place(BLK_ARROW);
                        craft_world_set(wx, wy, wz, BLK_DISPENSER_ON);
                    }
                } else {
                    if (b == BLK_TRAPDOOR_ON)
                        craft_world_set(wx, wy, wz, BLK_TRAPDOOR_OFF);
                    else if (b == BLK_DISPENSER_ON)
                        craft_world_set(wx, wy, wz, BLK_DISPENSER);
                    else if (b == BLK_DOOR_ON) {
                        craft_world_set(wx, wy, wz, BLK_DOOR_OFF);
                        DOOR_SYNC_NEIGHBOUR(wx, wy, wz, BLK_DOOR_OFF);
                    }
                    else if (b == BLK_PISTON_ON || b == BLK_STICKY_PISTON_ON) {
                        /* Retract. The arm sits at piston+face. A
                         * STICKY piston drags the block sitting beyond
                         * the arm (piston+2×face) back into the arm
                         * cell; a regular piston just retracts the arm
                         * and leaves the pushed block where it is. */
                        bool sticky = (b == BLK_STICKY_PISTON_ON);
                        extern int craft_redstone_piston_orient(int wx, int wy, int wz);
                        int face = craft_redstone_piston_orient(wx, wy, wz) & 0x07;
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
                            if (sticky && beyond != BLK_AIR &&
                                beyond != BLK_PISTON_ARM) {
                                /* Drag pushed block back. */
                                craft_world_set(ax, ay, az, beyond);
                                craft_world_set(bx, by, bz, BLK_AIR);
                            } else {
                                craft_world_set(ax, ay, az, BLK_AIR);
                            }
                        }
                        craft_world_set(wx, wy, wz,
                                        sticky ? BLK_STICKY_PISTON_OFF
                                               : BLK_PISTON_OFF);
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
        for (int ri = 0; ri < s_rs_n; ri++) {
            int lx = s_rs_lx[ri], wy = s_rs_wy[ri], lz = s_rs_lz[ri];
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
                        /* Low 3 bits of the orient byte are the face;
                         * DELAY packs its delay setting in bits 3-4. */
                        int face = craft_torches_lookup_orient(lx + ox, wy, lz + oz) & 0x07;
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
                                ib == BLK_NOT_GATE_ON || ib == BLK_DELAY_ON ||
                                ib == BLK_OBSERVER_ON || ib == BLK_TARGET_ON) {
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
                                nb == BLK_NOT_GATE_ON || nb == BLK_DELAY_ON ||
                                nb == BLK_OBSERVER_ON || nb == BLK_TARGET_ON) {
                                input_powered = true;
                            }
                        }
                        for (int pi = 0; !input_powered && pi < s_pad_n; pi++) {
                            int dwx = (lx + ox) - s_pad_wx[pi];
                            int dwy = wy         - s_pad_wy[pi];
                            int dwz = (lz + oz) - s_pad_wz[pi];
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
                    else if (is_lamp) next = input_powered ? BLK_LAMP_ON      : BLK_LAMP;
                    else if (is_note) next = input_powered ? BLK_NOTE_BLOCK_ON : BLK_NOTE_BLOCK;
                    else /* is_dly */ {
                        /* Repeater with a 1-4 tick adjustable delay
                         * (bits 3-4 of the orient byte, +1). A
                         * countdown in the transient meta store gates
                         * the OFF<->ON flip: it starts when the desired
                         * output differs from the current state and
                         * the flip only lands when it reaches zero. */
                        int setting = (craft_torches_lookup_orient(
                                           lx + ox, wy, lz + oz) >> 3) & 0x03;
                        bool want_on = input_powered;
                        bool is_on   = (b == BLK_DELAY_ON);
                        if (want_on == is_on) {
                            meta_set(lx + ox, wy, lz + oz, 0);
                            next = b;
                        } else {
                            int cd = meta_get(lx + ox, wy, lz + oz, 0);
                            if (cd <= 0) cd = setting + 1;
                            cd--;
                            if (cd <= 0) {
                                next = want_on ? BLK_DELAY_ON : BLK_DELAY;
                                meta_set(lx + ox, wy, lz + oz, 0);
                            } else {
                                next = b;
                                meta_set(lx + ox, wy, lz + oz, (uint8_t)cd);
                            }
                        }
                    }

                    if (is_note && next == BLK_NOTE_BLOCK_ON && b == BLK_NOTE_BLOCK) {
                        /* Rising edge — play the per-block pitch stored
                         * in the orient slot (note blocks have no
                         * facing). Defaults to mid-range if unset. */
                        int pitch = craft_torches_lookup_orient(lx + ox, wy, lz + oz);
                        if (pitch < 0 || pitch > 23) pitch = 12;
                        craft_audio_note(pitch);
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

    /* Phase 5 — end TARGET pulses. A TARGET_ON emitted power this
     * whole tick (phase 0.5 seeded its wires, the BFS propagated);
     * revert it to TARGET now so the pulse is exactly one tick long.
     * A fresh arrow hit next frame re-arms it. */
    for (int ri = 0; ri < s_rs_n; ri++) {
        int lx = s_rs_lx[ri], wy = s_rs_wy[ri], lz = s_rs_lz[ri];
                int idx = (wy * CRAFT_WORLD_Z + lz) * CRAFT_WORLD_X + lx;
                if (craft_world_blocks[idx] == BLK_TARGET_ON) {
                    craft_world_set(lx + ox, wy, lz + oz, BLK_TARGET);
                    state_changed = true;
                }
    }

    s_in_tick = false;

    /* Re-checksum the lit-wire set; compare to the snapshot taken at the
     * top of the tick. */
    uint32_t wire_sum_after = 0;
    for (int ri = 0; ri < s_rs_n; ri++) {
        int wi = (s_rs_wy[ri] * CRAFT_WORLD_Z + s_rs_lz[ri]) * CRAFT_WORLD_X + s_rs_lx[ri];
        if (craft_world_blocks[wi] == BLK_REDSTONE_WIRE_ON)
            wire_sum_after ^= (uint32_t)wi * 2654435761u;
    }

    /* Rebuild the sprite list only on a REAL change: a wire actually
     * switched on/off (checksum differs), or a non-wire sprite-bearing
     * change happened (state_changed — observers/gates only set this on
     * an actual transition, never every tick). A steadily-powered wire
     * matches neither, so it no longer forces a full rebuild per tick. */
    if (state_changed || wire_sum_before != wire_sum_after)
        craft_torches_rebuild();

    /* Remember this tick's pad count so the next tick still runs once
     * after the last pad releases — that settle tick reverts pad-driven
     * dispensers / doors that have no wire to clear them otherwise. */
    s_pad_n_prev = s_pad_n;
}

/*
 * ThumbyCraft — water flow simulation (impl).
 *
 * One tick every WATER_TICK_PERIOD seconds. Per tick:
 *   1. Scan window for water cells (BLK_WATER, any level).
 *   2. For each: queue propagation events into adjacent air cells.
 *   3. For each non-source flowing cell with no neighbour at a
 *      lower level: queue a decay event.
 *   4. Apply queued events at end of tick.
 *
 * The queue is bounded so a single tick can never spend more than
 * WATER_EVENT_MAX writes; remaining changes are picked up next
 * tick (the simulation converges over a few ticks regardless).
 */
#include "craft_water.h"
#include "craft_world.h"
#include "craft_gen.h"
#include "craft_blocks.h"
#include "craft_types.h"

#define WATER_TICK_PERIOD 0.20f       /* 5 Hz sim rate */
#define WATER_EVENT_MAX   256

#define WATER_LEVEL_MAX   7           /* 0 source, 1..7 flowing, >7 dries up.
                                       * Vanilla-matching reach now that the
                                       * cell ID field is the full 8 bits and
                                       * water levels live in dedicated
                                       * BlockIds rather than packed upper
                                       * bits. */

/* Local-window coords keep each event to 4 bytes — saves ~2 KB BSS
 * vs storing world int32s. Events are applied immediately after the
 * scan completes (same tick) so the window origin can't drift
 * underneath us. */
typedef struct {
    uint8_t  lx;
    uint8_t  lz;
    uint8_t  wy;
    uint8_t  byte;
} WaterEvent;

static WaterEvent s_events[WATER_EVENT_MAX];
static int        s_n_events;
static float      s_accum;

void craft_water_init(void) {
    s_accum    = 0.0f;
    s_n_events = 0;
}

/* With 8-bit BlockIds the cell byte IS the id — no more masking
 * or packed level bits. block_id_of is preserved as identity for
 * code clarity. water_level_of / water_byte just bounce through the
 * inline helpers in craft_blocks.h. */
static inline uint8_t block_id_of(uint8_t b)    { return b; }
static inline uint8_t water_level_of(uint8_t b) { return craft_water_level(b); }
static inline uint8_t water_byte(uint8_t level) {
    return (uint8_t)craft_water_for_level((int)level);
}
static inline bool    is_water(uint8_t b)       { return craft_is_water_id(b); }

static void queue_event(int lx, int wy, int lz, uint8_t byte) {
    if (s_n_events >= WATER_EVENT_MAX) return;
    WaterEvent *e = &s_events[s_n_events++];
    e->lx = (uint8_t)lx; e->wy = (uint8_t)wy; e->lz = (uint8_t)lz; e->byte = byte;
}

/* True if (wx, wy, wz) is a "source" water cell — natural lake or
 * ocean below water level. Sources never decay and always feed
 * neighbours at level 1 in the spread phase. */
static inline bool is_source_pos(int wy) {
    return wy <= CRAFT_WATER_LEVEL;
}

void craft_water_tick(float dt) {
    s_accum += dt;
    if (s_accum < WATER_TICK_PERIOD) return;
    s_accum -= WATER_TICK_PERIOD;

    int ox = craft_world_origin_x;
    int oz = craft_world_origin_z;
    s_n_events = 0;

    /* Inner loop is X (contiguous in memory) so the prefetcher
     * gets a clean run. Y is outer — water mostly clusters in a
     * narrow Y band around the water level so the inner skips are
     * mostly cheap. */
    for (int wy = 1; wy < CRAFT_WORLD_Y - 1; wy++) {
        for (int lz = 0; lz < CRAFT_WORLD_Z; lz++) {
            for (int lx = 0; lx < CRAFT_WORLD_X; lx++) {
                int idx = (wy * CRAFT_WORLD_Z + lz) * CRAFT_WORLD_X + lx;
                uint8_t b = craft_world_blocks[idx];
                if (!is_water(b)) continue;

                uint8_t lvl     = water_level_of(b);
                bool    source  = is_source_pos(wy) && lvl == 0;

                /* --- Fall down --- */
                int below_idx = idx - CRAFT_WORLD_Z * CRAFT_WORLD_X;
                uint8_t below = craft_world_blocks[below_idx];
                if (below == BLK_AIR) {
                    /* Falling water becomes effectively a fresh source
                     * for the cell below — level 0 so it can spread
                     * sideways from there too. Don't ALSO spread
                     * sideways from this cell when there's an outlet
                     * straight down (matches Minecraft behaviour). */
                    queue_event(lx, wy - 1, lz, water_byte(0));
                    continue;
                }

                /* --- Spread sideways --- *
                 * The new level for any neighbour we fill is:
                 *   - source cell → neighbour gets level 1
                 *   - flowing at L < 7 → neighbour gets level L+1
                 *   - flowing at L = 7 → can't spread further
                 */
                uint8_t spread_level = source ? 1 : (uint8_t)(lvl + 1);
                if (spread_level <= WATER_LEVEL_MAX) {
                    static const int dx[4] = { 1, -1, 0,  0 };
                    static const int dz[4] = { 0,  0, 1, -1 };
                    for (int d = 0; d < 4; d++) {
                        int nlx = lx + dx[d];
                        int nlz = lz + dz[d];
                        if ((unsigned)nlx >= CRAFT_WORLD_X) continue;
                        if ((unsigned)nlz >= CRAFT_WORLD_Z) continue;
                        int nidx = (wy * CRAFT_WORLD_Z + nlz) * CRAFT_WORLD_X + nlx;
                        uint8_t nb = craft_world_blocks[nidx];
                        if (nb == BLK_AIR) {
                            queue_event(nlx, wy, nlz,
                                        water_byte(spread_level));
                        } else if (is_water(nb) &&
                                   water_level_of(nb) > spread_level) {
                            /* Existing flowing water on this neighbour
                             * is "farther from source" than us — promote
                             * it (smaller level = closer to source) so
                             * it's tagged as fed by us. */
                            queue_event(nlx, wy, nlz,
                                        water_byte(spread_level));
                        }
                    }
                }

                /* --- Decay --- *
                 * Non-source flowing water must have at least one
                 * neighbour at a STRICTLY LOWER level (or a source
                 * directly above or beside). Otherwise it's
                 * orphaned — increment its level. At level > MAX,
                 * dry up entirely. */
                if (!source) {
                    bool fed = false;
                    /* Above */
                    int up_idx = idx + CRAFT_WORLD_Z * CRAFT_WORLD_X;
                    if (wy + 1 < CRAFT_WORLD_Y) {
                        uint8_t ub = craft_world_blocks[up_idx];
                        if (is_water(ub)) {
                            /* Falling water from above always feeds us. */
                            fed = true;
                        }
                    }
                    /* Sides */
                    if (!fed) {
                        static const int dx2[4] = { 1, -1, 0,  0 };
                        static const int dz2[4] = { 0,  0, 1, -1 };
                        for (int d = 0; d < 4 && !fed; d++) {
                            int nlx = lx + dx2[d];
                            int nlz = lz + dz2[d];
                            if ((unsigned)nlx >= CRAFT_WORLD_X) continue;
                            if ((unsigned)nlz >= CRAFT_WORLD_Z) continue;
                            int nidx = (wy * CRAFT_WORLD_Z + nlz) * CRAFT_WORLD_X + nlx;
                            uint8_t nb = craft_world_blocks[nidx];
                            if (!is_water(nb)) continue;
                            int nwy = wy;
                            bool n_source = is_source_pos(nwy) && water_level_of(nb) == 0;
                            if (n_source) { fed = true; break; }
                            if (water_level_of(nb) < lvl) { fed = true; break; }
                        }
                    }
                    if (!fed) {
                        /* Unfed flowing water decays toward MAX. At
                         * MAX it either persists (if contained) or
                         * evaporates (if exposed):
                         *
                         *   contained = every horizontal neighbour is
                         *   solid or water — i.e. there's no air for
                         *   the cell to escape into.
                         *
                         * Cells stay at MAX rather than getting
                         * promoted to source — that distinction is
                         * what kept this from cascading like the old
                         * sweetener. MAX water can't spread further
                         * (spread_level = MAX+1 > MAX), so no new
                         * tendrils form.
                         *
                         * Effect on the standard scenarios:
                         *   - Hilltop dome: edge cells touch air →
                         *     evaporate → ring shrinks → dome
                         *     dissolves cleanly.
                         *   - Player-dug pool: every cell is
                         *     surrounded by walls and other water →
                         *     pool settles at MAX and persists, even
                         *     after the upstream source is broken.
                         *   - Pool with a side gap: edge adjacent to
                         *     the gap evaporates, exposes the next
                         *     ring, drains through the gap. */
                        if (lvl >= WATER_LEVEL_MAX) {
                            static const int dxp[4] = { 1, -1, 0,  0 };
                            static const int dzp[4] = { 0,  0, 1, -1 };
                            bool contained = true;
                            for (int d = 0; d < 4 && contained; d++) {
                                int nlx = lx + dxp[d];
                                int nlz = lz + dzp[d];
                                if ((unsigned)nlx >= CRAFT_WORLD_X ||
                                    (unsigned)nlz >= CRAFT_WORLD_Z) {
                                    contained = false; break;
                                }
                                int nidx = (wy * CRAFT_WORLD_Z + nlz) * CRAFT_WORLD_X + nlx;
                                uint8_t nb2 = craft_world_blocks[nidx];
                                if (nb2 == BLK_AIR) {
                                    contained = false; break;
                                }
                            }
                            if (!contained) {
                                queue_event(lx, wy, lz, (uint8_t)BLK_AIR);
                            } else {
                                /* Contained pool — SRAM already holds
                                 * the right byte. Persist it to the
                                 * chunk store so the pool survives a
                                 * window reload instead of vanishing
                                 * and getting regenerated from a stale
                                 * source edit. mod_set is idempotent
                                 * via the prev-equals-byte guard. */
                                craft_world_persist_byte(
                                    lx + ox, wy, lz + oz, b);
                            }
                        } else {
                            queue_event(lx, wy, lz, water_byte(lvl + 1));
                        }
                    }
                }
            }
        }
    }

    /* Apply queued events. Translate local coords back to world.
     * Only TERMINAL transitions are persisted:
     *   - Evaporation (BLK_AIR) goes through craft_world_set so the
     *     chunk store learns the cell is no longer water, purging
     *     any stale player-placement edit at this position.
     *   - Level transitions stay on set_byte — purely transient. The
     *     "settled, fully contained pool" branch upstream persists
     *     those cells via craft_world_persist_byte when (and only
     *     when) they reach a stable shape. That's the deliberate
     *     terminal-state policy. */
    for (int i = 0; i < s_n_events; i++) {
        WaterEvent *e = &s_events[i];
        int wx = (int)e->lx + ox, wz = (int)e->lz + oz;
        if (e->byte == BLK_AIR) {
            craft_world_set(wx, (int)e->wy, wz, BLK_AIR);
        } else {
            craft_world_set_byte(wx, (int)e->wy, wz, e->byte);
        }
    }
}

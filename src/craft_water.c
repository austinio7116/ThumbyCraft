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

#include <stdint.h>   /* INT32_MAX for the cliff-effect cost sentinel */

#define WATER_TICK_PERIOD 0.20f       /* 5 Hz sim rate */
#define WATER_EVENT_MAX   256

#define WATER_LEVEL_MAX   3           /* L=0 source, L=1..3 flowing, decays
                                       * to evaporate beyond MAX. A small
                                       * cap keeps spread reach short so a
                                       * single placed bucket forms at most
                                       * a 5×5 puddle rather than a 13×13
                                       * sheet — vanilla MC uses 7 but this
                                       * game's world window is only 64×64
                                       * so 7-reach effectively means "the
                                       * whole world floods". */

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

/* Per-direction classification used by the spread step.
 *
 * Rule: water spreads to FLAT directions only when on actual flat
 * ground (no downhill within reach). When any direction has a
 * downhill path within MAX_DROP_DEPTH cells (counting straight or
 * one-turn paths), water spreads ONLY to those directions and the
 * flat directions are ignored.
 *
 *   DOWNHILL : a "drop" — a cell over air — exists within depth N
 *              by orthogonal moves from this direction's start
 *              neighbour. Catches gentle slopes (drop at depth 2-4
 *              even though the immediate neighbour is solid below).
 *   FLAT     : neighbour passable, no drop reachable in depth N.
 *   BLOCKED  : neighbour itself is solid (or off-window).
 */
typedef enum {
    DIR_BLOCKED  = 0,
    DIR_FLAT     = 1,
    DIR_DOWNHILL = 2,
} DirKind;

/* Per-direction downhill weight, derived from the 3×3 grid of
 * cells one Y below the source. Each of the 4 orthogonal
 * directions "owns" the 3-cell column on its side of that grid:
 *
 *           -Z column      +Z column
 *          +-----+-----+-----+
 *   -X col | NW  |  N  |  NE | +X col
 *          +-----+-----+-----+
 *          |  W  |  *  |  E  |    <- * is the cell directly below
 *          +-----+-----+-----+       (handled by FALL, skipped)
 *          | SW  |  S  |  SE |
 *          +-----+-----+-----+
 *
 *   +X (East) owns NE, E, SE.
 *   -X (West) owns NW, W, SW.
 *   +Z (South) owns SW, S, SE.
 *   -Z (North) owns NW, N, NE.
 *
 * Counting air cells per column gives a "width" of drop in that
 * direction. The widest direction(s) win — only they get spread.
 * On a slope dropping cleanly +X, the entire +X column is air
 * (3 hits) while -X is solid (0) and +Z/-Z each see 1 corner.
 * Spread goes only +X. On flat ground, all columns count 0 and
 * the caller falls back to a flat puddle. */
static void compute_drop_widths(int lx, int wy, int lz, int widths[4]) {
    widths[0] = widths[1] = widths[2] = widths[3] = 0;
    if (wy <= 0) return;
    for (int dz = -1; dz <= 1; dz++) {
        for (int dx = -1; dx <= 1; dx++) {
            if (dx == 0 && dz == 0) continue;
            int nlx = lx + dx;
            int nlz = lz + dz;
            if ((unsigned)nlx >= CRAFT_WORLD_X) continue;
            if ((unsigned)nlz >= CRAFT_WORLD_Z) continue;
            int nidx = ((wy - 1) * CRAFT_WORLD_Z + nlz) * CRAFT_WORLD_X + nlx;
            uint8_t cell = craft_world_blocks[nidx];
            /* A cell counts as "drop" if it's air or already flowing
             * water — flowing water below means there's an existing
             * cascade column the source can keep feeding. Treating
             * it as solid would let the bias flip to sideways the
             * moment the slope started filling. L=0 static water is
             * NOT counted: it's an established pool, not a drop. */
            if (cell != BLK_AIR && !(is_water(cell) && cell != BLK_WATER_L0)) continue;
            if (dx > 0) widths[0]++;
            if (dx < 0) widths[1]++;
            if (dz > 0) widths[2]++;
            if (dz < 0) widths[3]++;
        }
    }
}

static DirKind classify_passability(int lx, int wy, int lz, int dx, int dz) {
    int nlx = lx + dx;
    int nlz = lz + dz;
    if ((unsigned)nlx >= CRAFT_WORLD_X) return DIR_BLOCKED;
    if ((unsigned)nlz >= CRAFT_WORLD_Z) return DIR_BLOCKED;
    int nidx = (wy * CRAFT_WORLD_Z + nlz) * CRAFT_WORLD_X + nlx;
    uint8_t nb = craft_world_blocks[nidx];
    if (nb != BLK_AIR && !is_water(nb)) return DIR_BLOCKED;
    return DIR_FLAT;
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
                /* SKIP NATURAL WATER. L=0 cells are static — procgen
                 * lakes/oceans, or pool cells activated and then
                 * resettled. They do nothing per tick. They wake up
                 * (convert to L=1) only when craft_world_set sees a
                 * solid neighbour become air, i.e. when a player
                 * digs a wall away from them. Everything in this
                 * tick body therefore applies only to L=1..L=7
                 * flowing water. */
                if (b == BLK_WATER_L0) continue;

                uint8_t lvl = water_level_of(b);

                /* --- Fall down --- *
                 * A cell over air drops a flowing L=1 into the cell
                 * below. NOT L=0 — landing as L=0 would create a
                 * permanent source and the fall would flood the
                 * world. The current cell stays so the column
                 * visually streams. */
                int below_idx = idx - CRAFT_WORLD_Z * CRAFT_WORLD_X;
                uint8_t below = craft_world_blocks[below_idx];
                if (below == BLK_AIR) {
                    queue_event(lx, wy - 1, lz, water_byte(1));
                    continue;
                }

                /* --- Spread sideways --- *
                 * Neighbours get level L+1 (vanilla-style decay).
                 * At L=7 spread is skipped: spread_level=8 > MAX.
                 */
                uint8_t spread_level = (uint8_t)(lvl + 1);
                if (spread_level <= WATER_LEVEL_MAX) {
                    static const int dx[4] = { 1, -1, 0,  0 };
                    static const int dz[4] = { 0,  0, 1, -1 };
                    /* Score each direction by the width of its
                     * below-grid drop column, then pick the widest.
                     * Ties allowed — water tumbles off corners. */
                    int widths[4];
                    compute_drop_widths(lx, wy, lz, widths);
                    int max_width = 0;
                    for (int d = 0; d < 4; d++) {
                        if (widths[d] > max_width) max_width = widths[d];
                    }
                    bool any_downhill = (max_width > 0);
                    DirKind passable[4];
                    for (int d = 0; d < 4; d++) {
                        passable[d] = classify_passability(lx, wy, lz, dx[d], dz[d]);
                    }
                    for (int d = 0; d < 4; d++) {
                        if (passable[d] == DIR_BLOCKED) continue;
                        if (any_downhill && widths[d] != max_width) continue;
                        int nlx = lx + dx[d];
                        int nlz = lz + dz[d];
                        int nidx = (wy * CRAFT_WORLD_Z + nlz) * CRAFT_WORLD_X + nlx;
                        uint8_t nb = craft_world_blocks[nidx];
                        if (nb == BLK_AIR) {
                            queue_event(nlx, wy, nlz,
                                        water_byte(spread_level));
                        } else if (is_water(nb) &&
                                   water_level_of(nb) > spread_level) {
                            queue_event(nlx, wy, nlz,
                                        water_byte(spread_level));
                        }
                    }
                }

                /* --- Decay --- *
                 * Flowing water must have a neighbour at a strictly
                 * lower level OR a water cell above (an active fall
                 * column). Otherwise it's orphaned and decays one
                 * level per tick. At L=MAX it evaporates outright —
                 * no persistence in this build; only L=0 static
                 * cells (procgen, or activated by a wall break)
                 * carry permanence, and they don't tick at all.
                 *
                 * Note: only L>=1 neighbours can feed us. L=0 is
                 * the static-source marker and intentionally does
                 * NOT feed flowing water — a single placed bucket
                 * next to a lake would otherwise sustain an infinite
                 * outflow. The "wall break" path converts adjacent
                 * L=0 to L=1 so a true drain still happens.
                 */
                bool fed = false;
                /* Above-water-feeds is intentionally removed — sustained
                 * fall streams used to keep the cell at the bottom
                 * permanently at L=1, which then propagated a stable
                 * radius-MAX puddle that never decayed. Without it the
                 * bottom cell decays normally; new falls each tick still
                 * paint a fresh L=1 over it so the visible stream looks
                 * continuous, but the underlying cell's level marches up
                 * and the puddle around it shrinks. */
                {
                    static const int dx2[4] = { 1, -1, 0,  0 };
                    static const int dz2[4] = { 0,  0, 1, -1 };
                    for (int d = 0; d < 4 && !fed; d++) {
                        int nlx = lx + dx2[d];
                        int nlz = lz + dz2[d];
                        if ((unsigned)nlx >= CRAFT_WORLD_X) continue;
                        if ((unsigned)nlz >= CRAFT_WORLD_Z) continue;
                        int nidx = (wy * CRAFT_WORLD_Z + nlz) * CRAFT_WORLD_X + nlx;
                        uint8_t nb = craft_world_blocks[nidx];
                        if (!is_water(nb) || nb == BLK_WATER_L0) continue;
                        if (water_level_of(nb) < lvl) { fed = true; break; }
                    }
                }
                if (!fed) {
                    if (lvl >= WATER_LEVEL_MAX) {
                        /* Settled-pool detection. A cell that hit
                         * MAX with no feed is about to evaporate
                         * UNLESS it's truly contained — every
                         * horizontal neighbour is non-air (solid or
                         * water). Below is already known non-air
                         * (else FALL would have fired). A contained
                         * cell at MAX is a "bowl" cell — promote it
                         * to BLK_WATER_L0 static water AND persist
                         * to the chunk store so the pool survives
                         * window reloads. Once L=0 it stops ticking
                         * entirely; only a wall-break adjacent to
                         * it will wake it back up. */
                        static const int dxp[4] = { 1, -1, 0,  0 };
                        static const int dzp[4] = { 0,  0, 1, -1 };
                        bool contained = true;
                        bool has_solid_wall = false;
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
                            /* A wall is a non-air NON-WATER neighbour:
                             * solid block or already-settled L=0. We
                             * require at least one to persist, so a
                             * water-only-surrounded "inner cell" of a
                             * flat sheet on open ground doesn't lock
                             * in as L=0 (which would leave permanent
                             * puddles after the sheet was supposed to
                             * evaporate). Bowls naturally satisfy this
                             * because their rim cells touch the dug
                             * walls, and the rim cells then become L=0
                             * walls themselves for the next ring in. */
                            if (!is_water(nb2)) has_solid_wall = true;
                            else if (nb2 == BLK_WATER_L0) has_solid_wall = true;
                        }
                        if (contained && has_solid_wall) {
                            queue_event(lx, wy, lz, (uint8_t)BLK_WATER_L0);
                            craft_world_persist_byte(
                                lx + ox, wy, lz + oz, (uint8_t)BLK_WATER_L0);
                        } else {
                            queue_event(lx, wy, lz, (uint8_t)BLK_AIR);
                        }
                    } else {
                        queue_event(lx, wy, lz, water_byte(lvl + 1));
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

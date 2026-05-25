/*
 * ThumbyCraft — lava flow simulation (impl).
 *
 * A near-copy of the water automaton (craft_water.c), with three
 * differences:
 *   1. It ticks far more slowly (LAVA_TICK_PERIOD), so lava oozes.
 *   2. Source cells are BLK_LAVA (static, woken by a wall break) rather
 *      than a y-threshold; flowing cells are BLK_LAVA_L1..L3.
 *   3. A lava cell adjacent to water petrifies to obsidian instead of
 *      flowing — the symmetric half of the rule the water sim already
 *      applies when flowing water touches lava.
 *
 * Lava is opaque AND a light emitter, so each craft_world_set during a
 * tick would normally force a lightmap rebuild. The event-apply loop is
 * wrapped in craft_world_begin/end_batch so a whole tick's worth of
 * flow costs at most one lightmap + one torch rebuild.
 */
#include "craft_lava.h"
#include "craft_world.h"
#include "craft_gen.h"
#include "craft_blocks.h"
#include "craft_types.h"

#include <stdint.h>

#define LAVA_TICK_PERIOD 0.90f     /* ~1.1 Hz — ~4.5× slower than water */
#define LAVA_EVENT_MAX   192
#define LAVA_LEVEL_MAX   3         /* same reach as water (source + L1..L3) */

typedef struct {
    uint8_t  lx;
    uint8_t  lz;
    uint8_t  wy;
    uint8_t  byte;
} LavaEvent;

static LavaEvent s_events[LAVA_EVENT_MAX];
static int       s_n_events;
static float     s_accum;

void craft_lava_init(void) {
    s_accum    = 0.0f;
    s_n_events = 0;
}

static inline uint8_t lava_level_of(uint8_t b) { return craft_lava_level(b); }
static inline uint8_t lava_byte(int level)     { return (uint8_t)craft_lava_for_level(level); }
static inline bool    is_lava(uint8_t b)       { return craft_is_lava_id(b); }

static void queue_event(int lx, int wy, int lz, uint8_t byte) {
    if (s_n_events >= LAVA_EVENT_MAX) return;
    LavaEvent *e = &s_events[s_n_events++];
    e->lx = (uint8_t)lx; e->wy = (uint8_t)wy; e->lz = (uint8_t)lz; e->byte = byte;
}

/* Downhill bias — identical to water's: count air / flowing-lava cells
 * in the 3×3 grid one Y below, attributed to the four orthogonal
 * directions. The widest column(s) win; flat ground gives a puddle. */
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
            /* Air or already-flowing lava counts as drop; a static
             * BLK_LAVA source below is an established pool, not a drop. */
            if (cell != BLK_AIR && !(is_lava(cell) && cell != BLK_LAVA)) continue;
            if (dx > 0) widths[0]++;
            if (dx < 0) widths[1]++;
            if (dz > 0) widths[2]++;
            if (dz < 0) widths[3]++;
        }
    }
}

/* True if any of the 6 neighbours is water — lava in contact with water
 * petrifies. */
static bool touches_water(int lx, int wy, int lz) {
    static const int dx[6] = { 1, -1, 0, 0, 0, 0 };
    static const int dy[6] = { 0, 0, 1, -1, 0, 0 };
    static const int dz[6] = { 0, 0, 0, 0, 1, -1 };
    for (int d = 0; d < 6; d++) {
        int nlx = lx + dx[d], nwy = wy + dy[d], nlz = lz + dz[d];
        if ((unsigned)nlx >= CRAFT_WORLD_X) continue;
        if ((unsigned)nlz >= CRAFT_WORLD_Z) continue;
        if ((unsigned)nwy >= CRAFT_WORLD_Y) continue;
        int nidx = (nwy * CRAFT_WORLD_Z + nlz) * CRAFT_WORLD_X + nlx;
        if (craft_is_water_id(craft_world_blocks[nidx])) return true;
    }
    return false;
}

void craft_lava_tick(float dt) {
    s_accum += dt;
    if (s_accum < LAVA_TICK_PERIOD) return;
    s_accum -= LAVA_TICK_PERIOD;

    int ox = craft_world_origin_x;
    int oz = craft_world_origin_z;
    s_n_events = 0;

    for (int wy = 1; wy < CRAFT_WORLD_Y - 1; wy++) {
        for (int lz = 0; lz < CRAFT_WORLD_Z; lz++) {
            for (int lx = 0; lx < CRAFT_WORLD_X; lx++) {
                int idx = (wy * CRAFT_WORLD_Z + lz) * CRAFT_WORLD_X + lx;
                uint8_t b = craft_world_blocks[idx];
                if (!is_lava(b)) continue;
                /* Static source — does nothing until a wall break wakes
                 * it (craft_world_set converts BLK_LAVA → BLK_LAVA_L1). */
                if (b == BLK_LAVA) continue;

                /* Petrify on water contact — the flowing cell hardens to
                 * obsidian and stops here. (The water sim hardens the
                 * other side independently; both are idempotent.) */
                if (touches_water(lx, wy, lz)) {
                    queue_event(lx, wy, lz, (uint8_t)BLK_OBSIDIAN);
                    continue;
                }

                uint8_t lvl = lava_level_of(b);

                /* --- Fall down --- */
                int below_idx = idx - CRAFT_WORLD_Z * CRAFT_WORLD_X;
                uint8_t below = craft_world_blocks[below_idx];
                if (below == BLK_AIR) {
                    queue_event(lx, wy - 1, lz, lava_byte(1));
                    continue;
                }

                /* --- Spread sideways (downhill-biased) --- */
                uint8_t spread_level = (uint8_t)(lvl + 1);
                if (spread_level <= LAVA_LEVEL_MAX) {
                    static const int dx[4] = { 1, -1, 0,  0 };
                    static const int dz[4] = { 0,  0, 1, -1 };
                    int widths[4];
                    compute_drop_widths(lx, wy, lz, widths);
                    int max_width = 0;
                    for (int d = 0; d < 4; d++)
                        if (widths[d] > max_width) max_width = widths[d];
                    bool any_downhill = (max_width > 0);
                    for (int d = 0; d < 4; d++) {
                        if (any_downhill && widths[d] != max_width) continue;
                        int nlx = lx + dx[d];
                        int nlz = lz + dz[d];
                        if ((unsigned)nlx >= CRAFT_WORLD_X) continue;
                        if ((unsigned)nlz >= CRAFT_WORLD_Z) continue;
                        int nidx = (wy * CRAFT_WORLD_Z + nlz) * CRAFT_WORLD_X + nlx;
                        uint8_t nb = craft_world_blocks[nidx];
                        if (nb == BLK_AIR) {
                            queue_event(nlx, wy, nlz, lava_byte(spread_level));
                        } else if (is_lava(nb) && nb != BLK_LAVA &&
                                   lava_level_of(nb) > spread_level) {
                            queue_event(nlx, wy, nlz, lava_byte(spread_level));
                        }
                    }
                }

                /* --- Decay --- *
                 * A flowing cell with no lower-level lava neighbour is
                 * orphaned and decays a level per tick; at MAX it
                 * evaporates to air (no lava persistence — only the
                 * BLK_LAVA source carries permanence, and it doesn't
                 * tick). Lava never settles into a static pool the way
                 * water does: a flow that reaches a contained bowl just
                 * fills it from the source side each tick, and a severed
                 * flow drains away. */
                bool fed = false;
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
                        if (!is_lava(nb)) continue;
                        /* A static source neighbour feeds us (level 0),
                         * and so does any strictly-lower flowing level. */
                        if (nb == BLK_LAVA) { fed = true; break; }
                        if (lava_level_of(nb) < lvl) { fed = true; break; }
                    }
                }
                if (!fed) {
                    if (lvl >= LAVA_LEVEL_MAX)
                        queue_event(lx, wy, lz, (uint8_t)BLK_AIR);
                    else
                        queue_event(lx, wy, lz, lava_byte(lvl + 1));
                }
            }
        }
    }

    /* Apply queued events. Lava is opaque AND a light emitter, so the
     * lightmap must follow the flow — but routing every cell through
     * craft_world_set (which rebuilds the lightmap each call, and would
     * also persist transient flow into the chunk store) is wrong on both
     * counts. Instead: write transient levels/air directly via set_byte
     * (SRAM only, like water), persist only obsidian (a permanent block),
     * and trigger ONE lightmap rebuild at the end if anything moved. */
    bool changed = (s_n_events > 0);
    for (int i = 0; i < s_n_events; i++) {
        LavaEvent *e = &s_events[i];
        int wx = (int)e->lx + ox, wz = (int)e->lz + oz;
        craft_world_set_byte(wx, (int)e->wy, wz, e->byte);
        if (e->byte == (uint8_t)BLK_OBSIDIAN) {
            /* Permanent — record in the chunk store so the petrified
             * rock survives window reloads and saves. */
            craft_world_persist_byte(wx, (int)e->wy, wz, (uint8_t)BLK_OBSIDIAN);
        }
    }
    if (changed) craft_world_rebuild_lightmap();
}

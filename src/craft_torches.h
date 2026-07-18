/*
 * ThumbyCraft — 3D torch rendering + orientation tracking.
 *
 * Torches occupy a world cell (for lighting + collision) but don't
 * render as a full cube. The raycaster treats BLK_TORCH cells as
 * transparent (rays pass through). After the world render, this
 * module draws each torch in the window as 2 small cuboids — a
 * thin stick + a flame block — offset to match how the torch was
 * mounted (top of the block below for floor torches, sticking out
 * of the wall for wall torches).
 *
 * Orientation is captured at placement time and stored in a small
 * sparse hash keyed on world coords (same trick as the mod table).
 * Rebuilt-on-window-shift, mirroring craft_world_lightmap.
 */
#ifndef CRAFT_TORCHES_H
#define CRAFT_TORCHES_H

#include <stddef.h>
#include "craft_types.h"
#include "craft_render.h"

/* Max sprite-cuboid cells drawn per window. Only the genuinely-3D
 * sprites still use this post-pass list: torches, levers, pistons (+
 * rare player-placed lily pads). Wires, ladders, doors, trapdoors and
 * vines moved to the DDA cutout path, so the old worst case (a redstone
 * build carpeted in wire) is gone and this can be much smaller — which
 * frees the SRAM the raycaster needs for CRAFT_HOT placement. Functional
 * sprites are scanned first, so an overflow only drops decorative lily
 * pads. Transient list, rebuilt each shift, ~20 bytes each. */
#define CRAFT_MAX_TORCHES 128

typedef enum {
    TORCH_KIND_TORCH = 0,
    TORCH_KIND_WIRE  = 1,    /* unpowered redstone dust on the floor */
    TORCH_KIND_WIRE_ON = 2,  /* powered redstone dust — bright */
    TORCH_KIND_LADDER  = 3,  /* vertical rail sprite */
    TORCH_KIND_PRESSURE_PAD = 4, /* horizontal pad on floor */
    TORCH_KIND_DOOR_CLOSED = 5,
    TORCH_KIND_DOOR_OPEN   = 6,
    TORCH_KIND_TRAPDOOR_CLOSED = 7,
    TORCH_KIND_TRAPDOOR_OPEN   = 8,
    /* Pistons are split into three render kinds so the base, shaft,
     * and head can be modelled separately and combined per cell
     * depending on extend/retract state. */
    TORCH_KIND_PISTON_OFF  = 9,   /* base + short shaft + head, retracted */
    TORCH_KIND_PISTON_ON   = 10,  /* base + shaft only (head moved to arm cell) */
    TORCH_KIND_PISTON_ARM  = 11,  /* shaft continuation + head, extended */
    TORCH_KIND_LEVER_OFF   = 12,  /* base plate + tilted handle */
    TORCH_KIND_LEVER_ON    = 13,
    TORCH_KIND_VINE        = 14,  /* green hanging strands (ladder-like) */
    TORCH_KIND_LILY_PAD    = 15,  /* flat green disc on the water surface */
} TorchKind;

typedef struct {
    bool    alive;
    int32_t wx, wz;
    int16_t wy;
    uint8_t orient;     /* Face enum — which face of the parent block
                           the torch is mounted on (torches only). */
    uint8_t kind;       /* TorchKind — torches share this render
                           pipeline with redstone wires because wires
                           need the same "non-opaque world cell + small
                           sprite overlay" treatment. */
    uint8_t connect;    /* 4-bit neighbour mask for wires only —
                           bit 0 = +X, 1 = -X, 2 = +Z, 3 = -Z. The
                           wire only renders arms that point at a
                           connected neighbour, like vanilla MC. */
} CraftTorch;

extern CraftTorch craft_torches[CRAFT_MAX_TORCHES];

/* Record orientation for a torch being placed at (wx, wy, wz). The
 * orient value is the Face enum from craft_render_pick (which face
 * of the parent block the player aimed at). Call this from the
 * player place path right after craft_world_set. */
void craft_torches_record_orient(int wx, int wy, int wz, int face);

/* Look up a previously-recorded orient (Face enum). Returns FACE_PY
 * if no orient is on file for the cell. Public so the redstone tick
 * can determine piston facing for extend/retract. */
int  craft_torches_lookup_orient(int wx, int wy, int wz);

/* Redstone wire connection mask at a world cell (bits 0=+X 1=-X 2=+Z
 * 3=-Z). Used by the DDA wire renderer to cut the dust shape. */
uint8_t craft_torches_wire_connect_at(int wx, int wy, int wz);

/* Co-op: iterate occupied orient-hash entries (cursor 0 to start; feed
 * the return value back in; -1 = done) + total count. Lets craft_net
 * stream orientations entry-by-entry instead of buffering the whole
 * serialised blob. */
int craft_torches_orient_iter(int cursor, int32_t *wx, int *wy, int32_t *wz,
                              uint8_t *orient);
int craft_torches_orient_count(void);

/* Forget orientation for a torch being removed. */
void craft_torches_forget_orient(int wx, int wy, int wz);

/* --- Save persistence -------------------------------------------- *
 * Walk the orient hash and emit a length-prefixed list of occupied
 * cells: u16 count, followed by N × (i32 wx + i32 wz + u8 wy + u8
 * orient) = 10 bytes per entry. Worst-case 2 + 256*10 = 2562 B.
 * Returns bytes written. */
#define CRAFT_ORIENTS_BLOB_PER_ENTRY 10
#define CRAFT_ORIENTS_BLOB_MAX_BYTES (2 + 256 * CRAFT_ORIENTS_BLOB_PER_ENTRY)
size_t craft_torches_orient_serialise(uint8_t *out, size_t out_cap);

/* Reset + restore the orient hash from a serialised blob. Truncates
 * silently if the count exceeds 256 (defensive — should never happen
 * with our own writer). Returns true on a clean parse. */
bool   craft_torches_orient_deserialise(const uint8_t *in, size_t in_n);

/* Refresh the resident torch list from BLK_TORCH cells in the
 * current world window. Pulls the cached orientation per torch
 * from the hash; defaults to floor (FACE_PY) if not present.
 * Called automatically by craft_world after shifts/load. */
void craft_torches_rebuild(void);

/* Incremental update for streaming shifts: sprite entries use world
 * coords, so the overlap's survive a slide. Drop scrolled-out entries,
 * add a freshly-exposed strip, refresh wire-connections on the seam. */
void craft_torches_drop_outside(void);
void craft_torches_drop_region(int lx0, int lx1, int lz0, int lz1);
void craft_torches_add_region(int lx0, int lx1, int lz0, int lz1);
void craft_torches_refresh_connect(int lx0, int lx1, int lz0, int lz1);

/* Pick ray — returns the index of the closest torch within max_dist
 * along the camera ray, or -1. Used by the player attack path so A
 * can break torches (they're skipped by the world raycaster). */
int  craft_torches_pick(const CraftCamera *cam, float max_dist);

/* Render all live torches as 3D cuboids, z-tested against
 * craft_zbuf so they appear correctly behind world blocks. Call
 * after the world strip render, before HUD. */
void craft_torches_render(const CraftCamera *cam, uint16_t *fb);

/* Highlight a single sprite cell — call before craft_torches_render
 * each frame with the picker's hit cell so the cuboid for that cell
 * paints in a brighter "selected" tint. Pass `enabled = false` to
 * disable highlighting for the next frame. */
void craft_torches_set_highlight(int wx, int wy, int wz, bool enabled);

/* Build the cuboid model for a sprite-based block (ladder, door,
 * trapdoor, wire, pad) for use by the held-item viewport. Writes
 * up to `max_n` parts to `out`, in CELL-LOCAL coords (0..1 — the
 * caller is expected to re-centre + scale). Returns part count, or
 * 0 if `b` isn't a sprite block. */
#include "craft_tool_models.h"
int craft_torches_block_model(uint8_t b, int orient,
                              CraftToolPart *out, int max_n);

#endif

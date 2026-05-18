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

#include "craft_types.h"
#include "craft_render.h"

#define CRAFT_MAX_TORCHES 64

typedef enum {
    TORCH_KIND_TORCH = 0,
    TORCH_KIND_WIRE  = 1,    /* unpowered redstone dust on the floor */
    TORCH_KIND_WIRE_ON = 2,  /* powered redstone dust — bright */
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
} CraftTorch;

extern CraftTorch craft_torches[CRAFT_MAX_TORCHES];

/* Record orientation for a torch being placed at (wx, wy, wz). The
 * orient value is the Face enum from craft_render_pick (which face
 * of the parent block the player aimed at). Call this from the
 * player place path right after craft_world_set. */
void craft_torches_record_orient(int wx, int wy, int wz, int face);

/* Forget orientation for a torch being removed. */
void craft_torches_forget_orient(int wx, int wy, int wz);

/* Refresh the resident torch list from BLK_TORCH cells in the
 * current world window. Pulls the cached orientation per torch
 * from the hash; defaults to floor (FACE_PY) if not present.
 * Called automatically by craft_world after shifts/load. */
void craft_torches_rebuild(void);

/* Pick ray — returns the index of the closest torch within max_dist
 * along the camera ray, or -1. Used by the player attack path so A
 * can break torches (they're skipped by the world raycaster). */
int  craft_torches_pick(const CraftCamera *cam, float max_dist);

/* Render all live torches as 3D cuboids, z-tested against
 * craft_zbuf so they appear correctly behind world blocks. Call
 * after the world strip render, before HUD. */
void craft_torches_render(const CraftCamera *cam, uint16_t *fb);

#endif

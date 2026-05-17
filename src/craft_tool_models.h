/*
 * ThumbyCraft — held-item cuboid models (pickaxes, swords, bow, arrow).
 *
 * Multi-cuboid models in the same axis-aligned-parts style as
 * craft_mobs.c's s_models[], but ONLY used by the held-item viewport
 * renderer in craft_render.c. They never enter the world frame; the
 * model is rendered into a fixed bottom-right viewport from a virtual
 * near-camera so the player sees the item they're holding.
 *
 * Model conventions:
 *   - Local origin is at the centre of the held item.
 *   - +Y is up, +X is right, +Z is into the screen.
 *   - Front of the model faces -Z (toward the viewer).
 *   - Sizes are in metres; total bounding extent ~0.5 m so the model
 *     comfortably fits in the viewport's virtual frustum.
 */
#ifndef CRAFT_TOOL_MODELS_H
#define CRAFT_TOOL_MODELS_H

#include "craft_types.h"
#include "craft_blocks.h"

/* Local mirror of craft_mobs.c's CuboidPart. Kept private here so we
 * don't drag mob-renderer internals into a public header — same memory
 * layout though, since it's the natural minimum to describe an
 * axis-aligned coloured box. */
typedef struct {
    float    cx, cy, cz;       /* part centre */
    float    hx, hy, hz;       /* half-sizes */
    uint16_t color;
} CraftToolPart;

/* The model data itself lives in flash as const tables — `parts` is
 * a pointer rather than an inline array so we don't burn precious
 * SRAM holding what's effectively read-only level data. */
typedef struct {
    int                   n_parts;
    const CraftToolPart  *parts;
} CraftToolModel;

/* No-op today (model data lives in flash) but kept in the public
 * API so future model variants that need procedural setup (e.g. tier
 * recolour, animation) have a hook ready. Call from craft_main_init
 * after craft_mobs_build_sprites. */
void craft_tool_models_init(void);

/* Return the model for the given item id. For unknown ids or items
 * that have no held-view model (BLK_AIR, blocks, raw materials) the
 * returned model has n_parts == 0 so the caller can early-out. */
CraftToolModel craft_tool_model(BlockId b);

#endif

/*
 * ThumbyCraft — block types, properties, and texture atlas.
 *
 * v1 has 8 visible block types fitting in 4 bits. Bit layout in the
 * world array (one byte per block) currently uses bits 0..3 for the
 * type id; bits 4..7 are reserved for per-block flags (lighting,
 * variant, water level) that come in later phases.
 */
#ifndef CRAFT_BLOCKS_H
#define CRAFT_BLOCKS_H

#include "craft_types.h"

typedef enum {
    BLK_AIR    = 0,
    BLK_STONE  = 1,
    BLK_DIRT   = 2,
    BLK_GRASS  = 3,
    BLK_SAND   = 4,
    BLK_WOOD   = 5,
    BLK_LEAVES = 6,
    BLK_WATER  = 7,
    BLK_COBBLE = 8,
    BLK_PLANK  = 9,
    BLK_GLASS  = 10,
    BLK_COUNT
} BlockId;

typedef enum {
    FACE_PX = 0,  /* +X / east  */
    FACE_NX = 1,  /* -X / west  */
    FACE_PY = 2,  /* +Y / top   */
    FACE_NY = 3,  /* -Y / bottom*/
    FACE_PZ = 4,  /* +Z / south */
    FACE_NZ = 5   /* -Z / north */
} Face;

/* Texture atlas: 16×16 textures, RGB565, packed contiguously. */
#define CRAFT_TEX_SIZE   16
#define CRAFT_TEX_PIXELS (CRAFT_TEX_SIZE * CRAFT_TEX_SIZE)
#define CRAFT_TEX_COUNT  (BLK_COUNT * 3)   /* top / side / bottom slot */

extern uint16_t craft_textures[CRAFT_TEX_COUNT * CRAFT_TEX_PIXELS];

/* Fill the texture atlas with procedurally generated tiles. Call once
 * at startup before rendering. Deterministic — same atlas every run. */
void craft_blocks_build_textures(void);

/* Animate the water texture by shifting the stripe pattern based on
 * world time. Call every frame before render_strip. Cheap — 16x16
 * regenerate writes only the water tile, no allocation. */
void craft_blocks_animate_water(float t);

/* For a given block id and face, return a pointer to the 16×16 RGB565
 * texture data. Hot path — keep this small. */
const uint16_t *craft_block_texture(BlockId blk, Face face);

/* Whether this block is opaque (stops a ray). Water and air are
 * non-opaque; everything else is. */
static inline bool craft_block_opaque(BlockId blk) {
    return blk != BLK_AIR && blk != BLK_WATER && blk != BLK_GLASS;
}

/* Whether this block stops player movement (collidable). */
static inline bool craft_block_solid(BlockId blk) {
    return blk != BLK_AIR && blk != BLK_WATER;
}

/* Human-readable name for the hotbar HUD. */
const char *craft_block_name(BlockId blk);

#endif

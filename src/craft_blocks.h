/*
 * ThumbyCraft — block types, properties, and texture atlas.
 *
 * Bit layout in the world array (one byte per block): bits 0..5 are
 * the block type id (64 distinct types); bits 6..7 are the water
 * level used by the water-flow sim (0 = source, 1..3 = flowing).
 */
#ifndef CRAFT_BLOCKS_H
#define CRAFT_BLOCKS_H

#include "craft_types.h"

typedef enum {
    BLK_AIR           = 0,
    BLK_STONE         = 1,
    BLK_DIRT          = 2,
    BLK_GRASS         = 3,
    BLK_SAND          = 4,
    BLK_WOOD          = 5,
    BLK_LEAVES        = 6,
    BLK_WATER         = 7,
    BLK_COBBLE        = 8,
    BLK_PLANK         = 9,
    BLK_GLASS         = 10,
    BLK_COAL_ORE      = 11,
    BLK_TORCH         = 12,
    BLK_IRON_ORE      = 13,    /* mountain ore — needs stone pickaxe */
    /* Inventory-only items below — never written to a world cell. */
    BLK_STICK         = 14,
    BLK_IRON_INGOT    = 15,
    BLK_PICKAXE_WOOD  = 16,    /* renamed from BLK_PICKAXE */
    BLK_PICKAXE_STONE = 17,
    BLK_PICKAXE_IRON  = 18,
    BLK_SWORD_WOOD    = 19,
    BLK_SWORD_STONE   = 20,
    BLK_SWORD_IRON    = 21,
    BLK_BOW           = 22,
    BLK_ARROW         = 23,
    /* Placeable functional blocks resume here. Items that are
     * never world-cells stay above BLK_STICK; functional placeables
     * (furnace, chest, ...) are real blocks so they live below it
     * conceptually but for compatibility we just put new IDs at
     * the end and let placeable() check by id range explicitly. */
    BLK_FURNACE       = 24,
    BLK_CHEST         = 25,
    /* Precious-metal ores (world-placeable). */
    BLK_SILVER_ORE    = 26,
    BLK_GOLD_ORE      = 27,
    BLK_DIAMOND_ORE   = 28,
    BLK_REDSTONE_ORE  = 29,
    /* Smelt/mine drops (inventory items only). */
    BLK_SILVER_INGOT  = 30,
    BLK_GOLD_INGOT    = 31,
    BLK_DIAMOND       = 32,    /* gem */
    BLK_REDSTONE      = 33,    /* dust */
    /* Higher-tier tools. */
    BLK_PICKAXE_SILVER  = 34,
    BLK_PICKAXE_GOLD    = 35,
    BLK_PICKAXE_DIAMOND = 36,
    BLK_SWORD_SILVER    = 37,
    BLK_SWORD_GOLD      = 38,
    BLK_SWORD_DIAMOND   = 39,
    /* Storage blocks — 9 ingots/gems/dust crafts a solid block. */
    BLK_SILVER_BLOCK    = 40,
    BLK_GOLD_BLOCK      = 41,
    BLK_DIAMOND_BLOCK   = 42,
    BLK_REDSTONE_BLOCK  = 43,
    BLK_COUNT
} BlockId;

/* Backwards-compat alias for older code that referenced PICKAXE
 * without a tier qualifier. */
#define BLK_PICKAXE BLK_PICKAXE_WOOD

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

/* Atlas storage is an implementation detail of craft_blocks.c.
 *
 * Two modes:
 *  - CRAFT_TEXTURES_BAKED — pre-baked const array lives in flash
 *    (~32 KB SRAM saved). Animated water still needs writable scratch
 *    but that's only 1 KB.
 *  - Otherwise — full 33 KB writable BSS, built at boot. Used by the
 *    tools/bake_textures host tool to produce the baked file. */

/* Fill the texture atlas with procedurally generated tiles. Call once
 * at startup before rendering. Deterministic — same atlas every run.
 * No-op when CRAFT_TEXTURES_BAKED is defined (textures already in
 * flash). */
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

/* Whether this block stops player movement (collidable). Items never
 * sit in a world cell so they only matter via the BLK_COUNT > 13
 * inventory side — guard placeable check below. Placeable functional
 * blocks (furnace, future chest) live above BLK_STICK in the enum so
 * they need an explicit allow-list. */
static inline bool craft_block_solid(BlockId blk) {
    if (blk == BLK_AIR || blk == BLK_WATER || blk == BLK_TORCH) return false;
    if (blk == BLK_FURNACE) return true;
    if (blk == BLK_CHEST) return true;
    if (blk >= BLK_SILVER_ORE && blk <= BLK_REDSTONE_ORE)   return true;
    if (blk >= BLK_SILVER_BLOCK && blk <= BLK_REDSTONE_BLOCK) return true;
    if (blk >= BLK_STICK) return false;   /* inventory items */
    return true;
}

/* Whether this block id can be placed in the world by B. Tool / item
 * entries have inventory slots but never become cells. */
static inline bool craft_block_placeable(BlockId blk) {
    if (blk == BLK_AIR) return false;
    if (blk == BLK_FURNACE || blk == BLK_CHEST) return true;
    if (blk >= BLK_SILVER_ORE && blk <= BLK_REDSTONE_ORE)   return true;
    if (blk >= BLK_SILVER_BLOCK && blk <= BLK_REDSTONE_BLOCK) return true;
    if (blk >= BLK_STICK) return false;
    return true;
}

/* Mining tier required. 0 = barehanded, 1 = wood+ pickaxe,
 * 2 = stone+ pickaxe, 3 = iron+ pickaxe. */
static inline int craft_block_pickaxe_tier(BlockId blk) {
    if (blk == BLK_STONE || blk == BLK_COBBLE || blk == BLK_COAL_ORE) return 1;
    if (blk == BLK_IRON_ORE || blk == BLK_SILVER_ORE) return 2;
    if (blk == BLK_GOLD_ORE || blk == BLK_DIAMOND_ORE ||
        blk == BLK_REDSTONE_ORE) return 3;
    if (blk >= BLK_SILVER_BLOCK && blk <= BLK_REDSTONE_BLOCK) return 3;
    return 0;
}
static inline bool craft_block_needs_pickaxe(BlockId blk) {
    return craft_block_pickaxe_tier(blk) > 0;
}

/* Human-readable name for the hotbar HUD. */
const char *craft_block_name(BlockId blk);

#endif

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
    /* Redstone circuit blocks. LEVER_* and WIRE_* are state pairs:
     * lever toggles between OFF/ON via B-interact; wire transitions
     * automatically driven by craft_redstone's propagation tick. */
    BLK_LEVER_OFF       = 44,
    BLK_LEVER_ON        = 45,
    BLK_REDSTONE_WIRE   = 46,
    BLK_REDSTONE_WIRE_ON = 47,
    /* Polish-phase placeables with state. Trapdoor/door/piston are
     * driven by adjacent redstone power; ladders are climbable but
     * not redstone-triggerable; pressure pads emit redstone power
     * when a mob or player stands on them; TNT lights its fuse on
     * redstone power and explodes after 3 s. */
    BLK_LADDER          = 48,
    BLK_TRAPDOOR_OFF    = 49,
    BLK_TRAPDOOR_ON     = 50,
    BLK_DOOR_OFF        = 51,
    BLK_DOOR_ON         = 52,
    BLK_PRESSURE_PAD    = 53,
    BLK_PISTON_OFF      = 54,
    BLK_PISTON_ON       = 55,
    BLK_PISTON_ARM      = 56,
    BLK_TNT             = 57,
    BLK_TNT_FUSED       = 58,
    /* Redstone-related (fills the 6-bit cell ID space — 64 is the
     * cap; widening the cell would double the world buffer). All
     * five are single-ID with state derived live via the redstone
     * tick or per-cell upper-bit storage. */
    BLK_OBSERVER        = 59,   /* emits 1-tick pulse on neighbour change */
    BLK_NOTE_BLOCK      = 60,   /* tone burst on rising edge */
    BLK_LAMP            = 61,   /* lit when any neighbour is redstone-powered */
    BLK_NOT_GATE        = 62,   /* output ON when input face unpowered */
    BLK_DELAY           = 63,   /* 1-tick delay; B-cycle adjusts setting */
    /* --- 8-bit BlockId space (added in v8 format) ------------------ *
     * The cell byte is now a full 8-bit BlockId; the upper-bit pack
     * used by v1..v7 for water level + redstone state is retired.
     * That frees us from the 64-ID cap and gives ~250 IDs total.
     *
     * Water now spans IDs 7 + 64..70 (8 levels matching vanilla),
     * with WATER_L0 the source and L7 the "about to evaporate"
     * tendril. Helpers below treat any cell in this range as water.
     * Redstone state blocks get explicit _ON variants — clean state
     * round-trip through the chunk store with no upper-bit games. */
    BLK_WATER_L1        = 64,
    BLK_WATER_L2        = 65,
    BLK_WATER_L3        = 66,
    BLK_WATER_L4        = 67,
    BLK_WATER_L5        = 68,
    BLK_WATER_L6        = 69,
    BLK_WATER_L7        = 70,
    BLK_OBSERVER_ON     = 71,   /* pulse-active variant of OBSERVER */
    BLK_NOTE_BLOCK_ON   = 72,   /* was-powered variant */
    BLK_LAMP_ON         = 73,   /* lit variant */
    BLK_NOT_GATE_ON     = 74,   /* output-on variant */
    BLK_DELAY_ON        = 75,   /* this-tick output-on variant */
    /* --- Second redstone wave (save v9) ---------------------------- *
     * Dispenser fires an arrow along its orient face on a rising
     * redstone edge; target emits a 1-tick pulse when an arrow hits
     * it; slime block bounces fall damage and crafts from slimeballs.
     * Each redstone-stateful block keeps its _ON variant for clean
     * chunk-store round-tripping. */
    BLK_DISPENSER       = 76,
    BLK_DISPENSER_ON    = 77,   /* was-powered (rising-edge latch) */
    BLK_TARGET          = 78,
    BLK_TARGET_ON       = 79,   /* arrow-struck pulse variant */
    BLK_SLIME_BLOCK     = 80,
    BLK_SLIMEBALL       = 81,   /* inventory item — slime mob drop */
    /* Sticky piston — pulls its block back on retract. The original
     * BLK_PISTON_* is now the plain (non-sticky) piston that just
     * drops the block when it retracts. Sticky shares BLK_PISTON_ARM
     * and is crafted from a plain piston + a slimeball. */
    BLK_STICKY_PISTON_OFF = 82,
    BLK_STICKY_PISTON_ON  = 83,
    /* --- Biome blocks (save v10) ----------------------------------- *
     * Surface/feature blocks for the first-wave biomes: snow caps the
     * taiga, sandstone + cactus flesh out the desert, lily pads dress
     * swamp water, and vines hang from swamp trees / climbable walls. */
    BLK_SNOW            = 84,   /* snowy surface — taiga / tundra */
    BLK_SANDSTONE       = 85,   /* desert sub-surface */
    BLK_CACTUS          = 86,   /* desert plant */
    BLK_VINE            = 87,   /* hanging / climbable sprite */
    BLK_LILY_PAD        = 88,   /* flat pad on swamp water */
    BLK_SNOWY_ROCK      = 89,   /* mountain cap — snow on stone (save v11) */
    BLK_ICE             = 90,   /* frozen tundra water surface (save v12) */
    BLK_LAVA            = 91,   /* cave lava — light source + hazard (save v13) */
    BLK_OBSIDIAN        = 92,   /* water-quenched lava — diamond-pick only (save v14) */
    BLK_GRAVEL          = 93,   /* worldgen patches; mining ~10% drops flint (save v15) */
    BLK_FLINT           = 94,   /* item — lights an obsidian portal frame (save v15) */
    BLK_PORTAL          = 95,   /* lit portal — swirling purple, walk-through (save v15) */
    /* Flowing lava (save v16). BLK_LAVA is the static source (L0);
     * these are the decaying flow levels the lava sim spreads, mirroring
     * water's L1..L7 — but lava flows far more slowly. */
    BLK_LAVA_L1         = 96,
    BLK_LAVA_L2         = 97,
    BLK_LAVA_L3         = 98,
    /* --- Ambient surface decoration (save v17) --------------------- *
     * Worldgen-only cross-sprite plants: non-solid (walk through),
     * non-sky-blocking, rendered via the DDA cutout-CROSS path (two
     * perpendicular leafy/petal quads). Hidden from the inventory and
     * drop nothing when cleared — pure scenery for meadow biomes. */
    BLK_TALL_GRASS      = 99,   /* biome-tinted grass tuft */
    BLK_FLOWER_RED      = 100,  /* red bloom */
    BLK_FLOWER_YELLOW   = 101,  /* yellow bloom */
    BLK_COUNT
} BlockId;

/* Alias: the original BLK_WATER is now the source level (L0). */
#define BLK_WATER_L0 BLK_WATER

/* True if a raw cell byte is any water level. Cheap range check —
 * relies on WATER_L0 == 7 and WATER_L1..L7 packed at 64..70. */
static inline bool craft_is_water_id(uint8_t b) {
    return b == BLK_WATER_L0 || (b >= BLK_WATER_L1 && b <= BLK_WATER_L7);
}

/* Water level 0..7 for a water cell; 0 for non-water. Source = 0. */
static inline uint8_t craft_water_level(uint8_t b) {
    if (b == BLK_WATER_L0) return 0;
    if (b >= BLK_WATER_L1 && b <= BLK_WATER_L7) return (uint8_t)(1 + (b - BLK_WATER_L1));
    return 0;
}

/* BlockId for a given water level. level=0 → WATER_L0, 1..7 → L1..L7. */
static inline BlockId craft_water_for_level(int level) {
    if (level <= 0) return BLK_WATER_L0;
    if (level >= 7) return BLK_WATER_L7;
    return (BlockId)(BLK_WATER_L1 + (level - 1));
}

/* Lava level scheme — mirrors water. BLK_LAVA is the static source
 * (L0, never decays); BLK_LAVA_L1..L3 are flowing levels. The flow sim
 * (craft_lava.c) is identical in shape to water's but ticks far slower. */
static inline bool craft_is_lava_id(uint8_t b) {
    return b == BLK_LAVA || (b >= BLK_LAVA_L1 && b <= BLK_LAVA_L3);
}
static inline uint8_t craft_lava_level(uint8_t b) {
    if (b == BLK_LAVA) return 0;
    if (b >= BLK_LAVA_L1 && b <= BLK_LAVA_L3) return (uint8_t)(1 + (b - BLK_LAVA_L1));
    return 0;
}
static inline BlockId craft_lava_for_level(int level) {
    if (level <= 0) return BLK_LAVA;
    if (level >= 3) return BLK_LAVA_L3;
    return (BlockId)(BLK_LAVA_L1 + (level - 1));
}

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
    if (blk == BLK_REDSTONE_WIRE || blk == BLK_REDSTONE_WIRE_ON) return false;
    /* Sprite-style overlays — raycaster skips, post-pass renders. */
    if (blk == BLK_LADDER || blk == BLK_PRESSURE_PAD) return false;
    /* Doors + trapdoors render as thin slabs via the sprite system
     * in BOTH states so the player can see them open/close. The
     * closed states are still solid (see craft_block_solid). */
    if (blk == BLK_DOOR_OFF || blk == BLK_DOOR_ON) return false;
    if (blk == BLK_TRAPDOOR_OFF || blk == BLK_TRAPDOOR_ON) return false;
    /* Pistons render via sprite system (base + shaft + head) with
     * orient-aware geometry so they look like real MC pistons. The
     * base cells are still solid for collision. */
    if (blk == BLK_PISTON_OFF || blk == BLK_PISTON_ON ||
        blk == BLK_PISTON_ARM ||
        blk == BLK_STICKY_PISTON_OFF || blk == BLK_STICKY_PISTON_ON) return false;
    /* Levers render as a 3D mounted switch via the sprite system,
     * not as a full-cell cube. */
    if (blk == BLK_LEVER_OFF || blk == BLK_LEVER_ON) return false;
    /* Vines + lily pads are thin sprites — non-opaque so the
     * raycaster passes through and the sprite pass draws them. */
    if (blk == BLK_VINE || blk == BLK_LILY_PAD) return false;
    /* Cross-sprite plants — the DDA cutout path traces through their
     * transparent texels, so they must not read as opaque cubes. */
    if (blk == BLK_TALL_GRASS || blk == BLK_FLOWER_RED ||
        blk == BLK_FLOWER_YELLOW) return false;
    return blk != BLK_AIR && !craft_is_water_id((uint8_t)blk) && blk != BLK_GLASS;
}

/* Whether this block stops player movement (collidable). Items never
 * sit in a world cell so they only matter via the BLK_COUNT > 13
 * inventory side — guard placeable check below. Placeable functional
 * blocks (furnace, future chest) live above BLK_STICK in the enum so
 * they need an explicit allow-list. */
static inline bool craft_block_solid(BlockId blk) {
    if (blk == BLK_AIR || craft_is_water_id((uint8_t)blk) || blk == BLK_TORCH) return false;
    if (craft_is_lava_id((uint8_t)blk)) return false;   /* fluid — you sink into it */
    if (blk == BLK_PORTAL) return false; /* walk-through shimmer */
    if (blk == BLK_REDSTONE_WIRE || blk == BLK_REDSTONE_WIRE_ON) return false;
    if (blk == BLK_FURNACE) return true;
    if (blk == BLK_CHEST) return true;
    if (blk == BLK_LEVER_OFF || blk == BLK_LEVER_ON) return true;
    /* Polish blocks. Open variants don't block movement. */
    if (blk == BLK_LADDER || blk == BLK_PRESSURE_PAD) return false;
    if (blk == BLK_DOOR_ON || blk == BLK_TRAPDOOR_ON) return false;
    if (blk == BLK_DOOR_OFF || blk == BLK_TRAPDOOR_OFF) return true;
    if (blk == BLK_PISTON_OFF || blk == BLK_PISTON_ON || blk == BLK_PISTON_ARM ||
        blk == BLK_STICKY_PISTON_OFF || blk == BLK_STICKY_PISTON_ON) return true;
    if (blk == BLK_TNT || blk == BLK_TNT_FUSED) return true;
    /* All five new redstone blocks are solid cubes (no sprite-cell
     * pass-through). Their state visualisation rides through the
     * normal cube renderer with state-aware texture selection. */
    if (blk == BLK_OBSERVER || blk == BLK_NOTE_BLOCK || blk == BLK_LAMP ||
        blk == BLK_NOT_GATE || blk == BLK_DELAY) return true;
    /* Second-wave blocks: dispenser/target/slime are solid cubes
     * (incl. their _ON variants). Slimeball is an item, not solid. */
    if (blk == BLK_DISPENSER || blk == BLK_DISPENSER_ON ||
        blk == BLK_TARGET     || blk == BLK_TARGET_ON    ||
        blk == BLK_SLIME_BLOCK) return true;
    /* Biome blocks: snow/sandstone/cactus are solid cubes. Vine and
     * lily pad are pass-through sprites (handled by the >=STICK
     * default below — they're not solid). */
    if (blk == BLK_SNOW || blk == BLK_SANDSTONE || blk == BLK_CACTUS ||
        blk == BLK_SNOWY_ROCK || blk == BLK_ICE || blk == BLK_OBSIDIAN ||
        blk == BLK_GRAVEL) return true;
    if (blk >= BLK_SILVER_ORE && blk <= BLK_REDSTONE_ORE)   return true;
    if (blk >= BLK_SILVER_BLOCK && blk <= BLK_REDSTONE_BLOCK) return true;
    if (blk >= BLK_STICK) return false;   /* inventory items */
    return true;
}

/* Whether this block id can be placed in the world by B. Tool / item
 * entries have inventory slots but never become cells. */
static inline bool craft_block_placeable(BlockId blk) {
    if (blk == BLK_AIR) return false;
    if (craft_is_lava_id((uint8_t)blk)) return false;   /* world-gen hazard, no bucket yet */
    if (blk == BLK_FURNACE || blk == BLK_CHEST) return true;
    /* BLK_REDSTONE is an inventory item; placing it converts to
     * BLK_REDSTONE_WIRE (handled in the player's B-press path), so
     * accept it as placeable here. BLK_LEVER_OFF is both item and
     * placed block. */
    if (blk == BLK_REDSTONE || blk == BLK_LEVER_OFF) return true;
    if (blk == BLK_LADDER || blk == BLK_PRESSURE_PAD) return true;
    if (blk == BLK_TRAPDOOR_OFF || blk == BLK_DOOR_OFF) return true;
    if (blk == BLK_PISTON_OFF || blk == BLK_STICKY_PISTON_OFF) return true;
    if (blk == BLK_TNT) return true;
    if (blk == BLK_OBSERVER || blk == BLK_NOTE_BLOCK || blk == BLK_LAMP ||
        blk == BLK_NOT_GATE || blk == BLK_DELAY) return true;
    if (blk == BLK_DISPENSER || blk == BLK_TARGET ||
        blk == BLK_SLIME_BLOCK) return true;
    if (blk == BLK_SNOW || blk == BLK_SANDSTONE || blk == BLK_CACTUS ||
        blk == BLK_VINE || blk == BLK_LILY_PAD || blk == BLK_ICE ||
        blk == BLK_OBSIDIAN || blk == BLK_GRAVEL) return true;
    if (blk == BLK_TALL_GRASS || blk == BLK_FLOWER_RED ||
        blk == BLK_FLOWER_YELLOW) return true;
    if (blk >= BLK_SILVER_ORE && blk <= BLK_REDSTONE_ORE)   return true;
    if (blk >= BLK_SILVER_BLOCK && blk <= BLK_REDSTONE_BLOCK) return true;
    if (blk >= BLK_STICK) return false;
    return true;
}

/* Mining tier required. 0 = barehanded, 1 = wood+ pickaxe,
 * 2 = stone+ pickaxe, 3 = iron+ pickaxe, 4 = diamond pickaxe only. */
static inline int craft_block_pickaxe_tier(BlockId blk) {
    if (blk == BLK_STONE || blk == BLK_COBBLE || blk == BLK_COAL_ORE ||
        blk == BLK_SNOWY_ROCK) return 1;
    if (blk == BLK_IRON_ORE || blk == BLK_SILVER_ORE) return 2;
    if (blk == BLK_GOLD_ORE || blk == BLK_DIAMOND_ORE ||
        blk == BLK_REDSTONE_ORE) return 3;
    if (blk == BLK_OBSIDIAN) return 4;   /* diamond pickaxe only */
    if (blk >= BLK_SILVER_BLOCK && blk <= BLK_REDSTONE_BLOCK) return 3;
    return 0;
}
static inline bool craft_block_needs_pickaxe(BlockId blk) {
    return craft_block_pickaxe_tier(blk) > 0;
}

/* Human-readable name for the hotbar HUD. */
const char *craft_block_name(BlockId blk);

#endif

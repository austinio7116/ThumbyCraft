/*
 * ThumbyCraft — furnace state + smelting tick.
 *
 * A furnace is a placeable BLK_FURNACE cell with three associated
 * "slots" of state: input (the raw material being smelted), fuel
 * (the burnable that powers the smelt), and output (the result).
 * State lives in a sparse coord-keyed table; lookups are O(N) over
 * a small N (max 32 simultaneous furnaces in active play).
 *
 * Tick logic per furnace per frame:
 *   - If fuel_remaining_t > 0: burn down by dt.
 *   - Else if input + fuel + room in output + valid smelt: consume
 *     one unit of fuel → fuel_remaining_t = burn_time(fuel).
 *   - If burning AND valid smelt: advance smelt_t by dt.
 *   - When smelt_t crosses SMELT_TIME: input--, output++.
 *
 * Smeltables:
 *   BLK_IRON_ORE → BLK_IRON_INGOT
 *   BLK_SAND     → BLK_GLASS
 *   BLK_COBBLE   → BLK_STONE
 *
 * Fuels (and burn time per unit, seconds):
 *   BLK_COAL_ORE  : 80
 *   BLK_WOOD      : 15
 *   BLK_PLANK     : 15
 *   BLK_STICK     : 5
 */
#ifndef CRAFT_FURNACE_H
#define CRAFT_FURNACE_H

#include "craft_types.h"
#include "craft_blocks.h"

/* 8 active furnace state slots. Furnaces beyond this still exist as
 * world blocks (the chunk store persists the BLK_FURNACE cell) —
 * they just get a fresh empty state when the player interacts with
 * them. 8 is plenty for typical play (one or two active smelts at
 * once). Sized tight against the SRAM ceiling. */
#define CRAFT_MAX_FURNACES 8

typedef struct {
    bool    used;
    int32_t wx, wy, wz;

    /* Slot block ids fit in uint8_t — BlockId values are < 256. Using
     * smaller types here saves ~24 B per entry which keeps the table
     * under ~700 B total. */
    uint8_t input_blk;
    uint8_t input_n;
    uint8_t fuel_blk;
    uint8_t fuel_n;
    uint8_t output_blk;
    uint8_t output_n;

    float   smelt_t;          /* sec progress on current item, 0..SMELT_TIME */
    float   fuel_remaining_t; /* sec left on currently-burning fuel unit */
} CraftFurnace;

extern CraftFurnace craft_furnaces[CRAFT_MAX_FURNACES];

#define CRAFT_FURNACE_SMELT_TIME 10.0f

void craft_furnace_init(void);

/* Get the furnace state for (wx,wy,wz) — creates the entry on first
 * touch. Returns NULL if the table is full. */
CraftFurnace *craft_furnace_at(int wx, int wy, int wz);

/* Look up an existing furnace; no side-effects. */
CraftFurnace *craft_furnace_find(int wx, int wy, int wz);

/* Forget a furnace state record (called when the block is broken). */
void craft_furnace_remove(int wx, int wy, int wz);

/* Advance every furnace's smelt/burn timers. */
void craft_furnace_tick(float dt);

/* Predicates used by the UI page. */
bool    craft_furnace_is_smeltable(BlockId b);
BlockId craft_furnace_smelt_output(BlockId b);
float   craft_furnace_fuel_time(BlockId b);    /* 0 if not a fuel */

/* Fixed-size byte serialisation for the save blob.
 *   per furnace = 1 (used) + 12 (xyz, int32 LE) + 6 (slots) + 8 (timers)
 *   total       = CRAFT_MAX_FURNACES × 27 bytes
 * Floats are written as 32-bit IEEE 754 LE bit patterns. */
#define CRAFT_FURNACES_BLOB_PER_ENTRY 27
#define CRAFT_FURNACES_BLOB_BYTES     (CRAFT_MAX_FURNACES * CRAFT_FURNACES_BLOB_PER_ENTRY)

void craft_furnaces_serialise(uint8_t out[CRAFT_FURNACES_BLOB_BYTES]);
void craft_furnaces_deserialise(const uint8_t in[CRAFT_FURNACES_BLOB_BYTES]);

#endif

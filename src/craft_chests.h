/*
 * ThumbyCraft — chest contents storage.
 *
 * A placed BLK_CHEST cell can be opened by pressing B on it; the
 * player gets a 4×4 = 16-slot inventory grid bound to that chest.
 * Contents are stored in a small sparse coord-keyed table — 4 active
 * chests in SRAM. Chests beyond this still exist as world blocks
 * (the chunk store persists the BLK_CHEST cell) but show empty
 * contents when first opened.
 *
 * Each slot holds (block_id, count) packed into 2 bytes. Maximum
 * stack per slot is 64 — same convention as the furnace input/output.
 *
 * Lost-state policy: when a chest is broken, its state record is
 * freed and contents vanish. Same as vanilla "chest punched without
 * emptying first".
 */
#ifndef CRAFT_CHESTS_H
#define CRAFT_CHESTS_H

#include "craft_types.h"
#include "craft_blocks.h"

#define CRAFT_MAX_CHESTS 4
#define CRAFT_CHEST_SLOTS 16

typedef struct {
    uint8_t blk;     /* BlockId, 0 = empty */
    uint8_t n;       /* 0..64 */
} CraftChestSlot;

typedef struct {
    bool    used;
    int32_t wx, wy, wz;
    CraftChestSlot slots[CRAFT_CHEST_SLOTS];
} CraftChest;

extern CraftChest craft_chests[CRAFT_MAX_CHESTS];

void craft_chests_init(void);

/* Look up an existing chest record; NULL if none. */
CraftChest *craft_chest_find(int wx, int wy, int wz);

/* Get-or-create — claims a free slot on first touch. NULL if the
 * table is full. */
CraftChest *craft_chest_at(int wx, int wy, int wz);

/* Wipe a chest's record (called when the block is broken). */
void craft_chest_remove(int wx, int wy, int wz);

#endif

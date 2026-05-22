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
 * Break policy: when a chest is broken, the player's attack
 * handler walks the slots and transfers contents to the player
 * inventory before calling craft_chest_remove — losing the loot
 * on a stray punch felt worse than vanilla on this device where
 * the chest UI is small and easy to mis-aim.
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

/* Fixed-size byte serialisation for the save blob.
 *   per chest = 1 (used) + 12 (wx,wy,wz, int32 LE) + 32 (16 slots × 2)
 *   total     = CRAFT_MAX_CHESTS × 45 = 180 bytes
 * Both calls write/read exactly CRAFT_CHESTS_BLOB_BYTES bytes. */
#define CRAFT_CHESTS_BLOB_PER_ENTRY 45
#define CRAFT_CHESTS_BLOB_BYTES     (CRAFT_MAX_CHESTS * CRAFT_CHESTS_BLOB_PER_ENTRY)

void craft_chests_serialise(uint8_t out[CRAFT_CHESTS_BLOB_BYTES]);
void craft_chests_deserialise(const uint8_t in[CRAFT_CHESTS_BLOB_BYTES]);

#endif

/*
 * ThumbyCraft — flash-backed per-chunk mod store.
 *
 * Purpose: persist player edits beyond the SRAM sliding window so
 * buildings survive walking far away, save+load cycles, and power
 * cycles.
 *
 * Architecture:
 *   - World is divided into CHUNK_SIZE × CHUNK_SIZE chunks (X/Z).
 *   - Each chunk's mods (cells where the player changed the block
 *     id away from the procedural baseline) are stored in one flash
 *     sector (4 KB), keyed by hash(chunk_x, chunk_z) → slot index.
 *   - Linear probe within a small window of slots on collisions.
 *   - One slot can hold ~340 mods (16×16×64 cell chunk → plenty of
 *     headroom for buildings).
 *
 * Lifecycle hooks from craft_world.c:
 *   - On chunk LEAVING the sliding window: gather its mods from the
 *     SRAM mod hash and craft_chunk_store_save(). Remove from hash.
 *   - On chunk ENTERING the sliding window: craft_chunk_store_load()
 *     and re-insert into the SRAM mod hash so the next regen pass
 *     picks them up.
 *
 * Host build: stub implementation in host/, no persistence.
 */
#ifndef CRAFT_CHUNK_STORE_H
#define CRAFT_CHUNK_STORE_H

#include <stdint.h>
#include <stdbool.h>

#define CHUNK_STORE_CHUNK_SIZE 16
#define CHUNK_STORE_MAX_MODS_PER_CHUNK 340

typedef struct {
    uint8_t lx;      /* 0..CHUNK_STORE_CHUNK_SIZE-1, chunk-local X */
    uint8_t y;       /* 0..63 (CRAFT_WORLD_Y-1) */
    uint8_t lz;      /* 0..CHUNK_STORE_CHUNK_SIZE-1, chunk-local Z */
    uint8_t blk;
} ChunkMod;

/* Initialise the store. world_seed is keyed into each record so a
 * fresh-world boot doesn't accidentally apply mods saved from a
 * previous seed. Idempotent — safe to call repeatedly. */
void craft_chunk_store_init(uint32_t world_seed);

/* Load any persisted mods for chunk (cx, cz) into out[0..max-1].
 * Returns number of mods loaded (0 if no record / corrupt / seed
 * mismatch). max should be at least CHUNK_STORE_MAX_MODS_PER_CHUNK. */
int craft_chunk_store_load(int chunk_x, int chunk_z,
                           ChunkMod *out, int max_entries);

/* Persist `n` mods for chunk (cx, cz) to flash. If n==0 the slot is
 * erased (chunk has no mods → free the sector). Returns true on
 * success. */
bool craft_chunk_store_save(int chunk_x, int chunk_z,
                            const ChunkMod *mods, int n);

/* Wipe all stored chunks. Call when starting a fresh world. */
void craft_chunk_store_clear(void);

#endif

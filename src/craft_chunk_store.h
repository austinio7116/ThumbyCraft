/*
 * ThumbyCraft — flash-backed per-chunk mod store (per-world, nonce-filtered).
 *
 * Each save slot owns a 1 MB chunk region. Plus one scratch region
 * for unsaved new worlds. The store is "bound" to one region + nonce
 * at a time; load/save only see sectors whose embedded nonce matches.
 *
 * The nonce makes "new world" free: instead of physically erasing
 * 1 MB of flash (~3 s), the engine just picks a fresh random nonce.
 * Old sectors remain physically present but get filtered out as
 * stale on every read, and the next save into the same hash position
 * just erase+programs over them.
 *
 * Nonce sources:
 *   - SCRATCH: in-RAM uint32, random at boot, re-randomised on each
 *     new-world action.
 *   - Slot 0..3: the slot's metadata sector seq number. Every save
 *     bumps the seq, so each save commits a fresh nonce automatically.
 *
 * Within each region: 256 sectors hashed by (cx, cz). Linear probe
 * up to 8 slots on collision. A sector counts as "free for probe"
 * if its magic is invalid OR its nonce doesn't match the current
 * binding — both readers and writers treat them the same way.
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
    uint8_t lx;
    uint8_t y;
    uint8_t lz;
    uint8_t blk;
} ChunkMod;

/* Bind the store to (region, nonce). All subsequent load/save calls
 * read/write that region's flash sectors and filter by nonce. Safe
 * to call repeatedly. */
void craft_chunk_store_bind(int region, uint32_t nonce);

/* Currently bound region / nonce. */
int      craft_chunk_store_bound(void);
uint32_t craft_chunk_store_bound_nonce(void);

/* Load mods for chunk (cx, cz) from the bound region — but only
 * sectors whose nonce matches the bound nonce. Returns mod count
 * (0 on miss / stale / corrupt). */
int craft_chunk_store_load(int chunk_x, int chunk_z,
                           ChunkMod *out, int max_entries);

/* Persist `n` mods for chunk (cx, cz) into the bound region, stamped
 * with the bound nonce. n==0 erases the sector. */
bool craft_chunk_store_save(int chunk_x, int chunk_z,
                            const ChunkMod *mods, int n);

/* Bulk-erase a region. Only used for explicit "delete slot" actions
 * (no such UI yet). Per-region nonce flipping is the cheap way to
 * invalidate without touching flash. */
void craft_chunk_store_erase_region(int region);

/* Copy all valid sectors from `src_region` (matching `src_nonce`)
 * into `dst_region`, re-stamped with `dst_nonce`. Used at save time
 * to promote scratch chunks into a slot's region. */
void craft_chunk_store_copy(int src_region, uint32_t src_nonce,
                            int dst_region, uint32_t dst_nonce);

#endif

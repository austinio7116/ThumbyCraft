/*
 * ThumbyCraft — world persistence.
 *
 * Format v2 (all little-endian, single fixed-size record):
 *   magic        u32  'TCFT' = 0x54434654
 *   version      u32  current = 2
 *   seed         u32  identifies the world
 *   mode         u8
 *   hp           u8
 *   hotbar_idx   u8
 *   _pad         u8
 *   hotbar[8]    u8
 *   cam_pos      3 × f32
 *   cam_yaw      f32
 *   cam_pitch    f32
 *   inventory[BLK_COUNT] u32   — counts per item id
 *   crc32        u32  over everything above
 *
 * Why no world deltas?  The chunk store (craft_chunk_store.c) already
 * persists per-chunk player edits to flash on every dirty-chunk
 * drain, so the world survives power cycles without involving the
 * save blob. craft_main_save flushes the dirty queue before this
 * serialise runs so it's always coherent. The save blob is now just
 * the player's transient state + seed for regen.
 *
 * The OLD v1 format tried to record every changed cell in a 4 KB
 * buffer; with the infinite-world refactor the bookkeeping coords
 * stopped matching and every cell looked "modified" → buffer
 * overflow → permanent "Save failed". v2 fixes this by sidestepping
 * the problem: there's no world data in the save at all.
 */
#ifndef CRAFT_SAVE_H
#define CRAFT_SAVE_H

#include <stddef.h>
#include "craft_types.h"
#include "craft_player.h"

#define CRAFT_SAVE_MAGIC   0x54434654u
#define CRAFT_SAVE_VERSION 2u
#define CRAFT_SAVE_MAX_BYTES (4096 - 32)   /* one flash sector minus header */

/* Returns bytes written into `out` (≤ out_cap), or 0 on error.
 * Caller passes the seed used to regenerate the base terrain. */
size_t craft_save_serialise(uint32_t seed,
                            const CraftPlayer *p,
                            uint8_t *out, size_t out_cap);

/* Returns true on success. Re-seeds + regenerates the world, then
 * applies deltas + restores player state. */
bool   craft_save_deserialise(const uint8_t *in, size_t n,
                              uint32_t *out_seed, CraftPlayer *p);

/* --- Save-slot metadata (platform-provided) --------------------- *
 *
 * Slots are flash-backed (device) or file-backed (host). The engine
 * uses these queries to drive the slot picker UI and the title page.
 * Actual read/write goes via the platform's craft_main request flags.
 */
#define CRAFT_SAVE_SLOT_COUNT_PUBLIC 4
#define CRAFT_SAVE_THUMB_DIM         32

/* True if slot has a valid saved world. */
bool craft_save_slot_used(int slot);

/* Pointer to the slot's 32×32 RGB565 thumbnail in storage-mapped
 * memory (XIP flash on device, mmap'd file on host), or NULL when
 * the slot is empty. Pointer is stable until the slot is written. */
const uint16_t *craft_save_slot_thumb(int slot);

#endif

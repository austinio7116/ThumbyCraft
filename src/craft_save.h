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
/* v4 — per-world chunk-store layout with explicit chunks_nonce field
 *      in the record. The nonce is independent of the slot's
 *      sequence number so in-place saves keep the same nonce (no
 *      "load shows stripped world" bug).
 * v5 — chest + furnace SRAM tables are now serialised into the save
 *      blob (was lost on load → hut chests refilled with
 *      deterministic loot every reload). v4 saves no longer load —
 *      the only carriers are recent dev worlds.
 * v6 — torch/lever/piston/door/trapdoor/ladder orient hash table is
 *      now serialised too (was lost on load → every mechanical block
 *      came back with the default FACE_PY mount). v5 saves still load
 *      via dual-read; they just come back without orient data.
 * v7 — five new redstone BlockIds (OBSERVER, NOTE_BLOCK, LAMP,
 *      NOT_GATE, DELAY) bumped BLK_COUNT from 59 to 64, which grows
 *      the inventory section by 5*4 = 20 bytes and shifts every
 *      downstream offset. v6 saves still load via dual-read using
 *      the old inventory size; the inventory counts for the new
 *      block IDs default to zero on a v6 load.
 * v8 — block-id field widened from 6 bits to the full byte. Water
 *      moved from upper-bit-packed level into 8 dedicated IDs
 *      (WATER_L0..L7) and the new redstone blocks gained _ON
 *      variants instead of overloading bit 6. BLK_COUNT jumped to
 *      76; inventory section grows by another (76-64)*4 = 48 bytes.
 *      v6/v7 loads dual-read with the old inventory size; chunk
 *      bytes that were old-format water (id=7 with upper bits set)
 *      get translated to the new WATER_L* ID on chunk restore. */
#define CRAFT_SAVE_VERSION 8u
#define CRAFT_SAVE_VERSION_V5 5u   /* legacy, read-only */
#define CRAFT_SAVE_VERSION_V6 6u   /* legacy, dual-read on load only */
#define CRAFT_SAVE_VERSION_V7 7u   /* legacy, dual-read on load only */
/* Inventory-array length used by each save version. The current
 * BLK_COUNT is the v8 value; older versions were written with
 * fewer block IDs. */
#define CRAFT_SAVE_INVENTORY_LEN_V6 59
#define CRAFT_SAVE_INVENTORY_LEN_V7 64
#define CRAFT_SAVE_MAX_BYTES (4096 - 32)   /* one flash sector minus header */

/* Public field offset for the chunks_nonce inside the serialised
 * record. craft_main_load pre-reads it BEFORE deserialise so the
 * chunk store can be bound with the right nonce ahead of the
 * embedded world_load_around. */
#define CRAFT_SAVE_OFF_CHUNKS_NONCE 12

/* Returns bytes written into `out` (≤ out_cap), or 0 on error.
 * autosave_level is 1..4 — stored in the (previously zero-filled)
 * pad byte so it survives across loads without growing the record. */
size_t craft_save_serialise(uint32_t seed, uint32_t chunks_nonce,
                            uint8_t autosave_level,
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

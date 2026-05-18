/*
 * ThumbyCraft — flash-backed save sink.
 *
 * Layout (4 fixed slots, 4 KB each = 16 KB total — same region the
 * old single-blob save used, repurposed as 4 fixed slots):
 *
 *   slot i [0..3]: one 4 KB flash sector
 *     bytes  0..11:  u32 magic = 'TCSV', u32 seq, u32 record_len
 *     bytes 12.. :   record bytes (engine save format)
 *     pad to 4-byte boundary
 *     u32 crc32      over [magic..pad]
 *     bytes 2048.. : 32×32 RGB565 thumbnail (exactly 2048 bytes)
 *
 * The old single-blob API (craft_save_flash_write/read) is preserved
 * as a back-compat wrapper that targets slot 0; new callers should
 * use the slot APIs below.
 */
#ifndef CRAFT_SAVE_FLASH_H
#define CRAFT_SAVE_FLASH_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define CRAFT_SAVE_SLOT_COUNT 4
#define CRAFT_SAVE_THUMB_W    32
#define CRAFT_SAVE_THUMB_H    32
#define CRAFT_SAVE_THUMB_PIX  (CRAFT_SAVE_THUMB_W * CRAFT_SAVE_THUMB_H)

/* Persist `n` bytes of save blob into slot `slot` along with an
 * optional thumbnail (64×64 RGB565, length CRAFT_SAVE_THUMB_PIX,
 * pass NULL to skip). Returns true on success. */
bool   craft_save_flash_write_slot(int slot,
                                   const uint8_t  *record, size_t n,
                                   const uint16_t *thumb);

/* Locate the blob in slot `slot`. Returns its byte count + an XIP
 * flash pointer (no copy); 0 if the slot is empty/invalid. */
size_t craft_save_flash_read_slot(int slot, const uint8_t **out_ptr);

/* Pointer to the slot's 64×64 RGB565 thumbnail in XIP flash, or
 * NULL if the slot is empty. */
const uint16_t *craft_save_flash_read_thumb(int slot);

/* Cheap check — does this slot hold a valid record? */
bool craft_save_flash_slot_used(int slot);

/* --- Back-compat single-slot wrappers ---------------------------- */
bool   craft_save_flash_write(const uint8_t *buf, size_t n);
size_t craft_save_flash_read(const uint8_t **out_ptr);

#endif

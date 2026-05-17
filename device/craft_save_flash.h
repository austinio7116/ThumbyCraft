/*
 * ThumbyCraft — flash-backed save sink.
 *
 * Saves live in a 16 KB region at the top of the slot's flash, used
 * as a 4-sector wear-ring. Each save picks the next sector,
 * programs the blob with magic + version + crc32 header (see
 * craft_save.h for the engine-side format), and increments a
 * sequence number. Load picks the most-recent valid sector.
 *
 * This module is the picoflash equivalent for ThumbyCraft —
 * everything below the `craft_save_serialise` boundary lives here.
 */
#ifndef CRAFT_SAVE_FLASH_H
#define CRAFT_SAVE_FLASH_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Persist `n` bytes of save blob. Returns true on success. */
bool craft_save_flash_write(const uint8_t *buf, size_t n);

/* Locate the most recent valid blob. On success returns its byte
 * count and a pointer into XIP flash (no allocation). Returns 0 if
 * nothing valid is present. */
size_t craft_save_flash_read(const uint8_t **out_ptr);

#endif

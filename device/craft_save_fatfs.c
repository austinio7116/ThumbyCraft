/*
 * ThumbyCraft — save metadata sink, FatFs backend (slot mode).
 *
 * Same API as craft_save_flash.c, but each slot's metadata lives in
 * a file on the shared FAT instead of a dedicated flash sector:
 *
 *   /thumbycraft/slot0.meta  ...  slot3.meta
 *
 * Each file mirrors the flash-sector layout (4096 bytes):
 *   bytes  0..11:  u32 magic ('TCS3'), u32 seq, u32 record_len
 *   bytes 12..    record body (engine save format)
 *   pad to 4-byte boundary
 *   u32 crc32 over [magic..pad]
 *   bytes 2048..   32×32 RGB565 thumbnail (exactly 2048 bytes)
 *
 * Per-slot 4 KB caches let read_slot / read_thumb / slot_used return
 * stable in-RAM pointers (FatFs has no mmap-equivalent). Caches are
 * lazy: each slot is read on first query and refreshed on write.
 */
#include "craft_save_flash.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "ff.h"

#define CRAFT_SAVE_MAGIC_FLASH 0x33534354u   /* 'TCS3' */

#define SECTOR_SIZE        4096u
#define THUMB_BYTES        (CRAFT_SAVE_THUMB_PIX * 2u)   /* 2 KB */
#define THUMB_OFFSET       2048u
#define MAX_RECORD_BYTES   (THUMB_OFFSET - 16u)

/* Per-slot 4 KB cache mirrors what would be the on-flash sector. */
static uint8_t s_cache[CRAFT_SAVE_SLOT_COUNT][SECTOR_SIZE] __attribute__((aligned(4)));
static bool    s_cache_valid[CRAFT_SAVE_SLOT_COUNT];
static bool    s_cache_used[CRAFT_SAVE_SLOT_COUNT];

static const char *slot_path(int slot) {
    static char path[32];
    snprintf(path, sizeof path, "/thumbycraft/slot%d.meta", slot);
    return path;
}

static void mkdir_once(void) {
    static bool done = false;
    if (done) return;
    done = true;
    f_mkdir("/thumbycraft");
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}
static uint32_t crc32_calc(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int j = 0; j < 8; j++)
            c = (c >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(c & 1));
    }
    return ~c;
}

/* Validate the cached sector for `slot`. Returns the record byte
 * count if valid, else 0. */
static uint32_t cache_record_len(int slot) {
    if ((unsigned)slot >= CRAFT_SAVE_SLOT_COUNT) return 0;
    const uint8_t *p = s_cache[slot];
    if (rd32(p) != CRAFT_SAVE_MAGIC_FLASH) return 0;
    uint32_t len = rd32(p + 8);
    if (len == 0 || len > MAX_RECORD_BYTES) return 0;
    uint32_t total   = 12 + len;
    uint32_t pad     = (4 - (total & 3)) & 3;
    uint32_t crc_off = total + pad;
    if (crc_off + 4 > SECTOR_SIZE) return 0;
    uint32_t stored = rd32(p + crc_off);
    if (crc32_calc(p, crc_off) != stored) return 0;
    return len;
}

/* Lazy-load slot's cache from the file. */
static void ensure_loaded(int slot) {
    if ((unsigned)slot >= CRAFT_SAVE_SLOT_COUNT) return;
    if (s_cache_valid[slot]) return;
    s_cache_valid[slot] = true;
    memset(s_cache[slot], 0xFF, SECTOR_SIZE);
    s_cache_used[slot] = false;
    mkdir_once();
    FIL fp;
    if (f_open(&fp, slot_path(slot), FA_READ | FA_OPEN_EXISTING) != FR_OK) {
        return;   /* file doesn't exist — slot reads as empty */
    }
    UINT br = 0;
    f_read(&fp, s_cache[slot], SECTOR_SIZE, &br);
    f_close(&fp);
    s_cache_used[slot] = (cache_record_len(slot) > 0);
}

static void invalidate_cache(int slot) {
    if ((unsigned)slot >= CRAFT_SAVE_SLOT_COUNT) return;
    s_cache_valid[slot] = false;
    s_cache_used[slot]  = false;
}

/* --- Public API ------------------------------------------------- */

bool craft_save_flash_slot_used(int slot) {
    if ((unsigned)slot >= CRAFT_SAVE_SLOT_COUNT) return false;
    ensure_loaded(slot);
    return s_cache_used[slot];
}

size_t craft_save_flash_read_slot(int slot, const uint8_t **out_ptr) {
    if ((unsigned)slot >= CRAFT_SAVE_SLOT_COUNT) {
        if (out_ptr) *out_ptr = NULL;
        return 0;
    }
    ensure_loaded(slot);
    uint32_t len = cache_record_len(slot);
    if (len == 0) {
        if (out_ptr) *out_ptr = NULL;
        return 0;
    }
    if (out_ptr) *out_ptr = &s_cache[slot][12];
    return len;
}

const uint16_t *craft_save_flash_read_thumb(int slot) {
    if (!craft_save_flash_slot_used(slot)) return NULL;
    return (const uint16_t *)&s_cache[slot][THUMB_OFFSET];
}

uint32_t craft_save_flash_slot_seq(int slot) {
    if ((unsigned)slot >= CRAFT_SAVE_SLOT_COUNT) return 0;
    ensure_loaded(slot);
    if (cache_record_len(slot) == 0) return 0;
    return rd32(&s_cache[slot][4]);
}

uint32_t craft_save_flash_pick_next_seq(void) {
    uint32_t best = 0;
    for (int s = 0; s < CRAFT_SAVE_SLOT_COUNT; s++) {
        ensure_loaded(s);
        if (cache_record_len(s) == 0) continue;
        uint32_t seq = rd32(&s_cache[s][4]);
        if (seq > best) best = seq;
    }
    return best + 1;
}

bool craft_save_flash_write_slot(int slot, uint32_t seq,
                                 const uint8_t *buf, size_t n,
                                 const uint16_t *thumb) {
    if ((unsigned)slot >= CRAFT_SAVE_SLOT_COUNT) return false;
    if (n > MAX_RECORD_BYTES) return false;
    if (seq == 0) seq = craft_save_flash_pick_next_seq();

    /* Build the full 4 KB sector image in-cache, then write the file. */
    uint8_t *page = s_cache[slot];
    memset(page, 0xFF, SECTOR_SIZE);
    wr32(page + 0, CRAFT_SAVE_MAGIC_FLASH);
    wr32(page + 4, seq);
    wr32(page + 8, (uint32_t)n);
    memcpy(page + 12, buf, n);
    uint32_t total   = 12 + n;
    uint32_t pad     = (4 - (total & 3)) & 3;
    uint32_t crc_off = total + pad;
    uint32_t crc     = crc32_calc(page, crc_off);
    wr32(page + crc_off, crc);
    if (thumb) {
        memcpy(page + THUMB_OFFSET, thumb, THUMB_BYTES);
    }

    mkdir_once();
    FIL fp;
    FRESULT r = f_open(&fp, slot_path(slot), FA_WRITE | FA_CREATE_ALWAYS);
    if (r != FR_OK) {
        invalidate_cache(slot);
        return false;
    }
    UINT bw = 0;
    r = f_write(&fp, page, SECTOR_SIZE, &bw);
    f_close(&fp);
    if (r != FR_OK || bw != SECTOR_SIZE) {
        invalidate_cache(slot);
        return false;
    }
    s_cache_valid[slot] = true;
    s_cache_used[slot]  = true;
    return true;
}

/* Back-compat single-slot wrappers (unused in slot mode, kept for
 * link parity with the flash backend). */
bool craft_save_flash_write(const uint8_t *buf, size_t n) {
    return craft_save_flash_write_slot(0, 0, buf, n, NULL);
}
size_t craft_save_flash_read(const uint8_t **out_ptr) {
    return craft_save_flash_read_slot(0, out_ptr);
}

/* Engine-side hooks declared in craft_save.h. The flash backend
 * forwards these via the back-compat shim file; in slot mode we
 * just call straight through. */
bool craft_save_slot_used(int slot) {
    return craft_save_flash_slot_used(slot);
}
const uint16_t *craft_save_slot_thumb(int slot) {
    return craft_save_flash_read_thumb(slot);
}

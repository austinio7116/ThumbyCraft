/*
 * ThumbyCraft — flash-backed save sink (impl).
 *
 * 4 slots × 12 KB = 48 KB at CRAFT_SAVE_FLASH_OFFSET. Each slot owns
 * three contiguous 4 KB sectors: one for the metadata + save record,
 * two for the 64×64 RGB565 thumbnail. See craft_save_flash.h for the
 * on-flash layout.
 *
 * Save semantics: writing a slot always erases + reprograms all three
 * of its sectors. There's no wear-leveling rotation any more — with
 * 4 slots a player will spread writes around naturally, and the
 * per-slot record sector only sees one erase per save anyway.
 */
#include "craft_save_flash.h"

#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/multicore.h"

#define CRAFT_SAVE_MAGIC_FLASH 0x56534354u   /* 'TCSV' */

#ifndef CRAFT_SAVE_FLASH_OFFSET
#  ifdef THUMBYONE_SLOT_MODE
#    include "slot_layout.h"
#    define CRAFT_SAVE_FLASH_OFFSET SLOT_CRAFT_SAVE_OFFSET
#  else
     /* Standalone: top 16 KB (4 slots × 4 KB) of a 2 MB image. */
#    define CRAFT_SAVE_FLASH_OFFSET (2u * 1024u * 1024u - 16u * 1024u)
#  endif
#endif

#define SECTOR_SIZE        FLASH_SECTOR_SIZE    /* 4096 */
#define SECTORS_PER_SLOT   1                    /* 4 KB per slot */
#define SLOT_STRIDE        (SECTORS_PER_SLOT * SECTOR_SIZE)
#define THUMB_BYTES        (CRAFT_SAVE_THUMB_PIX * 2u)  /* 32x32 = 2048 */
#define THUMB_OFFSET       2048u                 /* offset within slot sector */
#define MAX_RECORD_BYTES   (THUMB_OFFSET - 16u)   /* leaves room for crc */

static const uint8_t *flash_at(uint32_t off) {
    return (const uint8_t *)(XIP_BASE + off);
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
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

/* Returns the record byte count for `slot` if the header sector is
 * valid (magic + crc check out), else 0. */
static uint32_t slot_record_len(int slot) {
    if ((unsigned)slot >= CRAFT_SAVE_SLOT_COUNT) return 0;
    const uint8_t *p = flash_at(CRAFT_SAVE_FLASH_OFFSET + slot * SLOT_STRIDE);
    if (rd32(p) != CRAFT_SAVE_MAGIC_FLASH) return 0;
    uint32_t len = rd32(p + 8);
    if (len > MAX_RECORD_BYTES) return 0;
    uint32_t total   = 12 + len;
    uint32_t pad     = (4 - (total & 3)) & 3;
    uint32_t crc_off = total + pad;
    if (crc_off + 4 > SECTOR_SIZE) return 0;
    uint32_t stored = rd32(p + crc_off);
    if (crc32_calc(p, crc_off) != stored) return 0;
    return len;
}

bool craft_save_flash_slot_used(int slot) {
    return slot_record_len(slot) > 0;
}

size_t craft_save_flash_read_slot(int slot, const uint8_t **out_ptr) {
    uint32_t len = slot_record_len(slot);
    if (len == 0) { if (out_ptr) *out_ptr = NULL; return 0; }
    if (out_ptr) *out_ptr = flash_at(CRAFT_SAVE_FLASH_OFFSET +
                                     slot * SLOT_STRIDE + 12);
    return len;
}

const uint16_t *craft_save_flash_read_thumb(int slot) {
    if (!craft_save_flash_slot_used(slot)) return NULL;
    return (const uint16_t *)flash_at(CRAFT_SAVE_FLASH_OFFSET +
                                      slot * SLOT_STRIDE + THUMB_OFFSET);
}

/* Find the highest seq across all slots; new writes use seq+1 so the
 * "most recent" semantics carry over to slot-aware callers (e.g.,
 * the title page picks an initial selection). */
static uint32_t newest_seq(void) {
    uint32_t best = 0;
    for (int s = 0; s < CRAFT_SAVE_SLOT_COUNT; s++) {
        const uint8_t *p = flash_at(CRAFT_SAVE_FLASH_OFFSET + s * SLOT_STRIDE);
        if (rd32(p) != CRAFT_SAVE_MAGIC_FLASH) continue;
        if (slot_record_len(s) == 0) continue;
        uint32_t seq = rd32(p + 4);
        if (seq > best) best = seq;
    }
    return best;
}

bool craft_save_flash_write_slot(int slot,
                                 const uint8_t *buf, size_t n,
                                 const uint16_t *thumb) {
    if ((unsigned)slot >= CRAFT_SAVE_SLOT_COUNT) return false;
    if (n > MAX_RECORD_BYTES) return false;

    /* Build the header region in a single staging buffer. The record
     * body can exceed a 256-byte flash page (BLK_COUNT * 4 alone is
     * ~240 bytes today), so we program whole pages rather than a
     * single fixed page. Sized at 1 KB to keep BSS low — well above
     * the current 12 + 284 + 4 ≈ 300 byte payload but lots of room
     * to grow before we'd need to revisit. */
    #define HDR_BUF_PAGES   4     /* 4 × 256 = 1024 bytes */
    #define HDR_BUF_SIZE    (HDR_BUF_PAGES * FLASH_PAGE_SIZE)
    static uint8_t hdr_buf[HDR_BUF_SIZE] __attribute__((aligned(4)));
    for (size_t i = 0; i < sizeof hdr_buf; i++) hdr_buf[i] = 0xFF;
    uint32_t next_seq = newest_seq() + 1;
    hdr_buf[0] = CRAFT_SAVE_MAGIC_FLASH & 0xFF;
    hdr_buf[1] = (CRAFT_SAVE_MAGIC_FLASH >> 8) & 0xFF;
    hdr_buf[2] = (CRAFT_SAVE_MAGIC_FLASH >> 16) & 0xFF;
    hdr_buf[3] = (CRAFT_SAVE_MAGIC_FLASH >> 24) & 0xFF;
    hdr_buf[4] = next_seq & 0xFF;
    hdr_buf[5] = (next_seq >> 8) & 0xFF;
    hdr_buf[6] = (next_seq >> 16) & 0xFF;
    hdr_buf[7] = (next_seq >> 24) & 0xFF;
    hdr_buf[8]  = (uint32_t)n & 0xFF;
    hdr_buf[9]  = ((uint32_t)n >> 8) & 0xFF;
    hdr_buf[10] = ((uint32_t)n >> 16) & 0xFF;
    hdr_buf[11] = ((uint32_t)n >> 24) & 0xFF;
    for (size_t i = 0; i < n; i++) hdr_buf[12 + i] = buf[i];
    uint32_t total   = 12 + n;
    uint32_t pad     = (4 - (total & 3)) & 3;
    uint32_t crc_off = total + pad;
    uint32_t crc     = crc32_calc(hdr_buf, crc_off);
    hdr_buf[crc_off + 0] = crc & 0xFF;
    hdr_buf[crc_off + 1] = (crc >> 8) & 0xFF;
    hdr_buf[crc_off + 2] = (crc >> 16) & 0xFF;
    hdr_buf[crc_off + 3] = (crc >> 24) & 0xFF;
    /* Round up to whole flash pages so flash_range_program is happy. */
    uint32_t end_off  = crc_off + 4;
    uint32_t prog_len = (end_off + (FLASH_PAGE_SIZE - 1))
                        & ~(uint32_t)(FLASH_PAGE_SIZE - 1);
    if (prog_len > HDR_BUF_SIZE)  prog_len = HDR_BUF_SIZE;
    if (prog_len > THUMB_OFFSET)  prog_len = THUMB_OFFSET;

    uint32_t off = CRAFT_SAVE_FLASH_OFFSET + (uint32_t)slot * SLOT_STRIDE;
    uint32_t saved = save_and_disable_interrupts();
    multicore_lockout_start_blocking();
    /* Erase the slot's single 4 KB sector. The header + record live
     * in the first prog_len bytes (page-aligned); the thumbnail
     * (2 KB) lives at offset THUMB_OFFSET. Everything between is
     * left as 0xFF by the erase. */
    flash_range_erase(off, SLOT_STRIDE);
    flash_range_program(off, hdr_buf, prog_len);
    /* Thumbnail goes directly from the caller's RGB565 array — no
     * intermediate copy. uint16_t alignment is at least 2-byte;
     * flash_range_program tolerates that. Skip entirely if the
     * caller has no snapshot — erased flash reads as 0xFFFF (white). */
    if (thumb) {
        flash_range_program(off + THUMB_OFFSET,
                            (const uint8_t *)thumb,
                            THUMB_BYTES);
    }
    multicore_lockout_end_blocking();
    restore_interrupts(saved);
    return true;
}

/* --- Back-compat single-slot wrappers (slot 0) ------------------ */
size_t craft_save_flash_read(const uint8_t **out_ptr) {
    return craft_save_flash_read_slot(0, out_ptr);
}
bool craft_save_flash_write(const uint8_t *buf, size_t n) {
    return craft_save_flash_write_slot(0, buf, n, NULL);
}

/* --- Engine-facing portable hooks (declared in craft_save.h) ----
 * The slot picker + title page call these to render the UI; the
 * actual save/load goes via the existing request flags. */
bool craft_save_slot_used(int slot) {
    return craft_save_flash_slot_used(slot);
}
const uint16_t *craft_save_slot_thumb(int slot) {
    return craft_save_flash_read_thumb(slot);
}

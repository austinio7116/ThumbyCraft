/*
 * ThumbyCraft — flash-backed save sink.
 *
 * Layout per slot in the wear ring:
 *   u32 magic = 'TCSV'
 *   u32 seq           — monotonically increasing
 *   u32 blob_len
 *   u8  blob[blob_len]
 *   (pad to 4 bytes)
 *   u32 crc32         — over magic..pad
 *
 * The blob itself is the engine save format (craft_save.h).
 *
 * We reserve 16 KB at a fixed flash offset CRAFT_SAVE_FLASH_OFFSET.
 * For the standalone build this is set late in flash (well past
 * code + textures). For the ThumbyOne slot build the offset comes
 * from common/slot_layout.h.
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
     /* Standalone: reserve the top 16 KB of a 2 MB image. */
#    define CRAFT_SAVE_FLASH_OFFSET (2u * 1024u * 1024u - 16u * 1024u)
#  endif
#endif

#define SECTOR_SIZE        FLASH_SECTOR_SIZE    /* 4096 */
#define SAVE_SECTORS       4                    /* 16 KB wear-ring */

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

/* Returns sector index of newest valid save, or -1 if none. */
static int find_newest(uint32_t *out_seq, uint32_t *out_len) {
    int best = -1;
    uint32_t best_seq = 0;
    uint32_t best_len = 0;
    for (int i = 0; i < SAVE_SECTORS; i++) {
        const uint8_t *p = flash_at(CRAFT_SAVE_FLASH_OFFSET + i * SECTOR_SIZE);
        if (rd32(p) != CRAFT_SAVE_MAGIC_FLASH) continue;
        uint32_t seq = rd32(p + 4);
        uint32_t len = rd32(p + 8);
        if (len > SECTOR_SIZE - 16) continue;
        uint32_t total = 12 + len;
        uint32_t pad   = (4 - (total & 3)) & 3;
        uint32_t crc_off = total + pad;
        if (crc_off + 4 > SECTOR_SIZE) continue;
        uint32_t stored = rd32(p + crc_off);
        if (crc32_calc(p, crc_off) != stored) continue;
        if (best < 0 || seq > best_seq) {
            best = i; best_seq = seq; best_len = len;
        }
    }
    if (out_seq) *out_seq = best_seq;
    if (out_len) *out_len = best_len;
    return best;
}

size_t craft_save_flash_read(const uint8_t **out_ptr) {
    uint32_t seq, len;
    int idx = find_newest(&seq, &len);
    if (idx < 0) { if (out_ptr) *out_ptr = NULL; return 0; }
    *out_ptr = flash_at(CRAFT_SAVE_FLASH_OFFSET + idx * SECTOR_SIZE + 12);
    return len;
}

bool craft_save_flash_write(const uint8_t *buf, size_t n) {
    if (n > SECTOR_SIZE - 16) return false;
    uint32_t seq, dummy_len;
    int newest = find_newest(&seq, &dummy_len);
    int target = (newest < 0) ? 0 : ((newest + 1) % SAVE_SECTORS);
    uint32_t next_seq = (newest < 0) ? 1 : (seq + 1);

    /* Build the page payload in a stack buffer aligned to a flash page. */
    static uint8_t page[SECTOR_SIZE] __attribute__((aligned(4)));
    for (size_t i = 0; i < sizeof page; i++) page[i] = 0xFF;
    /* Header */
    page[0] = CRAFT_SAVE_MAGIC_FLASH & 0xFF;
    page[1] = (CRAFT_SAVE_MAGIC_FLASH >> 8) & 0xFF;
    page[2] = (CRAFT_SAVE_MAGIC_FLASH >> 16) & 0xFF;
    page[3] = (CRAFT_SAVE_MAGIC_FLASH >> 24) & 0xFF;
    page[4] = next_seq & 0xFF;
    page[5] = (next_seq >> 8) & 0xFF;
    page[6] = (next_seq >> 16) & 0xFF;
    page[7] = (next_seq >> 24) & 0xFF;
    page[8] = (uint32_t)n & 0xFF;
    page[9] = ((uint32_t)n >> 8) & 0xFF;
    page[10] = ((uint32_t)n >> 16) & 0xFF;
    page[11] = ((uint32_t)n >> 24) & 0xFF;
    for (size_t i = 0; i < n; i++) page[12 + i] = buf[i];
    uint32_t total = 12 + n;
    uint32_t pad   = (4 - (total & 3)) & 3;
    uint32_t crc_off = total + pad;
    uint32_t crc = crc32_calc(page, crc_off);
    page[crc_off + 0] = crc & 0xFF;
    page[crc_off + 1] = (crc >> 8) & 0xFF;
    page[crc_off + 2] = (crc >> 16) & 0xFF;
    page[crc_off + 3] = (crc >> 24) & 0xFF;

    /* Erase + program. Multicore lockout to keep core1 from racing
     * the XIP cache while flash is busy. */
    uint32_t off = CRAFT_SAVE_FLASH_OFFSET + (uint32_t)target * SECTOR_SIZE;
    uint32_t saved = save_and_disable_interrupts();
    multicore_lockout_start_blocking();
    flash_range_erase(off, SECTOR_SIZE);
    flash_range_program(off, page, SECTOR_SIZE);
    multicore_lockout_end_blocking();
    restore_interrupts(saved);
    return true;
}

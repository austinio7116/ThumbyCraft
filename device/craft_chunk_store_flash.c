/*
 * ThumbyCraft — flash-backed chunk mod store (device impl).
 *
 * Flash layout — reserved 256 KB region positioned just below the
 * 16 KB save sectors at the top of flash.
 *
 *   [0]                  +
 *   [code, textures, ...]
 *   [...]                |
 *   [CHUNK_STORE]   ←----+ CRAFT_CHUNK_STORE_OFFSET, 256 KB
 *   [SAVE WEAR-RING] ←-- last 16 KB
 *
 * The chunk store is 64 slots × 4 KB each. Slot index is
 * hash(chunk_x, chunk_z) & 63. On collision we linear-probe up to
 * CS_PROBE slots forward; if none of those match (or are empty),
 * we overwrite the slot at the head of the probe sequence
 * (effectively a "most-recently-used wins" tiebreak — the cost of
 * unbounded hash collisions is bounded eviction, not corruption).
 *
 * Sector format:
 *   u32 magic = 'TCMK'        // 'M'od chu'K'
 *   u32 world_seed             // invalidates records from old worlds
 *   i32 chunk_x
 *   i32 chunk_z
 *   u16 mod_count
 *   u16 _pad
 *   ChunkMod[mod_count]        // 4 bytes each
 *   u32 crc32                  // CRC over everything above
 *   (pad to sector boundary with 0xFF)
 */
#include "craft_chunk_store.h"

#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/multicore.h"

#include <string.h>

#define CS_MAGIC        0x4B4D4354u   /* 'TCMK' little-endian */
#define CS_SECTOR_SIZE  FLASH_SECTOR_SIZE   /* 4096 */
#define CS_SLOTS        64
#define CS_PROBE        4

/* Reserved 256 KB below the save sectors. Save occupies 16 KB at
 * the top of the image (4 fixed slots × 4 KB), so chunk store lives
 * at (TOP - 16 KB - 256 KB). For the standalone 2 MB build that's
 * 2,097,152 - 16,384 - 262,144 = 1,818,624 bytes. */
#ifndef CRAFT_CHUNK_STORE_OFFSET
#  ifdef THUMBYONE_SLOT_MODE
#    include "slot_layout.h"
#    define CRAFT_CHUNK_STORE_OFFSET SLOT_CRAFT_CHUNK_STORE_OFFSET
#  else
#    define CRAFT_CHUNK_STORE_OFFSET \
        (2u * 1024u * 1024u - 16u * 1024u - 256u * 1024u)
#  endif
#endif

static uint32_t s_world_seed = 0;

static const uint8_t *flash_at(uint32_t off) {
    return (const uint8_t *)(XIP_BASE + off);
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int32_t rd_i32(const uint8_t *p) { return (int32_t)rd32(p); }

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}
static void wr_i32(uint8_t *p, int32_t v) { wr32(p, (uint32_t)v); }

static uint32_t crc32_calc(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int j = 0; j < 8; j++)
            c = (c >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(c & 1));
    }
    return ~c;
}

/* Mix chunk coords into a 32-bit hash, then take low CS_SLOTS_BITS. */
static uint32_t slot_hash(int cx, int cz) {
    uint32_t h = (uint32_t)cx * 0x9E3779B1u ^ (uint32_t)cz * 0x85EBCA77u;
    h ^= h >> 16;
    h *= 0xC2B2AE3Du;
    h ^= h >> 13;
    return h;
}

/* Sector layout offsets. */
#define OFF_MAGIC      0
#define OFF_SEED       4
#define OFF_CX         8
#define OFF_CZ         12
#define OFF_COUNT      16
#define OFF_MODS       20
/* CRC is right after mods, then sector padded to CS_SECTOR_SIZE. */

/* Returns true and fills cx/cz/count if the sector at off is a valid
 * record. Validity = correct magic, matching world seed, sane count,
 * and matching CRC. */
static bool read_header(uint32_t off,
                        int *out_cx, int *out_cz, int *out_count) {
    const uint8_t *p = flash_at(off);
    if (rd32(p + OFF_MAGIC) != CS_MAGIC) return false;
    if (rd32(p + OFF_SEED)  != s_world_seed) return false;
    uint32_t cnt = rd32(p + OFF_COUNT) & 0xFFFFu;
    if (cnt > CHUNK_STORE_MAX_MODS_PER_CHUNK) return false;
    uint32_t data_end = OFF_MODS + cnt * sizeof(ChunkMod);
    if (data_end + 4 > CS_SECTOR_SIZE) return false;
    uint32_t stored_crc = rd32(p + data_end);
    if (crc32_calc(p, data_end) != stored_crc) return false;
    if (out_cx)    *out_cx    = rd_i32(p + OFF_CX);
    if (out_cz)    *out_cz    = rd_i32(p + OFF_CZ);
    if (out_count) *out_count = (int)cnt;
    return true;
}

/* Walk the probe sequence for (cx, cz). Returns sector offset of the
 * matching record if one exists; -1 otherwise.
 * If `or_first_empty` is true, falls through to return the first
 * empty/invalid slot in the probe (suitable for insert). */
static int32_t find_slot(int cx, int cz, bool or_first_empty) {
    uint32_t h = slot_hash(cx, cz);
    int first_empty = -1;
    for (int p = 0; p < CS_PROBE; p++) {
        int slot = (int)((h + (uint32_t)p) & (CS_SLOTS - 1));
        uint32_t off = CRAFT_CHUNK_STORE_OFFSET + (uint32_t)slot * CS_SECTOR_SIZE;
        int scx, scz, scnt;
        if (read_header(off, &scx, &scz, &scnt)) {
            if (scx == cx && scz == cz) return (int32_t)off;
        } else if (first_empty < 0) {
            first_empty = slot;
        }
    }
    if (or_first_empty && first_empty >= 0) {
        return (int32_t)(CRAFT_CHUNK_STORE_OFFSET +
                         (uint32_t)first_empty * CS_SECTOR_SIZE);
    }
    if (or_first_empty) {
        /* No empty slot in the probe sequence — evict slot at head of
         * probe (deterministic; trade some old chunk for the new one). */
        int slot = (int)(h & (CS_SLOTS - 1));
        return (int32_t)(CRAFT_CHUNK_STORE_OFFSET +
                         (uint32_t)slot * CS_SECTOR_SIZE);
    }
    return -1;
}

void craft_chunk_store_init(uint32_t world_seed) {
    s_world_seed = world_seed;
    /* Nothing to scan eagerly — load happens on demand per chunk. */
}

int craft_chunk_store_load(int chunk_x, int chunk_z,
                           ChunkMod *out, int max_entries) {
    int32_t off = find_slot(chunk_x, chunk_z, false);
    if (off < 0) return 0;
    int count = 0;
    if (!read_header((uint32_t)off, NULL, NULL, &count)) return 0;
    if (count > max_entries) count = max_entries;
    const uint8_t *p = flash_at((uint32_t)off + OFF_MODS);
    for (int i = 0; i < count; i++) {
        out[i].lx  = p[i * 4 + 0];
        out[i].y   = p[i * 4 + 1];
        out[i].lz  = p[i * 4 + 2];
        out[i].blk = p[i * 4 + 3];
    }
    return count;
}

bool craft_chunk_store_save(int chunk_x, int chunk_z,
                            const ChunkMod *mods, int n) {
    if (n > CHUNK_STORE_MAX_MODS_PER_CHUNK) n = CHUNK_STORE_MAX_MODS_PER_CHUNK;

    /* Empty save = delete: find the existing record (if any) and
     * erase its sector. Avoid touching flash if there's nothing here. */
    if (n == 0) {
        int32_t off = find_slot(chunk_x, chunk_z, false);
        if (off < 0) return true;
        uint32_t saved = save_and_disable_interrupts();
        multicore_lockout_start_blocking();
        flash_range_erase((uint32_t)off, CS_SECTOR_SIZE);
        multicore_lockout_end_blocking();
        restore_interrupts(saved);
        return true;
    }

    int32_t off = find_slot(chunk_x, chunk_z, true);
    if (off < 0) return false;

    static uint8_t page[CS_SECTOR_SIZE] __attribute__((aligned(4)));
    for (uint32_t i = 0; i < sizeof page; i++) page[i] = 0xFF;
    wr32   (page + OFF_MAGIC, CS_MAGIC);
    wr32   (page + OFF_SEED,  s_world_seed);
    wr_i32 (page + OFF_CX,    chunk_x);
    wr_i32 (page + OFF_CZ,    chunk_z);
    page[OFF_COUNT]     = (uint8_t)(n & 0xFF);
    page[OFF_COUNT + 1] = (uint8_t)((n >> 8) & 0xFF);
    page[OFF_COUNT + 2] = 0;
    page[OFF_COUNT + 3] = 0;
    for (int i = 0; i < n; i++) {
        page[OFF_MODS + i * 4 + 0] = mods[i].lx;
        page[OFF_MODS + i * 4 + 1] = mods[i].y;
        page[OFF_MODS + i * 4 + 2] = mods[i].lz;
        page[OFF_MODS + i * 4 + 3] = mods[i].blk;
    }
    uint32_t data_end = OFF_MODS + (uint32_t)n * sizeof(ChunkMod);
    uint32_t crc = crc32_calc(page, data_end);
    wr32(page + data_end, crc);

    uint32_t saved = save_and_disable_interrupts();
    multicore_lockout_start_blocking();
    flash_range_erase((uint32_t)off, CS_SECTOR_SIZE);
    flash_range_program((uint32_t)off, page, CS_SECTOR_SIZE);
    multicore_lockout_end_blocking();
    restore_interrupts(saved);
    return true;
}

void craft_chunk_store_clear(void) {
    uint32_t saved = save_and_disable_interrupts();
    multicore_lockout_start_blocking();
    for (int i = 0; i < CS_SLOTS; i++) {
        uint32_t off = CRAFT_CHUNK_STORE_OFFSET + (uint32_t)i * CS_SECTOR_SIZE;
        flash_range_erase(off, CS_SECTOR_SIZE);
    }
    multicore_lockout_end_blocking();
    restore_interrupts(saved);
}

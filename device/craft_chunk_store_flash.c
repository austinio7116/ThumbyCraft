/*
 * ThumbyCraft — flash-backed chunk store (per-world, nonce-filtered).
 *
 * Layout: each region is 1 MB = 256 sectors of 4 KB. Sector format:
 *   u32 magic    'TCM4'
 *   u32 nonce    region-binding nonce (must match on read)
 *   i32 chunk_x
 *   i32 chunk_z
 *   u16 mod_count + u16 reserved
 *   ChunkMod mods[count]   (4 bytes each: lx, y, lz, blk)
 *   u32 crc32   over [magic .. last mod]
 *   padding to sector boundary
 *
 * Hashing: hash(cx, cz) -> sector index. Linear probe up to 8 slots
 * on collision. A sector is "free for probe" if its magic is wrong
 * OR its nonce doesn't match the bound nonce — i.e. stale sectors
 * from a previous nonce look exactly like empty sectors. That's
 * what makes "new world" a single-instruction operation (bump nonce).
 */
#include "craft_chunk_store.h"
#include "slot_layout.h"

#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/multicore.h"

#include <string.h>

#define CS_MAGIC        0x344D4354u   /* 'TCM4' — bumped from v1 'TCMK' */
#define CS_SECTOR_SIZE  FLASH_SECTOR_SIZE
#define CS_SLOTS        TBC_CHUNK_REGION_SECTORS
#define CS_SLOTS_MASK   (CS_SLOTS - 1)
#define CS_PROBE        8

#define OFF_MAGIC      0
#define OFF_NONCE      4
#define OFF_CX         8
#define OFF_CZ         12
#define OFF_COUNT      16
#define OFF_MODS       20

static int       s_region = -1;
static uint32_t  s_nonce;

static const uint8_t *flash_at(uint32_t off) {
    return (const uint8_t *)(XIP_BASE + off);
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static int32_t rd_i32(const uint8_t *p) { return (int32_t)rd32(p); }
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
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

static uint32_t slot_hash(int cx, int cz) {
    uint32_t h = (uint32_t)cx * 0x9E3779B1u + (uint32_t)cz * 0x85EBCA77u;
    h ^= h >> 16;
    return h & CS_SLOTS_MASK;
}

static uint32_t region_base(int r) { return TBC_CHUNK_REGION_OFFSET(r); }

/* Returns true when the sector at (r, slot) is a valid record with
 * the supplied nonce. Mismatched-nonce sectors are treated as empty
 * — that's the whole point of the nonce filter. */
static bool read_header(int r, int slot, uint32_t nonce,
                        int *out_cx, int *out_cz, int *out_count) {
    if ((unsigned)slot >= CS_SLOTS) return false;
    uint32_t off = region_base(r) + (uint32_t)slot * CS_SECTOR_SIZE;
    const uint8_t *p = flash_at(off);
    if (rd32(p + OFF_MAGIC) != CS_MAGIC)  return false;
    if (rd32(p + OFF_NONCE) != nonce)     return false;
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

/* Probe chain for (cx, cz) under the given nonce. Returns the slot
 * with a matching record, or -1.
 * If or_first_empty: returns the first probe slot that is empty OR
 * holds a stale (mismatched-nonce) record, suitable for insertion.
 * If everything in the probe chain is occupied with valid records
 * for OTHER (cx, cz), evicts the probe head — bounded same-region
 * eviction only. */
static int find_slot(int r, uint32_t nonce, int cx, int cz, bool or_first_empty) {
    uint32_t h = slot_hash(cx, cz);
    int first_empty = -1;
    for (int p = 0; p < CS_PROBE; p++) {
        int slot = (int)((h + (uint32_t)p) & CS_SLOTS_MASK);
        int scx, scz, scnt;
        if (read_header(r, slot, nonce, &scx, &scz, &scnt)) {
            if (scx == cx && scz == cz) return slot;
        } else if (first_empty < 0) {
            first_empty = slot;
        }
    }
    if (or_first_empty && first_empty >= 0) return first_empty;
    if (or_first_empty) return (int)h;   /* evict probe head */
    return -1;
}

/* --- Public API ------------------------------------------------- */

void craft_chunk_store_bind(int region, uint32_t nonce) {
    if ((unsigned)region >= TBC_REGION_COUNT) return;
    s_region = region;
    s_nonce  = nonce;
}

int      craft_chunk_store_bound(void)       { return s_region; }
uint32_t craft_chunk_store_bound_nonce(void) { return s_nonce; }

int craft_chunk_store_load(int chunk_x, int chunk_z,
                           ChunkMod *out, int max_entries) {
    if (s_region < 0) return 0;
    int slot = find_slot(s_region, s_nonce, chunk_x, chunk_z, false);
    if (slot < 0) return 0;
    int count = 0;
    if (!read_header(s_region, slot, s_nonce, NULL, NULL, &count)) return 0;
    if (count > max_entries) count = max_entries;
    uint32_t off = region_base(s_region) + (uint32_t)slot * CS_SECTOR_SIZE;
    const uint8_t *p = flash_at(off + OFF_MODS);
    for (int i = 0; i < count; i++) {
        out[i].lx  = p[i * 4 + 0];
        out[i].y   = p[i * 4 + 1];
        out[i].lz  = p[i * 4 + 2];
        out[i].blk = p[i * 4 + 3];
    }
    return count;
}

/* Shared 4 KB staging page for save + copy. Keeps BSS down. */
uint8_t cs_staging_page[CS_SECTOR_SIZE] __attribute__((aligned(4)));

uint8_t *craft_chunk_store_scratch4k(void) { return cs_staging_page; }

/* Write a fully-built sector buffer to (region, slot). Erase-then-
 * program; lockout core 1 across the erase+program window. */
static void program_sector(int region, int slot, const uint8_t *page) {
    uint32_t off = region_base(region) + (uint32_t)slot * CS_SECTOR_SIZE;
    uint32_t saved = save_and_disable_interrupts();
    multicore_lockout_start_blocking();
    flash_range_erase(off, CS_SECTOR_SIZE);
    flash_range_program(off, page, CS_SECTOR_SIZE);
    multicore_lockout_end_blocking();
    restore_interrupts(saved);
}

/* Erase a single sector — used for chunk deletes. */
static void erase_sector(int region, int slot) {
    uint32_t off = region_base(region) + (uint32_t)slot * CS_SECTOR_SIZE;
    uint32_t saved = save_and_disable_interrupts();
    multicore_lockout_start_blocking();
    flash_range_erase(off, CS_SECTOR_SIZE);
    multicore_lockout_end_blocking();
    restore_interrupts(saved);
}

/* Fill cs_staging_page with a chunk record (nonce-stamped, CRC'd). */
static void build_sector(uint32_t nonce, int cx, int cz,
                         const ChunkMod *mods, int n) {
    uint8_t *page = cs_staging_page;
    for (uint32_t i = 0; i < CS_SECTOR_SIZE; i++) page[i] = 0xFF;
    wr32   (page + OFF_MAGIC, CS_MAGIC);
    wr32   (page + OFF_NONCE, nonce);
    wr_i32 (page + OFF_CX,    cx);
    wr_i32 (page + OFF_CZ,    cz);
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
}

bool craft_chunk_store_save(int chunk_x, int chunk_z,
                            const ChunkMod *mods, int n) {
    if (s_region < 0) return false;
    if (n > CHUNK_STORE_MAX_MODS_PER_CHUNK) n = CHUNK_STORE_MAX_MODS_PER_CHUNK;

    if (n == 0) {
        int slot = find_slot(s_region, s_nonce, chunk_x, chunk_z, false);
        if (slot < 0) return true;
        erase_sector(s_region, slot);
        return true;
    }

    int slot = find_slot(s_region, s_nonce, chunk_x, chunk_z, true);
    if (slot < 0) return false;

    build_sector(s_nonce, chunk_x, chunk_z, mods, n);
    program_sector(s_region, slot, cs_staging_page);
    return true;
}

int craft_chunk_store_slots(void) { return CS_SLOTS; }

int craft_chunk_store_read_slot(int slot, int *cx, int *cz,
                                ChunkMod *out, int max_entries) {
    if (s_region < 0) return -1;
    int count = 0;
    if (!read_header(s_region, slot, s_nonce, cx, cz, &count)) return -1;
    if (out) {
        int n = count > max_entries ? max_entries : count;
        uint32_t off = region_base(s_region) + (uint32_t)slot * CS_SECTOR_SIZE;
        const uint8_t *p = flash_at(off + OFF_MODS);
        for (int i = 0; i < n; i++) {
            out[i].lx  = p[i * 4 + 0];
            out[i].y   = p[i * 4 + 1];
            out[i].lz  = p[i * 4 + 2];
            out[i].blk = p[i * 4 + 3];
        }
    }
    return count;
}

void craft_chunk_store_erase_region(int region) {
    if ((unsigned)region >= TBC_REGION_COUNT) return;
    /* Per-sector erase, no progress bar — only invoked from explicit
     * destructive UI paths (none in Plan A). Roughly 3 s blocking. */
    for (int s = 0; s < CS_SLOTS; s++) erase_sector(region, s);
}

void craft_chunk_store_copy(int src_region, uint32_t src_nonce,
                            int dst_region, uint32_t dst_nonce) {
    if ((unsigned)src_region >= TBC_REGION_COUNT) return;
    if ((unsigned)dst_region >= TBC_REGION_COUNT) return;
    if (src_region == dst_region && src_nonce == dst_nonce) return;

    /* Walk source sectors. For each one whose magic + nonce match
     * src_nonce, re-build the sector with dst_nonce and program at
     * the same hash position in dst. Old dst sectors at other hash
     * positions stay physically present but are stale (different
     * nonce or never written) — they don't interfere because dst's
     * binding will filter on dst_nonce. */
    for (int slot = 0; slot < CS_SLOTS; slot++) {
        const uint8_t *src = flash_at(region_base(src_region) +
                                      (uint32_t)slot * CS_SECTOR_SIZE);
        if (rd32(src + OFF_MAGIC) != CS_MAGIC) continue;
        if (rd32(src + OFF_NONCE) != src_nonce) continue;
        uint32_t cnt = rd32(src + OFF_COUNT) & 0xFFFFu;
        if (cnt == 0 || cnt > CHUNK_STORE_MAX_MODS_PER_CHUNK) continue;
        int cx = rd_i32(src + OFF_CX);
        int cz = rd_i32(src + OFF_CZ);

        /* Reuse build_sector via staging page so the CRC + nonce field
         * are correct under dst_nonce. */
        for (uint32_t i = 0; i < CS_SECTOR_SIZE; i++) cs_staging_page[i] = 0xFF;
        wr32   (cs_staging_page + OFF_MAGIC, CS_MAGIC);
        wr32   (cs_staging_page + OFF_NONCE, dst_nonce);
        wr_i32 (cs_staging_page + OFF_CX,    cx);
        wr_i32 (cs_staging_page + OFF_CZ,    cz);
        cs_staging_page[OFF_COUNT]     = (uint8_t)(cnt & 0xFF);
        cs_staging_page[OFF_COUNT + 1] = (uint8_t)((cnt >> 8) & 0xFF);
        cs_staging_page[OFF_COUNT + 2] = 0;
        cs_staging_page[OFF_COUNT + 3] = 0;
        uint32_t data_end = OFF_MODS + cnt * sizeof(ChunkMod);
        for (uint32_t i = OFF_MODS; i < data_end; i++)
            cs_staging_page[i] = src[i];
        uint32_t crc = crc32_calc(cs_staging_page, data_end);
        wr32(cs_staging_page + data_end, crc);
        program_sector(dst_region, slot, cs_staging_page);
    }
}

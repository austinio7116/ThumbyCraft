/*
 * ThumbyCraft — chunk store, FatFs backend (slot mode).
 *
 * Storage model: one file per chunk, named "<cx>_<cz>.cnk", grouped
 * under a per-region directory:
 *
 *   /thumbycraft/scratch/<cx>_<cz>.cnk
 *   /thumbycraft/slot0/<cx>_<cz>.cnk  ... slot3/<cx>_<cz>.cnk
 *
 * Why not one file per region with hash-indexed sectors? FatFs has
 * no sparse-file support — `f_lseek` past EOF + `f_write` forces it
 * to allocate every cluster up to the write position. So a single
 * chunk written at hash slot 200 makes the file 800 KB. One file
 * per chunk gives genuinely-lazy allocation: each chunk costs one
 * 4 KB cluster regardless of (cx, cz) magnitude.
 *
 * Hash + probing isn't needed any more — the filesystem is the
 * lookup table. find_slot becomes "construct the path string".
 *
 * Nonce filter still works: each chunk file carries the embedded
 * nonce in its header bytes, and bind(region, nonce) sets the
 * current nonce so reads ignore stale files (e.g. New World bumps
 * the scratch nonce; old scratch/<cx>_<cz>.cnk files become
 * invisible until overwritten or `erase_region` deletes them).
 */
#include "craft_chunk_store.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "ff.h"

#define CS_MAGIC        0x344D4354u   /* 'TCM4' */
#define CS_SECTOR_SIZE  4096u
#define REGION_COUNT    5

#define OFF_MAGIC      0
#define OFF_NONCE      4
#define OFF_CX         8
#define OFF_CZ         12
#define OFF_COUNT      16
#define OFF_MODS       20

static int      s_region = -1;
static uint32_t s_nonce;

/* Shared 4 KB sector buffer. Reused by load / save / copy — bounded
 * to one chunk-op at a time per the engine's serial usage. */
static uint8_t s_page[CS_SECTOR_SIZE] __attribute__((aligned(4)));

uint8_t *craft_chunk_store_scratch4k(void) { return s_page; }

/* 256-bit bloom-style filter for "does this region have a file for
 * (cx, cz)?" — set by directory scan on bind and by save(); checked
 * by load() to skip the ~2 ms f_open(FR_NO_FILE) round-trip on
 * unedited chunks. Some hash collisions land an unedited chunk on
 * a set bit, costing a single false-positive f_open — that's fine.
 * Rebuilt on every bind. */
static uint8_t s_exists_bm[32];

static uint32_t exists_hash(int cx, int cz) {
    uint32_t h = (uint32_t)cx * 0x9E3779B1u + (uint32_t)cz * 0x85EBCA77u;
    h ^= h >> 16;
    return h & 0xFFu;
}
static bool exists_test(int cx, int cz) {
    uint32_t h = exists_hash(cx, cz);
    return (s_exists_bm[h >> 3] >> (h & 7)) & 1;
}
static void exists_set(int cx, int cz) {
    uint32_t h = exists_hash(cx, cz);
    s_exists_bm[h >> 3] |= (uint8_t)(1u << (h & 7));
}
static void exists_clear_all(void) { memset(s_exists_bm, 0, sizeof s_exists_bm); }

/* Chunk-coordinate table for enumeration (the co-op world transfer).
 * Built from the same directory scan bind() already does; save()
 * appends new coords. There's no random access into a FAT directory,
 * so this is what makes craft_chunk_store_read_slot O(1). i16 chunk
 * coords cover ±524k world cells — far past reachable play. */
#define COORD_TABLE_MAX 192
static struct { int16_t cx, cz; } s_coords[COORD_TABLE_MAX];
static int s_coord_n;

static void coord_add(int cx, int cz) {
    for (int i = 0; i < s_coord_n; i++)
        if (s_coords[i].cx == cx && s_coords[i].cz == cz) return;
    if (s_coord_n >= COORD_TABLE_MAX) return;   /* enumeration-only loss */
    s_coords[s_coord_n].cx = (int16_t)cx;
    s_coords[s_coord_n].cz = (int16_t)cz;
    s_coord_n++;
}

/* Parse "<cx>_<cz>.cnk" → (cx, cz). Returns true on success. */
static bool parse_chunk_name(const char *name, int *out_cx, int *out_cz) {
    int cx = 0, cz = 0, n = 0;
    if (sscanf(name, "%d_%d.cnk%n", &cx, &cz, &n) >= 2 &&
        name[n] == '\0') {
        if (out_cx) *out_cx = cx;
        if (out_cz) *out_cz = cz;
        return true;
    }
    return false;
}

static const char *region_dir(int region) {
    static const char *const dirs[REGION_COUNT] = {
        "/thumbycraft/slot0",
        "/thumbycraft/slot1",
        "/thumbycraft/slot2",
        "/thumbycraft/slot3",
        "/thumbycraft/scratch",
    };
    if ((unsigned)region >= REGION_COUNT) return dirs[0];
    return dirs[region];
}

/* Build "/thumbycraft/<region>/<cx>_<cz>.cnk" into `out` (≥48 bytes). */
static void chunk_path(char *out, size_t out_len, int region, int cx, int cz) {
    snprintf(out, out_len, "%s/%d_%d.cnk", region_dir(region), cx, cz);
}

static void mkdir_once(void) {
    static bool done = false;
    if (done) return;
    done = true;
    f_mkdir("/thumbycraft");
}

static void mkdir_region(int region) {
    mkdir_once();
    f_mkdir(region_dir(region));   /* FR_EXIST is fine */
}

/* --- Encoding helpers ------------------------------------------ */

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

/* Validate a 4 KB sector buffer against `nonce`. */
static bool validate_header(const uint8_t *p, uint32_t nonce,
                            int *out_cx, int *out_cz, int *out_count) {
    if (rd32(p + OFF_MAGIC) != CS_MAGIC) return false;
    if (rd32(p + OFF_NONCE) != nonce)    return false;
    uint32_t cnt = rd32(p + OFF_COUNT) & 0xFFFFu;
    if (cnt > CHUNK_STORE_MAX_MODS_PER_CHUNK) return false;
    uint32_t data_end = OFF_MODS + cnt * sizeof(ChunkMod);
    if (data_end + 4 > CS_SECTOR_SIZE) return false;
    uint32_t stored = rd32(p + data_end);
    if (crc32_calc(p, data_end) != stored) return false;
    if (out_cx)    *out_cx    = rd_i32(p + OFF_CX);
    if (out_cz)    *out_cz    = rd_i32(p + OFF_CZ);
    if (out_count) *out_count = (int)cnt;
    return true;
}

static void build_sector(uint32_t nonce, int cx, int cz,
                         const ChunkMod *mods, int n, uint8_t *page) {
    memset(page, 0xFF, CS_SECTOR_SIZE);
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

/* --- Public API ------------------------------------------------- */

void craft_chunk_store_bind(int region, uint32_t nonce) {
    if (region < 0 || region >= REGION_COUNT) return;
    s_region = region;
    s_nonce  = nonce;
    /* Rebuild the existence bitmap from the region's directory.
     * Each .cnk file's name is parsed back to (cx, cz) and the
     * bit set. Subsequent load() calls check the bit and skip the
     * f_open(FR_NO_FILE) cost for chunks we know aren't there.
     * One pass over the dir per bind — cheap. */
    exists_clear_all();
    s_coord_n = 0;
    DIR d;
    if (f_opendir(&d, region_dir(region)) != FR_OK) return;
    FILINFO fi;
    while (f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        if (fi.fattrib & AM_DIR) continue;
        int cx, cz;
        if (parse_chunk_name(fi.fname, &cx, &cz)) {
            exists_set(cx, cz);
            coord_add(cx, cz);
        }
    }
    f_closedir(&d);
}

int      craft_chunk_store_bound(void)       { return s_region; }
uint32_t craft_chunk_store_bound_nonce(void) { return s_nonce; }

int craft_chunk_store_load(int chunk_x, int chunk_z,
                           ChunkMod *out, int max_entries) {
    if (s_region < 0) return 0;
    /* Fast-path: skip the f_open if the existence bitmap says no
     * file is present for this (cx, cz). False positives (different
     * (cx, cz) hashing to the same bit) just cost one extra f_open
     * that returns FR_NO_FILE — still cheap. */
    if (!exists_test(chunk_x, chunk_z)) return 0;
    char path[48];
    chunk_path(path, sizeof path, s_region, chunk_x, chunk_z);

    FIL fp;
    if (f_open(&fp, path, FA_READ | FA_OPEN_EXISTING) != FR_OK) return 0;
    UINT br = 0;
    FRESULT r = f_read(&fp, s_page, CS_SECTOR_SIZE, &br);
    f_close(&fp);
    if (r != FR_OK || br < OFF_MODS) return 0;
    if (br < CS_SECTOR_SIZE) memset(s_page + br, 0xFF, CS_SECTOR_SIZE - br);

    int count = 0;
    if (!validate_header(s_page, s_nonce, NULL, NULL, &count)) return 0;
    if (count > max_entries) count = max_entries;
    for (int i = 0; i < count; i++) {
        out[i].lx  = s_page[OFF_MODS + i * 4 + 0];
        out[i].y   = s_page[OFF_MODS + i * 4 + 1];
        out[i].lz  = s_page[OFF_MODS + i * 4 + 2];
        out[i].blk = s_page[OFF_MODS + i * 4 + 3];
    }
    return count;
}

bool craft_chunk_store_save(int chunk_x, int chunk_z,
                            const ChunkMod *mods, int n) {
    if (s_region < 0) return false;
    if (n > CHUNK_STORE_MAX_MODS_PER_CHUNK) n = CHUNK_STORE_MAX_MODS_PER_CHUNK;

    char path[48];
    chunk_path(path, sizeof path, s_region, chunk_x, chunk_z);

    if (n == 0) {
        /* Delete = unlink the file. Leave the existence bit set —
         * a false-positive on a re-read is cheap (one f_open that
         * fails), and clearing the bit safely would require
         * verifying no other (cx, cz) hashes to the same slot. */
        f_unlink(path);   /* ignore FR_NO_FILE */
        return true;
    }

    mkdir_region(s_region);
    build_sector(s_nonce, chunk_x, chunk_z, mods, n, s_page);
    exists_set(chunk_x, chunk_z);
    coord_add(chunk_x, chunk_z);

    FIL fp;
    if (f_open(&fp, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return false;
    UINT bw = 0;
    /* Write only as much as actually contains data (header + mods +
     * CRC, rounded to 4-byte boundary). The header counts what's
     * valid; trailing 0xFF padding the standalone flash backend used
     * isn't needed on FAT — the file just ends after the CRC. */
    uint32_t data_end = OFF_MODS + (uint32_t)n * sizeof(ChunkMod);
    uint32_t end = data_end + 4;
    FRESULT r = f_write(&fp, s_page, end, &bw);
    f_close(&fp);
    return r == FR_OK && bw == end;
}

int craft_chunk_store_slots(void) { return s_coord_n; }

int craft_chunk_store_read_slot(int slot, int *cx, int *cz,
                                ChunkMod *out, int max_entries) {
    if (s_region < 0 || (unsigned)slot >= (unsigned)s_coord_n) return -1;
    int scx = s_coords[slot].cx, scz = s_coords[slot].cz;
    if (cx) *cx = scx;
    if (cz) *cz = scz;
    if (out) return craft_chunk_store_load(scx, scz, out, max_entries);
    /* Measuring pass — validate the file and return its count without
     * copying (uses the shared s_page like every other chunk op). */
    char path[48];
    chunk_path(path, sizeof path, s_region, scx, scz);
    FIL fp;
    if (f_open(&fp, path, FA_READ | FA_OPEN_EXISTING) != FR_OK) return -1;
    UINT br = 0;
    FRESULT r = f_read(&fp, s_page, CS_SECTOR_SIZE, &br);
    f_close(&fp);
    if (r != FR_OK || br < OFF_MODS) return -1;
    if (br < CS_SECTOR_SIZE) memset(s_page + br, 0xFF, CS_SECTOR_SIZE - br);
    int count = 0;
    if (!validate_header(s_page, s_nonce, NULL, NULL, &count)) return -1;
    return count;
}

void craft_chunk_store_erase_region(int region) {
    if (region < 0 || region >= REGION_COUNT) return;
    /* Walk the directory and unlink every .cnk file. */
    DIR d;
    if (f_opendir(&d, region_dir(region)) != FR_OK) return;
    FILINFO fi;
    char path[64];
    while (f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        if (fi.fattrib & AM_DIR) continue;
        snprintf(path, sizeof path, "%s/%s",
                 region_dir(region), fi.fname);
        f_unlink(path);
    }
    f_closedir(&d);
    if (region == s_region) exists_clear_all();
}

void craft_chunk_store_copy(int src_region, uint32_t src_nonce,
                            int dst_region, uint32_t dst_nonce) {
    if (src_region < 0 || src_region >= REGION_COUNT) return;
    if (dst_region < 0 || dst_region >= REGION_COUNT) return;
    if (src_region == dst_region && src_nonce == dst_nonce) return;

    mkdir_region(dst_region);

    /* Walk the source directory, validate each chunk file under
     * src_nonce, re-build with dst_nonce, write to dst region. */
    DIR d;
    if (f_opendir(&d, region_dir(src_region)) != FR_OK) return;
    FILINFO fi;
    char src_path[64], dst_path[64];
    static ChunkMod tmp_mods[CHUNK_STORE_MAX_MODS_PER_CHUNK];

    while (f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        if (fi.fattrib & AM_DIR) continue;
        snprintf(src_path, sizeof src_path, "%s/%s",
                 region_dir(src_region), fi.fname);

        /* Read source. */
        FIL sfp;
        if (f_open(&sfp, src_path, FA_READ | FA_OPEN_EXISTING) != FR_OK) continue;
        UINT br = 0;
        FRESULT r = f_read(&sfp, s_page, CS_SECTOR_SIZE, &br);
        f_close(&sfp);
        if (r != FR_OK || br < OFF_MODS) continue;
        if (br < CS_SECTOR_SIZE) memset(s_page + br, 0xFF, CS_SECTOR_SIZE - br);

        int scx, scz, scnt;
        if (!validate_header(s_page, src_nonce, &scx, &scz, &scnt)) continue;

        /* Snapshot mods into tmp_mods so build_sector can stomp s_page. */
        for (int i = 0; i < scnt; i++) {
            tmp_mods[i].lx  = s_page[OFF_MODS + i * 4 + 0];
            tmp_mods[i].y   = s_page[OFF_MODS + i * 4 + 1];
            tmp_mods[i].lz  = s_page[OFF_MODS + i * 4 + 2];
            tmp_mods[i].blk = s_page[OFF_MODS + i * 4 + 3];
        }
        build_sector(dst_nonce, scx, scz, tmp_mods, scnt, s_page);

        chunk_path(dst_path, sizeof dst_path, dst_region, scx, scz);
        FIL dfp;
        if (f_open(&dfp, dst_path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) continue;
        UINT bw = 0;
        uint32_t data_end = OFF_MODS + (uint32_t)scnt * sizeof(ChunkMod);
        uint32_t end = data_end + 4;
        f_write(&dfp, s_page, end, &bw);
        f_close(&dfp);
    }
    f_closedir(&d);
}

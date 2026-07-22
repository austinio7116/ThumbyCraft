/*
 * ThumbyCraft - POSIX file-backed chunk store for the FunKey/RG Nano OPK.
 *
 * The engine wants a nonce-filtered per-region chunk store. On Linux we keep
 * one file per edited chunk under:
 *
 *   <root>/slot0/<cx>_<cz>.cnk ... <root>/slot3/<cx>_<cz>.cnk
 *   <root>/scratch/<cx>_<cz>.cnk
 */
#include "craft_chunk_store.h"

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define CS_MAGIC        0x344D4354u
#define CS_SECTOR_SIZE  4096u
#define REGION_COUNT    5

#define OFF_MAGIC      0
#define OFF_NONCE      4
#define OFF_CX         8
#define OFF_CZ         12
#define OFF_COUNT      16
#define OFF_MODS       20

static char     s_root[256] = "/mnt/FunKey/.thumbycraft/chunks";
static int      s_region = -1;
static uint32_t s_nonce;
static uint8_t  s_page[CS_SECTOR_SIZE];
static uint8_t  s_exists_bm[32];

void craft_funkey_chunk_store_set_root(const char *root) {
    if (!root || !root[0]) return;
    snprintf(s_root, sizeof s_root, "%s", root);
}

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

static void exists_clear_all(void) {
    memset(s_exists_bm, 0, sizeof s_exists_bm);
}

static bool parse_chunk_name(const char *name, int *out_cx, int *out_cz) {
    int cx = 0, cz = 0, n = 0;
    if (sscanf(name, "%d_%d.cnk%n", &cx, &cz, &n) >= 2 && name[n] == '\0') {
        if (out_cx) *out_cx = cx;
        if (out_cz) *out_cz = cz;
        return true;
    }
    return false;
}

static const char *region_name(int region) {
    static const char *const names[REGION_COUNT] = {
        "slot0", "slot1", "slot2", "slot3", "scratch",
    };
    if ((unsigned)region >= REGION_COUNT) return names[0];
    return names[region];
}

static void region_dir(char *out, size_t out_len, int region) {
    snprintf(out, out_len, "%s/%s", s_root, region_name(region));
}

static void chunk_path(char *out, size_t out_len, int region, int cx, int cz) {
    char dir[320];
    region_dir(dir, sizeof dir, region);
    snprintf(out, out_len, "%s/%d_%d.cnk", dir, cx, cz);
}

static void mkdir_p(const char *path) {
    char tmp[320];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0777);
            *p = '/';
        }
    }
    mkdir(tmp, 0777);
}

static void mkdir_region(int region) {
    char dir[320];
    region_dir(dir, sizeof dir, region);
    mkdir_p(dir);
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int32_t rd_i32(const uint8_t *p) {
    return (int32_t)rd32(p);
}

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void wr_i32(uint8_t *p, int32_t v) {
    wr32(p, (uint32_t)v);
}

static uint32_t crc32_calc(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int j = 0; j < 8; j++) {
            c = (c >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(c & 1));
        }
    }
    return ~c;
}

static bool validate_header(const uint8_t *p, uint32_t nonce,
                            int *out_cx, int *out_cz, int *out_count) {
    if (rd32(p + OFF_MAGIC) != CS_MAGIC) return false;
    if (rd32(p + OFF_NONCE) != nonce) return false;
    uint32_t cnt = rd32(p + OFF_COUNT) & 0xFFFFu;
    if (cnt > CHUNK_STORE_MAX_MODS_PER_CHUNK) return false;
    uint32_t data_end = OFF_MODS + cnt * sizeof(ChunkMod);
    if (data_end + 4 > CS_SECTOR_SIZE) return false;
    if (crc32_calc(p, data_end) != rd32(p + data_end)) return false;
    if (out_cx) *out_cx = rd_i32(p + OFF_CX);
    if (out_cz) *out_cz = rd_i32(p + OFF_CZ);
    if (out_count) *out_count = (int)cnt;
    return true;
}

static void build_sector(uint32_t nonce, int cx, int cz,
                         const ChunkMod *mods, int n, uint8_t *page) {
    memset(page, 0xFF, CS_SECTOR_SIZE);
    wr32(page + OFF_MAGIC, CS_MAGIC);
    wr32(page + OFF_NONCE, nonce);
    wr_i32(page + OFF_CX, cx);
    wr_i32(page + OFF_CZ, cz);
    wr32(page + OFF_COUNT, (uint32_t)n);
    for (int i = 0; i < n; i++) {
        page[OFF_MODS + i * 4 + 0] = mods[i].lx;
        page[OFF_MODS + i * 4 + 1] = mods[i].y;
        page[OFF_MODS + i * 4 + 2] = mods[i].lz;
        page[OFF_MODS + i * 4 + 3] = mods[i].blk;
    }
    uint32_t data_end = OFF_MODS + (uint32_t)n * sizeof(ChunkMod);
    wr32(page + data_end, crc32_calc(page, data_end));
}

void craft_chunk_store_bind(int region, uint32_t nonce) {
    if (region < 0 || region >= REGION_COUNT) return;
    s_region = region;
    s_nonce = nonce;
    exists_clear_all();

    char dir_path[320];
    region_dir(dir_path, sizeof dir_path, region);
    DIR *dir = opendir(dir_path);
    if (!dir) return;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        int cx = 0, cz = 0;
        if (parse_chunk_name(ent->d_name, &cx, &cz)) exists_set(cx, cz);
    }
    closedir(dir);
}

int craft_chunk_store_bound(void) {
    return s_region;
}

uint32_t craft_chunk_store_bound_nonce(void) {
    return s_nonce;
}

int craft_chunk_store_load(int chunk_x, int chunk_z,
                           ChunkMod *out, int max_entries) {
    if (s_region < 0) return 0;
    if (!exists_test(chunk_x, chunk_z)) return 0;

    char path[384];
    chunk_path(path, sizeof path, s_region, chunk_x, chunk_z);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t got = fread(s_page, 1, CS_SECTOR_SIZE, f);
    fclose(f);
    if (got < OFF_MODS) return 0;
    if (got < CS_SECTOR_SIZE) memset(s_page + got, 0xFF, CS_SECTOR_SIZE - got);

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

    char path[384];
    chunk_path(path, sizeof path, s_region, chunk_x, chunk_z);

    if (n == 0) {
        unlink(path);
        return true;
    }

    mkdir_region(s_region);
    build_sector(s_nonce, chunk_x, chunk_z, mods, n, s_page);
    exists_set(chunk_x, chunk_z);

    FILE *f = fopen(path, "wb");
    if (!f) return false;
    uint32_t end = OFF_MODS + (uint32_t)n * sizeof(ChunkMod) + 4;
    size_t put = fwrite(s_page, 1, end, f);
    bool ok = (put == end) && (fclose(f) == 0);
    return ok;
}

void craft_chunk_store_erase_region(int region) {
    if (region < 0 || region >= REGION_COUNT) return;

    char dir_path[320];
    region_dir(dir_path, sizeof dir_path, region);
    DIR *dir = opendir(dir_path);
    if (!dir) return;

    struct dirent *ent;
    char path[384];
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        snprintf(path, sizeof path, "%s/%s", dir_path, ent->d_name);
        unlink(path);
    }
    closedir(dir);
    if (region == s_region) exists_clear_all();
}

void craft_chunk_store_copy(int src_region, uint32_t src_nonce,
                            int dst_region, uint32_t dst_nonce) {
    if (src_region < 0 || src_region >= REGION_COUNT) return;
    if (dst_region < 0 || dst_region >= REGION_COUNT) return;
    if (src_region == dst_region && src_nonce == dst_nonce) return;

    char src_dir[320], src_path[384], dst_path[384];
    region_dir(src_dir, sizeof src_dir, src_region);
    DIR *dir = opendir(src_dir);
    if (!dir) return;
    mkdir_region(dst_region);

    struct dirent *ent;
    static ChunkMod mods[CHUNK_STORE_MAX_MODS_PER_CHUNK];
    while ((ent = readdir(dir)) != NULL) {
        int cx = 0, cz = 0;
        if (!parse_chunk_name(ent->d_name, &cx, &cz)) continue;

        snprintf(src_path, sizeof src_path, "%s/%s", src_dir, ent->d_name);
        FILE *sf = fopen(src_path, "rb");
        if (!sf) continue;
        size_t got = fread(s_page, 1, CS_SECTOR_SIZE, sf);
        fclose(sf);
        if (got < OFF_MODS) continue;
        if (got < CS_SECTOR_SIZE) memset(s_page + got, 0xFF, CS_SECTOR_SIZE - got);

        int scx = 0, scz = 0, count = 0;
        if (!validate_header(s_page, src_nonce, &scx, &scz, &count)) continue;
        for (int i = 0; i < count; i++) {
            mods[i].lx  = s_page[OFF_MODS + i * 4 + 0];
            mods[i].y   = s_page[OFF_MODS + i * 4 + 1];
            mods[i].lz  = s_page[OFF_MODS + i * 4 + 2];
            mods[i].blk = s_page[OFF_MODS + i * 4 + 3];
        }

        build_sector(dst_nonce, scx, scz, mods, count, s_page);
        chunk_path(dst_path, sizeof dst_path, dst_region, scx, scz);
        FILE *df = fopen(dst_path, "wb");
        if (!df) continue;
        uint32_t end = OFF_MODS + (uint32_t)count * sizeof(ChunkMod) + 4;
        fwrite(s_page, 1, end, df);
        fclose(df);
        if (dst_region == s_region) exists_set(scx, scz);
    }
    closedir(dir);
}

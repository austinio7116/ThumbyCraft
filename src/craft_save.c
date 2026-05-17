/*
 * ThumbyCraft — world persistence.
 *
 * Strategy: the terrain generator is a pure function of (x, y, z,
 * seed) (see craft_gen.h). To save we walk the world and emit the
 * (idx, block) pairs for every cell that disagrees with what the
 * generator would have produced. To load we regenerate the base
 * and apply the same deltas.
 *
 * This keeps saves tiny (a few KB) and — critically — avoids
 * holding a second 256 KB copy of the world in SRAM.
 */
#include "craft_save.h"
#include "craft_world.h"
#include "craft_gen.h"

#include <string.h>

/* --- CRC32 -------------------------------------------------------- */
static uint32_t crc32_byte(uint8_t b, uint32_t crc) {
    crc ^= b;
    for (int i = 0; i < 8; i++)
        crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1));
    return crc;
}
static uint32_t crc32(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = crc32_byte(p[i], c);
    return ~c;
}

/* --- Little-endian helpers ---------------------------------------- */
static void put32(uint8_t *p, uint32_t v) {
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}
static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void putf(uint8_t *p, float f) {
    uint32_t u; memcpy(&u, &f, 4); put32(p, u);
}
static float getf(const uint8_t *p) {
    uint32_t u = get32(p); float f; memcpy(&f, &u, 4); return f;
}

#define HEADER_SIZE (4 + 4 + 4 + 4 + 1 + CRAFT_HOTBAR_SLOTS + 12 + 4 + 4 + 4)

size_t craft_save_serialise(uint32_t seed,
                            const CraftPlayer *p,
                            uint8_t *out, size_t out_cap) {
    if (out_cap < HEADER_SIZE + 4) return 0;

    size_t off = HEADER_SIZE;
    uint32_t count = 0;
    for (int y = 0; y < CRAFT_WORLD_Y; y++) {
        for (int z = 0; z < CRAFT_WORLD_Z; z++) {
            for (int x = 0; x < CRAFT_WORLD_X; x++) {
                BlockId base = craft_gen_block_at(x, y, z, seed);
                BlockId cur  = craft_world_get(x, y, z);
                if (cur == base) continue;
                if (off + 4 + 4 > out_cap) return 0;  /* leave room for CRC */
                uint32_t idx = (uint32_t)(y * CRAFT_WORLD_Z + z) * CRAFT_WORLD_X
                             + (uint32_t)x;
                uint32_t pack = (idx & 0x00FFFFFFu) | ((uint32_t)cur << 24);
                put32(out + off, pack);
                off += 4;
                count++;
            }
        }
    }

    /* Header. */
    put32(out + 0,  CRAFT_SAVE_MAGIC);
    put32(out + 4,  CRAFT_SAVE_VERSION);
    put32(out + 8,  seed);
    put32(out + 12, count);
    out[16] = (uint8_t)p->hotbar_idx;
    for (int i = 0; i < CRAFT_HOTBAR_SLOTS; i++) out[17 + i] = (uint8_t)p->hotbar[i];
    int o = 17 + CRAFT_HOTBAR_SLOTS;
    putf(out + o + 0,  p->cam.pos.x);
    putf(out + o + 4,  p->cam.pos.y);
    putf(out + o + 8,  p->cam.pos.z);
    putf(out + o + 12, p->cam.yaw);
    putf(out + o + 16, p->cam.pitch);
    put32(out + o + 20, 0);

    uint32_t c = crc32(out, off);
    put32(out + off, c);
    off += 4;
    return off;
}

bool craft_save_deserialise(const uint8_t *in, size_t n,
                            uint32_t *out_seed, CraftPlayer *p) {
    if (n < HEADER_SIZE + 4) return false;
    if (get32(in + 0) != CRAFT_SAVE_MAGIC)   return false;
    if (get32(in + 4) != CRAFT_SAVE_VERSION) return false;
    uint32_t stored_crc = get32(in + n - 4);
    if (crc32(in, n - 4) != stored_crc)      return false;

    uint32_t seed  = get32(in + 8);
    uint32_t count = get32(in + 12);
    if (HEADER_SIZE + (size_t)count * 4 + 4 > n) return false;

    /* Re-load the current window from the seed so mods will apply
     * via the mod table when craft_world_load_around runs. The
     * existing save format predates infinite world — for now we just
     * load around (0,0) and the player respawns at the spawn point.
     * TODO: bump save version and persist player position + full mod
     * table on save/load. */
    craft_world_load_around(0, 0, seed);

    /* Apply deltas. */
    size_t off = HEADER_SIZE;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t pack = get32(in + off);
        off += 4;
        uint32_t idx = pack & 0x00FFFFFFu;
        uint8_t  blk = (uint8_t)(pack >> 24);
        if (idx < CRAFT_WORLD_VOXELS) {
            craft_world_blocks[idx] = blk;
        }
    }
    craft_world_dirty = 0;

    /* Hotbar + camera. */
    p->hotbar_idx = in[16];
    if (p->hotbar_idx >= CRAFT_HOTBAR_SLOTS) p->hotbar_idx = 0;
    for (int i = 0; i < CRAFT_HOTBAR_SLOTS; i++) p->hotbar[i] = in[17 + i];
    int o = 17 + CRAFT_HOTBAR_SLOTS;
    p->cam.pos.x = getf(in + o + 0);
    p->cam.pos.y = getf(in + o + 4);
    p->cam.pos.z = getf(in + o + 8);
    p->cam.yaw   = getf(in + o + 12);
    p->cam.pitch = getf(in + o + 16);
    if (out_seed) *out_seed = seed;
    return true;
}

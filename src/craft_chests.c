/*
 * ThumbyCraft — chest contents storage (impl).
 */
#include "craft_chests.h"
#include <string.h>

CraftChest craft_chests[CRAFT_MAX_CHESTS];

void craft_chests_init(void) {
    memset(craft_chests, 0, sizeof craft_chests);
}

CraftChest *craft_chest_find(int wx, int wy, int wz) {
    for (int i = 0; i < CRAFT_MAX_CHESTS; i++) {
        CraftChest *c = &craft_chests[i];
        if (!c->used) continue;
        if (c->wx == wx && c->wy == wy && c->wz == wz) return c;
    }
    return NULL;
}

CraftChest *craft_chest_at(int wx, int wy, int wz) {
    CraftChest *c = craft_chest_find(wx, wy, wz);
    if (c) return c;
    for (int i = 0; i < CRAFT_MAX_CHESTS; i++) {
        c = &craft_chests[i];
        if (c->used) continue;
        memset(c, 0, sizeof *c);
        c->used = true;
        c->wx = wx; c->wy = wy; c->wz = wz;
        return c;
    }
    return NULL;
}

void craft_chest_remove(int wx, int wy, int wz) {
    CraftChest *c = craft_chest_find(wx, wy, wz);
    if (c) memset(c, 0, sizeof *c);
}

/* --- Save blob serialisation ------------------------------------- *
 *
 * Fixed-size, little-endian byte encoding so the on-disk format is
 * stable across hosts (x86_64 / Cortex-M33). The struct itself can't
 * be memcpy'd because of compiler-chosen padding.
 *
 * Per entry (45 B):
 *   off  0: u8  used flag
 *   off  1: i32 wx (LE)
 *   off  5: i32 wy (LE)
 *   off  9: i32 wz (LE)
 *   off 13: 16 × (u8 blk, u8 n)
 */
static void put_u32_le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
static uint32_t get_u32_le(const uint8_t *p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

void craft_chests_serialise(uint8_t out[CRAFT_CHESTS_BLOB_BYTES]) {
    for (int i = 0; i < CRAFT_MAX_CHESTS; i++) {
        const CraftChest *c = &craft_chests[i];
        uint8_t *p = out + i * CRAFT_CHESTS_BLOB_PER_ENTRY;
        p[0] = c->used ? 1u : 0u;
        put_u32_le(p + 1, (uint32_t)c->wx);
        put_u32_le(p + 5, (uint32_t)c->wy);
        put_u32_le(p + 9, (uint32_t)c->wz);
        for (int s = 0; s < CRAFT_CHEST_SLOTS; s++) {
            p[13 + s * 2 + 0] = c->slots[s].blk;
            p[13 + s * 2 + 1] = c->slots[s].n;
        }
    }
}

void craft_chests_deserialise(const uint8_t in[CRAFT_CHESTS_BLOB_BYTES]) {
    for (int i = 0; i < CRAFT_MAX_CHESTS; i++) {
        CraftChest *c = &craft_chests[i];
        const uint8_t *p = in + i * CRAFT_CHESTS_BLOB_PER_ENTRY;
        memset(c, 0, sizeof *c);
        c->used = (p[0] != 0);
        c->wx = (int32_t)get_u32_le(p + 1);
        c->wy = (int32_t)get_u32_le(p + 5);
        c->wz = (int32_t)get_u32_le(p + 9);
        for (int s = 0; s < CRAFT_CHEST_SLOTS; s++) {
            c->slots[s].blk = p[13 + s * 2 + 0];
            c->slots[s].n   = p[13 + s * 2 + 1];
        }
    }
}

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
#include "craft_main.h"
#include "craft_world.h"
#include "craft_gen.h"
#include "craft_chunk_store.h"
#include "craft_chests.h"
#include "craft_furnace.h"
#include "craft_torches.h"

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

/* Fixed-size record (v2 format).
 *
 *   off  size  field
 *     0     4  magic
 *     4     4  version
 *     8     4  seed
 *    12     1  mode
 *    13     1  hp
 *    14     1  hotbar_idx
 *    15     1  _pad
 *    16  HOTBAR  hotbar[]                   (= 8 bytes)
 *   24      4  cam.pos.x
 *   28      4  cam.pos.y
 *   32      4  cam.pos.z
 *   36      4  cam.yaw
 *   40      4  cam.pitch
 *   44   N*4  inventory[BLK_COUNT]          (4N bytes)
 *  END      4  crc32
 *
 * BLK_COUNT = 26 today → record size = 44 + 26*4 + 4 = 152 bytes.
 * Comfortably fits in any sane buffer. */
#define HDR_OFF_MAGIC         0
#define HDR_OFF_VERSION       4
#define HDR_OFF_SEED          8
#define HDR_OFF_CHUNKS_NONCE  12   /* per-world chunk-store nonce */
#define HDR_OFF_MODE          16
#define HDR_OFF_HP            17
#define HDR_OFF_HOTBARIDX     18
#define HDR_OFF_PAD           19
#define HDR_OFF_HOTBAR        20
#define HDR_OFF_CAM           (HDR_OFF_HOTBAR + CRAFT_HOTBAR_SLOTS)
#define HDR_OFF_INVENTORY     (HDR_OFF_CAM + 5 * 4)
#define HDR_OFF_CHESTS        (HDR_OFF_INVENTORY + BLK_COUNT * 4)
#define HDR_OFF_FURNACES      (HDR_OFF_CHESTS + CRAFT_CHESTS_BLOB_BYTES)
#define HDR_OFF_ORIENTS       (HDR_OFF_FURNACES + CRAFT_FURNACES_BLOB_BYTES)
/* v5 record terminator (no orient blob): CRC sits immediately after
 * the furnace blob — same byte position the old HDR_OFF_CRC pointed
 * at. v6 grows the record by the orient blob (variable-size). */
#define HDR_OFF_CRC_V5        HDR_OFF_ORIENTS
#define SAVE_RECORD_BYTES_V5  (HDR_OFF_CRC_V5 + 4)
#define SAVE_RECORD_MIN_BYTES SAVE_RECORD_BYTES_V5
#define SAVE_RECORD_MAX_BYTES (HDR_OFF_ORIENTS + CRAFT_ORIENTS_BLOB_MAX_BYTES + 4)

size_t craft_save_serialise(uint32_t seed, uint32_t chunks_nonce,
                            uint8_t autosave_level,
                            const CraftPlayer *p,
                            uint8_t *out, size_t out_cap) {
    if (out_cap < SAVE_RECORD_MAX_BYTES) return 0;

    put32(out + HDR_OFF_MAGIC,         CRAFT_SAVE_MAGIC);
    put32(out + HDR_OFF_VERSION,       CRAFT_SAVE_VERSION);
    put32(out + HDR_OFF_SEED,          seed);
    put32(out + HDR_OFF_CHUNKS_NONCE,  chunks_nonce);
    out[HDR_OFF_MODE]      = (uint8_t)p->mode;
    out[HDR_OFF_HP]        = (uint8_t)p->hp;
    out[HDR_OFF_HOTBARIDX] = (uint8_t)p->hotbar_idx;
    /* PAD byte packs two fields into 4-bit nibbles:
     *   low  nibble = autosave_level (1..4)
     *   high nibble = control scheme (1..4)
     * Old saves that predate the scheme field have high nibble = 0,
     * which deserialise() maps to CRAFT_SCHEME_CLASSIC (the original
     * input layout). */
    {
        uint8_t scheme  = (uint8_t)craft_main_scheme();
        uint8_t pad     = (uint8_t)((autosave_level & 0x0F) |
                                    ((scheme & 0x0F) << 4));
        out[HDR_OFF_PAD] = pad;
    }
    for (int i = 0; i < CRAFT_HOTBAR_SLOTS; i++) {
        out[HDR_OFF_HOTBAR + i] = (uint8_t)p->hotbar[i];
    }
    putf(out + HDR_OFF_CAM +  0, p->cam.pos.x);
    putf(out + HDR_OFF_CAM +  4, p->cam.pos.y);
    putf(out + HDR_OFF_CAM +  8, p->cam.pos.z);
    putf(out + HDR_OFF_CAM + 12, p->cam.yaw);
    putf(out + HDR_OFF_CAM + 16, p->cam.pitch);
    for (int i = 0; i < BLK_COUNT; i++) {
        put32(out + HDR_OFF_INVENTORY + i * 4, (uint32_t)p->inventory[i]);
    }
    /* Active chest + furnace tables (was lost on load before v5). */
    craft_chests_serialise(  out + HDR_OFF_CHESTS);
    craft_furnaces_serialise(out + HDR_OFF_FURNACES);
    /* Mechanical-block orient hash (new in v6 — was lost on load,
     * caused levers/pistons/doors to default to floor-mount). */
    size_t orient_bytes = craft_torches_orient_serialise(
        out + HDR_OFF_ORIENTS, out_cap - HDR_OFF_ORIENTS - 4);
    size_t crc_off = HDR_OFF_ORIENTS + orient_bytes;
    uint32_t c = crc32(out, crc_off);
    put32(out + crc_off, c);
    return crc_off + 4;
}

bool craft_save_deserialise(const uint8_t *in, size_t n,
                            uint32_t *out_seed, CraftPlayer *p) {
    if (n < SAVE_RECORD_MIN_BYTES)                         return false;
    if (get32(in + HDR_OFF_MAGIC) != CRAFT_SAVE_MAGIC)     return false;
    uint32_t version = get32(in + HDR_OFF_VERSION);
    /* Dual-read across v5..v7:
     *   v5 — inventory_len = 59, no orient blob.
     *   v6 — inventory_len = 59, orient blob present.
     *   v7 — inventory_len = BLK_COUNT (64), orient blob present.
     *
     * Inventory length determines every downstream offset, so we have
     * to compute them at runtime per version rather than at compile
     * time with HDR_OFF_CHESTS etc. */
    bool has_orient;
    int  inv_len;
    if (version == CRAFT_SAVE_VERSION_V5) {
        inv_len = CRAFT_SAVE_INVENTORY_LEN_V6;   /* v5 used the same count */
        has_orient = false;
    } else if (version == CRAFT_SAVE_VERSION_V6) {
        inv_len = CRAFT_SAVE_INVENTORY_LEN_V6;
        has_orient = true;
    } else if (version == CRAFT_SAVE_VERSION_V7) {
        inv_len = CRAFT_SAVE_INVENTORY_LEN_V7;
        has_orient = true;
    } else if (version == CRAFT_SAVE_VERSION_V8) {
        inv_len = CRAFT_SAVE_INVENTORY_LEN_V8;
        has_orient = true;
    } else if (version == CRAFT_SAVE_VERSION) {
        inv_len = BLK_COUNT;
        has_orient = true;
    } else {
        return false;
    }
    size_t off_chests   = HDR_OFF_INVENTORY + (size_t)inv_len * 4u;
    size_t off_furnaces = off_chests + CRAFT_CHESTS_BLOB_BYTES;
    size_t off_orients  = off_furnaces + CRAFT_FURNACES_BLOB_BYTES;
    size_t crc_off;
    size_t orient_bytes = 0;
    if (!has_orient) {
        crc_off = off_orients;
    } else {
        if (n < off_orients + 2 + 4) return false;
        uint16_t orient_count = (uint16_t)in[off_orients] |
                                ((uint16_t)in[off_orients + 1] << 8);
        if (orient_count > 256) return false;
        orient_bytes = 2 + (size_t)orient_count * CRAFT_ORIENTS_BLOB_PER_ENTRY;
        crc_off = off_orients + orient_bytes;
        if (n < crc_off + 4) return false;
    }
    uint32_t stored_crc = get32(in + crc_off);
    if (crc32(in, crc_off) != stored_crc)                  return false;

    uint32_t seed = get32(in + HDR_OFF_SEED);

    /* Chunk store binding is the caller's responsibility — it knows
     * which save slot this blob came from. craft_main_load wires up
     * craft_chunk_store_bind(slot) before calling deserialise. */

    /* Restore player state. */
    p->mode       = (CraftGameMode)in[HDR_OFF_MODE];
    p->hp         = in[HDR_OFF_HP];
    p->hotbar_idx = in[HDR_OFF_HOTBARIDX];
    if (p->hotbar_idx >= CRAFT_HOTBAR_SLOTS) p->hotbar_idx = 0;
    /* Restore the persisted autosave level (low nibble of PAD) +
     * control scheme (high nibble of PAD). Pre-scheme saves have a
     * zero high nibble — fall back to CRAFT_SCHEME_CLASSIC. Pre-
     * autosave saves have a zero low nibble — fall back to 1 (off). */
    {
        uint8_t pad    = in[HDR_OFF_PAD];
        uint8_t lv     = pad & 0x0F;
        uint8_t scheme = (uint8_t)((pad >> 4) & 0x0F);
        if (lv < 1 || lv > 4) lv = 1;
        if (scheme < CRAFT_SCHEME_MIN || scheme > CRAFT_SCHEME_MAX)
            scheme = CRAFT_SCHEME_CLASSIC;
        craft_main_set_autosave_level(lv);
        craft_main_set_scheme(scheme);
    }
    for (int i = 0; i < CRAFT_HOTBAR_SLOTS; i++) {
        p->hotbar[i] = (BlockId)in[HDR_OFF_HOTBAR + i];
    }
    p->cam.pos.x = getf(in + HDR_OFF_CAM +  0);
    p->cam.pos.y = getf(in + HDR_OFF_CAM +  4);
    p->cam.pos.z = getf(in + HDR_OFF_CAM +  8);
    p->cam.yaw   = getf(in + HDR_OFF_CAM + 12);
    p->cam.pitch = getf(in + HDR_OFF_CAM + 16);
    for (int i = 0; i < inv_len; i++) {
        p->inventory[i] = (int)get32(in + HDR_OFF_INVENTORY + i * 4);
    }
    /* Zero any BlockIds that didn't exist in the older save format. */
    for (int i = inv_len; i < BLK_COUNT; i++) {
        p->inventory[i] = 0;
    }

    /* Restore the chest + furnace tables (new in v5). Each fully
     * overwrites the SRAM table — any stale entries from the
     * previous world get cleared. */
    craft_chests_deserialise(  in + off_chests);
    craft_furnaces_deserialise(in + off_furnaces);
    /* Orient table (new in v6). v5 saves get an empty table → blocks
     * come back with the default FACE_PY mount (existing v5 behaviour
     * — we don't have the data to do better). */
    if (has_orient) {
        (void)craft_torches_orient_deserialise(in + off_orients,
                                               orient_bytes);
    } else {
        /* v5: wipe so a stale orient table from a previous load
         * doesn't bleed in. */
        (void)craft_torches_orient_deserialise((const uint8_t *)"\x00\x00", 2);
    }

    /* World comes back via the chunk store — load_around regenerates
     * the procedural terrain around the player's restored position
     * and restore_window pulls in any persisted mods. Persisted
     * settled-pool L=0 cells survive intact, since the water tick
     * skips them entirely; only L>=1 flow state is transient. */
    craft_world_load_around((int)p->cam.pos.x, (int)p->cam.pos.z, seed);

    /* Reset transient state that doesn't survive across worlds:
     *  - velocity / fall tracking — stale `fall_peak_y` or `vel.y`
     *    from the pre-save tick would otherwise trigger fall damage
     *    or "you've been falling" weirdness on the first tick.
     *  - damage cooldown + invuln flash — a damaging hit just before
     *    save shouldn't grant invuln on reload.
     *  - mob + arrow + drop pools — any hostile mob lingering in the
     *    pool from current play (skeleton arrow, slime behind a
     *    wall) would otherwise keep dealing melee damage from its
     *    pre-save position right next to the loaded player. The
     *    HUD never shows them because they're behind a wall, so the
     *    player just bleeds out in apparent daylight. */
    p->vel               = v3(0, 0, 0);
    p->fall_peak_y       = p->cam.pos.y;
    p->on_ground         = false;
    p->damage_cooldown   = 0.0f;
    p->damage_flash      = 0.0f;
    p->no_damage_t       = 0.0f;
    p->regen_acc         = 0.0f;
    p->respawn_timer     = 0.0f;
    p->spawn_point       = p->cam.pos;
    extern void craft_mobs_clear_all(void);
    craft_mobs_clear_all();

    if (out_seed) *out_seed = seed;
    return true;
}

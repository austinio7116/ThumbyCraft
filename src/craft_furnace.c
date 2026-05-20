/*
 * ThumbyCraft — furnace state + smelting tick (impl).
 */
#include "craft_furnace.h"
#include <string.h>
#include <stdint.h>

CraftFurnace craft_furnaces[CRAFT_MAX_FURNACES];

void craft_furnace_init(void) {
    memset(craft_furnaces, 0, sizeof craft_furnaces);
}

CraftFurnace *craft_furnace_find(int wx, int wy, int wz) {
    for (int i = 0; i < CRAFT_MAX_FURNACES; i++) {
        CraftFurnace *f = &craft_furnaces[i];
        if (!f->used) continue;
        if (f->wx == wx && f->wy == wy && f->wz == wz) return f;
    }
    return NULL;
}

CraftFurnace *craft_furnace_at(int wx, int wy, int wz) {
    CraftFurnace *f = craft_furnace_find(wx, wy, wz);
    if (f) return f;
    /* First touch — claim a free slot. */
    for (int i = 0; i < CRAFT_MAX_FURNACES; i++) {
        f = &craft_furnaces[i];
        if (f->used) continue;
        memset(f, 0, sizeof *f);
        f->used = true;
        f->wx = wx; f->wy = wy; f->wz = wz;
        return f;
    }
    return NULL;     /* table full — caller falls back to read-only */
}

void craft_furnace_remove(int wx, int wy, int wz) {
    CraftFurnace *f = craft_furnace_find(wx, wy, wz);
    if (f) memset(f, 0, sizeof *f);
}

bool craft_furnace_is_smeltable(BlockId b) {
    return b == BLK_IRON_ORE     || b == BLK_SAND     || b == BLK_COBBLE   ||
           b == BLK_SILVER_ORE   || b == BLK_GOLD_ORE ||
           b == BLK_DIAMOND_ORE  || b == BLK_REDSTONE_ORE;
}

BlockId craft_furnace_smelt_output(BlockId b) {
    switch (b) {
        case BLK_IRON_ORE:     return BLK_IRON_INGOT;
        case BLK_SAND:         return BLK_GLASS;
        case BLK_COBBLE:       return BLK_STONE;
        case BLK_SILVER_ORE:   return BLK_SILVER_INGOT;
        case BLK_GOLD_ORE:     return BLK_GOLD_INGOT;
        case BLK_DIAMOND_ORE:  return BLK_DIAMOND;
        case BLK_REDSTONE_ORE: return BLK_REDSTONE;
        default:               return BLK_AIR;
    }
}

float craft_furnace_fuel_time(BlockId b) {
    switch (b) {
        case BLK_COAL_ORE: return 80.0f;
        case BLK_WOOD:     return 15.0f;
        case BLK_PLANK:    return 15.0f;
        case BLK_STICK:    return 5.0f;
        default:           return 0.0f;
    }
}

void craft_furnace_tick(float dt) {
    if (dt > 0.1f) dt = 0.1f;
    for (int i = 0; i < CRAFT_MAX_FURNACES; i++) {
        CraftFurnace *f = &craft_furnaces[i];
        if (!f->used) continue;

        /* If fuel is burning, count it down. */
        if (f->fuel_remaining_t > 0.0f) {
            f->fuel_remaining_t -= dt;
            if (f->fuel_remaining_t < 0.0f) f->fuel_remaining_t = 0.0f;
        }

        /* What would we be smelting? */
        bool has_input = f->input_n > 0 && craft_furnace_is_smeltable(f->input_blk);
        BlockId target_out = has_input ? craft_furnace_smelt_output(f->input_blk) : BLK_AIR;
        /* Output slot can accept if empty OR same id with room. We
         * cap stacks at 64 to avoid silly accumulation. */
        bool output_room = has_input && (
            (f->output_n == 0) ||
            (f->output_blk == target_out && f->output_n < 64)
        );

        /* Need fuel if smelting and the burn just ran out. */
        if (has_input && output_room && f->fuel_remaining_t <= 0.0f) {
            if (f->fuel_n > 0 && craft_furnace_fuel_time(f->fuel_blk) > 0.0f) {
                f->fuel_remaining_t = craft_furnace_fuel_time(f->fuel_blk);
                f->fuel_n--;
                if (f->fuel_n == 0) f->fuel_blk = BLK_AIR;
            }
        }

        /* If actively burning AND a valid smelt is in progress, advance
         * the smelt timer. */
        bool burning = f->fuel_remaining_t > 0.0f;
        if (burning && has_input && output_room) {
            f->smelt_t += dt;
            if (f->smelt_t >= CRAFT_FURNACE_SMELT_TIME) {
                f->smelt_t -= CRAFT_FURNACE_SMELT_TIME;
                f->input_n--;
                if (f->input_n == 0) f->input_blk = BLK_AIR;
                if (f->output_n == 0) f->output_blk = target_out;
                f->output_n++;
            }
        } else {
            /* Nothing to smelt right now — reset progress to avoid
             * "almost done" persisting through an empty period. */
            f->smelt_t = 0.0f;
        }
    }
}

/* --- Save blob serialisation ------------------------------------- *
 *
 * Per entry (27 B):
 *   off  0: u8  used flag
 *   off  1: i32 wx (LE)
 *   off  5: i32 wy (LE)
 *   off  9: i32 wz (LE)
 *   off 13: u8 input_blk, u8 input_n, u8 fuel_blk, u8 fuel_n,
 *           u8 output_blk, u8 output_n
 *   off 19: f32 smelt_t (LE bit pattern, IEEE 754)
 *   off 23: f32 fuel_remaining_t (LE bit pattern)
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
static void put_f32_le(uint8_t *p, float f) {
    union { float f; uint32_t u; } cvt;
    cvt.f = f;
    put_u32_le(p, cvt.u);
}
static float get_f32_le(const uint8_t *p) {
    union { float f; uint32_t u; } cvt;
    cvt.u = get_u32_le(p);
    return cvt.f;
}

void craft_furnaces_serialise(uint8_t out[CRAFT_FURNACES_BLOB_BYTES]) {
    for (int i = 0; i < CRAFT_MAX_FURNACES; i++) {
        const CraftFurnace *f = &craft_furnaces[i];
        uint8_t *p = out + i * CRAFT_FURNACES_BLOB_PER_ENTRY;
        p[0] = f->used ? 1u : 0u;
        put_u32_le(p + 1, (uint32_t)f->wx);
        put_u32_le(p + 5, (uint32_t)f->wy);
        put_u32_le(p + 9, (uint32_t)f->wz);
        p[13] = f->input_blk;
        p[14] = f->input_n;
        p[15] = f->fuel_blk;
        p[16] = f->fuel_n;
        p[17] = f->output_blk;
        p[18] = f->output_n;
        put_f32_le(p + 19, f->smelt_t);
        put_f32_le(p + 23, f->fuel_remaining_t);
    }
}

void craft_furnaces_deserialise(const uint8_t in[CRAFT_FURNACES_BLOB_BYTES]) {
    for (int i = 0; i < CRAFT_MAX_FURNACES; i++) {
        CraftFurnace *f = &craft_furnaces[i];
        const uint8_t *p = in + i * CRAFT_FURNACES_BLOB_PER_ENTRY;
        memset(f, 0, sizeof *f);
        f->used = (p[0] != 0);
        f->wx = (int32_t)get_u32_le(p + 1);
        f->wy = (int32_t)get_u32_le(p + 5);
        f->wz = (int32_t)get_u32_le(p + 9);
        f->input_blk  = p[13];
        f->input_n    = p[14];
        f->fuel_blk   = p[15];
        f->fuel_n     = p[16];
        f->output_blk = p[17];
        f->output_n   = p[18];
        f->smelt_t          = get_f32_le(p + 19);
        f->fuel_remaining_t = get_f32_le(p + 23);
    }
}

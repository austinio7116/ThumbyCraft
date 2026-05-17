/*
 * ThumbyCraft — furnace state + smelting tick (impl).
 */
#include "craft_furnace.h"
#include <string.h>

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
    return b == BLK_IRON_ORE || b == BLK_SAND || b == BLK_COBBLE;
}

BlockId craft_furnace_smelt_output(BlockId b) {
    switch (b) {
        case BLK_IRON_ORE: return BLK_IRON_INGOT;
        case BLK_SAND:     return BLK_GLASS;
        case BLK_COBBLE:   return BLK_STONE;
        default:           return BLK_AIR;
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

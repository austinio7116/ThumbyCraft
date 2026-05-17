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

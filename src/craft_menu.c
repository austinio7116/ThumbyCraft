/*
 * ThumbyCraft — pause menu.
 *
 * Six items in a vertical list. D-pad U/D moves selection (with
 * wrap), A confirms, B or MENU closes. Action items return a
 * CRAFT_MENU_RESULT_* code so the caller can execute the heavy
 * lifting (save blob serialisation, etc) outside the menu's scope.
 */
#include "craft_menu.h"
#include "craft_font.h"
#include "craft_blocks.h"
#include "craft_audio.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    const char     *label;
    CraftMenuResult result;
    bool            close_on_confirm;
} MenuItem;

static const MenuItem ITEMS[] = {
    { "Resume",        CRAFT_MENU_RESULT_RESUME,     true  },
    { "Inventory",     CRAFT_MENU_RESULT_INVENTORY,  true  },
    { "Save world",    CRAFT_MENU_RESULT_SAVE,       true  },
    { "Load world",    CRAFT_MENU_RESULT_LOAD,       true  },
    { "Game mode",     CRAFT_MENU_RESULT_GAME_MODE,  false },
    { "Toggle fly",    CRAFT_MENU_RESULT_FLY_TOGGLE, true  },
    { "Invert Y",      CRAFT_MENU_RESULT_INVERT_Y,   false },
    { "Music",         CRAFT_MENU_RESULT_MUSIC,      false },
    { "New world",     CRAFT_MENU_RESULT_NEW_WORLD,  true  },
    { "Settings",      CRAFT_MENU_RESULT_SETTINGS,   true  },
};
#define ITEM_COUNT ((int)(sizeof(ITEMS) / sizeof(ITEMS[0])))

/* Menu has two pages: the main pause list, and the inventory grid
 * reached via the "Inventory" item. B from inventory returns to main;
 * A on a block assigns it to the active hotbar slot and closes the
 * whole menu so the player goes straight back to building. */
typedef enum { PAGE_MAIN = 0, PAGE_INVENTORY = 1 } MenuPage;
static MenuPage s_page;
static bool  s_open;
static int   s_sel;
static int   s_inv_sel;
static bool  s_input_prev_a;        /* edge filter so first A press
                                       doesn't confirm immediately on
                                       menu open */
static bool  s_input_prev_b;
static bool  s_input_prev_menu;
static bool  s_dpad_was_pressed;
static float s_dpad_repeat_t;

void craft_menu_open(const CraftInput *in) {
    s_open = true;
    s_page = PAGE_MAIN;
    s_sel  = 0;
    s_inv_sel = 0;
    /* Seed prev-state from the live input — otherwise the next tick
     * misreads a still-being-released button as a fresh edge and
     * closes the menu immediately. The menu opens on MENU *release*,
     * so in->menu is typically false here, but we don't assume. */
    s_input_prev_a    = in ? in->a    : false;
    s_input_prev_b    = in ? in->b    : false;
    s_input_prev_menu = in ? in->menu : false;
    s_dpad_was_pressed = in ?
        (in->up || in->down || in->left || in->right) : false;
}
void craft_menu_close(void) { s_open = false; }
bool craft_menu_is_open(void) { return s_open; }

/* D-pad U/D move repeats — slow auto-repeat. */
#define DPAD_INITIAL_DELAY 0.30f
#define DPAD_REPEAT       0.12f

/* --- Inventory page -------------------------------------------- *
 * 4 cols × 3 rows grid of blocks the player can put on the hotbar.
 * Selection wraps in both axes. A assigns to active hotbar slot and
 * closes the entire menu; B returns to the main page. */

#define INV_COLS 4
#define INV_ROWS 3
#define INV_CELLS (INV_COLS * INV_ROWS)

static const BlockId inv_blocks[INV_CELLS] = {
    BLK_GRASS,  BLK_DIRT,   BLK_STONE,  BLK_COBBLE,
    BLK_SAND,   BLK_WOOD,   BLK_PLANK,  BLK_LEAVES,
    BLK_GLASS,  BLK_WATER,  BLK_AIR,    BLK_AIR,
};

static CraftMenuResult tick_main_page(const CraftInput *in) {
    bool dpad_now = in->up || in->down;
    if (dpad_now && !s_dpad_was_pressed) {
        if (in->up)   s_sel = (s_sel + ITEM_COUNT - 1) % ITEM_COUNT;
        if (in->down) s_sel = (s_sel + 1) % ITEM_COUNT;
        s_dpad_repeat_t = DPAD_INITIAL_DELAY;
    } else if (dpad_now) {
        s_dpad_repeat_t -= 1.0f / 30.0f;
        if (s_dpad_repeat_t <= 0.0f) {
            if (in->up)   s_sel = (s_sel + ITEM_COUNT - 1) % ITEM_COUNT;
            if (in->down) s_sel = (s_sel + 1) % ITEM_COUNT;
            s_dpad_repeat_t = DPAD_REPEAT;
        }
    }
    s_dpad_was_pressed = dpad_now;

    bool b_just_released    = !in->b    && s_input_prev_b;
    bool menu_just_released = !in->menu && s_input_prev_menu;
    s_input_prev_a    = in->a;
    s_input_prev_b    = in->b;
    s_input_prev_menu = in->menu;

    if (b_just_released || menu_just_released) {
        s_open = false;
        return CRAFT_MENU_RESULT_RESUME;
    }
    if (in->a_pressed) {
        const MenuItem *item = &ITEMS[s_sel];
        if (item->result == CRAFT_MENU_RESULT_INVENTORY) {
            /* Switch to inventory page rather than closing. */
            s_page = PAGE_INVENTORY;
            s_dpad_was_pressed = in->up || in->down || in->left || in->right;
            return CRAFT_MENU_RESULT_NONE;
        }
        if (item->close_on_confirm) s_open = false;
        return item->result;
    }
    return CRAFT_MENU_RESULT_NONE;
}

static CraftMenuResult tick_inventory_page(const CraftInput *in,
                                           CraftPlayer *pmut) {
    bool dpad_now = in->up || in->down || in->left || in->right;
    if (dpad_now && !s_dpad_was_pressed) {
        if (in->left)  s_inv_sel = (s_inv_sel + INV_CELLS - 1) % INV_CELLS;
        if (in->right) s_inv_sel = (s_inv_sel + 1) % INV_CELLS;
        if (in->up)    s_inv_sel = (s_inv_sel + INV_CELLS - INV_COLS) % INV_CELLS;
        if (in->down)  s_inv_sel = (s_inv_sel + INV_COLS) % INV_CELLS;
        s_dpad_repeat_t = DPAD_INITIAL_DELAY;
    } else if (dpad_now) {
        s_dpad_repeat_t -= 1.0f / 30.0f;
        if (s_dpad_repeat_t <= 0.0f) {
            if (in->left)  s_inv_sel = (s_inv_sel + INV_CELLS - 1) % INV_CELLS;
            if (in->right) s_inv_sel = (s_inv_sel + 1) % INV_CELLS;
            if (in->up)    s_inv_sel = (s_inv_sel + INV_CELLS - INV_COLS) % INV_CELLS;
            if (in->down)  s_inv_sel = (s_inv_sel + INV_COLS) % INV_CELLS;
            s_dpad_repeat_t = DPAD_REPEAT;
        }
    }
    s_dpad_was_pressed = dpad_now;

    bool b_just_released    = !in->b    && s_input_prev_b;
    bool menu_just_released = !in->menu && s_input_prev_menu;
    s_input_prev_a    = in->a;
    s_input_prev_b    = in->b;
    s_input_prev_menu = in->menu;

    if (menu_just_released) {
        /* Close the whole menu when MENU is hit again. */
        s_open = false;
        return CRAFT_MENU_RESULT_RESUME;
    }
    if (b_just_released) {
        /* Back to main page. */
        s_page = PAGE_MAIN;
        s_dpad_was_pressed = in->up || in->down;
        return CRAFT_MENU_RESULT_NONE;
    }
    if (in->a_pressed) {
        BlockId b = inv_blocks[s_inv_sel];
        if (b != BLK_AIR) {
            pmut->hotbar[pmut->hotbar_idx] = b;
            craft_menu_toast(craft_block_name(b));
            s_open = false;
            return CRAFT_MENU_RESULT_RESUME;
        }
    }
    return CRAFT_MENU_RESULT_NONE;
}

CraftMenuResult craft_menu_tick(const CraftInput *in, const CraftPlayer *p) {
    if (!s_open) return CRAFT_MENU_RESULT_NONE;
    /* Inventory page mutates the player; cast away const carefully —
     * the engine owns that pointer and is fine with us writing to it. */
    if (s_page == PAGE_INVENTORY)
        return tick_inventory_page(in, (CraftPlayer *)p);
    return tick_main_page(in);
}

static void rect(uint16_t *fb, int x, int y, int w, int h, uint16_t c) {
    for (int yy = y; yy < y + h && yy < CRAFT_FB_H; yy++)
        for (int xx = x; xx < x + w && xx < CRAFT_FB_W; xx++)
            if (xx >= 0 && yy >= 0) fb[yy * CRAFT_FB_W + xx] = c;
}

static void darken_bg(uint16_t *fb) {
    for (int i = 0; i < CRAFT_FB_W * CRAFT_FB_H; i++) {
        uint16_t c = fb[i];
        int r = ((c >> 11) & 0x1F) / 3;
        int g = ((c >>  5) & 0x3F) / 3;
        int b = ( c        & 0x1F) / 3;
        fb[i] = (uint16_t)((r << 11) | (g << 5) | b);
    }
}

static void draw_main_page(uint16_t *fb, const CraftPlayer *p) {
    int panel_w = 100, panel_h = 110;
    int x0 = (CRAFT_FB_W - panel_w) / 2;
    int y0 = (CRAFT_FB_H - panel_h) / 2;
    rect(fb, x0, y0, panel_w, panel_h, rgb565(30, 30, 40));
    rect(fb, x0,             y0, panel_w, 1, rgb565(150, 150, 180));
    rect(fb, x0,             y0 + panel_h - 1, panel_w, 1, rgb565(150, 150, 180));
    rect(fb, x0,             y0, 1, panel_h, rgb565(150, 150, 180));
    rect(fb, x0 + panel_w-1, y0, 1, panel_h, rgb565(150, 150, 180));

    const char *title = "ThumbyCraft";
    int tw = craft_font_width(title);
    craft_font_draw(fb, title, x0 + (panel_w - tw) / 2, y0 + 4, 0xFFFF);
    rect(fb, x0 + 6, y0 + 11, panel_w - 12, 1, rgb565(80, 80, 100));

    int item_y = y0 + 16;
    for (int i = 0; i < ITEM_COUNT; i++) {
        const char *label = ITEMS[i].label;
        bool is_sel = (i == s_sel);
        if (is_sel) {
            rect(fb, x0 + 4, item_y - 1, panel_w - 8, 8, rgb565(70, 100, 160));
        }
        craft_font_draw(fb, is_sel ? ">" : " ", x0 + 6, item_y, 0xFFFF);
        craft_font_draw(fb, label,           x0 + 14, item_y,
                        is_sel ? 0xFFFF : rgb565(180, 180, 200));
        if (ITEMS[i].result == CRAFT_MENU_RESULT_FLY_TOGGLE) {
            const char *st = p->fly_mode ? "ON" : "OFF";
            int sw = craft_font_width(st);
            craft_font_draw(fb, st, x0 + panel_w - sw - 6, item_y,
                            p->fly_mode ? rgb565(120, 220, 255)
                                        : rgb565(120, 120, 130));
        } else if (ITEMS[i].result == CRAFT_MENU_RESULT_INVERT_Y) {
            const char *st = p->invert_y ? "ON" : "OFF";
            int sw = craft_font_width(st);
            craft_font_draw(fb, st, x0 + panel_w - sw - 6, item_y,
                            p->invert_y ? rgb565(120, 220, 255)
                                        : rgb565(120, 120, 130));
        } else if (ITEMS[i].result == CRAFT_MENU_RESULT_MUSIC) {
            bool on = craft_audio_music_is_enabled();
            const char *st = on ? "ON" : "OFF";
            int sw = craft_font_width(st);
            craft_font_draw(fb, st, x0 + panel_w - sw - 6, item_y,
                            on ? rgb565(120, 220, 255)
                               : rgb565(120, 120, 130));
        } else if (ITEMS[i].result == CRAFT_MENU_RESULT_GAME_MODE) {
            const char *st = (p->mode == CRAFT_MODE_SURVIVAL) ? "Survival" : "Creative";
            int sw = craft_font_width(st);
            craft_font_draw(fb, st, x0 + panel_w - sw - 6, item_y,
                            (p->mode == CRAFT_MODE_SURVIVAL)
                                ? rgb565(255, 140, 100)
                                : rgb565(140, 200, 255));
        }
        item_y += 10;
    }
}

/* Draw a chunky downscaled preview of a block's side texture. */
static void block_swatch_at(uint16_t *fb, int x, int y, int size, BlockId blk) {
    extern const uint16_t *craft_block_texture(BlockId, Face);
    const uint16_t *tex = craft_block_texture(blk, FACE_PZ);
    for (int dy = 0; dy < size; dy++) {
        for (int dx = 0; dx < size; dx++) {
            int tu = dx * CRAFT_TEX_SIZE / size;
            int tv = dy * CRAFT_TEX_SIZE / size;
            int fx = x + dx, fy = y + dy;
            if ((unsigned)fx < CRAFT_FB_W && (unsigned)fy < CRAFT_FB_H)
                fb[fy * CRAFT_FB_W + fx] = tex[tv * CRAFT_TEX_SIZE + tu];
        }
    }
}

static void draw_inventory_page(uint16_t *fb, const CraftPlayer *p) {
    int panel_w = 116, panel_h = 110;
    int x0 = (CRAFT_FB_W - panel_w) / 2;
    int y0 = (CRAFT_FB_H - panel_h) / 2;
    rect(fb, x0, y0, panel_w, panel_h, rgb565(30, 30, 40));
    rect(fb, x0,             y0, panel_w, 1, rgb565(150, 150, 180));
    rect(fb, x0,             y0 + panel_h - 1, panel_w, 1, rgb565(150, 150, 180));
    rect(fb, x0,             y0, 1, panel_h, rgb565(150, 150, 180));
    rect(fb, x0 + panel_w-1, y0, 1, panel_h, rgb565(150, 150, 180));

    const char *title = "Inventory";
    int tw = craft_font_width(title);
    craft_font_draw(fb, title, x0 + (panel_w - tw) / 2, y0 + 4, 0xFFFF);
    rect(fb, x0 + 6, y0 + 11, panel_w - 12, 1, rgb565(80, 80, 100));

    /* Grid */
    int cell = 22, gap = 3;
    int grid_w = INV_COLS * cell + (INV_COLS - 1) * gap;
    int grid_x = x0 + (panel_w - grid_w) / 2;
    int grid_y = y0 + 16;
    for (int r = 0; r < INV_ROWS; r++) {
        for (int c = 0; c < INV_COLS; c++) {
            int idx = r * INV_COLS + c;
            int cx = grid_x + c * (cell + gap);
            int cy = grid_y + r * (cell + gap);
            BlockId b = inv_blocks[idx];
            /* Cell background */
            rect(fb, cx, cy, cell, cell, rgb565(60, 60, 70));
            if (b != BLK_AIR) {
                block_swatch_at(fb, cx + 1, cy + 1, cell - 2, b);
            }
            /* Selection highlight */
            if (idx == s_inv_sel) {
                rect(fb, cx - 1, cy - 1, cell + 2, 1, 0xFFFF);
                rect(fb, cx - 1, cy + cell, cell + 2, 1, 0xFFFF);
                rect(fb, cx - 1, cy - 1, 1, cell + 2, 0xFFFF);
                rect(fb, cx + cell, cy - 1, 1, cell + 2, 0xFFFF);
            }
        }
    }

    /* Selected block name + hint at bottom of panel. */
    BlockId sel_b = inv_blocks[s_inv_sel];
    const char *name = (sel_b != BLK_AIR) ? craft_block_name(sel_b) : "empty";
    int nw = craft_font_width(name);
    craft_font_draw(fb, name, x0 + (panel_w - nw) / 2, y0 + panel_h - 18,
                    sel_b == BLK_AIR ? rgb565(120, 120, 130) : 0xFFFF);

    char hint[32];
    /* Show which hotbar slot will receive the block. */
    snprintf(hint, sizeof hint, "A:slot %d  B:back", p->hotbar_idx + 1);
    int hw = craft_font_width(hint);
    craft_font_draw(fb, hint, x0 + (panel_w - hw) / 2, y0 + panel_h - 8,
                    rgb565(180, 180, 200));
}

void craft_menu_draw(uint16_t *fb, const CraftPlayer *p) {
    if (!s_open) return;
    darken_bg(fb);
    if (s_page == PAGE_INVENTORY) draw_inventory_page(fb, p);
    else                          draw_main_page(fb, p);
}

/* --- Toast ----------------------------------------------------- */
static char  s_toast[32];
static float s_toast_t;

void craft_menu_toast(const char *msg) {
    if (!msg) { s_toast[0] = 0; s_toast_t = 0; return; }
    size_t n = strlen(msg);
    if (n >= sizeof s_toast) n = sizeof s_toast - 1;
    memcpy(s_toast, msg, n);
    s_toast[n] = 0;
    s_toast_t = 2.0f;     /* 2 seconds */
}
void craft_menu_toast_tick(float dt) {
    if (s_toast_t > 0) {
        s_toast_t -= dt;
        if (s_toast_t <= 0) { s_toast_t = 0; s_toast[0] = 0; }
    }
}
const char *craft_menu_toast_text(void) {
    return (s_toast_t > 0) ? s_toast : NULL;
}

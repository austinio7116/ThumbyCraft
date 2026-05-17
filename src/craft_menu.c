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
#include "craft_hud.h"

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
    { "Craft",         CRAFT_MENU_RESULT_CRAFT,      true  },
    { "Recipes",       CRAFT_MENU_RESULT_RECIPES,    true  },
    { "Controls",      CRAFT_MENU_RESULT_CONTROLS,   true  },
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

/* Menu pages: main pause list + sub-pages. B from a sub-page returns
 * to main; MENU closes the whole menu. */
typedef enum {
    PAGE_MAIN      = 0,
    PAGE_INVENTORY = 1,
    PAGE_CRAFT     = 2,
    PAGE_RECIPES   = 3,
    PAGE_CONTROLS  = 4,
} MenuPage;
static MenuPage s_page;
static bool  s_open;
static int   s_sel;
static int   s_scroll;          /* first visible item on main page */
static int   s_inv_sel;
static int   s_recipe_sel;      /* current recipe index on recipe page */
static int   s_controls_scroll; /* first visible line on controls page */

/* Crafting state — kept across menu opens so partial recipes persist
 * if the player closes the menu accidentally. */
static BlockId s_craft_grid[9];     /* row-major 3×3 grid */
static int     s_craft_sel;         /* 0..8 = grid, 9 = output */
static int     s_craft_last_row;    /* row to return to from output */
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
    s_scroll = 0;
    s_controls_scroll = 0;
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
 *
 * Scrollable view of every item the player actually owns
 * (inventory[blk] > 0). Grid is 5 cols × N visible rows. A on a cell
 * assigns that block to the currently-active hotbar slot. LB/RB cycle
 * which slot is active (always-visible hotbar at the bottom shows the
 * highlight). B returns to the main page.
 *
 * In creative mode "owned" means every block (infinite supply), so
 * the view shows all placeable + tool ids. */

#define INV_COLS 5
#define INV_VISIBLE_ROWS 4
#define INV_VISIBLE (INV_COLS * INV_VISIBLE_ROWS)
#define INV_MAX_ENTRIES BLK_COUNT     /* upper bound — one row per id */

/* Snapshot of "what the player has" — rebuilt every frame the
 * inventory page draws. Cheap (BLK_COUNT ≤ 32 ish). */
static BlockId s_inv_visible[INV_MAX_ENTRIES];
static int     s_inv_visible_count;
static int     s_inv_scroll;          /* topmost row index */

static void inv_rebuild_visible(const CraftPlayer *p) {
    s_inv_visible_count = 0;
    for (int b = 1; b < BLK_COUNT; b++) {
        bool owned = (p->mode == CRAFT_MODE_CREATIVE) || (p->inventory[b] > 0);
        if (!owned) continue;
        s_inv_visible[s_inv_visible_count++] = (BlockId)b;
    }
}

#define MAIN_VISIBLE_ITEMS 8

static void scroll_to_keep_visible(int sel, int total, int visible, int *scroll) {
    if (sel < *scroll) *scroll = sel;
    if (sel >= *scroll + visible) *scroll = sel - visible + 1;
    if (*scroll < 0) *scroll = 0;
    int max_scroll = total - visible;
    if (max_scroll < 0) max_scroll = 0;
    if (*scroll > max_scroll) *scroll = max_scroll;
}

static CraftMenuResult tick_main_page(const CraftInput *in, const CraftPlayer *p) {
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
    scroll_to_keep_visible(s_sel, ITEM_COUNT, MAIN_VISIBLE_ITEMS, &s_scroll);

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
            s_page = PAGE_INVENTORY;
            s_dpad_was_pressed = in->up || in->down || in->left || in->right;
            return CRAFT_MENU_RESULT_NONE;
        }
        if (item->result == CRAFT_MENU_RESULT_CRAFT) {
            s_page = PAGE_CRAFT;
            s_craft_sel = 0;
            s_craft_last_row = 0;
            s_dpad_was_pressed = in->up || in->down || in->left || in->right;
            return CRAFT_MENU_RESULT_NONE;
        }
        if (item->result == CRAFT_MENU_RESULT_RECIPES) {
            s_page = PAGE_RECIPES;
            s_recipe_sel = 0;
            s_dpad_was_pressed = in->up || in->down || in->left || in->right;
            return CRAFT_MENU_RESULT_NONE;
        }
        if (item->result == CRAFT_MENU_RESULT_CONTROLS) {
            s_page = PAGE_CONTROLS;
            s_controls_scroll = 0;
            s_dpad_was_pressed = in->up || in->down;
            return CRAFT_MENU_RESULT_NONE;
        }
        if (item->close_on_confirm) s_open = false;
        return item->result;
    }
    return CRAFT_MENU_RESULT_NONE;
}

static CraftMenuResult tick_inventory_page(const CraftInput *in,
                                           CraftPlayer *pmut) {
    inv_rebuild_visible(pmut);
    int n = s_inv_visible_count;
    if (n < 1) n = 1;                    /* still allow cursor at 0 */
    if (s_inv_sel >= n) s_inv_sel = n - 1;
    if (s_inv_sel < 0)  s_inv_sel = 0;

    bool dpad_now = in->up || in->down || in->left || in->right;
    if (dpad_now && !s_dpad_was_pressed) {
        if (in->left  && s_inv_sel > 0)         s_inv_sel--;
        if (in->right && s_inv_sel < n - 1)     s_inv_sel++;
        if (in->up    && s_inv_sel >= INV_COLS) s_inv_sel -= INV_COLS;
        if (in->down  && s_inv_sel + INV_COLS < n) s_inv_sel += INV_COLS;
        s_dpad_repeat_t = DPAD_INITIAL_DELAY;
    } else if (dpad_now) {
        s_dpad_repeat_t -= 1.0f / 30.0f;
        if (s_dpad_repeat_t <= 0.0f) {
            if (in->left  && s_inv_sel > 0)         s_inv_sel--;
            if (in->right && s_inv_sel < n - 1)     s_inv_sel++;
            if (in->up    && s_inv_sel >= INV_COLS) s_inv_sel -= INV_COLS;
            if (in->down  && s_inv_sel + INV_COLS < n) s_inv_sel += INV_COLS;
            s_dpad_repeat_t = DPAD_REPEAT;
        }
    }
    s_dpad_was_pressed = dpad_now;

    /* Auto-scroll to keep the selected cell on-screen. */
    int sel_row = s_inv_sel / INV_COLS;
    if (sel_row < s_inv_scroll) s_inv_scroll = sel_row;
    if (sel_row >= s_inv_scroll + INV_VISIBLE_ROWS) {
        s_inv_scroll = sel_row - INV_VISIBLE_ROWS + 1;
    }

    /* LB / RB cycle the active hotbar slot — the player can target
     * any of the 8 slots without leaving the menu. The always-visible
     * hotbar at the bottom shows the highlight, so the player sees
     * exactly where their A press will land. */
    if (in->lb_pressed) {
        pmut->hotbar_idx = (pmut->hotbar_idx + CRAFT_HOTBAR_SLOTS - 1)
                            % CRAFT_HOTBAR_SLOTS;
    }
    if (in->rb_pressed) {
        pmut->hotbar_idx = (pmut->hotbar_idx + 1) % CRAFT_HOTBAR_SLOTS;
    }

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
    if (in->a_pressed && s_inv_sel < s_inv_visible_count) {
        BlockId b = s_inv_visible[s_inv_sel];
        pmut->hotbar[pmut->hotbar_idx] = b;
        /* Stay in the inventory — player may want to fill multiple
         * slots. Toast confirms what landed where. */
        char toast[32];
        snprintf(toast, sizeof toast, "%s → slot %d",
                 craft_block_name(b), pmut->hotbar_idx + 1);
        craft_menu_toast(toast);
    }
    return CRAFT_MENU_RESULT_NONE;
}

/* --- Crafting page ---------------------------------------------- *
 *
 * 3×3 grid + arrow + output cell. Player navigates with D-pad:
 *   - within grid: 4-directional wrap
 *   - right from rightmost column → output cell
 *   - left from output → rightmost column of last-known row
 *
 * Actions:
 *   A on grid cell: place currently-active hotbar block (decrements
 *     in survival; no-op in creative if slot is empty/0)
 *   B on grid cell: clear cell back to AIR (refunds nothing — items
 *     in the grid still count as inventory until craft completes)
 *   A on output cell: if a recipe matches, execute it — consume grid
 *     inputs, add output to inventory, return to gameplay
 *   B on output cell or MENU: close menu (grid contents preserved)
 *
 * Recipes are SHAPED 3×3 (exact pattern, no translation). The user
 * places blocks matching one of the recipe layouts to enable craft.
 */

typedef struct {
    BlockId     pattern[9];
    BlockId     output;
    uint8_t     output_count;
    const char *name;
} CraftRecipe;

static const CraftRecipe RECIPES[] = {
    /* --- Materials --- */
    { { BLK_WOOD, BLK_AIR, BLK_AIR,
        BLK_AIR,  BLK_AIR, BLK_AIR,
        BLK_AIR,  BLK_AIR, BLK_AIR }, BLK_PLANK, 4, "Planks" },

    { { BLK_PLANK, BLK_AIR, BLK_AIR,
        BLK_PLANK, BLK_AIR, BLK_AIR,
        BLK_AIR,   BLK_AIR, BLK_AIR }, BLK_STICK, 4, "Sticks" },

    { { BLK_COBBLE, BLK_COBBLE, BLK_AIR,
        BLK_COBBLE, BLK_COBBLE, BLK_AIR,
        BLK_AIR,    BLK_AIR,    BLK_AIR }, BLK_STONE, 1, "Smooth stone" },

    /* Iron ore + coal → 1 iron ingot (in-grid "smelt" until we have
     * a furnace UI — input position matches what the player gathers). */
    { { BLK_IRON_ORE, BLK_AIR, BLK_AIR,
        BLK_COAL_ORE, BLK_AIR, BLK_AIR,
        BLK_AIR,      BLK_AIR, BLK_AIR }, BLK_IRON_INGOT, 1, "Iron ingot" },

    /* Sand + coal → 1 glass (in-grid "smelt", same convention as iron;
     * vanilla smelts sand in a furnace for 1 glass). */
    { { BLK_SAND,     BLK_AIR, BLK_AIR,
        BLK_COAL_ORE, BLK_AIR, BLK_AIR,
        BLK_AIR,      BLK_AIR, BLK_AIR }, BLK_GLASS, 1, "Glass" },

    /* --- Pickaxes (all use sticks for the handle) --- */
    { { BLK_PLANK, BLK_PLANK, BLK_PLANK,
        BLK_AIR,   BLK_STICK, BLK_AIR,
        BLK_AIR,   BLK_STICK, BLK_AIR }, BLK_PICKAXE_WOOD, 1, "Wood pick" },

    { { BLK_COBBLE, BLK_COBBLE, BLK_COBBLE,
        BLK_AIR,    BLK_STICK,  BLK_AIR,
        BLK_AIR,    BLK_STICK,  BLK_AIR }, BLK_PICKAXE_STONE, 1, "Stone pick" },

    { { BLK_IRON_INGOT, BLK_IRON_INGOT, BLK_IRON_INGOT,
        BLK_AIR,        BLK_STICK,      BLK_AIR,
        BLK_AIR,        BLK_STICK,      BLK_AIR }, BLK_PICKAXE_IRON, 1, "Iron pick" },

    /* --- Swords (blade on top, stick handle on bottom) --- */
    { { BLK_AIR, BLK_PLANK, BLK_AIR,
        BLK_AIR, BLK_PLANK, BLK_AIR,
        BLK_AIR, BLK_STICK, BLK_AIR }, BLK_SWORD_WOOD, 1, "Wood sword" },

    { { BLK_AIR, BLK_COBBLE, BLK_AIR,
        BLK_AIR, BLK_COBBLE, BLK_AIR,
        BLK_AIR, BLK_STICK,  BLK_AIR }, BLK_SWORD_STONE, 1, "Stone sword" },

    { { BLK_AIR, BLK_IRON_INGOT, BLK_AIR,
        BLK_AIR, BLK_IRON_INGOT, BLK_AIR,
        BLK_AIR, BLK_STICK,      BLK_AIR }, BLK_SWORD_IRON, 1, "Iron sword" },

    /* --- Lighting --- */
    { { BLK_COAL_ORE, BLK_AIR, BLK_AIR,
        BLK_STICK,    BLK_AIR, BLK_AIR,
        BLK_AIR,      BLK_AIR, BLK_AIR }, BLK_TORCH, 4, "Torches" },
};
#define RECIPE_COUNT ((int)(sizeof(RECIPES)/sizeof(RECIPES[0])))

static int find_matching_recipe(void) {
    for (int r = 0; r < RECIPE_COUNT; r++) {
        bool match = true;
        for (int i = 0; i < 9; i++) {
            if (s_craft_grid[i] != RECIPES[r].pattern[i]) { match = false; break; }
        }
        if (match) return r;
    }
    return -1;
}

/* How many of `b` does the player have available after subtracting
 * what they've already placed on the craft grid? Creative treats
 * blocks as infinite (returns a large value as long as inventory
 * has been touched). */
static int craft_block_available(const CraftPlayer *p, BlockId b) {
    if (b == BLK_AIR) return 0;
    int placed = 0;
    for (int i = 0; i < 9; i++)
        if (s_craft_grid[i] == b) placed++;
    if (p->mode == CRAFT_MODE_CREATIVE) {
        /* In creative, you have it if the inventory ever saw it. */
        return p->inventory[b] > 0 ? 999 : 0;
    }
    return p->inventory[b] - placed;
}

static CraftMenuResult tick_craft_page(const CraftInput *in, CraftPlayer *p) {
    /* D-pad nav with wrap. */
    bool dpad_now = in->up || in->down || in->left || in->right;
    if (dpad_now && !s_dpad_was_pressed) {
        if (s_craft_sel == 9) {
            /* Output cell — left exits to grid. */
            if (in->left) {
                s_craft_sel = s_craft_last_row * 3 + 2;
            }
            /* up/down/right on output: no-op */
        } else {
            int r = s_craft_sel / 3;
            int c = s_craft_sel % 3;
            if (in->left)  c = (c + 2) % 3;
            if (in->right) {
                if (c == 2) { s_craft_last_row = r; s_craft_sel = 9; goto nav_done; }
                c = (c + 1) % 3;
            }
            if (in->up)    r = (r + 2) % 3;
            if (in->down)  r = (r + 1) % 3;
            s_craft_sel = r * 3 + c;
        }
nav_done:
        s_dpad_repeat_t = DPAD_INITIAL_DELAY;
    } else if (dpad_now) {
        s_dpad_repeat_t -= 1.0f / 30.0f;
        if (s_dpad_repeat_t <= 0.0f) {
            /* Same logic — fire again on auto-repeat. */
            if (s_craft_sel == 9) {
                if (in->left) s_craft_sel = s_craft_last_row * 3 + 2;
            } else {
                int r = s_craft_sel / 3;
                int c = s_craft_sel % 3;
                if (in->left)  c = (c + 2) % 3;
                if (in->right) {
                    if (c == 2) { s_craft_last_row = r; s_craft_sel = 9; }
                    else c = (c + 1) % 3;
                }
                if (in->up)    r = (r + 2) % 3;
                if (in->down)  r = (r + 1) % 3;
                if (s_craft_sel != 9) s_craft_sel = r * 3 + c;
            }
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
        s_open = false;
        return CRAFT_MENU_RESULT_RESUME;
    }
    if (b_just_released) {
        if (s_craft_sel == 9) {
            /* B on output → back to main page. */
            s_page = PAGE_MAIN;
            s_dpad_was_pressed = in->up || in->down;
            return CRAFT_MENU_RESULT_NONE;
        }
        /* Clear selected grid cell. */
        s_craft_grid[s_craft_sel] = BLK_AIR;
        return CRAFT_MENU_RESULT_NONE;
    }
    if (in->a_pressed) {
        if (s_craft_sel == 9) {
            /* Try to execute the matching recipe. */
            int r = find_matching_recipe();
            if (r < 0) return CRAFT_MENU_RESULT_NONE;
            const CraftRecipe *rec = &RECIPES[r];
            /* Consume inputs from grid (survival also debits inventory). */
            for (int i = 0; i < 9; i++) {
                BlockId b = s_craft_grid[i];
                if (b == BLK_AIR) continue;
                if (p->mode == CRAFT_MODE_SURVIVAL && p->inventory[b] > 0)
                    p->inventory[b]--;
                s_craft_grid[i] = BLK_AIR;
            }
            /* Add output. In creative, inventory is irrelevant — just
             * auto-add to hotbar if not present. */
            if (p->mode == CRAFT_MODE_SURVIVAL)
                p->inventory[rec->output] += rec->output_count;
            bool present = false;
            for (int i = 0; i < CRAFT_HOTBAR_SLOTS; i++)
                if (p->hotbar[i] == rec->output) { present = true; break; }
            if (!present) {
                for (int i = 0; i < CRAFT_HOTBAR_SLOTS; i++)
                    if (p->hotbar[i] == BLK_AIR) {
                        p->hotbar[i] = rec->output;
                        break;
                    }
            }
            craft_menu_toast(craft_block_name(rec->output));
            return CRAFT_MENU_RESULT_NONE;
        }
        /* A on grid cell — place the active hotbar block. */
        BlockId held = p->hotbar[p->hotbar_idx];
        if (held == BLK_AIR) return CRAFT_MENU_RESULT_NONE;
        if (craft_block_available(p, held) <= 0)
            return CRAFT_MENU_RESULT_NONE;
        s_craft_grid[s_craft_sel] = held;
    }

    /* LB / RB cycle the hotbar slot — same semantics as the in-game
     * MENU+LB/RB chord, just available directly while the craft page
     * is open. The visible (darkened) hotbar at the bottom shows
     * which item A will place. */
    if (in->lb_pressed) {
        p->hotbar_idx = (p->hotbar_idx + CRAFT_HOTBAR_SLOTS - 1) % CRAFT_HOTBAR_SLOTS;
    }
    if (in->rb_pressed) {
        p->hotbar_idx = (p->hotbar_idx + 1) % CRAFT_HOTBAR_SLOTS;
    }
    return CRAFT_MENU_RESULT_NONE;
}

/* --- Recipe book page ------------------------------------------- */
static CraftMenuResult tick_recipes_page(const CraftInput *in) {
    bool dpad_now = in->left || in->right;
    if (dpad_now && !s_dpad_was_pressed) {
        if (in->left)  s_recipe_sel = (s_recipe_sel + RECIPE_COUNT - 1) % RECIPE_COUNT;
        if (in->right) s_recipe_sel = (s_recipe_sel + 1) % RECIPE_COUNT;
        s_dpad_repeat_t = DPAD_INITIAL_DELAY;
    } else if (dpad_now) {
        s_dpad_repeat_t -= 1.0f / 30.0f;
        if (s_dpad_repeat_t <= 0.0f) {
            if (in->left)  s_recipe_sel = (s_recipe_sel + RECIPE_COUNT - 1) % RECIPE_COUNT;
            if (in->right) s_recipe_sel = (s_recipe_sel + 1) % RECIPE_COUNT;
            s_dpad_repeat_t = DPAD_REPEAT;
        }
    }
    s_dpad_was_pressed = dpad_now;

    bool b_just_released    = !in->b    && s_input_prev_b;
    bool menu_just_released = !in->menu && s_input_prev_menu;
    s_input_prev_a    = in->a;
    s_input_prev_b    = in->b;
    s_input_prev_menu = in->menu;

    if (menu_just_released) { s_open = false; return CRAFT_MENU_RESULT_RESUME; }
    if (b_just_released)    { s_page = PAGE_MAIN; return CRAFT_MENU_RESULT_NONE; }
    return CRAFT_MENU_RESULT_NONE;
}

/* --- Controls page ---------------------------------------------- */
static const char *CONTROLS_LINES[] = {
    "LOOK",
    " D-pad L/R  turn",
    " D-pad U/D  pitch",
    "",
    "MOVE",
    " LB hold    walk fwd",
    " RB         jump",
    "",
    "INTERACT",
    " A          break",
    " B          place",
    "",
    "PAUSE MENU",
    " MENU       open",
    " MENU+LB/RB hotbar",
    " MENU+A     fly",
    "",
    "CRAFT PAGE",
    " LB/RB      pick",
    " A          place",
    " A on out   craft",
    "",
    "B: back",
};
#define CONTROLS_LINE_COUNT ((int)(sizeof(CONTROLS_LINES)/sizeof(CONTROLS_LINES[0])))
#define CONTROLS_VISIBLE 14

static CraftMenuResult tick_controls_page(const CraftInput *in) {
    bool dpad_now = in->up || in->down;
    if (dpad_now && !s_dpad_was_pressed) {
        if (in->up)   s_controls_scroll--;
        if (in->down) s_controls_scroll++;
        s_dpad_repeat_t = DPAD_INITIAL_DELAY;
    } else if (dpad_now) {
        s_dpad_repeat_t -= 1.0f / 30.0f;
        if (s_dpad_repeat_t <= 0.0f) {
            if (in->up)   s_controls_scroll--;
            if (in->down) s_controls_scroll++;
            s_dpad_repeat_t = DPAD_REPEAT;
        }
    }
    s_dpad_was_pressed = dpad_now;
    int max_scroll = CONTROLS_LINE_COUNT - CONTROLS_VISIBLE;
    if (max_scroll < 0) max_scroll = 0;
    if (s_controls_scroll < 0) s_controls_scroll = 0;
    if (s_controls_scroll > max_scroll) s_controls_scroll = max_scroll;

    bool b_just_released    = !in->b    && s_input_prev_b;
    bool menu_just_released = !in->menu && s_input_prev_menu;
    s_input_prev_a    = in->a;
    s_input_prev_b    = in->b;
    s_input_prev_menu = in->menu;
    if (menu_just_released) { s_open = false; return CRAFT_MENU_RESULT_RESUME; }
    if (b_just_released)    { s_page = PAGE_MAIN; return CRAFT_MENU_RESULT_NONE; }
    return CRAFT_MENU_RESULT_NONE;
}

CraftMenuResult craft_menu_tick(const CraftInput *in, const CraftPlayer *p) {
    if (!s_open) return CRAFT_MENU_RESULT_NONE;
    if (s_page == PAGE_INVENTORY)
        return tick_inventory_page(in, (CraftPlayer *)p);
    if (s_page == PAGE_CRAFT)
        return tick_craft_page(in, (CraftPlayer *)p);
    if (s_page == PAGE_RECIPES)
        return tick_recipes_page(in);
    if (s_page == PAGE_CONTROLS)
        return tick_controls_page(in);
    return tick_main_page(in, p);
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
    int visible_end = s_scroll + MAIN_VISIBLE_ITEMS;
    if (visible_end > ITEM_COUNT) visible_end = ITEM_COUNT;
    for (int i = s_scroll; i < visible_end; i++) {
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
    /* Scroll arrows on the right edge when items hidden above/below. */
    int arrow_x = x0 + panel_w - 6;
    if (s_scroll > 0) {
        rect(fb, arrow_x,     y0 + 15, 3, 1, 0xFFFF);
        rect(fb, arrow_x + 1, y0 + 14, 1, 1, 0xFFFF);
    }
    if (visible_end < ITEM_COUNT) {
        int by = y0 + 16 + MAIN_VISIBLE_ITEMS * 10 - 4;
        rect(fb, arrow_x,     by, 3, 1, 0xFFFF);
        rect(fb, arrow_x + 1, by + 1, 1, 1, 0xFFFF);
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
    /* Rebuild the live list — same source the tick uses. Cheap. */
    inv_rebuild_visible(p);

    /* Panel fills most of the screen but leaves the bottom hotbar
     * strip uncovered so the active-slot indicator stays visible. */
    int panel_w = CRAFT_FB_W - 4;
    int panel_h = CRAFT_FB_H - 22;     /* leave ~22 px for hotbar */
    int x0 = (CRAFT_FB_W - panel_w) / 2;
    int y0 = 2;
    rect(fb, x0, y0, panel_w, panel_h, rgb565(30, 30, 40));
    rect(fb, x0,             y0, panel_w, 1, rgb565(150, 150, 180));
    rect(fb, x0,             y0 + panel_h - 1, panel_w, 1, rgb565(150, 150, 180));
    rect(fb, x0,             y0, 1, panel_h, rgb565(150, 150, 180));
    rect(fb, x0 + panel_w-1, y0, 1, panel_h, rgb565(150, 150, 180));

    const char *title = "Inventory";
    int tw = craft_font_width(title);
    craft_font_draw(fb, title, x0 + (panel_w - tw) / 2, y0 + 3, 0xFFFF);
    rect(fb, x0 + 6, y0 + 10, panel_w - 12, 1, rgb565(80, 80, 100));

    /* Grid — INV_COLS × INV_VISIBLE_ROWS visible cells, scrolling
     * through s_inv_visible[]. */
    int cell = 18, gap = 2;
    int grid_w = INV_COLS * cell + (INV_COLS - 1) * gap;
    int grid_x = x0 + (panel_w - grid_w) / 2;
    int grid_y = y0 + 14;
    int n = s_inv_visible_count;
    int sel_row = s_inv_sel / INV_COLS;
    (void)sel_row;
    for (int r = 0; r < INV_VISIBLE_ROWS; r++) {
        int abs_row = s_inv_scroll + r;
        for (int c = 0; c < INV_COLS; c++) {
            int abs_idx = abs_row * INV_COLS + c;
            int cx = grid_x + c * (cell + gap);
            int cy = grid_y + r * (cell + gap);
            /* Empty (past end of list) — draw a faint placeholder. */
            if (abs_idx >= n) {
                rect(fb, cx, cy, cell, cell, rgb565(40, 40, 50));
                continue;
            }
            BlockId b = s_inv_visible[abs_idx];
            rect(fb, cx, cy, cell, cell, rgb565(60, 60, 70));
            block_swatch_at(fb, cx + 1, cy + 1, cell - 2, b);
            /* Count badge bottom-right. Skip in creative — supply is
             * infinite, count would be misleading. */
            if (p->mode == CRAFT_MODE_SURVIVAL && p->inventory[b] > 0) {
                char cbuf[6];
                snprintf(cbuf, sizeof cbuf, "%d", p->inventory[b]);
                int cw = craft_font_width(cbuf);
                /* Dark backing for legibility against textured swatch. */
                rect(fb, cx + cell - cw - 2, cy + cell - 6, cw + 1, 6,
                     rgb565(10, 10, 15));
                craft_font_draw(fb, cbuf, cx + cell - cw - 1,
                                cy + cell - 5, 0xFFFF);
            }
            /* Selection highlight */
            if (abs_idx == s_inv_sel) {
                rect(fb, cx - 1, cy - 1, cell + 2, 1, 0xFFFF);
                rect(fb, cx - 1, cy + cell, cell + 2, 1, 0xFFFF);
                rect(fb, cx - 1, cy - 1, 1, cell + 2, 0xFFFF);
                rect(fb, cx + cell, cy - 1, 1, cell + 2, 0xFFFF);
            }
        }
    }

    /* Scroll arrows when there's more above / below. */
    int total_rows = (n + INV_COLS - 1) / INV_COLS;
    if (s_inv_scroll > 0) {
        int ay = grid_y - 3;
        int ax = x0 + panel_w - 8;
        rect(fb, ax - 1, ay + 1, 3, 1, 0xFFFF);
        rect(fb, ax,     ay,     1, 1, 0xFFFF);
    }
    if (s_inv_scroll + INV_VISIBLE_ROWS < total_rows) {
        int ay = grid_y + INV_VISIBLE_ROWS * (cell + gap) - gap;
        int ax = x0 + panel_w - 8;
        rect(fb, ax - 1, ay, 3, 1, 0xFFFF);
        rect(fb, ax,     ay + 1, 1, 1, 0xFFFF);
    }

    /* Selected name + hint. */
    const char *name = (n > 0 && s_inv_sel < n)
                       ? craft_block_name(s_inv_visible[s_inv_sel])
                       : "empty";
    int nw = craft_font_width(name);
    craft_font_draw(fb, name, x0 + (panel_w - nw) / 2,
                    y0 + panel_h - 16, 0xFFFF);

    char hint[40];
    snprintf(hint, sizeof hint, "A:slot %d  LB/RB:slot  B:back",
             p->hotbar_idx + 1);
    int hw = craft_font_width(hint);
    craft_font_draw(fb, hint, x0 + (panel_w - hw) / 2,
                    y0 + panel_h - 8, rgb565(180, 180, 200));
}

static void draw_craft_page(uint16_t *fb, const CraftPlayer *p) {
    int panel_w = 120, panel_h = 108;
    int x0 = (CRAFT_FB_W - panel_w) / 2;
    int y0 = 2;
    rect(fb, x0, y0, panel_w, panel_h, rgb565(30, 30, 40));
    rect(fb, x0,             y0,                panel_w, 1, rgb565(150, 150, 180));
    rect(fb, x0,             y0 + panel_h - 1,  panel_w, 1, rgb565(150, 150, 180));
    rect(fb, x0,             y0,                1, panel_h, rgb565(150, 150, 180));
    rect(fb, x0 + panel_w-1, y0,                1, panel_h, rgb565(150, 150, 180));

    const char *title = "Craft";
    int tw = craft_font_width(title);
    craft_font_draw(fb, title, x0 + (panel_w - tw) / 2, y0 + 3, 0xFFFF);
    rect(fb, x0 + 6, y0 + 10, panel_w - 12, 1, rgb565(80, 80, 100));

    /* 3×3 grid */
    int cell = 18;
    int gap  = 2;
    int grid_w = 3 * cell + 2 * gap;     /* 58 */
    int grid_x = x0 + 8;
    int grid_y = y0 + 16;
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            int idx = r * 3 + c;
            int cx = grid_x + c * (cell + gap);
            int cy = grid_y + r * (cell + gap);
            rect(fb, cx, cy, cell, cell, rgb565(60, 60, 70));
            BlockId b = s_craft_grid[idx];
            if (b != BLK_AIR) {
                block_swatch_at(fb, cx + 1, cy + 1, cell - 2, b);
            }
            if (idx == s_craft_sel) {
                rect(fb, cx - 1, cy - 1,        cell + 2, 1, 0xFFFF);
                rect(fb, cx - 1, cy + cell,     cell + 2, 1, 0xFFFF);
                rect(fb, cx - 1, cy - 1,        1, cell + 2, 0xFFFF);
                rect(fb, cx + cell, cy - 1,     1, cell + 2, 0xFFFF);
            }
        }
    }

    /* Arrow + output cell. */
    int arrow_x = grid_x + grid_w + 4;
    int arrow_y = grid_y + (3 * cell + 2 * gap) / 2 - 1;
    for (int i = 0; i < 10; i++) {
        rect(fb, arrow_x + i, arrow_y, 1, 3, rgb565(220, 220, 220));
    }
    /* Triangle tip. */
    rect(fb, arrow_x + 8,  arrow_y - 1, 1, 5, rgb565(220, 220, 220));
    rect(fb, arrow_x + 9,  arrow_y,     1, 3, rgb565(220, 220, 220));

    int out_x = arrow_x + 12;
    int out_y = grid_y + (3 * cell + 2 * gap) / 2 - cell / 2;
    int out_size = cell + 4;
    rect(fb, out_x, out_y, out_size, out_size, rgb565(60, 60, 70));
    int match = find_matching_recipe();
    if (match >= 0) {
        block_swatch_at(fb, out_x + 1, out_y + 1, out_size - 2,
                        RECIPES[match].output);
        /* Output count badge. */
        char buf[6];
        snprintf(buf, sizeof buf, "x%d", RECIPES[match].output_count);
        craft_font_draw(fb, buf, out_x + 1, out_y + out_size - 6, 0xFFFF);
    }
    if (s_craft_sel == 9) {
        uint16_t hi = (match >= 0) ? rgb565(120, 240, 120) : 0xFFFF;
        rect(fb, out_x - 1, out_y - 1,            out_size + 2, 1, hi);
        rect(fb, out_x - 1, out_y + out_size,     out_size + 2, 1, hi);
        rect(fb, out_x - 1, out_y - 1,            1, out_size + 2, hi);
        rect(fb, out_x + out_size, out_y - 1,     1, out_size + 2, hi);
    }

    /* Held-block label — just shows the name of the active hotbar
     * slot. The actual swatch is visible in the hotbar at the
     * bottom of the screen. */
    BlockId held = p->hotbar[p->hotbar_idx];
    char label[24];
    if (held == BLK_AIR) {
        snprintf(label, sizeof label, "hotbar empty");
    } else if (p->mode == CRAFT_MODE_SURVIVAL) {
        snprintf(label, sizeof label, "%s x%d",
                 craft_block_name(held), p->inventory[held]);
    } else {
        snprintf(label, sizeof label, "%s", craft_block_name(held));
    }
    int lw = craft_font_width(label);
    craft_font_draw(fb, label, x0 + (panel_w - lw) / 2, y0 + panel_h - 18,
                    (held == BLK_AIR) ? rgb565(120, 120, 130) : 0xFFFF);

    /* Hint. */
    const char *hint = "LB/RB pick  A:put";
    int hw = craft_font_width(hint);
    craft_font_draw(fb, hint, x0 + (panel_w - hw) / 2, y0 + panel_h - 8,
                    rgb565(180, 180, 200));
}

static void draw_recipes_page(uint16_t *fb, const CraftPlayer *p) {
    (void)p;
    int panel_w = 120, panel_h = 110;
    int x0 = (CRAFT_FB_W - panel_w) / 2;
    int y0 = (CRAFT_FB_H - panel_h) / 2;
    rect(fb, x0, y0, panel_w, panel_h, rgb565(30, 30, 40));
    rect(fb, x0,             y0,                panel_w, 1, rgb565(150, 150, 180));
    rect(fb, x0,             y0 + panel_h - 1,  panel_w, 1, rgb565(150, 150, 180));
    rect(fb, x0,             y0,                1, panel_h, rgb565(150, 150, 180));
    rect(fb, x0 + panel_w-1, y0,                1, panel_h, rgb565(150, 150, 180));

    char hdr[24];
    snprintf(hdr, sizeof hdr, "Recipes  %d/%d", s_recipe_sel + 1, RECIPE_COUNT);
    int hw = craft_font_width(hdr);
    craft_font_draw(fb, hdr, x0 + (panel_w - hw) / 2, y0 + 4, 0xFFFF);
    rect(fb, x0 + 6, y0 + 11, panel_w - 12, 1, rgb565(80, 80, 100));

    const CraftRecipe *r = &RECIPES[s_recipe_sel];

    /* 3×3 pattern grid. */
    int cell = 14;
    int gap  = 2;
    int grid_w = 3 * cell + 2 * gap;
    int grid_x = x0 + 12;
    int grid_y = y0 + 22;
    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 3; col++) {
            int cx = grid_x + col * (cell + gap);
            int cy = grid_y + row * (cell + gap);
            rect(fb, cx, cy, cell, cell, rgb565(60, 60, 70));
            BlockId b = r->pattern[row * 3 + col];
            if (b != BLK_AIR) {
                block_swatch_at(fb, cx + 1, cy + 1, cell - 2, b);
            }
        }
    }

    /* Arrow + output. */
    int arrow_x = grid_x + grid_w + 6;
    int arrow_y = grid_y + (3 * cell + 2 * gap) / 2 - 1;
    for (int i = 0; i < 8; i++)
        rect(fb, arrow_x + i, arrow_y, 1, 3, rgb565(220, 220, 220));
    rect(fb, arrow_x + 7, arrow_y - 1, 1, 5, rgb565(220, 220, 220));

    int out_x = arrow_x + 10;
    int out_y = grid_y + (3 * cell + 2 * gap) / 2 - cell / 2;
    int out_size = cell + 4;
    rect(fb, out_x, out_y, out_size, out_size, rgb565(60, 60, 70));
    block_swatch_at(fb, out_x + 1, out_y + 1, out_size - 2, r->output);
    char cbuf[8];
    snprintf(cbuf, sizeof cbuf, "x%d", r->output_count);
    craft_font_draw(fb, cbuf, out_x + 1, out_y + out_size - 6, 0xFFFF);

    /* Recipe name. */
    int name_y = grid_y + 3 * cell + 2 * gap + 6;
    int nw = craft_font_width(r->name);
    craft_font_draw(fb, r->name, x0 + (panel_w - nw) / 2, name_y, 0xFFFF);

    /* Output name + count. */
    char outname[24];
    snprintf(outname, sizeof outname, "%s x%d",
             craft_block_name(r->output), r->output_count);
    int ow = craft_font_width(outname);
    craft_font_draw(fb, outname, x0 + (panel_w - ow) / 2, name_y + 8,
                    rgb565(180, 220, 180));

    const char *hint = "L/R: nav  B: back";
    int hw2 = craft_font_width(hint);
    craft_font_draw(fb, hint, x0 + (panel_w - hw2) / 2, y0 + panel_h - 8,
                    rgb565(180, 180, 200));
}

static void draw_controls_page(uint16_t *fb, const CraftPlayer *p) {
    (void)p;
    int panel_w = 120, panel_h = 110;
    int x0 = (CRAFT_FB_W - panel_w) / 2;
    int y0 = (CRAFT_FB_H - panel_h) / 2;
    rect(fb, x0, y0, panel_w, panel_h, rgb565(30, 30, 40));
    rect(fb, x0,             y0,                panel_w, 1, rgb565(150, 150, 180));
    rect(fb, x0,             y0 + panel_h - 1,  panel_w, 1, rgb565(150, 150, 180));
    rect(fb, x0,             y0,                1, panel_h, rgb565(150, 150, 180));
    rect(fb, x0 + panel_w-1, y0,                1, panel_h, rgb565(150, 150, 180));

    const char *title = "Controls";
    int tw = craft_font_width(title);
    craft_font_draw(fb, title, x0 + (panel_w - tw) / 2, y0 + 4, 0xFFFF);
    rect(fb, x0 + 6, y0 + 11, panel_w - 12, 1, rgb565(80, 80, 100));

    int line_y = y0 + 14;
    int visible_end = s_controls_scroll + CONTROLS_VISIBLE;
    if (visible_end > CONTROLS_LINE_COUNT) visible_end = CONTROLS_LINE_COUNT;
    for (int i = s_controls_scroll; i < visible_end; i++) {
        const char *t = CONTROLS_LINES[i];
        uint16_t col = (t[0] != ' ' && t[0] != 0)
                       ? rgb565(180, 220, 255)   /* section header */
                       : rgb565(220, 220, 230);  /* line */
        craft_font_draw(fb, t, x0 + 5, line_y, col);
        line_y += 6;
    }

    /* Scroll arrows. */
    int arrow_x = x0 + panel_w - 6;
    if (s_controls_scroll > 0) {
        rect(fb, arrow_x,     y0 + 14, 3, 1, 0xFFFF);
        rect(fb, arrow_x + 1, y0 + 13, 1, 1, 0xFFFF);
    }
    if (visible_end < CONTROLS_LINE_COUNT) {
        int by = y0 + 14 + CONTROLS_VISIBLE * 6 - 4;
        rect(fb, arrow_x,     by, 3, 1, 0xFFFF);
        rect(fb, arrow_x + 1, by + 1, 1, 1, 0xFFFF);
    }
}

void craft_menu_draw(uint16_t *fb, const CraftPlayer *p) {
    if (!s_open) return;
    darken_bg(fb);
    if (s_page == PAGE_INVENTORY)      draw_inventory_page(fb, p);
    else if (s_page == PAGE_CRAFT)     draw_craft_page(fb, p);
    else if (s_page == PAGE_RECIPES)   draw_recipes_page(fb, p);
    else if (s_page == PAGE_CONTROLS)  draw_controls_page(fb, p);
    else                               draw_main_page(fb, p);
    /* Hotbar always visible at full brightness over the dimmed bg
     * so the active-slot indicator stays legible while the player
     * navigates the menu — they need to see what they're holding
     * when picking what to craft / what to swap to. */
    craft_hud_draw_hotbar(fb, p);
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

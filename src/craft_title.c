/*
 * ThumbyCraft — boot title screen (impl).
 *
 * 2×3 tile grid: 4 save slots on top (2×2), one "New World" tile
 * spanning the third row. Up/Down/Left/Right cycles selection; A
 * confirms; for load, A on an empty slot is ignored (no-op). Music
 * plays once craft_main_init runs, so the title screen is silent.
 */
#include "craft_title.h"
#include "craft_save.h"
#include "craft_font.h"

#include <stdbool.h>
#include <string.h>

static uint16_t *s_fb;
static int       s_sel;         /* 0..3 = slot, 4 = "New World" */
static bool      s_prev_dpad;
static bool      s_prev_a;
static CraftTitleAction s_pending;
static int       s_chosen_slot;

#define TILE_NEW   4
#if defined(CRAFT_NET_ENABLED) && CRAFT_NET_ENABLED
/* Co-op builds split the bottom row: New World | Join Friend. */
#define TILE_JOIN  5
#define TILE_COUNT 6
#else
#define TILE_COUNT 5
#endif

static inline uint16_t rgb565_v(int r, int g, int b) {
    if (r > 255) r = 255; if (r < 0) r = 0;
    if (g > 255) g = 255; if (g < 0) g = 0;
    if (b > 255) b = 255; if (b < 0) b = 0;
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}
static void fill_rect(int x, int y, int w, int h, uint16_t c) {
    for (int yy = y; yy < y + h && yy < CRAFT_FB_H; yy++) {
        if (yy < 0) continue;
        for (int xx = x; xx < x + w && xx < CRAFT_FB_W; xx++) {
            if (xx < 0) continue;
            s_fb[yy * CRAFT_FB_W + xx] = c;
        }
    }
}
static void outline_rect(int x, int y, int w, int h, uint16_t c) {
    fill_rect(x,         y,         w, 1, c);
    fill_rect(x,         y + h - 1, w, 1, c);
    fill_rect(x,         y,         1, h, c);
    fill_rect(x + w - 1, y,         1, h, c);
}

static void blit_thumb(int x, int y, int size, const uint16_t *thumb) {
    if (!thumb) return;
    for (int dy = 0; dy < size; dy++) {
        int sy = (dy * CRAFT_SAVE_THUMB_DIM) / size;
        for (int dx = 0; dx < size; dx++) {
            int sx = (dx * CRAFT_SAVE_THUMB_DIM) / size;
            int fx = x + dx, fy = y + dy;
            if ((unsigned)fx >= CRAFT_FB_W) continue;
            if ((unsigned)fy >= CRAFT_FB_H) continue;
            s_fb[fy * CRAFT_FB_W + fx] =
                thumb[sy * CRAFT_SAVE_THUMB_DIM + sx];
        }
    }
}

void craft_title_init(uint16_t *fb) {
    s_fb = fb;
    s_sel = 0;
    s_prev_dpad = false;
    s_prev_a = false;
    s_pending = CRAFT_TITLE_STILL;
    s_chosen_slot = 0;
}

CraftTitleAction craft_title_step(const CraftInput *in) {
    bool dpad_now = in->up || in->down || in->left || in->right;
    if (dpad_now && !s_prev_dpad) {
        /* Layout: slots 0..3 form a 2×2 on top; row 2 is the bottom
         * row — New World alone (solo builds) or New World | Join
         * Friend (co-op builds). */
        int r, c;
        if (s_sel >= TILE_NEW) { r = 2; c = s_sel - TILE_NEW; }
        else                   { r = s_sel / 2; c = s_sel % 2; }
        if (in->up) {
            r = (r > 0) ? r - 1 : 2;
        } else if (in->down) {
            r = (r < 2) ? r + 1 : 0;
        } else if (in->left || in->right) {
            c = (c + 1) & 1;
        }
        if (r == 2) {
#if defined(CRAFT_NET_ENABLED) && CRAFT_NET_ENABLED
            s_sel = TILE_NEW + (c & 1);
#else
            s_sel = TILE_NEW;
#endif
        } else {
            s_sel = r * 2 + c;
        }
    }
    s_prev_dpad = dpad_now;

    /* A confirms. Treat the in->a edge-trigger ourselves so a held A
     * doesn't keep firing once the game loop starts. */
    bool a_now = in->a;
    if (a_now && !s_prev_a) {
        if (s_sel == TILE_NEW) {
            s_pending = CRAFT_TITLE_NEW;
#if defined(CRAFT_NET_ENABLED) && CRAFT_NET_ENABLED
        } else if (s_sel == TILE_JOIN) {
            s_pending = CRAFT_TITLE_JOIN;
#endif
        } else {
            if (craft_save_slot_used(s_sel)) {
                s_chosen_slot = s_sel;
                s_pending     = CRAFT_TITLE_LOAD;
            }
            /* Empty slot + A: no-op (silently ignore). */
        }
    }
    s_prev_a = a_now;
    return s_pending;
}

int craft_title_chosen_slot(void) { return s_chosen_slot; }

void craft_title_draw(void) {
    if (!s_fb) return;
    /* Background — dark navy gradient. */
    for (int y = 0; y < CRAFT_FB_H; y++) {
        int t = y * 255 / CRAFT_FB_H;
        uint16_t c = rgb565_v(10, 12, 30 + t / 4);
        for (int x = 0; x < CRAFT_FB_W; x++) {
            s_fb[y * CRAFT_FB_W + x] = c;
        }
    }
    /* Title text. Subtitle removed — used to push the New World
     * button off the bottom of the 128 px screen. */
    const char *title = "ThumbyCraft";
    int tw = craft_font_width_2x(title);
    craft_font_draw_2x(s_fb, title, (CRAFT_FB_W - tw) / 2, 2,
                       rgb565_v(255, 255, 255));

    /* Slot grid (2x2, 38 px tiles). Sized so the 18 px New World
     * tile underneath fits inside the 128 px screen with margin. */
    int tile     = 38;
    int gap      = 3;
    int grid_w   = 2 * tile + gap;
    int grid_x   = (CRAFT_FB_W - grid_w) / 2;
    int grid_y   = 18;
    for (int s = 0; s < 4; s++) {
        int r = s / 2, c = s % 2;
        int tx = grid_x + c * (tile + gap);
        int ty = grid_y + r * (tile + gap);
        const uint16_t *thumb = craft_save_slot_thumb(s);
        uint16_t tile_bg = thumb ? rgb565_v(30, 30, 40) : 0;
        fill_rect(tx, ty, tile, tile, tile_bg);
        if (thumb) {
            blit_thumb(tx + 2, ty + 2, tile - 4, thumb);
        } else {
            const char *lbl = "Empty";
            int lw = craft_font_width(lbl);
            craft_font_draw(s_fb, lbl, tx + (tile - lw) / 2,
                            ty + tile / 2 - 3,
                            rgb565_v(130, 130, 150));
        }
        outline_rect(tx, ty, tile, tile, rgb565_v(80, 80, 100));
        char nb[4]; snprintf(nb, sizeof nb, "%d", s + 1);
        craft_font_draw(s_fb, nb, tx + 2, ty + 2,
                        rgb565_v(255, 255, 255));
        if (s == s_sel) {
            uint16_t hi = rgb565_v(255, 255, 255);
            outline_rect(tx - 1, ty - 1, tile + 2, tile + 2, hi);
        }
    }

    /* Bottom row: "New World" spanning the panel (solo builds), or
     * "New World" | "Join Friend" side by side (co-op builds). */
    int nw_x = grid_x;
    int nw_y = grid_y + 2 * tile + gap + 2;
    int nw_w = grid_w;
    int nw_h = 18;
#if defined(CRAFT_NET_ENABLED) && CRAFT_NET_ENABLED
    nw_w = (grid_w - gap) / 2;
#endif
    fill_rect(nw_x, nw_y, nw_w, nw_h, rgb565_v(40, 80, 50));
    outline_rect(nw_x, nw_y, nw_w, nw_h, rgb565_v(100, 200, 120));
    const char *nw_label = "New World";
    int lw = craft_font_width(nw_label);
    craft_font_draw(s_fb, nw_label, nw_x + (nw_w - lw) / 2,
                    nw_y + (nw_h - 5) / 2,
                    rgb565_v(255, 255, 255));
    if (s_sel == TILE_NEW) {
        uint16_t hi = rgb565_v(255, 255, 255);
        outline_rect(nw_x - 1, nw_y - 1, nw_w + 2, nw_h + 2, hi);
    }
#if defined(CRAFT_NET_ENABLED) && CRAFT_NET_ENABLED
    {
        int jf_x = nw_x + nw_w + gap;
        int jf_w = grid_w - nw_w - gap;
        fill_rect(jf_x, nw_y, jf_w, nw_h, rgb565_v(40, 60, 90));
        outline_rect(jf_x, nw_y, jf_w, nw_h, rgb565_v(110, 160, 230));
        const char *jf_label = "Join Friend";
        int jlw = craft_font_width(jf_label);
        craft_font_draw(s_fb, jf_label, jf_x + (jf_w - jlw) / 2,
                        nw_y + (nw_h - 5) / 2,
                        rgb565_v(255, 255, 255));
        if (s_sel == TILE_JOIN) {
            uint16_t hi = rgb565_v(255, 255, 255);
            outline_rect(jf_x - 1, nw_y - 1, jf_w + 2, nw_h + 2, hi);
        }
    }
#endif
}

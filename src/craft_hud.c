/*
 * ThumbyCraft — overlay HUD.
 *
 * Hotbar lives at the bottom: 8 slots, each 14×14, framed and shaded.
 * Active slot has a brighter outline. Crosshair is a small 5-pixel
 * cross at the centre. FPS counter optional, top-right.
 */
#include "craft_hud.h"
#include "craft_blocks.h"
#include "craft_font.h"
#include "craft_menu.h"
#include "craft_world.h"

#include <stdio.h>
#include <string.h>

static inline void put(uint16_t *fb, int x, int y, uint16_t c) {
    if ((unsigned)x < CRAFT_FB_W && (unsigned)y < CRAFT_FB_H)
        fb[y * CRAFT_FB_W + x] = c;
}

static void rect(uint16_t *fb, int x, int y, int w, int h, uint16_t c) {
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            put(fb, xx, yy, c);
}

static void rect_outline(uint16_t *fb, int x, int y, int w, int h, uint16_t c) {
    for (int xx = x; xx < x + w; xx++) { put(fb, xx, y, c); put(fb, xx, y + h - 1, c); }
    for (int yy = y; yy < y + h; yy++) { put(fb, x, yy, c); put(fb, x + w - 1, yy, c); }
}

/* Draw a chunky downscaled preview of a block's side texture. */
static void block_swatch(uint16_t *fb, int x, int y, int size, BlockId blk) {
    const uint16_t *tex = craft_block_texture(blk, FACE_PZ);
    for (int dy = 0; dy < size; dy++) {
        for (int dx = 0; dx < size; dx++) {
            int tu = dx * CRAFT_TEX_SIZE / size;
            int tv = dy * CRAFT_TEX_SIZE / size;
            put(fb, x + dx, y + dy, tex[tv * CRAFT_TEX_SIZE + tu]);
        }
    }
}

static void crosshair(uint16_t *fb) {
    int cx = CRAFT_FB_W / 2;
    int cy = CRAFT_FB_H / 2 - 6;   /* hotbar takes up the bottom 14 px */
    uint16_t c = 0xFFFF;
    put(fb, cx,     cy,     c);
    put(fb, cx - 2, cy,     c);
    put(fb, cx + 2, cy,     c);
    put(fb, cx,     cy - 2, c);
    put(fb, cx,     cy + 2, c);
}

void craft_hud_draw(uint16_t *fb, const CraftPlayer *p, int fps) {
    crosshair(fb);

    int slot_w = 14, gap = 1;
    int total = CRAFT_HOTBAR_SLOTS * slot_w + (CRAFT_HOTBAR_SLOTS - 1) * gap;
    int x0 = (CRAFT_FB_W - total) / 2;
    int y0 = CRAFT_FB_H - slot_w - 1;

    /* Background plate */
    rect(fb, x0 - 2, y0 - 1, total + 4, slot_w + 2, rgb565(20, 20, 25));

    for (int i = 0; i < CRAFT_HOTBAR_SLOTS; i++) {
        int sx = x0 + i * (slot_w + gap);
        rect_outline(fb, sx, y0, slot_w, slot_w, rgb565(70, 70, 80));
        if (p->hotbar[i] != BLK_AIR)
            block_swatch(fb, sx + 1, y0 + 1, slot_w - 2, p->hotbar[i]);
        if (i == p->hotbar_idx)
            rect_outline(fb, sx - 1, y0 - 1, slot_w + 2, slot_w + 2, 0xFFFF);
    }

    /* Block name above hotbar when something just happened. */
    BlockId sel = p->hotbar[p->hotbar_idx];
    const char *name = craft_block_name(sel);
    char buf[24];
    if (p->mode == CRAFT_MODE_SURVIVAL && sel != BLK_AIR) {
        snprintf(buf, sizeof buf, "%s x%d", name, p->inventory[sel]);
        name = buf;
    }
    int nw = craft_font_width(name);
    craft_font_draw(fb, name, (CRAFT_FB_W - nw) / 2, y0 - 8, 0xFFFF);

    /* Per-slot count overlay in survival. */
    if (p->mode == CRAFT_MODE_SURVIVAL) {
        for (int i = 0; i < CRAFT_HOTBAR_SLOTS; i++) {
            BlockId b = p->hotbar[i];
            if (b == BLK_AIR) continue;
            int n = p->inventory[b];
            if (n <= 0) continue;
            char cbuf[6];
            snprintf(cbuf, sizeof cbuf, "%d", n);
            int sx = x0 + i * (slot_w + gap);
            craft_font_draw(fb, cbuf, sx + 1, y0 + slot_w - 6, 0xFFFF);
        }
    }

    if (fps > 0) {
        char buf[16];
        snprintf(buf, sizeof buf, "%d", fps);
        craft_font_draw(fb, buf, CRAFT_FB_W - craft_font_width(buf) - 2, 1, 0xFFE0);
    }
    /* Diagnostic: world position + window origin (top-right). */
    {
        char buf[24];
        snprintf(buf, sizeof buf, "%d,%d",
                 (int)p->cam.pos.x, (int)p->cam.pos.z);
        craft_font_draw(fb, buf,
                        CRAFT_FB_W - craft_font_width(buf) - 2, 8,
                        rgb565(180, 220, 255));
        snprintf(buf, sizeof buf, "[%d,%d]",
                 craft_world_origin_x, craft_world_origin_z);
        craft_font_draw(fb, buf,
                        CRAFT_FB_W - craft_font_width(buf) - 2, 15,
                        rgb565(150, 180, 210));
    }
    if (p->fly_mode) {
        craft_font_draw(fb, "FLY", 2, 1, rgb565(120, 220, 255));
    }
    if (p->look_sticky) {
        craft_font_draw(fb, "LOOK", 2, 8, rgb565(255, 255, 100));
    }

    /* Survival mode: HP + hunger + apple count top-left. */
    if (p->mode == CRAFT_MODE_SURVIVAL) {
        char line[20];
        snprintf(line, sizeof line, "HP %d", p->hp);
        uint16_t col = p->hp > 4 ? rgb565(220, 60, 60) : rgb565(255, 100, 100);
        craft_font_draw(fb, line, 2, 1, col);
        snprintf(line, sizeof line, "F %d", p->hunger);
        col = p->hunger >= CRAFT_PLAYER_REGEN_MIN_HUNGER
              ? rgb565(220, 180, 60) : rgb565(180, 100, 60);
        craft_font_draw(fb, line, 2, 8, col);
        if (p->apples > 0) {
            snprintf(line, sizeof line, "A %d", p->apples);
            craft_font_draw(fb, line, 2, 15, rgb565(220, 60, 60));
        }
        if (p->damage_flash > 0.0f) {
            /* Tint the whole framebuffer red briefly. */
            int t = (int)(p->damage_flash * 200.0f);
            if (t > 60) t = 60;
            for (int i = 0; i < CRAFT_FB_W * CRAFT_FB_H; i++) {
                uint16_t c = fb[i];
                int r = (c >> 11) & 0x1F;
                int g = (c >>  5) & 0x3F;
                int b =  c        & 0x1F;
                r += t / 4;
                g = g - t / 8; if (g < 0) g = 0;
                b = b - t / 8; if (b < 0) b = 0;
                if (r > 31) r = 31;
                fb[i] = (uint16_t)((r << 11) | (g << 5) | b);
            }
        }
        if (p->respawn_timer > 0.0f) {
            const char *msg = "YOU DIED";
            int w = craft_font_width_2x(msg);
            craft_font_draw_2x(fb, msg, (CRAFT_FB_W - w) / 2,
                               CRAFT_FB_H / 2 - 8, rgb565(255, 60, 60));
            char rs[20];
            snprintf(rs, sizeof rs, "respawn %.1fs", p->respawn_timer);
            int rw = craft_font_width(rs);
            craft_font_draw(fb, rs, (CRAFT_FB_W - rw) / 2,
                            CRAFT_FB_H / 2 + 8, 0xFFFF);
        }
    }

    /* Toast — centred near bottom (above hotbar). */
    const char *toast = craft_menu_toast_text();
    if (toast) {
        int tw = craft_font_width(toast);
        int tx = (CRAFT_FB_W - tw) / 2;
        int ty = y0 - 18;
        rect(fb, tx - 2, ty - 1, tw + 4, 8, rgb565(0, 0, 0));
        craft_font_draw(fb, toast, tx, ty, rgb565(255, 230, 120));
    }
}

/*
 * ThumbyCraft — text rendering.
 *
 * 3×5 bitmap glyphs in a 4×6 cell, written directly into the 128×128
 * RGB565 framebuffer. The glyph table was originally transcribed by
 * the ThumbyDOOM/ThumbyNES projects from Pemsa (MIT-licensed).
 */
#ifndef CRAFT_FONT_H
#define CRAFT_FONT_H

#include "craft_types.h"

#define CRAFT_FONT_CELL_W 4
#define CRAFT_FONT_CELL_H 6

int craft_font_draw(uint16_t *fb, const char *text, int x, int y, uint16_t color);
int craft_font_draw_2x(uint16_t *fb, const char *text, int x, int y, uint16_t color);
int craft_font_width(const char *text);
int craft_font_width_2x(const char *text);

/* Set the destination dimensions glyph writes are clamped/strided to. Used by
 * the HUD when rendering into a smaller upscale overlay. No-op (and the writer
 * stays a compile-time constant) unless CRAFT_HUD_SCALE>1. */
void craft_font_set_target(int w, int h);

#endif

/*
 * ThumbyCraft — overlay HUD.
 *
 * Crosshair, hotbar strip at the bottom, current block name, optional
 * FPS counter. All drawn directly into the RGB565 framebuffer after
 * the world render.
 */
#ifndef CRAFT_HUD_H
#define CRAFT_HUD_H

#include "craft_types.h"
#include "craft_player.h"

void craft_hud_draw(uint16_t *fb, const CraftPlayer *p, int fps);

/* Just the hotbar strip — used by the pause / craft menu to redraw
 * the hotbar at full brightness over the darkened background. */
void craft_hud_draw_hotbar(uint16_t *fb, const CraftPlayer *p);

#endif

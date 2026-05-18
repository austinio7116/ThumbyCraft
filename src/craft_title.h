/*
 * ThumbyCraft — boot title screen.
 *
 * Runs before craft_main_init so the player picks "New world" or
 * an existing save slot before any world generation work happens.
 * Renders directly into the platform framebuffer using the font +
 * thumbnail data already in flash; needs only craft_blocks_build_textures
 * + the slot APIs to be ready.
 *
 * Usage:
 *   craft_title_init(fb);
 *   while (1) {
 *       poll_input(&in);
 *       CraftTitleAction act = craft_title_step(&in);
 *       craft_title_draw();
 *       present_fb();
 *       if (act != CRAFT_TITLE_STILL) break;
 *   }
 *   if (craft_title_action_is_load()) { ... craft_main_load slot ... }
 *   else                              { ... fresh random seed ... }
 */
#ifndef CRAFT_TITLE_H
#define CRAFT_TITLE_H

#include "craft_types.h"
#include "craft_player.h"   /* for CraftInput */

typedef enum {
    CRAFT_TITLE_STILL    = 0,   /* user hasn't picked yet */
    CRAFT_TITLE_NEW      = 1,   /* New World tile confirmed */
    CRAFT_TITLE_LOAD     = 2,   /* save slot confirmed; query slot via getter */
} CraftTitleAction;

void               craft_title_init(uint16_t *fb);
CraftTitleAction   craft_title_step(const CraftInput *in);
void               craft_title_draw(void);
int                craft_title_chosen_slot(void);

#endif

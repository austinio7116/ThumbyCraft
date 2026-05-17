/*
 * ThumbyCraft — pause menu.
 *
 * Modal overlay drawn on top of a frozen world. While open, gameplay
 * doesn't tick — input routes to the menu instead.
 *
 * The menu is its own little state machine. Caller checks
 * craft_menu_is_open() to decide whether to tick the player or the
 * menu, then calls craft_menu_tick to advance and craft_menu_draw to
 * paint. A return value of CRAFT_MENU_RESULT_* tells the caller what
 * action (if any) the user just confirmed.
 *
 * Phase 18 in ROADMAP.md, shipped early because Phase 9's MENU button
 * needs somewhere to land.
 */
#ifndef CRAFT_MENU_H
#define CRAFT_MENU_H

#include "craft_types.h"
#include "craft_player.h"

typedef enum {
    CRAFT_MENU_RESULT_NONE = 0,    /* no action this tick */
    CRAFT_MENU_RESULT_RESUME,      /* close menu, return to game */
    CRAFT_MENU_RESULT_SAVE,        /* save world */
    CRAFT_MENU_RESULT_LOAD,        /* load most recent save */
    CRAFT_MENU_RESULT_FLY_TOGGLE,  /* toggle player.fly_mode */
    CRAFT_MENU_RESULT_NEW_WORLD,   /* regenerate with new seed */
    CRAFT_MENU_RESULT_INVENTORY,   /* enter inventory sub-screen (Phase 17) */
    CRAFT_MENU_RESULT_INVERT_Y,    /* toggle player.invert_y */
    CRAFT_MENU_RESULT_MUSIC,       /* toggle background music */
    CRAFT_MENU_RESULT_GAME_MODE,   /* toggle creative <-> survival */
    CRAFT_MENU_RESULT_SETTINGS     /* enter settings sub-screen (Phase 19) */
} CraftMenuResult;

/* Open the menu. `in` is the current input snapshot — used to seed
 * edge-trigger state so the next call to craft_menu_tick doesn't
 * misread a still-being-released MENU button as a fresh close. */
void              craft_menu_open(const CraftInput *in);
void              craft_menu_close(void);
bool              craft_menu_is_open(void);
CraftMenuResult   craft_menu_tick(const CraftInput *in,
                                  const CraftPlayer *p);
void              craft_menu_draw(uint16_t *fb,
                                  const CraftPlayer *p);

/* Set a one-line toast that appears at the bottom of the HUD for ~2 s.
 * Caller is the menu / save layer ("World saved", "Load failed", etc). */
void              craft_menu_toast(const char *msg);

/* Tick toasts forward — called every frame regardless of menu state.
 * dt in seconds. */
void              craft_menu_toast_tick(float dt);
const char       *craft_menu_toast_text(void);

#endif

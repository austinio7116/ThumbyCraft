/*
 * ThumbyCraft — game loop entry point.
 *
 * The platform layer (host SDL2 or device bare-metal) initialises
 * its drivers and then calls craft_main_init/craft_main_step in a
 * loop. craft_main_step does one full frame:
 *   1. read pre-built CraftInput
 *   2. tick player + physics
 *   3. render world via raycaster (or render_strip on a strip)
 *   4. draw HUD overlay
 *   5. produce audio samples (caller pulls separately)
 *
 * The platform layer owns: input polling, framebuffer presentation,
 * audio sink, save persistence (sink interface below).
 */
#ifndef CRAFT_MAIN_H
#define CRAFT_MAIN_H

#include <stddef.h>
#include "craft_player.h"

/* Caller-owned framebuffer used for all rendering. Pointer captured
 * for the lifetime of the program by craft_main_init. */
void craft_main_init(uint16_t *fb, uint32_t seed);

/* Optional: load a previously-saved blob. Returns true on success. */
bool craft_main_load(const uint8_t *blob, size_t n);

/* Returns the number of bytes serialised, 0 on failure. Caller decides
 * where to persist the blob (file on host, flash sector on device). */
size_t craft_main_save(uint8_t *out, size_t cap);

/* One full frame. dt in seconds. */
void craft_main_step(const CraftInput *in, float dt, int reported_fps);

/* Multi-phase render split for the device's dual-core renderer.
 *   craft_main_tick(in, dt)        — input + physics on core0
 *   craft_main_render_begin()      — precompute camera basis (core0)
 *   craft_main_render_strip(y0,y1) — called once per core on its strip
 *   craft_main_draw_hud(fps)       — overlay HUD onto the framebuffer
 */
void craft_main_tick(const CraftInput *in, float dt);
void craft_main_render_begin(void);
void craft_main_render_strip(int y_start, int y_end);
void craft_main_draw_hud(int reported_fps);

/* Accessor for the seed (e.g. for save). */
uint32_t craft_main_seed(void);

/* Accessor for the player (HUD draw + save). */
const CraftPlayer *craft_main_player(void);

/* True if the world has unsaved changes. */
bool craft_main_dirty(void);

/* --- Pause menu integration ------------------------------------ *
 * The engine raises a flag when the user activates a menu item.
 * Platform code (host_main / device_main) polls the take_* helpers
 * each tick and performs the heavy lifting (flash write, file
 * write, world regen) outside the engine. craft_menu_toast() lets
 * the platform report success / failure back to the player. */

/* Returns true once if the user asked to save this tick. Auto-clears. */
bool craft_main_take_save_request(void);

/* Returns true once if the user asked to load. Auto-clears. */
bool craft_main_take_load_request(void);

/* Returns true once if the user asked for a new world (random seed).
 * Auto-clears. The engine itself runs the regen. */
bool craft_main_take_new_world_request(void);

/* Current world time in seconds (wraps at DAY_LENGTH). For HUD use. */
float craft_main_world_time(void);

#endif

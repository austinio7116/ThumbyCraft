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

/* Pointer to the 64×64 RGB565 thumbnail captured the last time the
 * player opened the pause menu, or NULL if no menu has opened yet
 * this session. Used by the save layer when committing a slot save. */
const uint16_t *craft_main_thumb(void);

/* Save slot the next save/load request operates on. The slot picker
 * (menu) and title page (boot) set this before raising the request
 * flag; the platform polls both. Default is slot 0. */
void craft_main_set_save_slot(int slot);
int  craft_main_save_slot(void);

/* Chunk-store region currently in use. Slot index 0..3 = a saved
 * world; TBC_REGION_SCRATCH (== TBC_SLOT_COUNT) = unsaved new
 * world. Set by craft_main_init from the title screen / menu. */
void craft_main_set_active_region(int region);
int  craft_main_active_region(void);

/* Set the chunk-store binding nonce for slot N. Platform calls this
 * before save (with the freshly picked seq) and before load (with
 * the seq read from the slot's metadata sector). */
void craft_main_set_slot_nonce(int slot, uint32_t nonce);

/* Auto-save level 1..4 (1=off, 2=5min, 3=2min, 4=30s). Set is
 * called by the menu's A-press handler to cycle the value. */
void        craft_main_set_autosave_level(int level);
int         craft_main_autosave_level(void);
const char *craft_main_autosave_label(void);

/* Control scheme: which input layout the player is using. Selected
 * from the pause menu's Controls page; persisted in the save record
 * (high nibble of HDR_OFF_PAD). Schemes are 1..4 — see
 * craft_player.c for the per-scheme input mapping. */
#define CRAFT_SCHEME_CLASSIC        1   /* LB walk, RB jump, D-pad look */
#define CRAFT_SCHEME_CLASSIC_FLIP   2   /* LB jump, RB walk, D-pad look */
#define CRAFT_SCHEME_DPAD_STRAFE    3   /* D-pad fwd/back/strafe, LB look mod, RB jump */
#define CRAFT_SCHEME_DPAD_TURN      4   /* D-pad fwd/back/turn, LB look mod, RB jump */
/* Console-style mappings: action buttons live on A/B/LB/RB, D-pad
 * is movement, and B-hold puts the D-pad into look mode (same
 * pattern as DPAD_STRAFE/TURN's LB-hold). */
#define CRAFT_SCHEME_CONSOLE_TURN   5   /* D-pad fwd/back/turn, B-hold look mod, A jump, LB place, RB break */
#define CRAFT_SCHEME_CONSOLE_STRAFE 6   /* D-pad fwd/back/strafe, B-hold look mod, A jump, LB place, RB break */
#define CRAFT_SCHEME_MIN            1
#define CRAFT_SCHEME_MAX            6

void        craft_main_set_scheme(int scheme);
int         craft_main_scheme(void);
const char *craft_main_scheme_label(int scheme);    /* short label per scheme */

/* Master output volume — bridged to the cross-slot shared settings
 * mirror on device. The setter persists to the shared store under
 * THUMBYONE_SLOT_MODE so a lobby change shows up next launch and a
 * change here shows up in the lobby + other slots. Range 0.0..1.0. */
void        craft_main_set_master_volume(float vol);
float       craft_main_get_master_volume(void);

/* FPS-display toggle state (read by the menu to draw ON/OFF). */
bool        craft_main_get_show_fps(void);


/* Accessor for the player (HUD draw + save). */
const CraftPlayer *craft_main_player(void);

/* Host mouse-look: add yaw/pitch deltas (radians) to the player camera.
 * No-op concept on device (no pointer). See craft_main.c. */
void        craft_main_look(float dyaw, float dpitch);

/* Host hotbar control (mouse wheel + number keys). */
void        craft_main_hotbar_cycle(int dir);
void        craft_main_hotbar_select(int slot);

/* Host mouse-look sensitivity multiplier (0.2..3.0, default 1.0),
 * driven by the host-only "Mouse sens" menu slider. */
float       craft_main_mouse_sens(void);
void        craft_main_set_mouse_sens(float s);

/* Player's Invert-Y preference (the "Invert Y" menu toggle). Lets a
 * platform's free-look (host mouse / Android touch) match the setting. */
bool        craft_main_get_invert_y(void);

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

/* Returns true once if the user asked to quit back to the ThumbyOne
 * lobby. Slot-mode only; the standalone host build never sets the
 * flag because the "Quit to lobby" menu item is compiled out there. */
bool craft_main_take_quit_to_lobby_request(void);

/* Current world time in seconds (wraps at DAY_LENGTH). For HUD use. */
float craft_main_world_time(void);

#endif

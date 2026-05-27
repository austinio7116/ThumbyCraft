/*
 * ThumbyCraft — game loop orchestrator.
 *
 * Owns the player, the framebuffer, the world clock, the pause-menu
 * state. Hosts the dispatch logic that ticks the right system
 * (gameplay vs menu vs inventory) on each frame and translates
 * menu confirmations into request flags for the platform layer to
 * fulfil.
 */
#include "craft_main.h"
#include "slot_layout.h"
#include <stdio.h>
#include "craft_world.h"
#include "craft_gen.h"
#include "craft_render.h"
#include "craft_hud.h"
#include "craft_audio.h"
#include "craft_save.h"
#include "craft_blocks.h"
#include "craft_menu.h"
#include "craft_mobs.h"
#include "craft_particles.h"
#include "craft_torches.h"
#include "craft_chunk_store.h"
#include "craft_tool_models.h"
#include "craft_drops.h"
#include "craft_furnace.h"
#include "craft_chests.h"
#include "craft_water.h"
#include "craft_lava.h"
#include "craft_redstone.h"

#ifdef THUMBYONE_SLOT_MODE
/* Cross-slot shared volume / brightness store (one flash sector,
 * XIP-mapped; lobby + every slot read/write the same byte). */
#include "thumbyone_settings.h"
#endif

#include <string.h>

#define CRAFT_DAY_LENGTH 300.0f      /* 5 min total cycle. Sun curve
                                      * in craft_render is skewed so
                                      * day occupies 180 s (was 120 s
                                      * in the symmetric 240s cycle)
                                      * and night stays at 120 s,
                                      * matching the original. */

static CraftPlayer s_player;
static uint16_t   *s_fb;
static uint32_t    s_seed;
static float       s_world_time = 60.0f;   /* start at "morning" */

/* Pre-menu screenshot — captured at the moment the player opens the
 * pause menu, downsampled to 64×64 RGB565 so it fits in 8 KB. The
 * save layer reads from here when the player commits a slot save.
 * Captured eagerly on menu-open so the snapshot reflects the last
 * in-game frame, not the menu overlay. */
#define CRAFT_THUMB_W 32
#define CRAFT_THUMB_H 32
static uint16_t s_thumb[CRAFT_THUMB_W * CRAFT_THUMB_H];
static bool     s_thumb_valid;

static void capture_thumb_from_fb(void) {
    if (!s_fb) return;
    /* 4×4 average down 128×128 → 32×32. RGB565 needs unpack/pack. */
    for (int y = 0; y < CRAFT_THUMB_H; y++) {
        for (int x = 0; x < CRAFT_THUMB_W; x++) {
            int sx = x * 4, sy = y * 4;
            int r = 0, g = 0, b = 0;
            for (int dy = 0; dy < 4; dy++) {
                for (int dx = 0; dx < 4; dx++) {
                    uint16_t c = s_fb[(sy + dy) * CRAFT_FB_W + (sx + dx)];
                    r += (c >> 11) & 0x1F;
                    g += (c >> 5)  & 0x3F;
                    b +=  c        & 0x1F;
                }
            }
            r >>= 4; g >>= 4; b >>= 4;
            s_thumb[y * CRAFT_THUMB_W + x] =
                (uint16_t)((r << 11) | (g << 5) | b);
        }
    }
    s_thumb_valid = true;
}

const uint16_t *craft_main_thumb(void) {
    return s_thumb_valid ? s_thumb : NULL;
}

static int s_save_slot = 0;
void craft_main_set_save_slot(int slot) {
    if ((unsigned)slot >= 4) slot = 0;
    s_save_slot = slot;
}
int craft_main_save_slot(void) { return s_save_slot; }

/* Active chunk-store region — the region the engine reads / writes
 * during gameplay. Stays at "none" until craft_main_init binds it
 * (either to scratch for a new world or to a save slot for load).
 *
 * Per-region nonce: chunks in flash carry an embedded nonce; the
 * binding only sees chunks whose nonce matches. SCRATCH's nonce is
 * an in-RAM uint32 that re-randomises on every new-world action —
 * that's what makes "New World" free (no flash erase needed).
 * Save-slot nonces are the slot's metadata seq number; bumping the
 * seq on every save automatically rotates the nonce. */
#define TBC_REGION_NONE  (-1)
static int      s_active_region = TBC_REGION_NONE;
static uint32_t s_scratch_nonce;
static uint32_t s_slot_nonce[TBC_SLOT_COUNT];

extern uint32_t craft_platform_rand32(void);

/* The nonce to use when binding `region`. Slot regions read from
 * s_slot_nonce[], scratch reads from s_scratch_nonce. */
static uint32_t region_nonce(int region) {
    if (region == TBC_REGION_SCRATCH) return s_scratch_nonce;
    if ((unsigned)region < TBC_SLOT_COUNT) return s_slot_nonce[region];
    return 0;
}

void craft_main_set_active_region(int region) {
    s_active_region = region;
    craft_chunk_store_bind(region, region_nonce(region));
}
int  craft_main_active_region(void) { return s_active_region; }

/* Auto-save mode — cycled 1..4 from the menu. Stored in the (was-
 * always-zero) pad byte of the save record so the choice persists
 * across loads.
 *   1 = Off        manual save only. Dirty-queue overflow still
 *                  flushes individual chunks when the 32-entry
 *                  queue fills, but player metadata stays at the
 *                  last explicit save.
 *   2 = Periodic   full save every 60 s. One audible hitch per
 *                  minute; power-cut loses at most ~60 s.
 *   3 = Idle       full save after 5 s of no input + no walking.
 *                  Hides the hitch behind a moment you weren't
 *                  doing anything anyway.
 *   4 = Events     full save on menu open, chest close, day/night
 *                  flip — natural pause points. (DEFAULT.)
 *
 * Auto-save only ticks while we're playing a real save slot
 * (active_region == s_save_slot). Scratch worlds (unsaved new
 * world) don't auto-save because there's no target slot until the
 * user picks one via the explicit Save menu. */
#define AUTOSAVE_OFF        1
#define AUTOSAVE_PERIODIC   2
#define AUTOSAVE_IDLE       3
#define AUTOSAVE_EVENTS     4

#define AUTOSAVE_PERIODIC_SEC   60.0f
#define AUTOSAVE_IDLE_SEC        5.0f
#define AUTOSAVE_DEBOUNCE_SEC    5.0f   /* min gap between auto-saves */

static int   s_autosave_level    = AUTOSAVE_EVENTS;  /* default */
static float s_autosave_timer    = AUTOSAVE_PERIODIC_SEC;
static float s_idle_timer        = 0.0f;
static bool  s_idle_already_fired= false;
static float s_since_last_save   = 999.0f;   /* big initial so first fire isn't blocked */
static bool  s_event_pending     = false;
static float s_last_pos_x        = 0.0f;
static float s_last_pos_z        = 0.0f;
static float s_last_sun_y        = 1.0f;

static const char *autosave_label(int level) {
    switch (level) {
        case AUTOSAVE_PERIODIC: return "60s";
        case AUTOSAVE_IDLE:     return "Idle";
        case AUTOSAVE_EVENTS:   return "Event";
        default:                return "Off";
    }
}

void craft_main_set_autosave_level(int level) {
    if (level < AUTOSAVE_OFF)    level = AUTOSAVE_OFF;
    if (level > AUTOSAVE_EVENTS) level = AUTOSAVE_EVENTS;
    s_autosave_level     = level;
    s_autosave_timer     = AUTOSAVE_PERIODIC_SEC;
    s_idle_timer         = 0.0f;
    s_idle_already_fired = false;
    s_event_pending      = false;
}
int  craft_main_autosave_level(void)  { return s_autosave_level; }
const char *craft_main_autosave_label(void) {
    return autosave_label(s_autosave_level);
}

/* Control scheme state (1..4). Default is CLASSIC — the original
 * input layout — so new worlds and saves predating the scheme
 * field land on the existing behavior with no surprises. */
static int s_scheme = CRAFT_SCHEME_CLASSIC;

void craft_main_set_scheme(int scheme) {
    if (scheme < CRAFT_SCHEME_MIN) scheme = CRAFT_SCHEME_MIN;
    if (scheme > CRAFT_SCHEME_MAX) scheme = CRAFT_SCHEME_MAX;
    s_scheme = scheme;
}
int craft_main_scheme(void) { return s_scheme; }
const char *craft_main_scheme_label(int scheme) {
    switch (scheme) {
        case CRAFT_SCHEME_CLASSIC:        return "Classic";
        case CRAFT_SCHEME_CLASSIC_FLIP:   return "Classic flip";
        case CRAFT_SCHEME_DPAD_STRAFE:    return "Walk + strafe";
        case CRAFT_SCHEME_DPAD_TURN:      return "Walk + turn";
        case CRAFT_SCHEME_CONSOLE_TURN:   return "Console + turn";
        case CRAFT_SCHEME_CONSOLE_STRAFE: return "Console + strafe";
        default:                          return "?";
    }
}

/* Master volume — slot mode persists through the shared mirror so
 * every slot + the lobby see the same value; host build just keeps
 * the live audio gain in memory. */
void craft_main_set_master_volume(float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    craft_audio_set_master_volume(v);
#ifdef THUMBYONE_SLOT_MODE
    uint8_t units = (uint8_t)(v * (float)THUMBYONE_VOLUME_MAX + 0.5f);
    if (units > THUMBYONE_VOLUME_MAX) units = THUMBYONE_VOLUME_MAX;
    thumbyone_settings_save_volume(units);
#endif
}
float craft_main_get_master_volume(void) {
    return craft_audio_get_master_volume();
}

/* Event hooks — call sites in the game logic invoke these to flag
 * a natural pause point. The actual save fires through autosave_tick
 * so all four modes share one debounce window and toast message. */
void craft_main_notify_autosave_event(void) { s_event_pending = true; }

/* Per-tick autosave check is defined further down (after s_save_req
 * comes into scope). Forward declaration here so the rest of this
 * section can reference it without a re-ordering. */
static void autosave_tick(float dt, const CraftInput *in);

/* Fresh random nonce for SCRATCH. Called at boot and on every
 * new-world (in-game or title). Old scratch sectors stay physically
 * on flash but become invisible — find_slot treats them as empty. */
static void scratch_new_nonce(void) {
    s_scratch_nonce = craft_platform_rand32();
    /* Avoid zero / 0xFFFFFFFF which could collide with erased flash
     * or uninitialised stack patterns. Astronomically unlikely but
     * cheap to guard against. */
    if (s_scratch_nonce == 0 || s_scratch_nonce == 0xFFFFFFFFu) {
        s_scratch_nonce = 0xA5A5A5A5u;
    }
}

/* Set slot N's chunk-store nonce. Called by the save flow with the
 * slot's freshly-bumped seq number. */
void craft_main_set_slot_nonce(int slot, uint32_t nonce) {
    if ((unsigned)slot < TBC_SLOT_COUNT) s_slot_nonce[slot] = nonce;
}


/* Flags the platform polls + clears. */
static bool s_save_req;
static bool s_load_req;
static bool s_new_world_req;
static bool s_quit_to_lobby_req;

/* FPS-display toggle. When off, craft_hud_draw is passed fps=0 so the
 * FPS counter + biome letter + world coords debug readout all hide.
 * Default off — clean HUD; players enable it to measure framerate. */
static bool s_show_fps = false;
bool craft_main_get_show_fps(void) { return s_show_fps; }

/* Held-item swing animation — 1.0 right after the player swings,
 * decays linearly back to 0 at ~5/sec. Ticked in craft_main_step /
 * craft_main_tick so both host and device paths stay in sync. */
static float s_held_swing_t = 0.0f;

/* Background chunk-persist drain — fires one flash erase+program
 * every PERSIST_PERIOD seconds. Spreads ~70 ms hitches so they don't
 * bundle into a multi-chunk stutter on window shift.
 *
 * In slot mode the chunk store is on FatFs, where each per-chunk
 * write costs ~30-50 ms with all IRQs masked. The audio PWM
 * peripheral keeps outputting whatever level was last set during
 * that window — when IRQs come back the next sample's value jumps,
 * which reads as a click in the music. Solution: don't schedule
 * background writes at all. Save-only persistence, same as the
 * other ThumbyOne slots. The dirty queue's 32-entry overflow
 * still force-flushes the oldest if you go without saving forever,
 * so progress can't be lost — and force_persist_window on save
 * still drains everything synchronously. */
#ifdef THUMBYONE_SLOT_MODE
#define PERSIST_PERIOD  (1.0e9f)   /* effectively disabled */
#else
#define PERSIST_PERIOD  2.0f
#endif
static float s_persist_timer = PERSIST_PERIOD;

/* RNG helper for new-world seeds.
 *
 * Previously a fixed-seed xorshift LCG that could produce the same
 * sequence across boots — two players hitting "New World" right after
 * power-on would get the same world. Now we pull from the platform's
 * hardware RNG (Pico SDK get_rand_32 on device, time-of-day on host)
 * so every new-world action gets uncorrelated entropy. */
extern uint32_t craft_platform_rand32(void);
static uint32_t next_seed(void) {
    return craft_platform_rand32();
}

void craft_main_init(uint16_t *fb, uint32_t seed) {
    s_fb = fb;
    s_seed = seed;
    craft_world_init();
    /* Pick a fresh scratch nonce on every init so any leftover
     * scratch sectors from a previous power cycle are invisible.
     * Boot path or in-game New World both go through here when the
     * scratch region is the target — slot loads override below. */
    scratch_new_nonce();
    /* If no active region was set up by the platform before init
     * (i.e. boot path skipped set_active_region), default to scratch
     * — host build, fallback paths, etc. */
    if (s_active_region == TBC_REGION_NONE) {
        craft_main_set_active_region(TBC_REGION_SCRATCH);
    } else {
        /* Already bound — re-bind so the new scratch nonce takes
         * effect if the active region is scratch. */
        craft_main_set_active_region(s_active_region);
    }
    craft_blocks_build_textures();
    craft_audio_init();
    /* Ambient "wind" hiss disabled — was barely audible at the
     * original mixer level but the 3× loudness boost made it an
     * obvious continuous hiss. Leave the API in place in case we
     * want zone-based ambient layers later. */
    craft_audio_set_ambient(0.0f);
    craft_audio_music_enable(true);
    craft_audio_music_set_volume(0.5f);
#ifdef THUMBYONE_SLOT_MODE
    /* Bridge master volume to the cross-slot shared mirror. Same
     * byte the lobby and other slots read/write — a change in the
     * lobby's volume slider lands here on next launch. */
    {
        uint8_t v = thumbyone_settings_load_volume();
        if (v > THUMBYONE_VOLUME_MAX) v = THUMBYONE_VOLUME_MAX;
        craft_audio_set_master_volume((float)v / (float)THUMBYONE_VOLUME_MAX);
    }
#endif
    /* Window-loaded around the world origin; spawn point picks a
     * grass tile inside the initial window. */
    craft_world_load_around(0, 0, seed);
    Vec3 spawn = craft_gen_spawn();
    craft_player_init(&s_player, spawn);
    craft_mobs_build_sprites();
    craft_tool_models_init();
    craft_mobs_spawn_around(spawn, seed);
    craft_particles_init();
    craft_drops_init();
    craft_furnace_init();
    craft_chests_init();
    craft_water_init();
    craft_lava_init();
    craft_redstone_init();
    /* Starter chest 2 blocks east of spawn — pre-stocked with a bow
     * and arrows so the player can verify the ranged loop without
     * having to melee a skeleton first. */
    {
        /* spawn.y is eye height: grass_y + 1 + 1.6 → (int)spawn.y is
         * grass_y + 2. Chest sits on the grass at grass_y + 1, so
         * subtract 1 to land on the surface. */
        int chest_x = (int)spawn.x + 2;
        int chest_z = (int)spawn.z;
        int chest_y = (int)spawn.y - 1;
        craft_world_set(chest_x, chest_y, chest_z, BLK_CHEST);
        CraftChest *c = craft_chest_at(chest_x, chest_y, chest_z);
        if (c) {
            /* Full tool sampler for testing — every tier + every
             * material so the user can verify each in the held-item
             * viewport and combat without having to craft them. */
            c->slots[ 0].blk = BLK_BOW;             c->slots[ 0].n = 1;
            c->slots[ 1].blk = BLK_ARROW;           c->slots[ 1].n = 32;
            c->slots[ 2].blk = BLK_PICKAXE_DIAMOND; c->slots[ 2].n = 1;
            c->slots[ 3].blk = BLK_SWORD_DIAMOND;   c->slots[ 3].n = 1;
            c->slots[ 4].blk = BLK_DIAMOND_BLOCK;   c->slots[ 4].n = 4;
            c->slots[ 5].blk = BLK_REDSTONE;        c->slots[ 5].n = 32;
            c->slots[ 6].blk = BLK_LEVER_OFF;       c->slots[ 6].n = 4;
            c->slots[ 7].blk = BLK_PRESSURE_PAD;    c->slots[ 7].n = 4;
            c->slots[ 8].blk = BLK_TORCH;           c->slots[ 8].n = 16;
            c->slots[ 9].blk = BLK_FURNACE;         c->slots[ 9].n = 1;
            c->slots[10].blk = BLK_PISTON_OFF;      c->slots[10].n = 4;
            c->slots[11].blk = BLK_TNT;             c->slots[11].n = 8;
            c->slots[12].blk = BLK_IRON_INGOT;      c->slots[12].n = 8;
            c->slots[13].blk = BLK_LADDER;          c->slots[13].n = 8;
            c->slots[14].blk = BLK_DOOR_OFF;        c->slots[14].n = 4;
            c->slots[15].blk = BLK_TRAPDOOR_OFF;    c->slots[15].n = 4;
        }
    }
    /* Defaults — start in survival with invert-Y on. Player can flip
     * either from the pause menu. */
    s_player.invert_y = true;
    craft_player_set_mode(&s_player, CRAFT_MODE_SURVIVAL);
    craft_mobs_spawn_hostile(&s_player, 4);
}

bool craft_main_load(const uint8_t *blob, size_t n) {
    /* Pre-extract the chunks_nonce from the blob and bind the chunk
     * store BEFORE deserialise — world_load_around inside deserialise
     * reads chunks from the active region with the active nonce, so
     * the binding must be set first. The platform sets the save slot
     * via craft_main_set_save_slot() before calling us. */
    if (n < (CRAFT_SAVE_OFF_CHUNKS_NONCE + 4)) return false;
    uint32_t chunks_nonce = (uint32_t)blob[CRAFT_SAVE_OFF_CHUNKS_NONCE + 0]
                         | ((uint32_t)blob[CRAFT_SAVE_OFF_CHUNKS_NONCE + 1] << 8)
                         | ((uint32_t)blob[CRAFT_SAVE_OFF_CHUNKS_NONCE + 2] << 16)
                         | ((uint32_t)blob[CRAFT_SAVE_OFF_CHUNKS_NONCE + 3] << 24);
    int slot = craft_main_save_slot();
    craft_main_set_slot_nonce(slot, chunks_nonce);
    craft_main_set_active_region(slot);
    uint32_t seed;
    if (!craft_save_deserialise(blob, n, &seed, &s_player)) return false;
    s_seed = seed;
    return true;
}

size_t craft_main_save(uint8_t *out, size_t cap) {
    /* Force-persist every chunk in the window that has mods to the
     * CURRENT active region (scratch if unsaved-new-world, slot N if
     * loaded). This guarantees flash matches SRAM before we either
     * promote (copy to a new slot) or just rewrite metadata. */
    craft_world_chunks_force_persist_window();

    int target_slot = craft_main_save_slot();
    int active = craft_main_active_region();
    uint32_t target_nonce;

    if (active != target_slot) {
        /* Promote scratch (or another slot) into target_slot. Pick
         * a FRESH random nonce — old stale sectors in the target
         * slot's region (from a previous save) stay invisible
         * under the new binding. */
        target_nonce = craft_platform_rand32();
        if (target_nonce == 0 || target_nonce == 0xFFFFFFFFu) {
            target_nonce = 0xC3C3C3C3u;
        }
        craft_chunk_store_copy(active, region_nonce(active),
                               target_slot, target_nonce);
        craft_main_set_slot_nonce(target_slot, target_nonce);
        craft_main_set_active_region(target_slot);
    } else {
        /* In-place save: chunks are already live in target_slot
         * under the slot's existing nonce. Keep the same nonce so
         * the chunks remain readable on next load. */
        target_nonce = region_nonce(target_slot);
    }

    return craft_save_serialise(s_seed, target_nonce,
                                (uint8_t)s_autosave_level,
                                &s_player, out, cap);
}

bool craft_main_take_save_request(void) {
    bool r = s_save_req; s_save_req = false; return r;
}

static void capture_thumb_from_fb(void);   /* defined below */

static void fire_autosave(void) {
    capture_thumb_from_fb();
    s_save_req = true;
    s_since_last_save = 0.0f;
}

static void autosave_tick(float dt, const CraftInput *in) {
    s_since_last_save += dt;

    /* Track activity for the Idle mode — any button press OR a
     * change in player XZ position counts as "the player did
     * something". Y can change from gravity even when standing
     * still, so XZ-only is the right gate. */
    bool any_input = in && (in->up || in->down || in->left || in->right ||
                            in->a  || in->b  || in->lb   || in->rb   ||
                            in->menu);
    bool moving = (s_player.cam.pos.x != s_last_pos_x) ||
                  (s_player.cam.pos.z != s_last_pos_z);
    s_last_pos_x = s_player.cam.pos.x;
    s_last_pos_z = s_player.cam.pos.z;
    if (any_input || moving) {
        s_idle_timer = 0.0f;
        s_idle_already_fired = false;
    } else {
        s_idle_timer += dt;
    }

    /* Detect day/night-phase flips for the Events mode. sun_y > 0
     * is daytime, < 0 is night; crossing zero is a sunrise or sunset. */
    float sun_y = craft_render_sun_y();
    if ((sun_y >= 0.0f) != (s_last_sun_y >= 0.0f)) {
        s_event_pending = true;
    }
    s_last_sun_y = sun_y;

    /* Detect menu-open / menu-close transitions. Either edge counts
     * as an event — opening the menu is a natural pause moment;
     * closing it (returning from chest / inventory / craft / etc.)
     * is the "leave a chest UI" case the user called out. */
    static bool was_menu_open = false;
    bool is_menu_open = craft_menu_is_open();
    if (is_menu_open != was_menu_open) {
        s_event_pending = true;
        was_menu_open = is_menu_open;
    }

    if (s_autosave_level == AUTOSAVE_OFF)         return;
    if (s_active_region != s_save_slot)            return;
    if ((unsigned)s_save_slot >= TBC_SLOT_COUNT)   return;
    if (s_since_last_save < AUTOSAVE_DEBOUNCE_SEC) return;

    switch (s_autosave_level) {
        case AUTOSAVE_PERIODIC: {
            s_autosave_timer -= dt;
            if (s_autosave_timer <= 0.0f) {
                s_autosave_timer = AUTOSAVE_PERIODIC_SEC;
                fire_autosave();
            }
            break;
        }
        case AUTOSAVE_IDLE: {
            if (s_idle_timer >= AUTOSAVE_IDLE_SEC && !s_idle_already_fired) {
                s_idle_already_fired = true;
                fire_autosave();
            }
            break;
        }
        case AUTOSAVE_EVENTS: {
            if (s_event_pending) {
                s_event_pending = false;
                fire_autosave();
            }
            break;
        }
        default: break;
    }
}
bool craft_main_take_load_request(void) {
    bool r = s_load_req; s_load_req = false; return r;
}
bool craft_main_take_new_world_request(void) {
    bool r = s_new_world_req; s_new_world_req = false; return r;
}
bool craft_main_take_quit_to_lobby_request(void) {
    bool r = s_quit_to_lobby_req; s_quit_to_lobby_req = false; return r;
}

float craft_main_world_time(void) { return s_world_time; }

/* Bump the held-item swing animation in response to a just-completed
 * player tick. broke_block / placed_block are flagged by player_tick
 * for one frame each; either kick starts a swing. The cooldown decays
 * at ~5/sec so a typical swing visible-window is ~200 ms. */
static void held_swing_tick_after_player(float dt) {
    if (s_player.broke_block || s_player.placed_block) s_held_swing_t = 1.0f;
    s_held_swing_t -= dt * 5.0f;
    if (s_held_swing_t < 0.0f) s_held_swing_t = 0.0f;
}

/* Translate a menu confirmation into actions. */
static void handle_menu_result(CraftMenuResult r) {
    switch (r) {
        case CRAFT_MENU_RESULT_NONE:
        case CRAFT_MENU_RESULT_RESUME:
            break;
        case CRAFT_MENU_RESULT_SAVE:
            s_save_req = true;
            break;
        case CRAFT_MENU_RESULT_LOAD:
            s_load_req = true;
            break;
        case CRAFT_MENU_RESULT_FLY_TOGGLE:
            s_player.fly_mode = !s_player.fly_mode;
            s_player.vel = v3(0, 0, 0);
            craft_menu_toast(s_player.fly_mode ? "Fly mode ON" : "Fly mode OFF");
            break;
        case CRAFT_MENU_RESULT_INVERT_Y:
            s_player.invert_y = !s_player.invert_y;
            craft_menu_toast(s_player.invert_y ? "Invert Y ON" : "Invert Y OFF");
            break;
        case CRAFT_MENU_RESULT_FAR_LOD: {
            bool on = !craft_render_get_far_lod();
            craft_render_set_far_lod(on);
            craft_menu_toast(on ? "Far LOD ON" : "Far LOD OFF");
            break;
        }
        case CRAFT_MENU_RESULT_GROUND_COVER: {
            bool on = !craft_render_get_groundcover();
            craft_render_set_groundcover(on);
            craft_menu_toast(on ? "Ground cover ON" : "Ground cover OFF");
            break;
        }
        case CRAFT_MENU_RESULT_INTERLACE: {
            bool on = !craft_render_get_interlace();
            craft_render_set_interlace(on);
            craft_menu_toast(on ? "Interlace ON" : "Interlace OFF");
            break;
        }
        case CRAFT_MENU_RESULT_LOWRES: {
            bool on = !craft_render_get_lowres();
            craft_render_set_lowres(on);
            craft_menu_toast(on ? "Low-res ON" : "Low-res OFF");
            break;
        }
        case CRAFT_MENU_RESULT_TORCH_LIGHT: {
            bool on = !craft_render_get_torch_light();
            craft_render_set_torch_light(on);
            craft_menu_toast(on ? "Torch light ON" : "Torch light OFF");
            break;
        }
        case CRAFT_MENU_RESULT_SHOW_FPS: {
            s_show_fps = !s_show_fps;
            craft_menu_toast(s_show_fps ? "FPS ON" : "FPS OFF");
            break;
        }
        case CRAFT_MENU_RESULT_QUIT_TO_LOBBY:
            s_quit_to_lobby_req = true;
            break;
        case CRAFT_MENU_RESULT_MUSIC: {
            bool on = !craft_audio_music_is_enabled();
            craft_audio_music_enable(on);
            craft_menu_toast(on ? "Music ON" : "Music OFF");
            break;
        }
        case CRAFT_MENU_RESULT_GAME_MODE: {
            CraftGameMode m = (s_player.mode == CRAFT_MODE_CREATIVE)
                              ? CRAFT_MODE_SURVIVAL : CRAFT_MODE_CREATIVE;
            craft_player_set_mode(&s_player, m);
            if (m == CRAFT_MODE_SURVIVAL) {
                craft_mobs_spawn_hostile(&s_player, 3);
                craft_menu_toast("Survival mode");
            } else {
                craft_menu_toast("Creative mode");
            }
            break;
        }
        case CRAFT_MENU_RESULT_NEW_WORLD: {
            /* Capture settings BEFORE player_init memsets them away. The
             * previous version of this code captured AFTER init and so
             * always read the freshly-zeroed defaults — that's why
             * settings appeared to reset on every new world. */
            CraftGameMode mode_was = s_player.mode;
            bool          inv_was  = s_player.invert_y;
            bool          music_was = craft_audio_music_is_enabled();

            uint32_t ns = next_seed();
            s_seed = ns;

            /* New world goes into the scratch region. With the
             * nonce filter, "wipe scratch" reduces to picking a
             * fresh random nonce — old scratch sectors stay
             * physically on flash but become invisible. Save slots
             * are untouched and only get overwritten on an
             * explicit Save → slot N. */
            scratch_new_nonce();
            craft_main_set_active_region(TBC_REGION_SCRATCH);
            craft_world_reset_mods();
            craft_chests_init();
            craft_furnace_init();
            craft_water_init();
            craft_lava_init();
            craft_redstone_init();
            craft_drops_init();
            craft_particles_init();

            craft_world_load_around(0, 0, ns);
            Vec3 sp = craft_gen_spawn();
            craft_player_init(&s_player, sp);
            craft_mobs_spawn_around(sp, ns);

            /* Restore preserved settings. */
            craft_player_set_mode(&s_player, mode_was);
            s_player.invert_y = inv_was;
            craft_audio_music_enable(music_was);
            if (mode_was == CRAFT_MODE_SURVIVAL)
                craft_mobs_spawn_hostile(&s_player, 3);
            s_world_time = 60.0f;
            craft_menu_toast("New world");
            break;
        }
        case CRAFT_MENU_RESULT_INVENTORY:
        case CRAFT_MENU_RESULT_CRAFT:
        case CRAFT_MENU_RESULT_RECIPES:
        case CRAFT_MENU_RESULT_CONTROLS:
            /* Page switch handled inside the menu itself — nothing
             * for the host to do here. */
            break;
        case CRAFT_MENU_RESULT_AUTOSAVE: {
            /* A-press cycles the level 1 → 2 → 3 → 4 → 1. The menu
             * stays open and the row redraws with the new label. */
            int next = s_autosave_level + 1;
            if (next > 4) next = 1;
            craft_main_set_autosave_level(next);
            char buf[24];
            snprintf(buf, sizeof buf, "Auto save: %s",
                     craft_main_autosave_label());
            craft_menu_toast(buf);
            break;
        }
    }
}

void craft_main_step(const CraftInput *in, float dt, int fps) {
    craft_menu_toast_tick(dt);
    if (craft_menu_is_open()) {
        CraftMenuResult r = craft_menu_tick(in, &s_player);
        handle_menu_result(r);
        s_player.cam.pos.y -= s_player.step_lag;
        craft_render_set_time(s_world_time);
        craft_render_begin(&s_player.cam);
        craft_render_strip(&s_player.cam, s_fb, 0, CRAFT_FB_H);
        craft_render_stars(&s_player.cam, s_fb);
        craft_render_celestials(&s_player.cam, s_fb);
        craft_mobs_render(&s_player.cam, s_fb);
        craft_arrows_render(&s_player.cam, s_fb);
        craft_drops_render(&s_player.cam, s_fb);
        craft_torches_render(&s_player.cam, s_fb);
        craft_particles_render(&s_player.cam, s_fb);
        craft_render_pick_outline(&s_player.cam, s_fb);
        craft_hud_draw(s_fb, &s_player, s_show_fps ? fps : 0);
        craft_menu_draw(s_fb, &s_player);
        s_player.cam.pos.y += s_player.step_lag;
        return;
    }
    s_world_time += dt;
    if (s_world_time >= CRAFT_DAY_LENGTH) s_world_time -= CRAFT_DAY_LENGTH;
    /* Reset the pressed-pad set; the player + mob ticks below re-report
     * any pads they're standing on this frame. */
    craft_redstone_pads_clear();
    craft_player_tick(&s_player, in, dt);
    held_swing_tick_after_player(dt);
    craft_world_maybe_shift((int)s_player.cam.pos.x,
                            (int)s_player.cam.pos.z, s_seed);
    s_persist_timer -= dt;
    if (s_persist_timer <= 0.0f) {
        craft_world_persist_tick();
        s_persist_timer = PERSIST_PERIOD;
    }
    autosave_tick(dt, in);
    if (s_player.broke_block) {
        Vec3 centre = v3((float)s_player.last_action_x + 0.5f,
                         (float)s_player.last_action_y + 0.5f,
                         (float)s_player.last_action_z + 0.5f);
        craft_particles_emit_break(centre, s_player.last_block_touched);
    }
    craft_particles_tick(dt);
    craft_mobs_tick(dt, &s_player);
    craft_arrows_tick(dt, &s_player);
    craft_drops_tick(dt, &s_player);
    craft_furnace_tick(dt);
    craft_water_tick(dt);
    craft_lava_tick(dt);
    craft_redstone_tick(dt);
    craft_redstone_tick_fuses(dt);
    craft_mobs_day_night_tick(dt, craft_render_sun_y(), &s_player);
    craft_audio_music_set_sun(craft_render_sun_y());
    craft_audio_music_set_altitude(s_player.cam.pos.y / (float)CRAFT_WORLD_Y);
    craft_audio_music_tick(dt);
    craft_blocks_animate_water(s_world_time);
    if (s_player.request_menu) {
        s_player.request_menu = false;
        /* Grab a 64×64 thumbnail of the last in-game frame before
         * the menu overlays it — the slot picker reads from this
         * when the player commits a save. */
        capture_thumb_from_fb();
        craft_menu_open(in);
    }
    if (s_player.request_furnace_open) {
        s_player.request_furnace_open = false;
        craft_menu_open_furnace(in,
            s_player.furnace_open_x,
            s_player.furnace_open_y,
            s_player.furnace_open_z);
    }
    if (s_player.request_chest_open) {
        s_player.request_chest_open = false;
        int cx = s_player.chest_open_x;
        int cy = s_player.chest_open_y;
        int cz = s_player.chest_open_z;
        /* Seed hut chest loot on first touch. craft_chest_find returns
         * NULL only if no state record exists yet → fresh open. We
         * pre-create the record and populate it before handing off
         * to the menu so the player sees the loot on this open. */
        if (!craft_chest_find(cx, cy, cz) &&
            (craft_gen_is_hut_chest(cx, cy, cz, s_seed) ||
             craft_gen_is_dungeon_chest(cx, cy, cz, s_seed))) {
            CraftChest *hc = craft_chest_at(cx, cy, cz);
            if (hc) craft_gen_seed_hut_chest(hc, cx, cy, cz, s_seed);
        }
        craft_menu_open_chest(in, cx, cy, cz);
    }
    if (s_player.request_fly_toast) {
        s_player.request_fly_toast = false;
        craft_menu_toast(s_player.fly_mode ? "Fly mode ON" : "Fly mode OFF");
    }
    s_player.cam.pos.y -= s_player.step_lag;
    craft_render_set_time(s_world_time);
    craft_render_begin(&s_player.cam);
    craft_render_strip(&s_player.cam, s_fb, 0, CRAFT_FB_H);
    craft_render_stars(&s_player.cam, s_fb);
    craft_render_celestials(&s_player.cam, s_fb);
    craft_mobs_render(&s_player.cam, s_fb);
    craft_arrows_render(&s_player.cam, s_fb);
    craft_drops_render(&s_player.cam, s_fb);
    craft_torches_render(&s_player.cam, s_fb);
    craft_particles_render(&s_player.cam, s_fb);
    craft_render_pick_outline(&s_player.cam, s_fb);
    /* Held item overlay sits on top of the world but UNDER the HUD —
     * the hotbar must remain visible over the held-item viewport so
     * you can see which slot you're holding. */
    craft_render_held_item(s_player.hotbar[s_player.hotbar_idx],
                           s_fb, s_held_swing_t, s_player.bow_draw_t);
    craft_hud_draw(s_fb, &s_player, s_show_fps ? fps : 0);
    s_player.cam.pos.y += s_player.step_lag;
}

void craft_main_tick(const CraftInput *in, float dt) {
    craft_menu_toast_tick(dt);
    if (craft_menu_is_open()) {
        CraftMenuResult r = craft_menu_tick(in, &s_player);
        handle_menu_result(r);
        return;
    }
    s_world_time += dt;
    if (s_world_time >= CRAFT_DAY_LENGTH) s_world_time -= CRAFT_DAY_LENGTH;
    /* Reset the pressed-pad set; the player + mob ticks below re-report
     * any pads they're standing on this frame. */
    craft_redstone_pads_clear();
    craft_player_tick(&s_player, in, dt);
    held_swing_tick_after_player(dt);
    craft_world_maybe_shift((int)s_player.cam.pos.x,
                            (int)s_player.cam.pos.z, s_seed);
    s_persist_timer -= dt;
    if (s_persist_timer <= 0.0f) {
        craft_world_persist_tick();
        s_persist_timer = PERSIST_PERIOD;
    }
    autosave_tick(dt, in);
    if (s_player.broke_block) {
        Vec3 centre = v3((float)s_player.last_action_x + 0.5f,
                         (float)s_player.last_action_y + 0.5f,
                         (float)s_player.last_action_z + 0.5f);
        craft_particles_emit_break(centre, s_player.last_block_touched);
    }
    craft_particles_tick(dt);
    craft_mobs_tick(dt, &s_player);
    craft_arrows_tick(dt, &s_player);
    craft_drops_tick(dt, &s_player);
    craft_furnace_tick(dt);
    craft_water_tick(dt);
    craft_lava_tick(dt);
    craft_redstone_tick(dt);
    craft_redstone_tick_fuses(dt);
    craft_mobs_day_night_tick(dt, craft_render_sun_y(), &s_player);
    craft_audio_music_set_sun(craft_render_sun_y());
    craft_audio_music_set_altitude(s_player.cam.pos.y / (float)CRAFT_WORLD_Y);
    craft_audio_music_tick(dt);
    craft_blocks_animate_water(s_world_time);
    if (s_player.request_menu) {
        s_player.request_menu = false;
        /* Grab a 64×64 thumbnail of the last in-game frame before
         * the menu overlays it — the slot picker reads from this
         * when the player commits a save. */
        capture_thumb_from_fb();
        craft_menu_open(in);
    }
    if (s_player.request_furnace_open) {
        s_player.request_furnace_open = false;
        craft_menu_open_furnace(in,
            s_player.furnace_open_x,
            s_player.furnace_open_y,
            s_player.furnace_open_z);
    }
    if (s_player.request_chest_open) {
        s_player.request_chest_open = false;
        int cx = s_player.chest_open_x;
        int cy = s_player.chest_open_y;
        int cz = s_player.chest_open_z;
        /* Seed hut chest loot on first touch. craft_chest_find returns
         * NULL only if no state record exists yet → fresh open. We
         * pre-create the record and populate it before handing off
         * to the menu so the player sees the loot on this open. */
        if (!craft_chest_find(cx, cy, cz) &&
            (craft_gen_is_hut_chest(cx, cy, cz, s_seed) ||
             craft_gen_is_dungeon_chest(cx, cy, cz, s_seed))) {
            CraftChest *hc = craft_chest_at(cx, cy, cz);
            if (hc) craft_gen_seed_hut_chest(hc, cx, cy, cz, s_seed);
        }
        craft_menu_open_chest(in, cx, cy, cz);
    }
    if (s_player.request_fly_toast) {
        s_player.request_fly_toast = false;
        craft_menu_toast(s_player.fly_mode ? "Fly mode ON" : "Fly mode OFF");
    }
}
void craft_main_render_begin(void) {
    /* Apply the auto-step camera lag: render sees cam.pos.y slightly
     * below the player's logical y so the post-step camera rise is
     * smooth rather than instantaneous. Restored at the end of
     * craft_main_draw_hud so physics next tick uses the logical y. */
    s_player.cam.pos.y -= s_player.step_lag;
    craft_render_set_time(s_world_time);
    /* Held-torch lighting: active only when the option is on AND the
     * selected hotbar item is a torch. Recomputed per frame so it lights
     * up the instant the player scrolls to a torch and goes dark again
     * when they scroll away — all at render time, no lightmap work. */
    craft_render_set_player_light(
        craft_render_get_torch_light() &&
        s_player.hotbar[s_player.hotbar_idx] == BLK_TORCH);
    craft_render_begin(&s_player.cam);
}
void craft_main_render_strip(int y_start, int y_end) {
    craft_render_strip(&s_player.cam, s_fb, y_start, y_end);
}
void craft_main_draw_hud(int fps) {
    craft_render_stars(&s_player.cam, s_fb);
    craft_render_celestials(&s_player.cam, s_fb);
    craft_mobs_render(&s_player.cam, s_fb);
    craft_arrows_render(&s_player.cam, s_fb);
    craft_drops_render(&s_player.cam, s_fb);
    /* Pre-compute the picker hit so the sprite render can highlight
     * the targeted cell in a brighter tint. The render_pick_outline
     * call below uses the same trace result but only for the
     * outline; cheap (one trace per frame). */
    {
        CraftRayHit ph = craft_render_pick(&s_player.cam);
        bool en = ph.hit && ph.distance <= 8.0f;
        craft_torches_set_highlight(ph.bx, ph.by, ph.bz, en);
    }
    craft_torches_render(&s_player.cam, s_fb);
    craft_particles_render(&s_player.cam, s_fb);
    craft_render_pick_outline(&s_player.cam, s_fb);
    /* Held item overlay sits under the hotbar — the active-slot
     * indicator must stay visible on top of the viewport. */
    craft_render_held_item(s_player.hotbar[s_player.hotbar_idx],
                           s_fb, s_held_swing_t, s_player.bow_draw_t);
    craft_hud_draw(s_fb, &s_player, s_show_fps ? fps : 0);
    if (craft_menu_is_open()) craft_menu_draw(s_fb, &s_player);
    /* Restore logical cam y so next tick's physics is correct. */
    s_player.cam.pos.y += s_player.step_lag;
}

uint32_t craft_main_seed(void) { return s_seed; }
const CraftPlayer *craft_main_player(void) { return &s_player; }

/* Mouse-look hook for the host build: add yaw/pitch deltas (radians)
 * straight to the player camera. The device has no pointer, so this is
 * unused there; the host feeds relative mouse motion here each frame
 * (with the DPAD_STRAFE scheme handling WASD movement), giving proper
 * FPS-style look. Pitch is clamped to the same ±85° the player tick uses. */
void craft_main_look(float dyaw, float dpitch) {
    s_player.cam.yaw   += dyaw;
    s_player.cam.pitch += dpitch;
    const float pmax = 85.0f * 3.14159265f / 180.0f;
    if (s_player.cam.pitch >  pmax) s_player.cam.pitch =  pmax;
    if (s_player.cam.pitch < -pmax) s_player.cam.pitch = -pmax;
}

/* Host-only mouse-look sensitivity multiplier (1.0 = default). Adjusted
 * by the "Mouse sens" menu slider; the host multiplies its base
 * radians-per-pixel by this. Not persisted — a host dev convenience. */
static float s_mouse_sens = 1.0f;
float craft_main_mouse_sens(void) { return s_mouse_sens; }
void  craft_main_set_mouse_sens(float s) {
    if (s < 0.2f) s = 0.2f; else if (s > 3.0f) s = 3.0f;
    s_mouse_sens = s;
}

/* Host hotbar selection (Minecraft-style mouse wheel + number keys). The
 * device cycles the hotbar with a MENU+LB/RB chord; the host drives the
 * index directly here instead. cycle(±1) wraps; select(n) jumps. */
void craft_main_hotbar_cycle(int dir) {
    int n = CRAFT_HOTBAR_SLOTS;
    s_player.hotbar_idx = (s_player.hotbar_idx + (dir % n) + n) % n;
}
void craft_main_hotbar_select(int slot) {
    if (slot >= 0 && slot < CRAFT_HOTBAR_SLOTS) s_player.hotbar_idx = slot;
}
bool craft_main_dirty(void) { return craft_world_dirty != 0; }

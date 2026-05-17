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

#include <string.h>

#define CRAFT_DAY_LENGTH 240.0f      /* 4 minutes */

static CraftPlayer s_player;
static uint16_t   *s_fb;
static uint32_t    s_seed;
static float       s_world_time = 60.0f;   /* start at "morning" */

/* Flags the platform polls + clears. */
static bool s_save_req;
static bool s_load_req;
static bool s_new_world_req;

/* RNG helper for new-world seeds. Cheap LCG seeded from time
 * accumulator + xorshift, since we don't have rand() everywhere. */
static uint32_t s_rng = 0x12345678u;
static uint32_t next_seed(void) {
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng ^ (uint32_t)(s_world_time * 1000.0f);
}

void craft_main_init(uint16_t *fb, uint32_t seed) {
    s_fb = fb;
    s_seed = seed;
    s_rng ^= seed;
    craft_world_init();
    craft_blocks_build_textures();
    craft_audio_init();
    /* Ambient "wind" hiss disabled — was barely audible at the
     * original mixer level but the 3× loudness boost made it an
     * obvious continuous hiss. Leave the API in place in case we
     * want zone-based ambient layers later. */
    craft_audio_set_ambient(0.0f);
    craft_audio_music_enable(true);
    craft_audio_music_set_volume(0.5f);
    /* Window-loaded around the world origin; spawn point picks a
     * grass tile inside the initial window. */
    craft_world_load_around(0, 0, seed);
    Vec3 spawn = craft_gen_spawn();
    craft_player_init(&s_player, spawn);
    craft_mobs_build_sprites();
    craft_mobs_spawn_around(spawn, seed);
    craft_particles_init();
    /* Defaults — start in survival with invert-Y on. Player can flip
     * either from the pause menu. */
    s_player.invert_y = true;
    craft_player_set_mode(&s_player, CRAFT_MODE_SURVIVAL);
    craft_mobs_spawn_hostile(&s_player, 4);
}

bool craft_main_load(const uint8_t *blob, size_t n) {
    uint32_t seed;
    if (!craft_save_deserialise(blob, n, &seed, &s_player)) return false;
    s_seed = seed;
    return true;
}

size_t craft_main_save(uint8_t *out, size_t cap) {
    return craft_save_serialise(s_seed, &s_player, out, cap);
}

bool craft_main_take_save_request(void) {
    bool r = s_save_req; s_save_req = false; return r;
}
bool craft_main_take_load_request(void) {
    bool r = s_load_req; s_load_req = false; return r;
}
bool craft_main_take_new_world_request(void) {
    bool r = s_new_world_req; s_new_world_req = false; return r;
}

float craft_main_world_time(void) { return s_world_time; }

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
            uint32_t ns = next_seed();
            s_seed = ns;
            craft_world_load_around(0, 0, ns);
            Vec3 sp = craft_gen_spawn();
            craft_player_init(&s_player, sp);
            craft_mobs_spawn_around(sp, ns);
            /* Preserve current mode + invert preference across regen. */
            CraftGameMode mode_was = s_player.mode;
            bool inv_was = s_player.invert_y;
            craft_player_set_mode(&s_player, mode_was);
            s_player.invert_y = inv_was;
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
        case CRAFT_MENU_RESULT_SETTINGS:
            craft_menu_toast("Settings: soon");
            break;
    }
}

void craft_main_step(const CraftInput *in, float dt, int fps) {
    craft_menu_toast_tick(dt);
    if (craft_menu_is_open()) {
        CraftMenuResult r = craft_menu_tick(in, &s_player);
        handle_menu_result(r);
        craft_render_set_time(s_world_time);
        craft_render_begin(&s_player.cam);
        craft_render_strip(&s_player.cam, s_fb, 0, CRAFT_FB_H);
        craft_render_stars(&s_player.cam, s_fb);
        craft_render_celestials(&s_player.cam, s_fb);
        craft_mobs_render(&s_player.cam, s_fb);
        craft_torches_render(&s_player.cam, s_fb);
        craft_particles_render(&s_player.cam, s_fb);
        craft_render_pick_outline(&s_player.cam, s_fb);
        craft_hud_draw(s_fb, &s_player, fps);
        craft_menu_draw(s_fb, &s_player);
        return;
    }
    s_world_time += dt;
    if (s_world_time >= CRAFT_DAY_LENGTH) s_world_time -= CRAFT_DAY_LENGTH;
    craft_player_tick(&s_player, in, dt);
    craft_world_maybe_shift((int)s_player.cam.pos.x,
                            (int)s_player.cam.pos.z, s_seed);
    if (s_player.broke_block) {
        Vec3 centre = v3((float)s_player.last_action_x + 0.5f,
                         (float)s_player.last_action_y + 0.5f,
                         (float)s_player.last_action_z + 0.5f);
        craft_particles_emit_break(centre, s_player.last_block_touched);
    }
    craft_particles_tick(dt);
    craft_mobs_tick(dt, &s_player);
    craft_mobs_day_night_tick(dt, craft_render_sun_y(), &s_player);
    craft_audio_music_set_sun(craft_render_sun_y());
    craft_audio_music_tick(dt);
    craft_blocks_animate_water(s_world_time);
    if (s_player.request_menu) {
        s_player.request_menu = false;
        craft_menu_open(in);
    }
    if (s_player.request_fly_toast) {
        s_player.request_fly_toast = false;
        craft_menu_toast(s_player.fly_mode ? "Fly mode ON" : "Fly mode OFF");
    }
    craft_render_set_time(s_world_time);
    craft_render_begin(&s_player.cam);
    craft_render_strip(&s_player.cam, s_fb, 0, CRAFT_FB_H);
    craft_render_stars(&s_player.cam, s_fb);
    craft_render_celestials(&s_player.cam, s_fb);
    craft_mobs_render(&s_player.cam, s_fb);
        craft_torches_render(&s_player.cam, s_fb);
    craft_particles_render(&s_player.cam, s_fb);
    craft_render_pick_outline(&s_player.cam, s_fb);
    craft_hud_draw(s_fb, &s_player, fps);
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
    craft_player_tick(&s_player, in, dt);
    craft_world_maybe_shift((int)s_player.cam.pos.x,
                            (int)s_player.cam.pos.z, s_seed);
    if (s_player.broke_block) {
        Vec3 centre = v3((float)s_player.last_action_x + 0.5f,
                         (float)s_player.last_action_y + 0.5f,
                         (float)s_player.last_action_z + 0.5f);
        craft_particles_emit_break(centre, s_player.last_block_touched);
    }
    craft_particles_tick(dt);
    craft_mobs_tick(dt, &s_player);
    craft_mobs_day_night_tick(dt, craft_render_sun_y(), &s_player);
    craft_audio_music_set_sun(craft_render_sun_y());
    craft_audio_music_tick(dt);
    craft_blocks_animate_water(s_world_time);
    if (s_player.request_menu) {
        s_player.request_menu = false;
        craft_menu_open(in);
    }
    if (s_player.request_fly_toast) {
        s_player.request_fly_toast = false;
        craft_menu_toast(s_player.fly_mode ? "Fly mode ON" : "Fly mode OFF");
    }
}
void craft_main_render_begin(void) {
    craft_render_set_time(s_world_time);
    craft_render_begin(&s_player.cam);
}
void craft_main_render_strip(int y_start, int y_end) {
    craft_render_strip(&s_player.cam, s_fb, y_start, y_end);
}
void craft_main_draw_hud(int fps) {
    craft_render_stars(&s_player.cam, s_fb);
    craft_render_celestials(&s_player.cam, s_fb);
    craft_mobs_render(&s_player.cam, s_fb);
        craft_torches_render(&s_player.cam, s_fb);
    craft_particles_render(&s_player.cam, s_fb);
    craft_render_pick_outline(&s_player.cam, s_fb);
    craft_hud_draw(s_fb, &s_player, fps);
    if (craft_menu_is_open()) craft_menu_draw(s_fb, &s_player);
}

uint32_t craft_main_seed(void) { return s_seed; }
const CraftPlayer *craft_main_player(void) { return &s_player; }
bool craft_main_dirty(void) { return craft_world_dirty != 0; }

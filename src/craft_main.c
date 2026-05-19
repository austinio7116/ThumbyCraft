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
#include "craft_chunk_store.h"
#include "craft_tool_models.h"
#include "craft_drops.h"
#include "craft_furnace.h"
#include "craft_chests.h"
#include "craft_water.h"
#include "craft_redstone.h"

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

/* Flags the platform polls + clears. */
static bool s_save_req;
static bool s_load_req;
static bool s_new_world_req;

/* Held-item swing animation — 1.0 right after the player swings,
 * decays linearly back to 0 at ~5/sec. Ticked in craft_main_step /
 * craft_main_tick so both host and device paths stay in sync. */
static float s_held_swing_t = 0.0f;

/* Background chunk-persist drain — fires one flash erase+program
 * every PERSIST_PERIOD seconds. Spreads ~70 ms hitches so they don't
 * bundle into a multi-chunk stutter on window shift. */
#define PERSIST_PERIOD 2.0f
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
    /* Seed the chunk store so the next load_around restores any
     * mods previously persisted for this seed. */
    craft_chunk_store_init(seed);
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
    craft_tool_models_init();
    craft_mobs_spawn_around(spawn, seed);
    craft_particles_init();
    craft_drops_init();
    craft_furnace_init();
    craft_chests_init();
    craft_water_init();
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
    uint32_t seed;
    if (!craft_save_deserialise(blob, n, &seed, &s_player)) return false;
    s_seed = seed;
    return true;
}

size_t craft_main_save(uint8_t *out, size_t cap) {
    /* Force-persist every chunk in the window that has mods. The
     * regular dirty-queue drain only catches chunks marked dirty;
     * the force variant also re-persists chunks that have mods in
     * the hash but aren't currently dirty, guaranteeing the chunk
     * store on flash matches the in-memory state at save time. */
    craft_world_chunks_force_persist_window();
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

            /* Re-key the flash chunk store so records from the prior
             * world are rejected, AND clear in-SRAM state that's
             * keyed by world coords (mods, chests, furnaces, water,
             * drops, particles). Without this, the previous world's
             * starter chest + any player edits would re-apply on top
             * of the new procedural terrain — that's the "chest in
             * the sky" / accumulating-chests behaviour. */
            craft_chunk_store_init(ns);
            craft_world_reset_mods();
            craft_chests_init();
            craft_furnace_init();
            craft_water_init();
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
        craft_hud_draw(s_fb, &s_player, fps);
        craft_menu_draw(s_fb, &s_player);
        s_player.cam.pos.y += s_player.step_lag;
        return;
    }
    s_world_time += dt;
    if (s_world_time >= CRAFT_DAY_LENGTH) s_world_time -= CRAFT_DAY_LENGTH;
    craft_player_tick(&s_player, in, dt);
    held_swing_tick_after_player(dt);
    craft_world_maybe_shift((int)s_player.cam.pos.x,
                            (int)s_player.cam.pos.z, s_seed);
    s_persist_timer -= dt;
    if (s_persist_timer <= 0.0f) {
        craft_world_persist_tick();
        s_persist_timer = PERSIST_PERIOD;
    }
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
            craft_gen_is_hut_chest(cx, cy, cz, s_seed)) {
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
    craft_hud_draw(s_fb, &s_player, fps);
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
    craft_player_tick(&s_player, in, dt);
    held_swing_tick_after_player(dt);
    craft_world_maybe_shift((int)s_player.cam.pos.x,
                            (int)s_player.cam.pos.z, s_seed);
    s_persist_timer -= dt;
    if (s_persist_timer <= 0.0f) {
        craft_world_persist_tick();
        s_persist_timer = PERSIST_PERIOD;
    }
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
            craft_gen_is_hut_chest(cx, cy, cz, s_seed)) {
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
    craft_hud_draw(s_fb, &s_player, fps);
    if (craft_menu_is_open()) craft_menu_draw(s_fb, &s_player);
    /* Restore logical cam y so next tick's physics is correct. */
    s_player.cam.pos.y += s_player.step_lag;
}

uint32_t craft_main_seed(void) { return s_seed; }
const CraftPlayer *craft_main_player(void) { return &s_player; }
bool craft_main_dirty(void) { return craft_world_dirty != 0; }

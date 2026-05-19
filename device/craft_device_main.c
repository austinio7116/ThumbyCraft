/*
 * ThumbyCraft — device entry point.
 *
 * Brings up LCD + buttons + audio, draws a quick splash, then runs
 * the craft_main game loop. Dual-core rendering: core1 renders the
 * top half of the framebuffer while core0 renders the bottom; both
 * cores wait on a barrier before the present DMA fires.
 *
 * Saves persist via craft_save_flash (4-sector wear ring at the top
 * of the slot's flash). Standalone build flashes the whole device;
 * THUMBYONE_SLOT_MODE wraps us as an embedded slot.
 *
 * RP2350 @ 280 MHz (max stable on this part) — raycaster is the hot
 * path so we run flat-out.
 */
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/rand.h"
#include "hardware/clocks.h"

#include "craft_lcd_gc9107.h"
#include "craft_buttons.h"
#include "craft_audio_pwm.h"
#include "craft_save_flash.h"

#include "slot_layout.h"
#include "craft_chunk_store.h"
#include "craft_main.h"
#include "craft_audio.h"
#include "craft_player.h"
#include "craft_font.h"
#include "craft_save.h"
#include "craft_title.h"
#include "craft_blocks.h"
#include "craft_menu.h"
#include "craft_menu.h"

#ifdef THUMBYONE_SLOT_MODE
#  include "thumbyone_led.h"
#endif

/* Single framebuffer — DMA push is async, so we just wait at the
 * top of each new frame before touching it again. Saves the 32 KB
 * of a second buffer. */
static uint16_t g_fb[CRAFT_FB_W * CRAFT_FB_H];

/* --- Splash ------------------------------------------------------- */
static void fb_fill(uint16_t c) {
    for (int i = 0; i < CRAFT_FB_W * CRAFT_FB_H; i++) g_fb[i] = c;
}

static void splash(void) {
    fb_fill(0x8C71);   /* dim slate */
    craft_font_draw_2x(g_fb, "ThumbyCraft", 14, 50, 0xFFFF);
    craft_font_draw(g_fb,    "loading...",  38, 70, 0xCE79);
    craft_lcd_present(g_fb);
    craft_lcd_wait_idle();
}

/* --- Core1 render: tile-based work stealing ----------------------
 *
 * The screen is sliced into TILE_COUNT horizontal bands of TILE_H
 * rows each (16 × 8 = 128 rows). Both cores grab tiles from a single
 * atomic counter — whichever core finishes its current tile first
 * grabs the next one. Self-balancing under any view direction:
 * sky-heavy strips and texture-heavy strips redistribute themselves
 * automatically without core 0 sitting idle waiting on core 1 (or
 * vice versa).
 *
 * Tile size of 8 rows is a sweet spot: per-tile cost ≈ 1024 pixels
 * × a few μs each ≈ 3–5 ms, far larger than the ~50 ns atomic CAS
 * overhead. 16 tiles gives a worst-case load mismatch of one tile
 * (~6 %), much better than the static 50/50 split. */
#define TILE_H            8
#define TILE_COUNT        (CRAFT_FB_H / TILE_H)

static volatile uint32_t s_next_tile;
static volatile bool     c1_run, c1_done;

static void run_tiles(void) {
    for (;;) {
        uint32_t t = __atomic_fetch_add(&s_next_tile, 1, __ATOMIC_RELAXED);
        if (t >= TILE_COUNT) break;
        int y0 = (int)t * TILE_H;
        int y1 = y0 + TILE_H;
        if (y1 > CRAFT_FB_H) y1 = CRAFT_FB_H;
        craft_main_render_strip(y0, y1);
    }
}

static void core1_main(void) {
    /* Register as the lockout victim — when core 0 needs to do a
     * flash erase or program, multicore_lockout_start_blocking()
     * signals via the SIO FIFO IRQ and waits for our ACK. Without
     * this init core 0 hangs forever the first time it tries to
     * write flash (chunk store, save). */
    multicore_lockout_victim_init();
    for (;;) {
        while (!c1_run) tight_loop_contents();
        c1_run = false;
        run_tiles();
        c1_done = true;
    }
}

/* --- Input edge-trigger state ------------------------------------- */
typedef struct { bool prev; uint32_t pressed_at_ms; } EdgeState;
static EdgeState e_a, e_b, e_lb, e_rb, e_menu;

static void edge_update(EdgeState *e, bool now, uint32_t t_ms,
                        bool *pressed, bool *long_pressed) {
    *pressed = false;
    *long_pressed = false;
    if (now && !e->prev) { *pressed = true; e->pressed_at_ms = t_ms; }
    if (now && e->prev && t_ms - e->pressed_at_ms > 400) *long_pressed = true;
    e->prev = now;
}

/* --- Drain menu requests into platform actions -------------------- */
static void drain_requests(void) {
    if (craft_main_take_save_request()) {
        static uint8_t buf[CRAFT_SAVE_MAX_BYTES];
        int slot = craft_main_save_slot();
        /* craft_main_save picks/keeps the chunks nonce internally
         * and stamps it into the serialised blob (HDR_OFF_CHUNKS_NONCE).
         * The seq is independent and just gives the slot picker its
         * "newest" ordering. */
        size_t n = craft_main_save(buf, sizeof buf);
        if (n > 0 && craft_save_flash_write_slot(
                         slot, craft_save_flash_pick_next_seq(),
                         buf, n, craft_main_thumb()))
            craft_menu_toast("World saved");
        else
            craft_menu_toast("Save failed");
    }
    if (craft_main_take_load_request()) {
        int slot = craft_main_save_slot();
        const uint8_t *blob;
        size_t blob_n = craft_save_flash_read_slot(slot, &blob);
        if (blob_n > 0) {
            /* craft_main_load pre-reads chunks_nonce from the blob
             * and binds the chunk store. No upfront setup needed. */
            if (craft_main_load(blob, blob_n))
                craft_menu_toast("World loaded");
            else
                craft_menu_toast("Load failed");
        } else {
            craft_menu_toast("No save found");
        }
    }
    (void)craft_main_take_new_world_request();   /* engine handles it */
}

/* Platform-random hook used by craft_main's next_seed(). On RP2350
 * get_rand_32 reads ROSC entropy + boot-rom RNG, fresh per call. */
uint32_t craft_platform_rand32(void) { return get_rand_32(); }

int main(void) {
    /* Crank to 280 MHz — the raycaster wants every cycle. */
    set_sys_clock_khz(280000, true);

    stdio_init_all();
    craft_lcd_init();
    craft_buttons_init();
    craft_audio_pwm_init();

#ifdef THUMBYONE_SLOT_MODE
    thumbyone_slot_init_brightness_and_led(true);
#endif

    /* Launch core 1 BEFORE the title screen so multicore_lockout
     * works during pre-game flash operations (e.g. clear scratch
     * region on New World). Core 1 spins on c1_run until the main
     * render loop starts setting it. */
    multicore_launch_core1(core1_main);

    splash();

    /* Title screen — block until the player picks New or a save
     * slot. craft_blocks_build_textures runs inside craft_main_init,
     * which we defer until after the pick, so the title page only
     * needs the font + slot thumbnails (already in flash). */
    craft_blocks_build_textures();
    craft_title_init(g_fb);
    {
        CraftTitleAction act = CRAFT_TITLE_STILL;
        absolute_time_t t_prev_ts = get_absolute_time();
        (void)t_prev_ts;
        while (act == CRAFT_TITLE_STILL) {
            CraftRawButtons raw_t; craft_buttons_read(&raw_t);
            CraftInput in_t = {0};
            in_t.up    = raw_t.up;
            in_t.down  = raw_t.down;
            in_t.left  = raw_t.left;
            in_t.right = raw_t.right;
            in_t.a     = raw_t.a;
            in_t.b     = raw_t.b;
            in_t.menu  = raw_t.menu;
            act = craft_title_step(&in_t);
            craft_title_draw();
            /* Push the framebuffer to the LCD. The dual-core render
             * isn't running yet — single-core present is fine here. */
            extern void craft_lcd_present(const uint16_t *fb);
            craft_lcd_present(g_fb);
        }
        if (act == CRAFT_TITLE_LOAD) {
            int slot = craft_title_chosen_slot();
            const uint8_t *blob;
            size_t blob_n = craft_save_flash_read_slot(slot, &blob);
            if (blob_n > 0) {
                /* Pre-read seed + chunks_nonce out of the blob so
                 * craft_main_init can bind the chunk store BEFORE
                 * world_load_around runs. craft_main_load below
                 * does the same binding inside (idempotent). */
                uint32_t boot_seed = (uint32_t)blob[8]
                                   | ((uint32_t)blob[9]  << 8)
                                   | ((uint32_t)blob[10] << 16)
                                   | ((uint32_t)blob[11] << 24);
                uint32_t chunks_nonce = (uint32_t)blob[CRAFT_SAVE_OFF_CHUNKS_NONCE + 0]
                                      | ((uint32_t)blob[CRAFT_SAVE_OFF_CHUNKS_NONCE + 1] << 8)
                                      | ((uint32_t)blob[CRAFT_SAVE_OFF_CHUNKS_NONCE + 2] << 16)
                                      | ((uint32_t)blob[CRAFT_SAVE_OFF_CHUNKS_NONCE + 3] << 24);
                craft_main_set_save_slot(slot);
                craft_main_set_slot_nonce(slot, chunks_nonce);
                craft_main_set_active_region(slot);
                craft_main_init(g_fb, boot_seed);
                craft_main_load(blob, blob_n);
            } else {
                /* Fallback if slot turned out invalid: fresh world
                 * in scratch. No flash wipe needed — craft_main_init
                 * picks a fresh scratch nonce that invalidates any
                 * stale sectors. */
                craft_main_set_active_region(TBC_REGION_SCRATCH);
                craft_main_init(g_fb, get_rand_32());
            }
        } else {
            /* CRAFT_TITLE_NEW: fresh random world. Same path —
             * nonce rotation is instant. */
            craft_main_set_active_region(TBC_REGION_SCRATCH);
            uint32_t seed = get_rand_32();
            craft_main_init(g_fb, seed);
        }
    }

    /* Core 1 was launched earlier (before title screen) so the
     * multicore_lockout pattern works during pre-game flash ops. */

    absolute_time_t prev = get_absolute_time();
    int fps_value = 0, fps_frames = 0;
    absolute_time_t fps_window = prev;

    /* Pre-render audio buffer the IRQ pulls from. */
    static int16_t mix_buf[512];

    for (;;) {
        /* Audio top-up — feed the ring every frame. */
        int room = craft_audio_pwm_room();
        while (room > 0) {
            int chunk = room > 512 ? 512 : room;
            craft_audio_render(mix_buf, chunk);
            craft_audio_pwm_push(mix_buf, chunk);
            room -= chunk;
        }

        absolute_time_t now = get_absolute_time();
        float dt = absolute_time_diff_us(prev, now) * 1e-6f;
        if (dt > 0.1f) dt = 0.1f;
        prev = now;

        CraftRawButtons raw;
        craft_buttons_read(&raw);
        uint32_t t_ms = to_ms_since_boot(now);

        CraftInput in = {0};
        in.up = raw.up; in.down = raw.down;
        in.left = raw.left; in.right = raw.right;
        in.a = raw.a; in.b = raw.b;
        in.lb = raw.lb; in.rb = raw.rb;
        in.menu = raw.menu;

        bool dummy;
        edge_update(&e_a,    in.a,    t_ms, &in.a_pressed,    &in.a_long);
        edge_update(&e_b,    in.b,    t_ms, &in.b_pressed,    &dummy);
        edge_update(&e_lb,   in.lb,   t_ms, &in.lb_pressed,   &dummy);
        edge_update(&e_rb,   in.rb,   t_ms, &in.rb_pressed,   &dummy);
        edge_update(&e_menu, in.menu, t_ms, &in.menu_pressed, &in.menu_long);

        /* Phase 1: input + physics on core0 only. */
        craft_main_tick(&in, dt);
        drain_requests();

        /* Wait for previous LCD DMA before touching framebuffer. */
        craft_lcd_wait_idle();

        /* Phase 2a: compute camera basis on core0 ONLY (write to
         * shared state, can't race with core1). */
        craft_main_render_begin();

        /* Phase 2b: tile-based work-stealing render. Reset the
         * shared tile counter and release core 1 — both cores then
         * grab tiles from `s_next_tile` until they're exhausted. */
        __atomic_store_n(&s_next_tile, 0, __ATOMIC_RELAXED);
        c1_done = false;
        __sync_synchronize();
        c1_run = true;
        run_tiles();
        while (!c1_done) tight_loop_contents();
        __sync_synchronize();

        /* Phase 3: HUD overlay — single-core, after both strips done,
         * since the hotbar straddles the seam. */
        craft_main_draw_hud(fps_value);

#ifdef THUMBYONE_SLOT_MODE
        /* Front LED: mirror the player's depth — surface=cyan,
         * underground=red. Cheap and atmospheric. */
        const CraftPlayer *pp = craft_main_player();
        int depth = (int)(28.0f - pp->cam.pos.y);
        if (depth < 0) depth = 0; if (depth > 24) depth = 24;
        int r = depth * 10, g = 80 - depth * 3, b = 200 - depth * 8;
        if (g < 0) g = 0; if (b < 0) b = 0;
        thumbyone_led_set_rgb((uint8_t)r, (uint8_t)g, (uint8_t)b);
#endif

        craft_lcd_present(g_fb);

        /* FPS rolling window. */
        fps_frames++;
        if (absolute_time_diff_us(fps_window, now) > 500000) {
            fps_value = fps_frames * 2;
            fps_frames = 0;
            fps_window = now;
        }
    }
}

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

#include "craft_main.h"
#include "craft_audio.h"
#include "craft_player.h"
#include "craft_font.h"
#include "craft_save.h"
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

/* --- Core1 render strip ------------------------------------------- */
static volatile int  c1_y_start, c1_y_end;
static volatile bool c1_run, c1_done;

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
        craft_main_render_strip(c1_y_start, c1_y_end);
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
        size_t n = craft_main_save(buf, sizeof buf);
        if (n > 0 && craft_save_flash_write(buf, n))
            craft_menu_toast("World saved");
        else
            craft_menu_toast("Save failed");
    }
    if (craft_main_take_load_request()) {
        const uint8_t *blob;
        size_t blob_n = craft_save_flash_read(&blob);
        if (blob_n > 0 && craft_main_load(blob, blob_n))
            craft_menu_toast("World loaded");
        else
            craft_menu_toast("No save found");
    }
    (void)craft_main_take_new_world_request();   /* engine handles it */
}

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

    splash();

    /* Seed from the SDK's ROSC-backed entropy source — to_ms_since_boot
     * here would be a small, near-constant value (we're milliseconds
     * past power-on) and gave the same world every boot. */
    uint32_t seed = get_rand_32();
    craft_main_init(g_fb, seed);

    /* Restore previous save if present. */
    const uint8_t *blob;
    size_t blob_n = craft_save_flash_read(&blob);
    if (blob_n > 0) craft_main_load(blob, blob_n);

    /* Launch core1 for the top-half render. */
    multicore_launch_core1(core1_main);

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

        /* Phase 2b: split-render top half on core1, bottom on core0. */
        c1_y_start = 0;
        c1_y_end   = CRAFT_FB_H / 2;
        c1_done = false;
        __sync_synchronize();
        c1_run = true;
        craft_main_render_strip(CRAFT_FB_H / 2, CRAFT_FB_H);
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

/*
 * ThumbyCraft — SDL2 host build.
 *
 * Runs the same craft_main engine code that the device runs, but
 * drives input from the keyboard, presents the framebuffer in a
 * scaled SDL2 window, and sinks audio through SDL2's audio queue.
 * Save blobs persist to ./thumbycraft.sav in the working directory.
 *
 * Keyboard map (mirrors physical Thumby Color GPIO layout):
 *   Arrows / WSAD   D-pad (UP/DOWN/LEFT/RIGHT)
 *   Z               A button
 *   X               B button
 *   LShift          LB
 *   Return          RB
 *   Escape          MENU
 *   F1              Toggle fog
 *   F5              Save
 *   F9              Load
 *   F12             Quit
 *
 * Window is 128×128 logical, scaled 5× to 640×640.
 */
#include "craft_main.h"
#include "craft_audio.h"
#include "craft_render.h"
#include "craft_save.h"
#include "craft_menu.h"

#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SCALE 5
#define WIN_W (CRAFT_FB_W * SCALE)
#define WIN_H (CRAFT_FB_H * SCALE)

static uint16_t g_fb[CRAFT_FB_W * CRAFT_FB_H];

static SDL_Window   *win;
static SDL_Renderer *ren;
static SDL_Texture  *tex;

static SDL_AudioDeviceID audio_dev;

static void audio_cb(void *ud, Uint8 *stream, int len) {
    (void)ud;
    int n = len / (int)sizeof(int16_t);
    craft_audio_render((int16_t *)stream, n);
}

static void audio_init(void) {
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = CRAFT_AUDIO_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 512;
    want.callback = audio_cb;
    audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (audio_dev != 0) SDL_PauseAudioDevice(audio_dev, 0);
}

/* --- Edge-trigger state -------------------------------------------- */
typedef struct { bool prev; uint32_t pressed_at_ms; } EdgeState;

static void edge_update(EdgeState *e, bool now, uint32_t t_ms,
                        bool *pressed, bool *long_pressed) {
    *pressed = false;
    *long_pressed = false;
    if (now && !e->prev) { *pressed = true; e->pressed_at_ms = t_ms; }
    if (now && e->prev) {
        if (t_ms - e->pressed_at_ms > 400) *long_pressed = true;
    }
    e->prev = now;
}

/* --- Save/load helpers --------------------------------------------- */
static const char *SAV_PATH = "thumbycraft.sav";

static bool try_load(void) {
    FILE *f = fopen(SAV_PATH, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > (long)CRAFT_SAVE_MAX_BYTES) { fclose(f); return false; }
    uint8_t *buf = (uint8_t *)malloc((size_t)n);
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { free(buf); return false; }
    bool ok = craft_main_load(buf, (size_t)n);
    free(buf);
    if (ok) printf("[host] loaded %ld bytes from %s\n", n, SAV_PATH);
    else    printf("[host] %s present but invalid — ignored\n", SAV_PATH);
    return ok;
}
static void try_save(void) {
    uint8_t buf[CRAFT_SAVE_MAX_BYTES];
    size_t n = craft_main_save(buf, sizeof buf);
    if (n == 0) { printf("[host] save failed (too many deltas?)\n"); return; }
    FILE *f = fopen(SAV_PATH, "wb");
    if (!f) { perror("save"); return; }
    fwrite(buf, 1, n, f);
    fclose(f);
    printf("[host] saved %zu bytes\n", n);
}

int main(int argc, char **argv) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1;
    }
    win = SDL_CreateWindow("ThumbyCraft (host)",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        WIN_W, WIN_H, SDL_WINDOW_SHOWN);
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB565,
        SDL_TEXTUREACCESS_STREAMING, CRAFT_FB_W, CRAFT_FB_H);

    audio_init();

    uint32_t seed = (argc > 1) ? (uint32_t)atoi(argv[1]) : (uint32_t)time(NULL);
    printf("[host] seed = %u\n", seed);
    craft_main_init(g_fb, seed);
    try_load();

    EdgeState e_a = {0}, e_b = {0}, e_lb = {0}, e_rb = {0}, e_menu = {0};
    EdgeState e_f1 = {0}, e_f5 = {0}, e_f9 = {0};
    bool fog_on = true;

    Uint32 last_ms = SDL_GetTicks();
    Uint32 fps_window_start = last_ms;
    int    fps_frames = 0, fps_value = 0;

    bool running = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) running = false;
            if (ev.type == SDL_KEYDOWN &&
                ev.key.keysym.scancode == SDL_SCANCODE_F12) running = false;
        }
        const Uint8 *k = SDL_GetKeyboardState(NULL);

        Uint32 now_ms = SDL_GetTicks();
        float dt = (now_ms - last_ms) * 0.001f;
        if (dt > 0.1f) dt = 0.1f;
        last_ms = now_ms;

        CraftInput in = {0};
        in.up    = k[SDL_SCANCODE_UP]    || k[SDL_SCANCODE_W];
        in.down  = k[SDL_SCANCODE_DOWN]  || k[SDL_SCANCODE_S];
        in.left  = k[SDL_SCANCODE_LEFT]  || k[SDL_SCANCODE_A];
        in.right = k[SDL_SCANCODE_RIGHT] || k[SDL_SCANCODE_D];
        in.a     = k[SDL_SCANCODE_Z];
        in.b     = k[SDL_SCANCODE_X];
        in.lb    = k[SDL_SCANCODE_LSHIFT];
        in.rb    = k[SDL_SCANCODE_RETURN];
        in.menu  = k[SDL_SCANCODE_ESCAPE];

        bool dummy;
        edge_update(&e_a,    in.a,    now_ms, &in.a_pressed,    &in.a_long);
        edge_update(&e_b,    in.b,    now_ms, &in.b_pressed,    &dummy);
        edge_update(&e_lb,   in.lb,   now_ms, &in.lb_pressed,   &dummy);
        edge_update(&e_rb,   in.rb,   now_ms, &in.rb_pressed,   &dummy);
        edge_update(&e_menu, in.menu, now_ms, &in.menu_pressed, &in.menu_long);

        bool press_f1, press_f5, press_f9, long_dummy;
        edge_update(&e_f1, k[SDL_SCANCODE_F1], now_ms, &press_f1, &long_dummy);
        edge_update(&e_f5, k[SDL_SCANCODE_F5], now_ms, &press_f5, &long_dummy);
        edge_update(&e_f9, k[SDL_SCANCODE_F9], now_ms, &press_f9, &long_dummy);
        if (press_f1) { fog_on = !fog_on; craft_render_set_fog(fog_on); }
        if (press_f5) try_save();
        if (press_f9) try_load();

        craft_main_step(&in, dt, fps_value);
        /* Drain menu requests. */
        if (craft_main_take_save_request()) {
            try_save();
            craft_menu_toast("World saved");
        }
        if (craft_main_take_load_request()) {
            if (try_load()) craft_menu_toast("World loaded");
            else            craft_menu_toast("No save found");
        }
        if (craft_main_take_new_world_request()) {
            /* New-world action is handled inside craft_main itself. */
        }

        SDL_UpdateTexture(tex, NULL, g_fb, CRAFT_FB_W * sizeof(uint16_t));
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);

        fps_frames++;
        if (now_ms - fps_window_start >= 500) {
            fps_value = fps_frames * 1000 / (now_ms - fps_window_start);
            fps_frames = 0;
            fps_window_start = now_ms;
        }
    }

    if (audio_dev) SDL_CloseAudioDevice(audio_dev);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}

/*
 * ThumbyCraft — SDL2 host build.
 *
 * Runs the same craft_main engine code that the device runs, but
 * drives input from the keyboard, presents the framebuffer in a
 * scaled SDL2 window, and sinks audio through SDL2's audio queue.
 * Save blobs persist to ./thumbycraft.sav in the working directory.
 *
 * Minecraft-style controls (host uses DPAD_STRAFE scheme + mouse-look):
 *   Mouse           Look (yaw + pitch)
 *   Arrow keys      Look (no-mouse fallback — always works)
 *   W / A / S / D   Forward / strafe-left / back / strafe-right
 *   Space           Jump
 *   Left click / Z  Mine / attack
 *   Right click / X Place / use
 *   Mouse wheel     Cycle hotbar
 *   1 – 8           Select hotbar slot
 *   Escape          Pause menu (Inventory, Save, etc.)
 *   G / `           Toggle mouse capture (to free the cursor)
 *   F1 fog · F5 save · F9 load · F12 quit
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

/* Host shim for the engine's portable slot queries. Host uses a
 * single file at SAV_PATH for now; the slot APIs surface it as
 * slot 0 only. No thumbnail (the host build is for development
 * iteration, not multi-slot bookkeeping). */
bool craft_save_slot_used(int slot) {
    if (slot != 0) return false;
    FILE *f = fopen(SAV_PATH, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}
const uint16_t *craft_save_slot_thumb(int slot) {
    (void)slot;
    return NULL;
}

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

/* Platform-random hook used by craft_main's next_seed() — host uses
 * the C library RNG seeded from time(); see SDL_Init below. */
uint32_t craft_platform_rand32(void) { return (uint32_t)rand(); }

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

    /* Seed the C library RNG so craft_platform_rand32 (and any later
     * srand consumers) produce uncorrelated streams across runs. */
    srand((unsigned)time(NULL));
    uint32_t seed = (argc > 1) ? (uint32_t)atoi(argv[1]) : (uint32_t)time(NULL);
    printf("[host] seed = %u\n", seed);
    craft_main_init(g_fb, seed);
    try_load();

    /* Host plays FPS-style: WASD move/strafe via the DPAD_STRAFE scheme,
     * yaw/pitch from the mouse.
     *
     * Mouse-look method: we do NOT use SDL relative mode. Under
     * WSLg/XWayland every relative path (warp fallback, raw XInput2)
     * reports bogus deltas that spin the view uncontrollably. What IS
     * reliable there is the absolute cursor position inside the window,
     * so we compute our own delta = (pos - last_pos) each frame, clamp
     * it hard against spurious jumps, then recentre the cursor only when
     * it nears a window edge (rare warps → no per-frame round-trip lag).
     * `need_reseed` makes the next sample seed last_pos WITHOUT applying
     * a delta — set on launch, grab toggle, and window re-entry so a big
     * entry jump can never whip the camera. */
    craft_main_set_scheme(CRAFT_SCHEME_DPAD_STRAFE);
    bool mouse_grab = true;
    SDL_ShowCursor(SDL_DISABLE);
    int  last_mx = 0, last_my = 0;
    bool need_reseed = true;
    const float LOOK_SENS = 0.0035f;   /* radians per pixel of cursor travel */
    const float KEY_LOOK  = 2.2f;      /* arrow-key look rate, radians/sec */
    const int   MOUSE_CLAMP = 40;      /* max px delta applied per frame */
    const int   EDGE_MARGIN = 80;      /* recentre when cursor within this */

    EdgeState e_a = {0}, e_b = {0}, e_lb = {0}, e_rb = {0}, e_menu = {0};
    EdgeState e_f1 = {0}, e_f5 = {0}, e_f9 = {0}, e_grab = {0};
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
            /* Re-entering / refocusing the window can deliver one big
             * position jump — reseed so it isn't applied as look. */
            if (ev.type == SDL_WINDOWEVENT &&
                (ev.window.event == SDL_WINDOWEVENT_ENTER ||
                 ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED))
                need_reseed = true;
            if (ev.type == SDL_MOUSEWHEEL) {
                /* Wheel up = previous slot, down = next (Minecraft feel). */
                if (ev.wheel.y > 0)      craft_main_hotbar_cycle(-1);
                else if (ev.wheel.y < 0) craft_main_hotbar_cycle(+1);
            }
            if (ev.type == SDL_KEYDOWN) {
                SDL_Scancode sc = ev.key.keysym.scancode;
                if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_8)
                    craft_main_hotbar_select(sc - SDL_SCANCODE_1);
            }
        }
        const Uint8 *k = SDL_GetKeyboardState(NULL);
        int mx, my;
        Uint32 mb = SDL_GetMouseState(&mx, &my);

        /* Mouse-look from absolute-position delta. First sample after a
         * reseed only stores last_pos (no look) so window-entry jumps
         * never whip the camera. Deltas are clamped to ±MOUSE_CLAMP px to
         * kill any residual spurious spike, then the cursor is recentred
         * only when it strays near an edge (so warps are rare → no lag).
         * Yaw right = +dx, look up = cursor up = -dy. */
        if (mouse_grab) {
            if (need_reseed) {
                last_mx = mx; last_my = my;
                need_reseed = false;
            } else {
                int dx = mx - last_mx;
                int dy = my - last_my;
                if (dx >  MOUSE_CLAMP) dx =  MOUSE_CLAMP;
                else if (dx < -MOUSE_CLAMP) dx = -MOUSE_CLAMP;
                if (dy >  MOUSE_CLAMP) dy =  MOUSE_CLAMP;
                else if (dy < -MOUSE_CLAMP) dy = -MOUSE_CLAMP;
                if (dx || dy) {
                    float s = LOOK_SENS * craft_main_mouse_sens();
                    craft_main_look((float)dx * s, -(float)dy * s);
                }
                if (mx < EDGE_MARGIN || mx > WIN_W - EDGE_MARGIN ||
                    my < EDGE_MARGIN || my > WIN_H - EDGE_MARGIN) {
                    SDL_WarpMouseInWindow(win, WIN_W / 2, WIN_H / 2);
                    last_mx = WIN_W / 2; last_my = WIN_H / 2;
                } else {
                    last_mx = mx; last_my = my;
                }
            }
        }

        Uint32 now_ms = SDL_GetTicks();
        float dt = (now_ms - last_ms) * 0.001f;
        if (dt > 0.1f) dt = 0.1f;
        last_ms = now_ms;

        /* Keyboard look fallback — arrow keys, guaranteed to work even
         * if the compositor mangles mouse motion. Rate-based so it's
         * framerate-independent. */
        {
            float kl = KEY_LOOK * craft_main_mouse_sens() * dt;
            float dyaw = 0.0f, dpitch = 0.0f;
            if (k[SDL_SCANCODE_LEFT])  dyaw   -= kl;
            if (k[SDL_SCANCODE_RIGHT]) dyaw   += kl;
            if (k[SDL_SCANCODE_UP])    dpitch += kl;
            if (k[SDL_SCANCODE_DOWN])  dpitch -= kl;
            if (dyaw != 0.0f || dpitch != 0.0f) craft_main_look(dyaw, dpitch);
        }

        bool ml = (mb & SDL_BUTTON(SDL_BUTTON_LEFT))  != 0;
        bool mr = (mb & SDL_BUTTON(SDL_BUTTON_RIGHT)) != 0;
        CraftInput in = {0};
        in.up    = k[SDL_SCANCODE_W];   /* forward  (arrows = look) */
        in.down  = k[SDL_SCANCODE_S];   /* back */
        in.left  = k[SDL_SCANCODE_A];   /* strafe L */
        in.right = k[SDL_SCANCODE_D];   /* strafe R */
        in.a     = k[SDL_SCANCODE_Z] || ml;                      /* mine / attack */
        in.b     = k[SDL_SCANCODE_X] || mr;                      /* place / use */
        in.lb    = false;            /* unused — mouse handles look, no look-mod */
        in.rb    = k[SDL_SCANCODE_SPACE] || k[SDL_SCANCODE_RETURN];  /* jump */
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

        bool press_grab;
        edge_update(&e_grab, k[SDL_SCANCODE_G] || k[SDL_SCANCODE_GRAVE],
                    now_ms, &press_grab, &long_dummy);
        if (press_grab) {
            mouse_grab = !mouse_grab;
            SDL_ShowCursor(mouse_grab ? SDL_DISABLE : SDL_ENABLE);
            need_reseed = true;   /* don't apply the gap as a look delta */
        }

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

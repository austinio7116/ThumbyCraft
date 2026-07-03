/*
 * ThumbyCraft - FunKey S / RG Nano SDL 1.2 launcher.
 */
#include "craft_main.h"
#include "craft_audio.h"
#include "craft_menu.h"
#include "craft_render.h"
#include "craft_save.h"

#include <SDL/SDL.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef SDL_INIT_EVENTS
#define SDL_INIT_EVENTS 0
#endif

#define SCREEN_W 240
#define SCREEN_H 240
#define SLOT_COUNT CRAFT_SAVE_SLOT_COUNT_PUBLIC
#define THUMB_N (CRAFT_SAVE_THUMB_DIM * CRAFT_SAVE_THUMB_DIM)

extern void craft_funkey_chunk_store_set_root(const char *root);

static uint16_t g_fb[CRAFT_FB_W * CRAFT_FB_H];
static uint16_t g_scaled[SCREEN_W * SCREEN_H];
static uint16_t g_thumb_cache[SLOT_COUNT][THUMB_N];
static bool     g_thumb_have[SLOT_COUNT];
static char     g_app_dir[256] = "/mnt/FunKey/.thumbycraft";
static char     g_chunk_dir[256] = "/mnt/FunKey/.thumbycraft/chunks";
static SDL_Surface *g_screen;
static SDL_Joystick *g_joy;
static bool g_running = true;

typedef struct {
    bool prev;
    uint32_t pressed_at_ms;
} EdgeState;

static void mkdir_p(const char *path) {
    char tmp[320];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0777);
            *p = '/';
        }
    }
    mkdir(tmp, 0777);
}

static void slot_path(char *out, size_t out_len, int slot, const char *ext) {
    snprintf(out, out_len, "%s/slot%d.%s", g_app_dir, slot, ext);
}

static void edge_update(EdgeState *e, bool now, uint32_t t_ms,
                        bool *pressed, bool *long_pressed) {
    *pressed = false;
    *long_pressed = false;
    if (now && !e->prev) {
        *pressed = true;
        e->pressed_at_ms = t_ms;
    }
    if (now && e->prev && t_ms - e->pressed_at_ms > 400) {
        *long_pressed = true;
    }
    e->prev = now;
}

uint32_t craft_platform_rand32(void) {
    uint32_t a = (uint32_t)rand();
    uint32_t b = (uint32_t)SDL_GetTicks();
    return (a << 16) ^ a ^ b;
}

bool craft_save_slot_used(int slot) {
    if ((unsigned)slot >= SLOT_COUNT) return false;
    char path[320];
    slot_path(path, sizeof path, slot, "sav");
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

const uint16_t *craft_save_slot_thumb(int slot) {
    if ((unsigned)slot >= SLOT_COUNT) return NULL;
    if (!g_thumb_have[slot]) {
        char path[320];
        slot_path(path, sizeof path, slot, "thumb");
        FILE *f = fopen(path, "rb");
        if (!f) return NULL;
        size_t got = fread(g_thumb_cache[slot], sizeof(uint16_t), THUMB_N, f);
        fclose(f);
        if (got != THUMB_N) return NULL;
        g_thumb_have[slot] = true;
    }
    return g_thumb_cache[slot];
}

static bool try_load_slot(int slot) {
    if ((unsigned)slot >= SLOT_COUNT) return false;
    char path[320];
    slot_path(path, sizeof path, slot, "sav");
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > (long)CRAFT_SAVE_MAX_BYTES) {
        fclose(f);
        return false;
    }
    uint8_t *buf = (uint8_t *)malloc((size_t)n);
    if (!buf) {
        fclose(f);
        return false;
    }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    craft_main_set_save_slot(slot);
    bool ok = (got == (size_t)n) && craft_main_load(buf, (size_t)n);
    free(buf);
    fprintf(stderr, "[funkey] load slot %d: %s\n", slot, ok ? "ok" : "failed");
    return ok;
}

static bool try_save_slot(int slot) {
    if ((unsigned)slot >= SLOT_COUNT) return false;
    mkdir_p(g_app_dir);

    uint8_t buf[CRAFT_SAVE_MAX_BYTES];
    craft_main_set_save_slot(slot);
    size_t n = craft_main_save(buf, sizeof buf);
    if (n == 0) {
        fprintf(stderr, "[funkey] save slot %d failed\n", slot);
        return false;
    }

    char path[320];
    slot_path(path, sizeof path, slot, "sav");
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "[funkey] open %s: %s\n", path, strerror(errno));
        return false;
    }
    size_t put = fwrite(buf, 1, n, f);
    bool ok = (put == n) && (fclose(f) == 0);
    fprintf(stderr, "[funkey] save slot %d: %s (%zu bytes)\n",
            slot, ok ? "ok" : "failed", n);

    const uint16_t *thumb = craft_main_thumb();
    if (thumb) {
        memcpy(g_thumb_cache[slot], thumb, sizeof g_thumb_cache[slot]);
        g_thumb_have[slot] = true;
        slot_path(path, sizeof path, slot, "thumb");
        FILE *tf = fopen(path, "wb");
        if (tf) {
            fwrite(g_thumb_cache[slot], sizeof(uint16_t), THUMB_N, tf);
            fclose(tf);
        }
    }
    return ok;
}

static void audio_cb(void *ud, Uint8 *stream, int len) {
    (void)ud;
    craft_audio_render((int16_t *)stream, len / (int)sizeof(int16_t));
}

static void audio_init(void) {
    SDL_AudioSpec want;
    SDL_AudioSpec have;
    memset(&want, 0, sizeof want);
    want.freq = CRAFT_AUDIO_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 512;
    want.callback = audio_cb;
    if (SDL_OpenAudio(&want, &have) == 0) {
        SDL_PauseAudio(0);
    } else {
        fprintf(stderr, "[funkey] SDL_OpenAudio: %s\n", SDL_GetError());
    }
}

static bool key_down(SDLKey key) {
    Uint8 *k = SDL_GetKeyState(NULL);
    return k[key] != 0;
}

static bool joy_button(int idx) {
    if (!g_joy || idx < 0 || idx >= SDL_JoystickNumButtons(g_joy)) return false;
    return SDL_JoystickGetButton(g_joy, idx) != 0;
}

static int joy_axis(int idx) {
    if (!g_joy || idx < 0 || idx >= SDL_JoystickNumAxes(g_joy)) return 0;
    return SDL_JoystickGetAxis(g_joy, idx);
}

static void poll_input(CraftInput *in) {
    SDL_PumpEvents();
    memset(in, 0, sizeof *in);

    int ax0 = joy_axis(0);
    int ax1 = joy_axis(1);
    in->up    = key_down(SDLK_UP)    || key_down(SDLK_u) || ax1 < -12000;
    in->down  = key_down(SDLK_DOWN)  || key_down(SDLK_d) || ax1 >  12000;
    in->left  = key_down(SDLK_LEFT)  || key_down(SDLK_l) || ax0 < -12000;
    in->right = key_down(SDLK_RIGHT) || key_down(SDLK_r) || ax0 >  12000;

    bool btn_a = key_down(SDLK_a) || key_down(SDLK_RETURN) || key_down(SDLK_SPACE) || joy_button(0);
    bool btn_b = key_down(SDLK_b) || joy_button(1);
    bool btn_x = key_down(SDLK_x) || joy_button(2);
    bool btn_y = key_down(SDLK_y) || joy_button(3);
    bool btn_l = key_down(SDLK_n) || joy_button(4);
    bool btn_r = key_down(SDLK_m) || joy_button(5);

    in->a = btn_a;
    in->b = btn_b;
    in->lb = btn_y || btn_l;
    in->rb = btn_x || btn_r;
    in->menu = key_down(SDLK_s) || key_down(SDLK_ESCAPE) || joy_button(6);
}

static void present_frame(void) {
    const int dst_w = SCREEN_W;
    const int dst_h = SCREEN_H;
    for (int y = 0; y < dst_h; y++) {
        int sy = y * CRAFT_FB_H / dst_h;
        for (int x = 0; x < dst_w; x++) {
            int sx = x * CRAFT_FB_W / dst_w;
            g_scaled[y * SCREEN_W + x] = g_fb[sy * CRAFT_FB_W + sx];
        }
    }

    if (SDL_MUSTLOCK(g_screen)) SDL_LockSurface(g_screen);
    if (g_screen->format->BitsPerPixel == 16 &&
        g_screen->format->Rmask == 0xF800 &&
        g_screen->format->Gmask == 0x07E0 &&
        g_screen->format->Bmask == 0x001F) {
        for (int y = 0; y < SCREEN_H; y++) {
            memcpy((uint8_t *)g_screen->pixels + y * g_screen->pitch,
                   &g_scaled[y * SCREEN_W],
                   SCREEN_W * sizeof(uint16_t));
        }
    } else {
        for (int y = 0; y < SCREEN_H; y++) {
            uint8_t *row = (uint8_t *)g_screen->pixels + y * g_screen->pitch;
            for (int x = 0; x < SCREEN_W; x++) {
                uint16_t c = g_scaled[y * SCREEN_W + x];
                uint8_t r = (uint8_t)(((c >> 11) & 0x1F) << 3);
                uint8_t g = (uint8_t)(((c >> 5) & 0x3F) << 2);
                uint8_t b = (uint8_t)((c & 0x1F) << 3);
                uint32_t mapped = SDL_MapRGB(g_screen->format, r, g, b);
                memcpy(row + x * g_screen->format->BytesPerPixel,
                       &mapped, g_screen->format->BytesPerPixel);
            }
        }
    }
    if (SDL_MUSTLOCK(g_screen)) SDL_UnlockSurface(g_screen);
    SDL_Flip(g_screen);
}

static void handle_events(void) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) {
            g_running = false;
        } else if (ev.type == SDL_KEYDOWN) {
            SDLKey key = ev.key.keysym.sym;
            if (key == SDLK_q) {
                g_running = false;
            } else if (key == SDLK_k) {
                craft_main_hotbar_cycle(+1);
            }
        } else if (ev.type == SDL_JOYBUTTONDOWN) {
            if (ev.jbutton.button == 7) craft_main_hotbar_cycle(+1);
        }
    }
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    const char *app_dir = getenv("THUMBYCRAFT_HOME");
    if (app_dir && app_dir[0]) {
        snprintf(g_app_dir, sizeof g_app_dir, "%s", app_dir);
        snprintf(g_chunk_dir, sizeof g_chunk_dir, "%s/chunks", app_dir);
    }
    mkdir_p(g_app_dir);
    mkdir_p(g_chunk_dir);
    craft_funkey_chunk_store_set_root(g_chunk_dir);

    char log_path[320];
    snprintf(log_path, sizeof log_path, "%s/thumbycraft.log", g_app_dir);
    freopen(log_path, "a", stderr);
    fprintf(stderr, "\n[funkey] start %ld\n", (long)time(NULL));

    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "[funkey] SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_ShowCursor(SDL_DISABLE);
    if (SDL_NumJoysticks() > 0) {
        g_joy = SDL_JoystickOpen(0);
        SDL_JoystickEventState(SDL_ENABLE);
    }

    g_screen = SDL_SetVideoMode(SCREEN_W, SCREEN_H, 16, SDL_HWSURFACE | SDL_FULLSCREEN);
    if (!g_screen) {
        g_screen = SDL_SetVideoMode(SCREEN_W, SCREEN_H, 16, SDL_SWSURFACE);
    }
    if (!g_screen) {
        fprintf(stderr, "[funkey] SDL_SetVideoMode: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_WM_SetCaption("ThumbyCraft", "ThumbyCraft");
    audio_init();

    uint32_t seed = craft_platform_rand32();
    craft_main_set_save_slot(0);
    craft_main_init(g_fb, seed);
    craft_main_set_scheme(CRAFT_SCHEME_CONSOLE_TURN);
    try_load_slot(0);

    EdgeState e_a = {0}, e_b = {0}, e_lb = {0}, e_rb = {0}, e_menu = {0};
    uint32_t last_ms = SDL_GetTicks();
    uint32_t fps_window_start = last_ms;
    int fps_frames = 0;
    int fps_value = 0;

    while (g_running) {
        handle_events();

        uint32_t now_ms = SDL_GetTicks();
        float dt = (now_ms - last_ms) * 0.001f;
        if (dt > 0.1f) dt = 0.1f;
        last_ms = now_ms;

        CraftInput in;
        poll_input(&in);
        bool dummy = false;
        edge_update(&e_a, in.a, now_ms, &in.a_pressed, &in.a_long);
        edge_update(&e_b, in.b, now_ms, &in.b_pressed, &dummy);
        edge_update(&e_lb, in.lb, now_ms, &in.lb_pressed, &dummy);
        edge_update(&e_rb, in.rb, now_ms, &in.rb_pressed, &dummy);
        edge_update(&e_menu, in.menu, now_ms, &in.menu_pressed, &in.menu_long);

        craft_main_step(&in, dt, fps_value);
        if (craft_main_take_save_request()) {
            int slot = craft_main_save_slot();
            craft_menu_toast(try_save_slot(slot) ? "World saved" : "Save failed");
        }
        if (craft_main_take_load_request()) {
            int slot = craft_main_save_slot();
            craft_menu_toast(try_load_slot(slot) ? "World loaded" : "No save found");
        }
        if (craft_main_take_new_world_request()) {
            craft_main_set_scheme(CRAFT_SCHEME_CONSOLE_TURN);
        }
        if (craft_main_take_quit_to_lobby_request()) {
            g_running = false;
        }

        present_frame();

        fps_frames++;
        if (now_ms - fps_window_start >= 500) {
            fps_value = fps_frames * 1000 / (now_ms - fps_window_start);
            fps_frames = 0;
            fps_window_start = now_ms;
        }
    }

    try_save_slot(craft_main_save_slot());
    SDL_CloseAudio();
    if (g_joy) SDL_JoystickClose(g_joy);
    SDL_Quit();
    return 0;
}

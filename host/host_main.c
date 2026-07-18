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
 *   G / `           Toggle mouse capture (frees the cursor; alt-tab also
 *                   releases it, refocusing the window recaptures)
 *   F1 fog · F5 save · F9 load · F12 quit
 *
 * Window is 128×128 logical, scaled 5× to 640×640.
 */
#include "craft_main.h"
#include "craft_audio.h"
#include "craft_render.h"
#include "craft_save.h"
#include "craft_menu.h"

#if defined(CRAFT_NET_ENABLED) && CRAFT_NET_ENABLED
#include "craft_net.h"
#include "craft_world.h"   /* co-op self-test: verify edit propagation */
#endif

#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(CRAFT_HOST_X11)
#include <SDL2/SDL_syswm.h>
#include <X11/Xlib.h>
#endif

#define SCALE 5
#define WIN_W (CRAFT_FB_W * SCALE)
#define WIN_H (CRAFT_FB_H * SCALE)

static uint16_t g_fb[CRAFT_FB_W * CRAFT_FB_H];

static SDL_Window   *win;
static SDL_Renderer *ren;
static SDL_Texture  *tex;

/* Mouse-look via raw Xlib on SDL's own X11 connection. Under WSLg's XWayland,
 * neither SDL relative mode, SDL grab, nor XGrabPointer(confine) actually trap
 * the cursor (measured) -- but XWarpPointer DOES move it and XQueryPointer DOES
 * read it. So we pin the pointer to the window centre every frame (Quake-style)
 * and read its offset as the look delta. All no-ops if not running on X11. */
#if defined(CRAFT_HOST_X11)
static Display *s_xdpy;
static Window   s_xwin;
static void x11_pointer_init(SDL_Window *w) {
    SDL_SysWMinfo info; SDL_VERSION(&info.version);
    if (SDL_GetWindowWMInfo(w, &info) && info.subsystem == SDL_SYSWM_X11) {
        s_xdpy = info.info.x11.display;
        s_xwin = info.info.x11.window;
    }
}
static bool x11_have(void) { return s_xdpy != NULL; }
/* Read pointer offset from window centre; returns false if unavailable. */
static bool x11_query_delta(int *dx, int *dy) {
    if (!s_xdpy) return false;
    Window rr, cr; int rx, ry, wx, wy; unsigned int mask;
    if (!XQueryPointer(s_xdpy, s_xwin, &rr, &cr, &rx, &ry, &wx, &wy, &mask))
        return false;
    *dx = wx - WIN_W / 2;
    *dy = wy - WIN_H / 2;
    return true;
}
static void x11_pointer_center(void) {
    if (!s_xdpy) return;
    XWarpPointer(s_xdpy, None, s_xwin, 0, 0, 0, 0, WIN_W / 2, WIN_H / 2);
    XFlush(s_xdpy);
}
#else
static void x11_pointer_init(SDL_Window *w) { (void)w; }
static bool x11_have(void) { return false; }
static bool x11_query_delta(int *dx, int *dy) { (void)dx; (void)dy; return false; }
static void x11_pointer_center(void) { }
#endif

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
#if defined(CRAFT_NET_ENABLED) && CRAFT_NET_ENABLED
    craft_net_note_saving(true);
#endif
    size_t n = craft_main_save(buf, sizeof buf);
#if defined(CRAFT_NET_ENABLED) && CRAFT_NET_ENABLED
    craft_net_note_saving(false);
#endif
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
    /* Force the X11 video backend so we have a real X connection to drive
     * XWarpPointer/XQueryPointer (the Wayland backend exposes no usable warp
     * under WSLg). Respect an explicit SDL_VIDEODRIVER override if set. */
    if (!getenv("SDL_VIDEODRIVER"))
        SDL_SetHint(SDL_HINT_VIDEODRIVER, "x11");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1;
    }
    printf("[host] SDL video driver = %s\n", SDL_GetCurrentVideoDriver());
    win = SDL_CreateWindow("ThumbyCraft (host)",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        WIN_W, WIN_H, SDL_WINDOW_SHOWN);
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB565,
        SDL_TEXTUREACCESS_STREAMING, CRAFT_FB_W, CRAFT_FB_H);
    x11_pointer_init(win);

    audio_init();

    /* Seed the C library RNG so craft_platform_rand32 (and any later
     * srand consumers) produce uncorrelated streams across runs. */
    srand((unsigned)time(NULL));
    uint32_t seed = (argc > 1) ? (uint32_t)atoi(argv[1]) : (uint32_t)time(NULL);
    printf("[host] seed = %u\n", seed);
    craft_main_init(g_fb, seed);
    try_load();

#if defined(CRAFT_NET_ENABLED) && CRAFT_NET_ENABLED
    /* Scripted co-op start for two-instance testing:
     *   CRAFT_NET_AUTO=host  → begin hosting immediately
     *   CRAFT_NET_AUTO=join  → begin joining immediately
     * (Interactive: F2 hosts, F3 joins.) */
    {
        /* CRAFT_NET_TEST_BULK=N: journal N synthetic edits before the
         * link opens, so the join transfer streams a multi-kilobyte,
         * multi-section journal (exercises flow control end to end). */
        const char *bulk = getenv("CRAFT_NET_TEST_BULK");
        if (bulk) {
            int n = atoi(bulk);
            for (int i = 0; i < n; i++)
                craft_world_set(100 + (i % 40), 10 + (i / 1600),
                                100 + (i / 40) % 40, BLK_STONE);
            printf("[net-test] bulk: journalled %d edits, mods=%d\n",
                   n, craft_world_mod_count());
        }
        const char *an = getenv("CRAFT_NET_AUTO");
        if (an && strcmp(an, "host") == 0) craft_net_begin_host();
        if (an && strcmp(an, "join") == 0) craft_net_begin_guest();
    }
#endif

    /* Host plays FPS-style: WASD move/strafe via the DPAD_STRAFE scheme,
     * yaw/pitch from the mouse.
     *
     * Mouse-look method: Quake-style pin-to-centre via raw Xlib.
     *
     * Under WSLg's XWayland nothing confines the cursor -- not SDL relative
     * mode, SDL grab, nor XGrabPointer(confine) -- but XWarpPointer reliably
     * MOVES the pointer and XQueryPointer reliably READS it. So each frame we
     * read how far the pointer has drifted from the window centre (that's the
     * look delta), then warp it straight back to the centre. The pointer is
     * therefore re-pinned to the centre every frame and can never leave the
     * window. need_reseed swallows the delta for the first frame after launch/
     * focus/grab; MOUSE_CLAMP bounds any spike. Falls back to no mouse-look
     * (keyboard only) if not running on X11. */
    craft_main_set_scheme(CRAFT_SCHEME_DPAD_STRAFE);
    /* mouse_grab is the *intent* (toggled with G); focused tracks window
     * focus. We capture only when both are true. */
    bool mouse_grab = true;
    bool focused    = true;
    bool need_reseed = true;           /* skip the first frame's delta */
    SDL_ShowCursor(SDL_DISABLE);
    x11_pointer_center();
    const float LOOK_SENS = 0.0035f;   /* radians per pixel of cursor travel */
    const float KEY_LOOK  = 2.2f;      /* arrow-key look rate, radians/sec */
    const int   MOUSE_CLAMP = 200;     /* clamp delta/frame, kills entry spikes */

    EdgeState e_a = {0}, e_b = {0}, e_lb = {0}, e_rb = {0}, e_menu = {0};
    EdgeState e_f1 = {0}, e_f5 = {0}, e_f9 = {0}, e_grab = {0};
    EdgeState e_f2 = {0}, e_f3 = {0};
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
            /* Hide cursor on focus-in, show on focus-out so the user can
             * alt-tab / reach other windows. */
            if (ev.type == SDL_WINDOWEVENT) {
                if (ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                    focused = true;
                    need_reseed = true;
                    if (mouse_grab) { SDL_ShowCursor(SDL_DISABLE); x11_pointer_center(); }
                } else if (ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                    focused = false;
                    SDL_ShowCursor(SDL_ENABLE);
                }
            }
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
        Uint32 mb = SDL_GetMouseState(NULL, NULL);

        /* Mouse-look: read how far the pointer drifted from the window centre
         * (the look delta), then warp it back to centre so it stays pinned and
         * can never leave the window. Clamp guards spikes; need_reseed drops
         * the first frame. Mouse right = look right (yaw -= dx); mouse up =
         * look up (-dy, since screen y grows downward). */
        if (mouse_grab && focused && x11_have()) {
            int dx = 0, dy = 0;
            if (x11_query_delta(&dx, &dy)) {
                if (need_reseed) {
                    need_reseed = false;
                } else if (dx || dy) {
                    if (dx >  MOUSE_CLAMP) dx =  MOUSE_CLAMP;
                    else if (dx < -MOUSE_CLAMP) dx = -MOUSE_CLAMP;
                    if (dy >  MOUSE_CLAMP) dy =  MOUSE_CLAMP;
                    else if (dy < -MOUSE_CLAMP) dy = -MOUSE_CLAMP;
                    float s = LOOK_SENS * craft_main_mouse_sens();
                    craft_main_look(-(float)dx * s, -(float)dy * s);
                }
                x11_pointer_center();
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

#if defined(CRAFT_NET_ENABLED) && CRAFT_NET_ENABLED
        /* Link play (two instances pair via the Unix socket):
         *   F2 = host (invite into this world), F3 = join a host. */
        bool press_f2, press_f3;
        edge_update(&e_f2, k[SDL_SCANCODE_F2], now_ms, &press_f2, &long_dummy);
        edge_update(&e_f3, k[SDL_SCANCODE_F3], now_ms, &press_f3, &long_dummy);
        if (press_f2) { printf("[host] F2: hosting link session\n"); craft_net_begin_host(); }
        if (press_f3) { printf("[host] F3: joining link session\n"); craft_net_begin_guest(); }
#endif

        bool press_grab;
        edge_update(&e_grab, k[SDL_SCANCODE_G] || k[SDL_SCANCODE_GRAVE],
                    now_ms, &press_grab, &long_dummy);
        if (press_grab) {
            mouse_grab = !mouse_grab;
            bool on = mouse_grab && focused;
            SDL_ShowCursor(on ? SDL_DISABLE : SDL_ENABLE);
            if (on) x11_pointer_center();
            need_reseed = true;   /* don't apply the toggle gap as a look delta */
        }

        craft_main_step(&in, dt, fps_value);

#if defined(CRAFT_NET_ENABLED) && CRAFT_NET_ENABLED
        /* CRAFT_NET_TEST=1 self-test: once linked, the host places a
         * stone at a fixed absolute coordinate; the guest polls its mod
         * journal for it and reports PASS — proves join transfer + live
         * edit stream end to end without needing gameplay input. */
        if (getenv("CRAFT_NET_TEST") && craft_net_active()) {
            static float t_net_test;
            static int   net_test_stage;    /* 0 pending, 1 first leg done, 2 done */
            t_net_test += dt;
            if (craft_net_is_host()) {
                if (net_test_stage == 0 && t_net_test > 2.0f) {
                    net_test_stage = 1;
                    craft_world_set(5, 30, 5, BLK_STONE);
                    printf("[net-test] host placed STONE at (5,30,5), seed=%u, mods=%d\n",
                           craft_main_seed(), craft_world_mod_count());
                    fflush(stdout);
                } else if (net_test_stage == 1 &&
                           craft_world_mod_get(7, 30, 7) == (int)BLK_STONE) {
                    net_test_stage = 2;
                    printf("[net-test] PASS: host received guest edit, mods=%d\n",
                           craft_world_mod_count());
                    fflush(stdout);
                }
            } else {
                if (net_test_stage == 0 &&
                    craft_world_mod_get(5, 30, 5) == (int)BLK_STONE) {
                    net_test_stage = 1;
                    printf("[net-test] PASS: guest received edit, seed=%u, mods=%d\n",
                           craft_main_seed(), craft_world_mod_count());
                    craft_world_set(7, 30, 7, BLK_STONE);
                    printf("[net-test] guest placed STONE at (7,30,7)\n");
                    fflush(stdout);
                }
            }
        }
#endif
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

    SDL_ShowCursor(SDL_ENABLE);
    if (audio_dev) SDL_CloseAudioDevice(audio_dev);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}

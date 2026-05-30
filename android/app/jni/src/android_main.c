/*
 * ThumbyCraft — Android (SDL2) platform shell.
 *
 * Mirrors host/host_main.c: the engine (../../../../src) is identical to the
 * host and device builds; only this platform layer differs. It renders the
 * 128x128 RGB565 framebuffer scaled to the phone screen and drives the engine
 * with touch input:
 *
 *   Left thumb (floating stick)   Move: forward / back / strafe (DPAD_STRAFE)
 *   Right thumb (drag)            Look (yaw + pitch)
 *   On-screen buttons             A=mine  B=place  JUMP  MENU  hotbar -/+
 *
 * Texture atlas is pre-baked to const data (generated/craft_textures_baked.c),
 * same as host/device. Saves go to the app's internal storage.
 */
#include "craft_main.h"
#include "craft_audio.h"
#include "craft_render.h"
#include "craft_save.h"
#include "craft_menu.h"
#include "craft_player.h"
#include "craft_types.h"

#include <SDL.h>
#include <SDL_main.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

static uint16_t      g_fb[CRAFT_FB_W * CRAFT_FB_H];
static SDL_Window   *win;
static SDL_Renderer *ren;
static SDL_Texture  *tex;
static SDL_AudioDeviceID audio_dev;
static char          g_sav_path[1024];
static char          g_thumb_path[1024];

/* Slot 0 thumbnail (32x32 RGB565), persisted alongside the save so the slot
 * picker can show it before the world is loaded. */
#define THUMB_N (CRAFT_SAVE_THUMB_DIM * CRAFT_SAVE_THUMB_DIM)
static uint16_t s_thumb_cache[THUMB_N];
static bool     s_thumb_have;

/* ---- engine platform hooks (same contract as host) ----------------- */
uint32_t craft_platform_rand32(void) { return (uint32_t)rand(); }

bool craft_save_slot_used(int slot) {
    if (slot != 0) return false;
    SDL_RWops *f = SDL_RWFromFile(g_sav_path, "rb");
    if (!f) return false;
    SDL_RWclose(f);
    return true;
}
const uint16_t *craft_save_slot_thumb(int slot) {
    if (slot != 0) return NULL;
    if (!s_thumb_have) {
        /* Lazy-load from disk (e.g. on first launch this session). */
        SDL_RWops *f = SDL_RWFromFile(g_thumb_path, "rb");
        if (!f) return NULL;
        size_t got = SDL_RWread(f, s_thumb_cache, sizeof(uint16_t), THUMB_N);
        SDL_RWclose(f);
        if (got != THUMB_N) return NULL;
        s_thumb_have = true;
    }
    return s_thumb_cache;
}

/* ---- save / load --------------------------------------------------- */
static bool try_load(void) {
    SDL_RWops *f = SDL_RWFromFile(g_sav_path, "rb");
    if (!f) return false;
    Sint64 n = SDL_RWsize(f);
    if (n <= 0 || n > (Sint64)CRAFT_SAVE_MAX_BYTES) { SDL_RWclose(f); return false; }
    uint8_t *buf = (uint8_t *)malloc((size_t)n);
    size_t got = SDL_RWread(f, buf, 1, (size_t)n);
    SDL_RWclose(f);
    bool ok = (got == (size_t)n) && craft_main_load(buf, (size_t)n);
    free(buf);
    SDL_Log("[android] load %s: %s (%lld bytes)", g_sav_path, ok ? "ok" : "fail", (long long)n);
    return ok;
}
static void try_save(void) {
    uint8_t buf[CRAFT_SAVE_MAX_BYTES];
    size_t n = craft_main_save(buf, sizeof buf);
    if (n == 0) { SDL_Log("[android] save failed"); return; }
    SDL_RWops *f = SDL_RWFromFile(g_sav_path, "wb");
    if (!f) { SDL_Log("[android] save open failed"); return; }
    SDL_RWwrite(f, buf, 1, n);
    SDL_RWclose(f);
    SDL_Log("[android] saved %zu bytes", n);

    /* Persist the 32x32 thumbnail the engine grabbed of the last in-game
     * frame (captured when the menu opened), so the slot picker shows it. */
    const uint16_t *th = craft_main_thumb();
    if (th) {
        memcpy(s_thumb_cache, th, sizeof s_thumb_cache);
        s_thumb_have = true;
        SDL_RWops *tf = SDL_RWFromFile(g_thumb_path, "wb");
        if (tf) {
            SDL_RWwrite(tf, s_thumb_cache, sizeof(uint16_t), THUMB_N);
            SDL_RWclose(tf);
        }
    }
}

/* ---- audio --------------------------------------------------------- */
static void audio_cb(void *ud, Uint8 *stream, int len) {
    (void)ud;
    craft_audio_render((int16_t *)stream, len / (int)sizeof(int16_t));
}
static void audio_init(void) {
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = CRAFT_AUDIO_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 1024;
    want.callback = audio_cb;
    audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (audio_dev) SDL_PauseAudioDevice(audio_dev, 0);
}

/* ---- on-screen buttons (round, normalised centre + radius) ---------- */
typedef struct { float cx, cy, r; int id; Uint8 cr, cg, cb; } Button;
enum { BTN_A, BTN_B, BTN_JUMP, BTN_MENU, BTN_HOTL, BTN_HOTR, BTN_COUNT };

/* Landscape layout: action cluster bottom-right, hotbar + menu top corners.
 * cx,cy are fractions of the output w/h; r is a fraction of min(w,h). */
static const Button BUTTONS[BTN_COUNT] = {
    { 0.865f, 0.55f, 0.082f, BTN_A,    228,  92,  78 }, /* mine  - warm red   */
    { 0.800f, 0.78f, 0.078f, BTN_B,     86, 170, 226 }, /* place - sky blue   */
    { 0.945f, 0.83f, 0.094f, BTN_JUMP,  96, 202, 112 }, /* jump  - green      */
    { 0.955f, 0.10f, 0.050f, BTN_MENU, 205, 207, 212 }, /* menu  - light grey */
    { 0.050f, 0.10f, 0.046f, BTN_HOTL, 224, 198,  96 }, /* hotbar prev - gold */
    { 0.135f, 0.10f, 0.046f, BTN_HOTR, 224, 198,  96 }, /* hotbar next - gold */
};

/* Output size in px, cached from the last frame for touch hit-testing. */
static int g_ow = 1, g_oh = 1;

static int button_at(float nx, float ny) {
    int mind = g_ow < g_oh ? g_ow : g_oh;
    float px = nx * g_ow, py = ny * g_oh;
    for (int i = 0; i < BTN_COUNT; i++) {
        const Button *b = &BUTTONS[i];
        float bx = b->cx * g_ow, by = b->cy * g_oh;
        float rr = b->r * mind * 1.28f;     /* a little touch slop */
        float dx = px - bx, dy = py - by;
        if (dx * dx + dy * dy <= rr * rr) return i;
    }
    return -1;
}

/* ---- procedural anti-aliased control textures ---------------------- *
 * Each control is pre-rendered once into an RGBA texture: a soft drop
 * shadow, a colour body with a top-lit vertical gradient + edge vignette,
 * a glossy upper highlight, a bevelled rim, and a crisp white icon. We
 * draw hard-edged at BTN_SS× supersample into a float buffer, then box-
 * downsample for smooth anti-aliasing. At runtime we just blit (cheap),
 * tweaking brightness/scale for the pressed state. */
#define BTN_TEX 160          /* final texture size (px)        */
#define BTN_SS  3            /* supersample factor              */

static SDL_Texture *g_btn_tex[BTN_COUNT];
static SDL_Texture *g_stick_base, *g_stick_knob;

/* Premultiplied src-over into a float RGBA work buffer (4 floats/px). */
static void fcomp(float *buf, int W, int x, int y, float r, float g, float b, float a) {
    if (a <= 0.f || x < 0 || y < 0 || x >= W || y >= W) return;
    if (a > 1.f) a = 1.f;
    float *p = &buf[((size_t)y * W + x) * 4];
    float ia = 1.f - a;
    p[0] = r * a + p[0] * ia; p[1] = g * a + p[1] * ia;
    p[2] = b * a + p[2] * ia; p[3] = a     + p[3] * ia;
}
static void ss_disc(float *buf, int W, float cx, float cy, float r,
                    float cr, float cg, float cb, float a) {
    int x0 = (int)(cx - r) - 1, x1 = (int)(cx + r) + 1;
    int y0 = (int)(cy - r) - 1, y1 = (int)(cy + r) + 1;
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) {
        float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
        if (dx*dx + dy*dy <= r*r) fcomp(buf, W, x, y, cr, cg, cb, a);
    }
}
static void ss_seg(float *buf, int W, float ax, float ay, float bx, float by,
                   float hw, float cr, float cg, float cb, float a) {
    int x0 = (int)(fminf(ax,bx)-hw)-1, x1 = (int)(fmaxf(ax,bx)+hw)+1;
    int y0 = (int)(fminf(ay,by)-hw)-1, y1 = (int)(fmaxf(ay,by)+hw)+1;
    float vx = bx-ax, vy = by-ay, L2 = vx*vx+vy*vy; if (L2 < 1e-3f) L2 = 1e-3f;
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) {
        float px = x+0.5f-ax, py = y+0.5f-ay;
        float t = (px*vx+py*vy)/L2; if (t < 0) t = 0; if (t > 1) t = 1;
        float dx = px-vx*t, dy = py-vy*t;
        if (dx*dx + dy*dy <= hw*hw) fcomp(buf, W, x, y, cr, cg, cb, a);
    }
}
static void ss_tri(float *buf, int W, float ax, float ay, float bx, float by,
                   float ccx, float ccy, float cr, float cg, float cb, float a) {
    int x0 = (int)fminf(ax,fminf(bx,ccx))-1, x1 = (int)fmaxf(ax,fmaxf(bx,ccx))+1;
    int y0 = (int)fminf(ay,fminf(by,ccy))-1, y1 = (int)fmaxf(ay,fmaxf(by,ccy))+1;
    float d = (by-ccy)*(ax-ccx) + (ccx-bx)*(ay-ccy); if (fabsf(d) < 1e-3f) return;
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) {
        float px = x+0.5f, py = y+0.5f;
        float u = ((by-ccy)*(px-ccx) + (ccx-bx)*(py-ccy)) / d;
        float v = ((ccy-ay)*(px-ccx) + (ax-ccx)*(py-ccy)) / d;
        if (u >= 0 && v >= 0 && u+v <= 1) fcomp(buf, W, x, y, cr, cg, cb, a);
    }
}

/* White icon (with shades for the cube) centred at (cx,cy), extent s. */
static void ss_icon(float *buf, int W, int id, float cx, float cy, float s) {
    const float wr = 0.97f, wg = 0.98f, wb = 1.0f, A = 0.98f;
    float hw = s * 0.17f;
    switch (id) {
    case BTN_JUMP:                 /* up arrow: triangular head + shaft */
        ss_tri(buf, W, cx, cy - s, cx - s*0.95f, cy - s*0.05f, cx + s*0.95f, cy - s*0.05f, wr, wg, wb, A);
        ss_seg(buf, W, cx, cy - s*0.10f, cx, cy + s*0.72f, s*0.30f, wr, wg, wb, A);
        break;
    case BTN_B: {                  /* isometric cube (place) */
        float T  = cy - s*0.92f, U = cy - s*0.46f, Lo = cy + s*0.46f, Bo = cy + s*0.92f;
        float lx = cx - s*0.86f, rx = cx + s*0.86f;
        ss_tri(buf, W, cx, T, rx, U, cx, cy, 0.97f, 0.98f, 1.0f, 1.f);   /* top  */
        ss_tri(buf, W, cx, T, lx, U, cx, cy, 0.97f, 0.98f, 1.0f, 1.f);
        ss_tri(buf, W, lx, U, cx, cy, cx, Bo, 0.72f, 0.74f, 0.80f, 1.f); /* left */
        ss_tri(buf, W, lx, U, lx, Lo, cx, Bo, 0.72f, 0.74f, 0.80f, 1.f);
        ss_tri(buf, W, cx, cy, rx, U, cx, Bo, 0.55f, 0.57f, 0.63f, 1.f); /* right*/
        ss_tri(buf, W, rx, U, rx, Lo, cx, Bo, 0.55f, 0.57f, 0.63f, 1.f);
        break;
    }
    case BTN_A:                    /* pickaxe (mine) */
        ss_seg(buf, W, cx + s*0.55f, cy + s*0.75f, cx - s*0.18f, cy - s*0.28f, hw, wr, wg, wb, A);
        ss_seg(buf, W, cx - s*0.78f, cy + s*0.02f, cx - s*0.30f, cy - s*0.42f, hw*0.9f, wr, wg, wb, A);
        ss_seg(buf, W, cx - s*0.30f, cy - s*0.42f, cx + s*0.30f, cy - s*0.62f, hw*0.9f, wr, wg, wb, A);
        ss_seg(buf, W, cx + s*0.30f, cy - s*0.62f, cx + s*0.78f, cy - s*0.40f, hw*0.9f, wr, wg, wb, A);
        break;
    case BTN_MENU:                 /* three rounded bars */
        for (int k = -1; k <= 1; k++)
            ss_seg(buf, W, cx - s*0.72f, cy + k*s*0.52f, cx + s*0.72f, cy + k*s*0.52f, s*0.15f, wr, wg, wb, A);
        break;
    case BTN_HOTL:                 /* left chevron */
        ss_seg(buf, W, cx + s*0.40f, cy - s*0.70f, cx - s*0.45f, cy, hw, wr, wg, wb, A);
        ss_seg(buf, W, cx - s*0.45f, cy, cx + s*0.40f, cy + s*0.70f, hw, wr, wg, wb, A);
        break;
    case BTN_HOTR:                 /* right chevron */
        ss_seg(buf, W, cx - s*0.40f, cy - s*0.70f, cx + s*0.45f, cy, hw, wr, wg, wb, A);
        ss_seg(buf, W, cx + s*0.45f, cy, cx - s*0.40f, cy + s*0.70f, hw, wr, wg, wb, A);
        break;
    default: break;
    }
}

/* Render one round control to an SDL texture. bodyA = body opacity,
 * shadowA = drop-shadow strength, icon = BTN_* glyph or -1 for none. */
static SDL_Texture *gen_round(SDL_Renderer *ren, Uint8 R, Uint8 G, Uint8 B,
                              float bodyA, float shadowA, int icon) {
    int WS = BTN_TEX * BTN_SS;
    float *f = (float *)calloc((size_t)WS * WS * 4, sizeof(float));
    if (!f) return NULL;
    float cx = WS * 0.5f, cy = WS * 0.5f, rad = WS * 0.40f;
    float br = R/255.f, bg = G/255.f, bb = B/255.f;
    int x0 = (int)(cx-rad)-2, x1 = (int)(cx+rad)+2;
    int y0 = (int)(cy-rad)-2, y1 = (int)(cy+rad)+2;

    /* 1. soft drop shadow (stacked discs = cheap blur) */
    for (int k = 0; k < 7 && shadowA > 0.f; k++)
        ss_disc(f, WS, cx, cy + rad*0.10f, rad*1.0f + k*(WS*0.011f), 0,0,0, shadowA*0.10f);
    /* 2. body: top-lit vertical gradient + edge vignette */
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) {
        float dx = x+0.5f-cx, dy = y+0.5f-cy, d = sqrtf(dx*dx+dy*dy);
        if (d > rad) continue;
        float ny = (dy/rad)*0.5f + 0.5f;
        float shade = 1.28f - 0.52f*ny;
        float vign  = 0.80f + 0.20f*((rad-d)/rad);
        float cr = br*shade*vign, cg = bg*shade*vign, cb = bb*shade*vign;
        if (cr>1)cr=1; if (cg>1)cg=1; if (cb>1)cb=1;
        fcomp(f, WS, x, y, cr, cg, cb, bodyA);
    }
    /* 3. bevel rim: bright at top, dark at bottom */
    float rr = rad - WS*0.012f, rw = WS*0.020f;
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) {
        float dx = x+0.5f-cx, dy = y+0.5f-cy, d = sqrtf(dx*dx+dy*dy);
        if (d > rad) continue;
        float band = 1.f - fabsf(d-rr)/rw;
        if (band <= 0) continue;
        float topness = -dy/rad;
        if (topness > 0) fcomp(f, WS, x, y, 1,1,1, band*0.55f*topness);
        else             fcomp(f, WS, x, y, 0,0,0, band*0.40f*(-topness));
    }
    /* 4. glossy highlight (upper ellipse, clipped to body) */
    float gx = cx, gy = cy - rad*0.36f, grx = rad*0.66f, gry = rad*0.42f;
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) {
        float dx = x+0.5f-cx, dy = y+0.5f-cy;
        if (dx*dx+dy*dy > rad*rad) continue;
        float ex = (x+0.5f-gx)/grx, ey = (y+0.5f-gy)/gry;
        float gv = 1.f - (ex*ex+ey*ey);
        if (gv > 0) fcomp(f, WS, x, y, 1,1,1, gv*gv*0.32f);
    }
    /* 5. icon */
    if (icon >= 0) ss_icon(f, WS, icon, cx, cy, rad*0.52f);

    /* downsample → ARGB8888 (un-premultiply for straight-alpha texture) */
    uint32_t *out = (uint32_t *)malloc((size_t)BTN_TEX*BTN_TEX*4);
    float inv = 1.f / (BTN_SS*BTN_SS);
    for (int y = 0; y < BTN_TEX; y++) for (int x = 0; x < BTN_TEX; x++) {
        float ar=0, ag=0, ab=0, aa=0;
        for (int sy = 0; sy < BTN_SS; sy++) for (int sx = 0; sx < BTN_SS; sx++) {
            float *p = &f[(((size_t)(y*BTN_SS+sy))*WS + (x*BTN_SS+sx))*4];
            ar += p[0]; ag += p[1]; ab += p[2]; aa += p[3];
        }
        ar*=inv; ag*=inv; ab*=inv; aa*=inv;
        uint8_t A = (uint8_t)(aa*255.f + 0.5f), Rr=0, Gg=0, Bb=0;
        if (aa > 1e-4f) {
            Rr = (uint8_t)(fminf(ar/aa,1.f)*255.f+0.5f);
            Gg = (uint8_t)(fminf(ag/aa,1.f)*255.f+0.5f);
            Bb = (uint8_t)(fminf(ab/aa,1.f)*255.f+0.5f);
        }
        out[y*BTN_TEX+x] = ((uint32_t)A<<24)|((uint32_t)Rr<<16)|((uint32_t)Gg<<8)|Bb;
    }
    SDL_Texture *t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                       SDL_TEXTUREACCESS_STATIC, BTN_TEX, BTN_TEX);
    SDL_UpdateTexture(t, NULL, out, BTN_TEX*4);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    free(out); free(f);
    return t;
}

static void gen_controls(SDL_Renderer *ren) {
    /* Linear filtering for the (already AA) control textures. */
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    for (int i = 0; i < BTN_COUNT; i++)
        g_btn_tex[i] = gen_round(ren, BUTTONS[i].cr, BUTTONS[i].cg, BUTTONS[i].cb,
                                 1.0f, 0.32f, BUTTONS[i].id);
    g_stick_base = gen_round(ren, 206, 212, 224, 0.22f, 0.0f, -1);
    g_stick_knob = gen_round(ren, 238, 242, 250, 0.94f, 0.30f, -1);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");   /* back to nearest for the world */
}

/* Blit a pre-rendered round texture centred at (cx,cy) with body radius rad. */
static void blit_round(SDL_Renderer *ren, SDL_Texture *t, int cx, int cy,
                       int rad, float scale, Uint8 bright, Uint8 alpha) {
    if (!t) return;
    int ds = (int)(rad * 2.5f * scale);          /* body is 0.40 of the texture */
    SDL_Rect dst = { cx - ds/2, cy - ds/2, ds, ds };
    SDL_SetTextureColorMod(t, bright, bright, bright);
    SDL_SetTextureAlphaMod(t, alpha);
    SDL_RenderCopy(ren, t, NULL, &dst);
}

/* ---- touch tracking ------------------------------------------------ */
#define MAX_TOUCH 8
enum { ROLE_NONE, ROLE_MOVE, ROLE_LOOK, ROLE_BUTTON };
typedef struct {
    SDL_FingerID id; bool active; int role; int button;
    float sx, sy, cx, cy;   /* start + current, normalised */
} Touch;
static Touch touches[MAX_TOUCH];

static Touch *touch_find(SDL_FingerID id) {
    for (int i = 0; i < MAX_TOUCH; i++)
        if (touches[i].active && touches[i].id == id) return &touches[i];
    return NULL;
}
static Touch *touch_alloc(void) {
    for (int i = 0; i < MAX_TOUCH; i++)
        if (!touches[i].active) return &touches[i];
    return NULL;
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) != 0) {
        SDL_Log("SDL_Init: %s", SDL_GetError());
        return 1;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");   /* nearest, crisp pixels */

    win = SDL_CreateWindow("ThumbyCraft", SDL_WINDOWPOS_UNDEFINED,
        SDL_WINDOWPOS_UNDEFINED, 0, 0, SDL_WINDOW_FULLSCREEN | SDL_WINDOW_SHOWN);
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB565,
        SDL_TEXTUREACCESS_STREAMING, CRAFT_FB_W, CRAFT_FB_H);

    /* Save file in the app's private internal storage. */
    const char *store = SDL_AndroidGetInternalStoragePath();
    if (store) {
        SDL_snprintf(g_sav_path,   sizeof g_sav_path,   "%s/thumbycraft.sav",   store);
        SDL_snprintf(g_thumb_path, sizeof g_thumb_path, "%s/thumbycraft.thumb", store);
    } else {
        SDL_strlcpy(g_sav_path,   "thumbycraft.sav",   sizeof g_sav_path);
        SDL_strlcpy(g_thumb_path, "thumbycraft.thumb", sizeof g_thumb_path);
    }

    audio_init();
    gen_controls(ren);

    uint32_t seed = (uint32_t)(SDL_GetPerformanceCounter() & 0xFFFFFFFFu);
    srand(seed);
    craft_main_init(g_fb, seed);
    craft_main_set_scheme(CRAFT_SCHEME_DPAD_STRAFE);
    try_load();

    /* Look speed. The "Mouse sens" menu slider scales this further at runtime
     * (craft_main_mouse_sens); this base is tuned to feel responsive by
     * default — a full-screen drag turns ~5 rad before the slider. */
    const float LOOK_SENS = 5.5f;   /* radians per full-screen drag, * user sens */
    const float MOVE_DZ   = 0.035f; /* stick deadzone (normalised) */

    bool running = true;
    bool have_save = false;
    Uint32 last_ms = SDL_GetTicks();

    while (running) {
        float look_dx = 0.0f, look_dy = 0.0f;   /* accumulated this frame */

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT:
                running = false; break;
            case SDL_APP_WILLENTERBACKGROUND:
            case SDL_APP_TERMINATING:
                try_save(); have_save = true;
                if (audio_dev) SDL_PauseAudioDevice(audio_dev, 1);
                break;
            case SDL_APP_DIDENTERFOREGROUND:
                if (audio_dev) SDL_PauseAudioDevice(audio_dev, 0);
                break;
            case SDL_RENDER_DEVICE_RESET:
            case SDL_RENDER_TARGETS_RESET:
                if (tex) SDL_DestroyTexture(tex);
                tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB565,
                    SDL_TEXTUREACCESS_STREAMING, CRAFT_FB_W, CRAFT_FB_H);
                break;
            case SDL_FINGERDOWN: {
                Touch *t = touch_alloc();
                if (!t) break;
                t->active = true; t->id = ev.tfinger.fingerId;
                t->sx = t->cx = ev.tfinger.x; t->sy = t->cy = ev.tfinger.y;
                int b = button_at(ev.tfinger.x, ev.tfinger.y);
                if (b >= 0) {
                    t->role = ROLE_BUTTON; t->button = b;
                    if (b == BTN_HOTL) craft_main_hotbar_cycle(-1);
                    if (b == BTN_HOTR) craft_main_hotbar_cycle(+1);
                } else if (ev.tfinger.x < 0.40f) {
                    t->role = ROLE_MOVE;   /* left side = floating move stick */
                } else {
                    t->role = ROLE_LOOK;   /* right side = look drag */
                }
                break;
            }
            case SDL_FINGERMOTION: {
                Touch *t = touch_find(ev.tfinger.fingerId);
                if (!t) break;
                if (t->role == ROLE_LOOK) {
                    look_dx += ev.tfinger.x - t->cx;
                    look_dy += ev.tfinger.y - t->cy;
                }
                t->cx = ev.tfinger.x; t->cy = ev.tfinger.y;
                break;
            }
            case SDL_FINGERUP: {
                Touch *t = touch_find(ev.tfinger.fingerId);
                if (t) { t->active = false; t->role = ROLE_NONE; }
                break;
            }
            default: break;
            }
        }

        Uint32 now_ms = SDL_GetTicks();
        float dt = (now_ms - last_ms) * 0.001f;
        if (dt > 0.1f) dt = 0.1f;
        last_ms = now_ms;

        /* Apply look from this frame's drag. Horizontal: drag right = look
         * right. Vertical follows the in-game "Invert Y" toggle (default on =
         * drag down looks up); flip it in the menu to taste. */
        if (look_dx != 0.0f || look_dy != 0.0f) {
            float s = LOOK_SENS * craft_main_mouse_sens();
            float yd = craft_main_get_invert_y() ? 1.0f : -1.0f;
            craft_main_look(look_dx * s, yd * look_dy * s);
        }

        /* Build input from active touches. */
        CraftInput in = {0};
        for (int i = 0; i < MAX_TOUCH; i++) {
            Touch *t = &touches[i];
            if (!t->active) continue;
            if (t->role == ROLE_MOVE) {
                float dx = t->cx - t->sx, dy = t->cy - t->sy;
                if (dy < -MOVE_DZ) in.up = true;
                if (dy >  MOVE_DZ) in.down = true;
                if (dx < -MOVE_DZ) in.left = true;
                if (dx >  MOVE_DZ) in.right = true;
            } else if (t->role == ROLE_BUTTON) {
                switch (t->button) {
                case BTN_A:    in.a = true; break;
                case BTN_B:    in.b = true; break;
                case BTN_JUMP: in.rb = true; break;
                case BTN_MENU: in.menu = true; break;
                default: break;
                }
            }
        }
        /* Edge flags: the engine needs *_pressed for menu/place taps. Derive
         * from a one-frame latch. */
        static bool pa, pb, prb, pmenu;
        in.a_pressed    = in.a    && !pa;
        in.b_pressed    = in.b    && !pb;
        in.rb_pressed   = in.rb   && !prb;
        in.menu_pressed = in.menu && !pmenu;
        pa = in.a; pb = in.b; prb = in.rb; pmenu = in.menu;

        craft_main_step(&in, dt, 0);

        if (craft_main_take_save_request())     { try_save(); craft_menu_toast("World saved"); }
        if (craft_main_take_load_request())     { craft_menu_toast(try_load() ? "World loaded" : "No save"); }
        (void)craft_main_take_new_world_request();
        (void)have_save;

        /* Present: scale the (wide) game view to fit the screen, preserving
         * aspect (contain), centred. The whole framebuffer — including the
         * HUD — stays visible; any leftover is a thin black margin. */
        int ow, oh;
        SDL_GetRendererOutputSize(ren, &ow, &oh);
        g_ow = ow; g_oh = oh;
        int mind = ow < oh ? ow : oh;
        float sc = (float)ow / CRAFT_FB_W;
        float sch = (float)oh / CRAFT_FB_H;
        if (sch < sc) sc = sch;
        int dw = (int)(CRAFT_FB_W * sc), dh = (int)(CRAFT_FB_H * sc);
        SDL_Rect dst = { (ow - dw) / 2, (oh - dh) / 2, dw, dh };

        SDL_UpdateTexture(tex, NULL, g_fb, CRAFT_FB_W * sizeof(uint16_t));
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, &dst);

        /* --- control overlay ------------------------------------------ */
        /* Left: floating move joystick. Find the active move touch. */
        const Touch *mv = NULL;
        bool pressed[BTN_COUNT] = {0};
        for (int i = 0; i < MAX_TOUCH; i++) {
            if (!touches[i].active) continue;
            if (touches[i].role == ROLE_MOVE) mv = &touches[i];
            else if (touches[i].role == ROLE_BUTTON) pressed[touches[i].button] = true;
        }
        {
            int baseR = (int)(0.135f * mind);
            int knobR = (int)(0.45f * baseR);
            int bx, by, kx, ky;
            if (mv) {                       /* live, anchored where finger went down */
                bx = (int)(mv->sx * ow); by = (int)(mv->sy * oh);
                float dx = (mv->cx - mv->sx) * ow, dy = (mv->cy - mv->sy) * oh;
                float len = SDL_sqrtf(dx*dx + dy*dy);
                if (len > baseR) { dx = dx/len*baseR; dy = dy/len*baseR; }
                kx = bx + (int)dx; ky = by + (int)dy;
            } else {                        /* idle hint at the default anchor */
                bx = (int)(0.16f * ow); by = (int)(0.72f * oh);
                kx = bx; ky = by;
            }
            blit_round(ren, g_stick_base, bx, by, baseR, 1.0f, 255, mv ? 235 : 150);
            blit_round(ren, g_stick_knob, kx, ky, knobR, 1.0f, mv ? 255 : 235, mv ? 255 : 205);
        }

        /* Right: action buttons (pre-rendered textures, pressed = brighter+bigger). */
        for (int i = 0; i < BTN_COUNT; i++) {
            const Button *b = &BUTTONS[i];
            int cx = (int)(b->cx * ow), cy = (int)(b->cy * oh);
            int rad = (int)(b->r * mind);
            bool dn = pressed[i];
            blit_round(ren, g_btn_tex[i], cx, cy, rad,
                       dn ? 1.07f : 1.0f, dn ? 255 : 224, dn ? 255 : 232);
        }

        SDL_RenderPresent(ren);
    }

    try_save();
    for (int i = 0; i < BTN_COUNT; i++) if (g_btn_tex[i]) SDL_DestroyTexture(g_btn_tex[i]);
    if (g_stick_base) SDL_DestroyTexture(g_stick_base);
    if (g_stick_knob) SDL_DestroyTexture(g_stick_knob);
    if (audio_dev) SDL_CloseAudioDevice(audio_dev);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}

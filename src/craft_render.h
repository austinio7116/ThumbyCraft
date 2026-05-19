/*
 * ThumbyCraft — DDA voxel raycaster.
 *
 * Renders the world to a 128×128 RGB565 framebuffer from a camera
 * pose. The hot path is one DDA traversal per pixel; everything else
 * is per-frame setup.
 *
 * Strip-render variant exists so core1 can render the top half of
 * the frame while core0 renders the bottom on RP2350.
 */
#ifndef CRAFT_RENDER_H
#define CRAFT_RENDER_H

#include "craft_types.h"
#include "craft_blocks.h"

typedef struct {
    Vec3  pos;
    float yaw;     /* radians, 0 = +Z */
    float pitch;   /* radians, 0 = horizon, +0.5pi = straight up */
    float fov;     /* radians, vertical */
} CraftCamera;

/* Where the player's crosshair ray currently hits.
 *  hit    = true when a solid block intercepted the ray
 *  bx,by,bz = the block coordinate the crosshair points at
 *  fx,fy,fz = the air-side neighbour (where a placed block goes)
 *  face   = which face of (bx,by,bz) was struck
 */
typedef struct {
    bool hit;
    int  bx, by, bz;
    int  fx, fy, fz;
    int  face;
    float distance;
} CraftRayHit;

void craft_render_frame(const CraftCamera *cam, uint16_t *fb);

/* Two-call rendering path:
 *   craft_render_begin(cam)       — precompute basis + per-column rays
 *   craft_render_strip(cam, fb, y0, y1) — render a strip [y0, y1)
 *
 * On the device, core0 calls render_begin once per frame and BOTH
 * cores then call render_strip on their own y-range using the
 * shared basis. Calling render_strip without a preceding _begin in
 * the same frame is a bug (will use stale or zero basis).
 */
void craft_render_begin(const CraftCamera *cam);
void craft_render_strip(const CraftCamera *cam, uint16_t *fb,
                        int y_start, int y_end);

/* Cast the ray through the centre pixel — used for place/break. */
CraftRayHit craft_render_pick(const CraftCamera *cam);

/* Toggle distance fog on/off (perf knob — fog adds ~5% per pixel). */
void craft_render_set_fog(bool on);

/* Toggle the procedural cloud overlay on/off. When off, sky pixels
 * are pure gradient — cheaper, and avoids the bilinear-noise cost
 * for users who don't want the cloud texture. */
void craft_render_set_clouds(bool on);
bool craft_render_get_clouds(void);

/* Drive the day/night cycle. `world_time` is seconds since world
 * start (or current cycle position — wraps every 240 s by the
 * convention craft_main uses). Updates sun position, sky colours,
 * and per-face brightness for the next render_begin / strip. */
void craft_render_set_time(float world_time);

/* Where is the sun right now? Used by HUD / mob behaviour /
 * future shadow code. Returns sin(sun_angle) — +1 noon, -1
 * midnight, 0 at horizon. */
float craft_render_sun_y(void);

/* Day/night ambient brightness as 0..256 fixed-point Q8. The world
 * raycaster multiplies its base face_shade table by this before
 * applying the torch lightmap floor. Exposed so the sprite post-pass
 * can use the same value when shading cuboid blocks. */
int   craft_render_brightness_q8(void);

/* Paint the sun + moon discs onto the framebuffer (after the world
 * raycaster, before the HUD). Cheap — does a 3-axis dot-product per
 * disc to project onto the screen, then a textured filled circle.
 * Skipped entirely when the disc is behind the camera or below the
 * horizon. */
void craft_render_celestials(const CraftCamera *cam, uint16_t *fb);

/* Paint the fixed-celestial-sphere starfield. Stars live at static
 * world-space directions (set once at startup), so they stay put as
 * the player turns and only fade with the day/night cycle. Skipped
 * entirely when the sun is up. Z-tests against craft_zbuf so trees
 * etc. on the horizon hide the stars behind them.
 *
 * Call after craft_render_strip + before craft_render_celestials so
 * the moon draws on top of stars. */
void craft_render_stars(const CraftCamera *cam, uint16_t *fb);

/* --- Z-buffer (Phase 26) ------------------------------------------ *
 * Per-pixel quantised depth populated by craft_render_strip. Values
 * are clamp(world_distance * 255 / CRAFT_MAX_DIST_FOR_ZBUF, 0, 255);
 * sky/no-hit pixels are 255 (the "infinity" sentinel). Mobs and
 * future overlay sprites z-test against this buffer to occlude
 * correctly behind world blocks. */
#define CRAFT_MAX_DIST_FOR_ZBUF 60.0f
extern uint8_t craft_zbuf[CRAFT_FB_W * CRAFT_FB_H];

/* Project a world-space point onto the screen. Returns false if the
 * point is behind the camera (callers should skip drawing). sx/sy
 * are in framebuffer pixel coords; depth is the same quantised
 * units as craft_zbuf, ready for direct comparison. */
bool craft_render_project(const CraftCamera *cam, Vec3 world_pos,
                          int *out_sx, int *out_sy, uint8_t *out_depth,
                          float *out_dist);

/* Wireframe outline of the block the player's crosshair is pointing
 * at — 12 edges of a unit cube. Subtle dark grey, drawn after the
 * world strip + mobs so it sits in front of the block it outlines.
 * No-op when the pick ray doesn't hit anything or hits too far. */
void craft_render_pick_outline(const CraftCamera *cam, uint16_t *fb);

/* Paint the player's currently-held item into a fixed viewport in the
 * bottom-right of the framebuffer. Uses its own virtual near-camera —
 * does NOT read or write craft_zbuf, so the held item always sits in
 * front of whatever was there. Tools/weapons/bow/arrow render from
 * craft_tool_model(); placeable blocks render as a tilted cube with
 * top/side/bottom shading from the block texture's centre pixels.
 *
 * `swing_t` is 0.0..1.0: 0 = idle, 1.0 = just hit. The caller decays
 * it ~5/sec back to 0 — the renderer uses it to dip the item and
 * tilt it forward during the swing. */
void craft_render_held_item(BlockId held, uint16_t *fb, float swing_t,
                            float bow_draw_t);

#endif

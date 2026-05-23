/*
 * ThumbyCraft — player state, controls, AABB physics.
 *
 * Control scheme (v3):
 *
 *   D-pad L/R         turn left / right
 *   D-pad U/D         walk forward / back (or pitch camera in look mode)
 *   A                 break block
 *   B                 place block
 *   RB                jump (walk mode) / ascend (fly mode)
 *   LB held           look mode — D-pad U/D pitches camera while held
 *   LB tapped         toggle sticky look mode (D-pad keeps pitching
 *                     until tapped again)
 *
 *   MENU + LB         hotbar prev
 *   MENU + RB         hotbar next
 *   MENU + A          toggle fly mode (also available from pause menu)
 *   MENU (released without chord) → open pause menu
 *
 * No strafe in v3 — Minecraft 4K-style tank turning.
 * Starts in walk mode (gravity on); MENU + A or the pause menu
 * toggles to fly.
 *
 * Walk mode: gravity + AABB collision + auto-step-up onto 1-block
 * obstacles. RB tap jumps on ground.
 *
 * Fly mode: D-pad U/D moves along FULL camera forward (pitch
 * included). RB held ascends straight up.
 */
#include "craft_player.h"
#include "craft_main.h"
#include "craft_world.h"
#include "craft_render.h"
#include "craft_audio.h"
#include "craft_mobs.h"
#include "craft_redstone.h"
#include "craft_torches.h"
#include "craft_furnace.h"
#include "craft_chests.h"

#include <string.h>

#define PLAYER_HALF_W 0.30f
#define PLAYER_HEIGHT 1.70f
#define PLAYER_EYE    1.60f
#define WALK_SPEED    4.4f
#define FLY_SPEED     9.0f
#define TURN_SPEED    2.2f
#define PITCH_SPEED   2.0f
#define JUMP_VEL      7.5f
#define GRAVITY      -22.0f
#define LB_TAP_MAX    0.25f       /* sec — release within this = tap */

void craft_player_init(CraftPlayer *p, Vec3 spawn) {
    memset(p, 0, sizeof *p);
    p->cam.pos   = spawn;
    p->cam.yaw   = 0.0f;
    p->cam.pitch = 0.0f;
    p->cam.fov   = 70.0f * 3.14159265f / 180.0f;
    p->mode      = CRAFT_MODE_CREATIVE;
    p->fly_mode  = false;
    p->hp        = CRAFT_PLAYER_MAX_HP;
    p->spawn_point = spawn;
    /* Hotbar starts empty in BOTH modes — fills as the player mines.
     * memset above already zeroed it (BLK_AIR == 0). */
    p->hotbar_idx = 0;
}

void craft_player_set_mode(CraftPlayer *p, CraftGameMode mode) {
    p->mode = mode;
    if (mode == CRAFT_MODE_SURVIVAL) {
        p->fly_mode      = false;
        p->vel.y         = 0;
        p->hp            = CRAFT_PLAYER_MAX_HP;
        p->respawn_timer = 0.0f;
        for (int i = 0; i < BLK_COUNT; i++) p->inventory[i] = 0;
        /* Starter torches so the player can immediately test caves
         * and night exploration without having to find coal first. */
        p->inventory[BLK_TORCH] = 8;
        for (int i = 0; i < CRAFT_HOTBAR_SLOTS; i++) {
            if (p->hotbar[i] == BLK_AIR) { p->hotbar[i] = BLK_TORCH; break; }
        }
    } else {
        p->hp              = CRAFT_PLAYER_MAX_HP;
        p->damage_cooldown = 0;
        p->damage_flash    = 0;
        p->respawn_timer   = 0.0f;
    }
}

/* Set by craft_player_signal_win from anywhere in the codebase. The
 * player tick consumes the flag and arms the win-banner timer. */
static bool s_win_pending;
void craft_player_signal_win(void) { s_win_pending = true; }

void craft_player_take_damage(CraftPlayer *p, int amount) {
    if (p->mode != CRAFT_MODE_SURVIVAL) return;
    if (p->damage_cooldown > 0.0f) return;
    if (p->respawn_timer > 0.0f) return;     /* already dead */
    if (amount <= 0) return;
    p->hp -= amount;
    if (p->hp <= 0) {
        p->hp = 0;
        p->respawn_timer = 3.0f;        /* 3 sec before respawn */
    }
    p->damage_cooldown = CRAFT_PLAYER_DAMAGE_COOLDOWN;
    p->no_damage_t     = 0.0f;
    p->damage_flash    = 0.35f;
    craft_audio_jump();
}

static bool aabb_blocked(float px, float feet_y, float pz);   /* fwd */

bool craft_player_stuck_now(const CraftPlayer *p) {
    float feet_y = p->cam.pos.y - PLAYER_EYE;
    return aabb_blocked(p->cam.pos.x, feet_y, p->cam.pos.z);
}

int craft_player_neighbor_block(const CraftPlayer *p, int dx, int dz) {
    float feet_y = p->cam.pos.y - PLAYER_EYE;
    int cx = (int)floorf(p->cam.pos.x) + dx;
    int cy = (int)floorf(feet_y);
    int cz = (int)floorf(p->cam.pos.z) + dz;
    return (int)craft_world_get(cx, cy, cz);
}

static bool aabb_blocked(float px, float feet_y, float pz) {
    int x0 = (int)floorf(px - PLAYER_HALF_W);
    int x1 = (int)floorf(px + PLAYER_HALF_W);
    int z0 = (int)floorf(pz - PLAYER_HALF_W);
    int z1 = (int)floorf(pz + PLAYER_HALF_W);
    /* +1e-3 epsilon on the feet-Y floor defeats a float-precision bug
     * that put the player half a micro-unit below their integer-y
     * surface. Without the bias, (ground_y + 1) + PLAYER_EYE - PLAYER_EYE
     * occasionally rounds to ground_y + 0.9999996 instead of ground_y + 1,
     * making the foot AABB intersect the very grass cell the player
     * is standing on. The user reported this as "stuck on flat ground"
     * with HUD diagnostic fy=30.99..., grass everywhere — auto-step
     * was hiding it. */
    int y0 = (int)floorf(feet_y + 0.001f);
    int y1 = (int)floorf(feet_y + PLAYER_HEIGHT);
    for (int y = y0; y <= y1; y++)
        for (int z = z0; z <= z1; z++)
            for (int x = x0; x <= x1; x++)
                if (craft_block_solid(craft_world_get(x, y, z))) return true;
    return false;
}

/* Auto-step over 1-block obstacles.
 *
 * Strategy: smooth teleport (cam.pos.y += 1.0, no jump arc) gated by
 * a per-player cooldown. The cooldown is the critical piece — without
 * it, walking across bumpy terrain or up stairs triggers the step
 * every couple of frames and the player visibly bunny-hops. With
 * AUTOSTEP_COOLDOWN, at most one auto-step per ~350 ms, which is
 * slow enough to be invisible during normal walking but still fast
 * enough that stair climbing flows naturally.
 *
 * Two compound gates:
 *   - within-frame: *stepped_up caps to one step per tick so X+Z
 *     chained calls don't stack two upward bumps in one frame
 *     (that's how the old code accidentally let players climb).
 *   - cross-frame: p->autostep_cooldown enforces the time spacing. */
#define AUTOSTEP_COOLDOWN 0.40f

static bool try_horizontal(CraftPlayer *p, float nx, float nz,
                           bool *stepped_up) {
    float feet_y = p->cam.pos.y - PLAYER_EYE;
    if (!aabb_blocked(nx, feet_y, nz)) {
        p->cam.pos.x = nx;
        p->cam.pos.z = nz;
        return true;
    }
    /* Auto-step is INSTANT — no time gate, no cooldown — because the
     * visible "hop" used to come from teleporting cam.pos.y up by 1.0.
     * The new model decouples logical position from camera Y: we still
     * bump cam.pos.y so collision and forward motion continue, but we
     * also set step_lag so the renderer offsets the camera DOWN by 1
     * unit and lerps it back to zero over ~0.15 s. The player keeps
     * moving with no perceived pause; the camera glides up smoothly
     * to the new ground level. */
    if (!p->fly_mode && p->on_ground &&
        !*stepped_up &&
        p->vel.y <= 0.001f &&
        !aabb_blocked(nx, feet_y + 1.0f, nz)) {
        p->cam.pos.x = nx;
        p->cam.pos.z = nz;
        p->cam.pos.y += 1.0f;
        p->step_lag  += 1.0f;
        /* Cap accumulation so a fast traverse over a staircase doesn't
         * leave the camera floating metres below the player. */
        if (p->step_lag > 1.5f) p->step_lag = 1.5f;
        *stepped_up = true;
        p->stuck_against_wall_t = 0.0f;
        return true;
    }
    return false;
}

void craft_player_tick(CraftPlayer *p, const CraftInput *in, float dt) {
    p->broke_block = false;
    p->placed_block = false;
    /* Camera-lag decay — catch the visual y up to the logical position
     * over ~0.17 s (6 m/s × ~0.17 s = 1.0 cell). The visual offset is
     * applied at render time only; physics here uses cam.pos.y as the
     * logical position. */
    if (p->step_lag > 0.0f) {
        p->step_lag -= 6.0f * dt;
        if (p->step_lag < 0.0f) p->step_lag = 0.0f;
    }
    if (p->autostep_cooldown > 0.0f) {
        p->autostep_cooldown -= dt;
        if (p->autostep_cooldown < 0.0f) p->autostep_cooldown = 0.0f;
    }

    /* ----- MENU state machine (open-menu disambiguation) ------- */
    bool menu_just_pressed  = in->menu && !p->_menu_prev;
    bool menu_just_released = !in->menu && p->_menu_prev;
    if (menu_just_pressed) p->_menu_chord_used = false;

    /* ----- LB/RB state (walk-button + chord suppression) -------- */
    bool lb_just_pressed  = in->lb && !p->_lb_prev;
    bool rb_just_pressed  = in->rb && !p->_rb_prev;
    if (lb_just_pressed) p->_lb_consumed_by_chord = false;
    if (rb_just_pressed) p->_rb_consumed_by_chord = false;

    /* ----- MENU + chord actions -------------------------------- */
    if (in->menu) {
        if (in->lb_pressed) {
            p->hotbar_idx = (p->hotbar_idx + CRAFT_HOTBAR_SLOTS - 1) % CRAFT_HOTBAR_SLOTS;
            p->_menu_chord_used = true;
            p->_lb_consumed_by_chord = true;
        }
        if (in->rb_pressed) {
            p->hotbar_idx = (p->hotbar_idx + 1) % CRAFT_HOTBAR_SLOTS;
            p->_menu_chord_used = true;
            /* In CLASSIC_FLIP the walk button is RB — suppress walking
             * after the menu chord consumes the press, same pattern as
             * LB in CLASSIC. */
            p->_rb_consumed_by_chord = true;
        }
        if (in->a_pressed) {
            /* Fly toggle only allowed in creative mode. */
            if (p->mode == CRAFT_MODE_CREATIVE) {
                p->fly_mode = !p->fly_mode;
                p->vel = v3(0, 0, 0);
                p->request_fly_toast = true;
            }
            p->_menu_chord_used = true;
        }
    }

    /* MENU release without chord → open pause menu. */
    if (menu_just_released && !p->_menu_chord_used) {
        p->request_menu = true;
    }
    p->_menu_prev = in->menu;

    /* ----- Look / move dispatch by scheme ----------------------
     *
     *   1. CLASSIC      — LB walk, RB jump, D-pad LR turn, UD pitch
     *   2. CLASSIC_FLIP — LB jump, RB walk, D-pad LR turn, UD pitch
     *   3. DPAD_STRAFE  — D-pad UD walk fwd/back, LR strafe;
     *                     LB held flips D-pad into look mode
     *                     (LR turn, UD pitch); RB tap jump.
     *   4. DPAD_TURN    — D-pad UD walk fwd/back, LR turn;
     *                     LB held flips D-pad UD into pitch
     *                     (LR stays turn); RB tap jump.
     *
     * Schemes 1 + 2 use a double-tap-then-hold gesture on the walk
     * button (LB or RB respectively) to walk in reverse instead of
     * forward — see walk_dtap_update() below. */
    int scheme = craft_main_scheme();

    /* Console schemes: action buttons live on A/B/LB/RB and the
     * look-modifier moves from LB to B-hold. Movement still flows
     * through the D-pad like schemes 3/4. */
    bool console_turn   = (scheme == CRAFT_SCHEME_CONSOLE_TURN);
    bool console_strafe = (scheme == CRAFT_SCHEME_CONSOLE_STRAFE);
    bool console        = console_turn || console_strafe;

    /* Which D-pad axes drive yaw vs pitch this frame. */
    bool dpad_pitches, dpad_turns_lr, dpad_strafes_lr;
    if (scheme == CRAFT_SCHEME_CLASSIC || scheme == CRAFT_SCHEME_CLASSIC_FLIP) {
        dpad_pitches    = true;
        dpad_turns_lr   = true;
        dpad_strafes_lr = false;
    } else if (scheme == CRAFT_SCHEME_DPAD_STRAFE || console_strafe) {
        /* Look modifier: LB-held (or B-held in console) flips D-pad
         * from move into look. */
        bool look_mod   = console ? in->b
                                  : (in->lb && !p->_lb_consumed_by_chord);
        dpad_pitches    = look_mod;          /* UD only pitches when modifier held */
        dpad_turns_lr   = look_mod;          /* LR turns only when modifier held */
        dpad_strafes_lr = !look_mod;         /* default LR is strafe */
    } else { /* DPAD_TURN or CONSOLE_TURN */
        bool look_mod   = console ? in->b
                                  : (in->lb && !p->_lb_consumed_by_chord);
        dpad_pitches    = look_mod;          /* UD only pitches when modifier held */
        dpad_turns_lr   = true;              /* LR always turns */
        dpad_strafes_lr = false;
    }

    if (dpad_turns_lr) {
        if (in->left)  p->cam.yaw -= TURN_SPEED * dt;
        if (in->right) p->cam.yaw += TURN_SPEED * dt;
    }
    if (dpad_pitches) {
        float pitch_sign = p->invert_y ? -1.0f : 1.0f;
        if (in->up)    p->cam.pitch += PITCH_SPEED * dt * pitch_sign;
        if (in->down)  p->cam.pitch -= PITCH_SPEED * dt * pitch_sign;
    }
    const float pmax = 85.0f * 3.14159265f / 180.0f;
    if (p->cam.pitch >  pmax) p->cam.pitch =  pmax;
    if (p->cam.pitch < -pmax) p->cam.pitch = -pmax;

    /* ----- Camera basis vectors -------------------------------- */
    float cy = cosf(p->cam.yaw),  sy = sinf(p->cam.yaw);
    float cp = cosf(p->cam.pitch), sp = sinf(p->cam.pitch);
    Vec3 fwd_full = v3(sy * cp, sp, cy * cp);
    Vec3 fwd_h    = v3(sy, 0.0f, cy);
    /* Player-right (horizontal). Perpendicular to fwd_h, pointing to
     * the player's right hand. Used for strafe in DPAD_STRAFE. */
    Vec3 right_h  = v3(cy, 0.0f, -sy);

    /* ----- Walk-button double-tap-hold state (schemes 1+2 only) -
     *
     * Walk-btn = LB in scheme 1, RB in scheme 2. The two PRESSES of
     * the double-tap must both land within a 300 ms window from the
     * FIRST press — i.e. press-1 → release → press-2, all inside
     * 300 ms. The gap is timed from press-1 onward (not from the
     * release), so a long hold-then-quick-release-and-repress does
     * NOT count: the long hold consumes the window before the
     * second press lands. Once armed and a second press inside the
     * window arrives, holding it walks in reverse; releasing resets.
     * Schemes 3 and 4 use D-pad DOWN for reverse so the dtap state
     * stays parked. */
    bool walk_btn;
    bool walk_btn_just_pressed;
    bool walk_btn_consumed;
    if (scheme == CRAFT_SCHEME_CLASSIC_FLIP) {
        walk_btn              = in->rb;
        walk_btn_just_pressed = rb_just_pressed;
        walk_btn_consumed     = p->_rb_consumed_by_chord;
    } else {
        /* Scheme 1, plus the parked path for 3/4 (where this state
         * just stays idle). */
        walk_btn              = in->lb;
        walk_btn_just_pressed = lb_just_pressed;
        walk_btn_consumed     = p->_lb_consumed_by_chord;
    }
    bool walk_btn_just_released =
        !walk_btn && ((scheme == CRAFT_SCHEME_CLASSIC_FLIP) ? p->_rb_prev : p->_lb_prev);
    (void)walk_btn_just_released;   /* unused — see note above */

    if (scheme == CRAFT_SCHEME_CLASSIC || scheme == CRAFT_SCHEME_CLASSIC_FLIP) {
        if (walk_btn_just_pressed && !walk_btn_consumed) {
            if (p->_walk_dtap_armed && p->_walk_dtap_t <= 0.30f) {
                /* Second press inside the window: engage reverse. */
                p->_walk_reverse    = true;
                p->_walk_dtap_armed = false;
            } else {
                /* First press of a fresh gesture: start the timer. */
                p->_walk_dtap_t     = 0.0f;
                p->_walk_dtap_armed = true;
                p->_walk_reverse    = false;
            }
        }
        if (p->_walk_dtap_armed) {
            p->_walk_dtap_t += dt;
            if (p->_walk_dtap_t > 0.30f) p->_walk_dtap_armed = false;
        }
        /* Releasing the walk button always drops reverse — the next
         * single press is forward unless it lands inside a fresh
         * 300 ms window (i.e. it's the 2nd press of a new dtap). */
        if (!walk_btn) p->_walk_reverse = false;
    } else {
        /* Park the dtap state so a scheme switch can't start with a
         * latched "in reverse" flag. */
        p->_walk_dtap_armed = false;
        p->_walk_reverse    = false;
    }

    /* ----- Build wish vector ----------------------------------- */
    Vec3 wish = v3(0, 0, 0);
    Vec3 fwd  = p->fly_mode ? fwd_full : fwd_h;
    /* `walk_active` feeds the auto-step "stuck against wall" timer
     * further down (see line near vel.x/vel.z assignment). Treat it
     * as "player is trying to move horizontally this frame" so the
     * timer ticks up only while the wish-vector is non-zero. */
    bool walk_active;  /* assigned after wish-vector population below */

    if (scheme == CRAFT_SCHEME_CLASSIC || scheme == CRAFT_SCHEME_CLASSIC_FLIP) {
        /* Walk = walk_btn held, direction flips when _walk_reverse. */
        bool walk_active = walk_btn && !walk_btn_consumed;
        if (walk_active) {
            if (p->_walk_reverse) {
                wish = v3(-fwd.x, -fwd.y, -fwd.z);
            } else {
                wish = fwd;
            }
        }
    } else {
        /* Schemes 3/4/5/6: D-pad UP/DOWN walk fwd/back when NOT in
         * look modifier mode. Look-mod is LB for 3/4, B for 5/6. */
        bool look_mod = console ? in->b
                                : (in->lb && !p->_lb_consumed_by_chord);
        if (!look_mod) {
            if (in->up)   wish = fwd;
            if (in->down) wish = v3(-fwd.x, -fwd.y, -fwd.z);
            if (scheme == CRAFT_SCHEME_DPAD_STRAFE || console_strafe) {
                if (in->left) {
                    wish.x -= right_h.x;
                    wish.z -= right_h.z;
                }
                if (in->right) {
                    wish.x += right_h.x;
                    wish.z += right_h.z;
                }
            }
            /* DPAD_TURN / CONSOLE_TURN: LR is already turning via yaw above. */
        }
    }
    walk_active = (wish.x != 0.0f) || (wish.z != 0.0f);

    if (p->fly_mode) {
        /* Fly mode "ascend straight up" button:
         *   schemes 1, 3, 4 — RB held (RB is the spare button there)
         *   scheme 2        — RB is the walk button, so ascend is
         *                     reached by walking + looking up; no
         *                     extra straight-up override.
         */
        /* Ascend straight up: RB in schemes 1/3/4, A in console.
         * Scheme 2 (CLASSIC_FLIP) has no spare button — fly-ascend
         * comes from look-up + walk in that scheme. */
        bool ascend_held = console ? in->a : in->rb;
        if (scheme == CRAFT_SCHEME_CLASSIC_FLIP) ascend_held = false;
        if (ascend_held) wish.y += 1.0f;
        float wl = sqrtf(wish.x*wish.x + wish.y*wish.y + wish.z*wish.z);
        if (wl > 0.001f) {
            float s = FLY_SPEED / wl;
            p->vel.x = wish.x * s;
            p->vel.y = wish.y * s;
            p->vel.z = wish.z * s;
        } else {
            p->vel = v3(0, 0, 0);
        }
    } else {
        float wl = sqrtf(wish.x * wish.x + wish.z * wish.z);
        if (wl > 0.001f) {
            p->vel.x = wish.x / wl * WALK_SPEED;
            p->vel.z = wish.z / wl * WALK_SPEED;
        } else {
            p->vel.x = 0;
            p->vel.z = 0;
        }
        p->vel.y += GRAVITY * dt;
        /* Ladder climbing.
         *
         * Engagement varies by scheme:
         *   schemes 1, 2 — HOLD the walk button (LB or RB) with
         *                  positive pitch to ascend; pitch <= -0.05
         *                  descends once engaged. Direction is by
         *                  pitch.
         *   schemes 3, 4 — D-pad UP held to ascend, D-pad DOWN held
         *                  to descend. Direction is by D-pad axis;
         *                  no pitch dependency.
         *
         * In every scheme: once the climber lands on the ground a
         * lockout latches so the next ladder cell in a corridor
         * floor doesn't immediately re-grab. Release the engage
         * button + fresh press in the engage direction to re-grab. */
        int cx = (int)floorf(p->cam.pos.x);
        int cz = (int)floorf(p->cam.pos.z);
        int cy_feet = (int)floorf(p->cam.pos.y - PLAYER_EYE);
        int cy_chest = cy_feet + 1;
        bool ladder_adj = false;
        /* Include the player's own cell — the ladder sprite lives
         * in the cell adjacent to the wall, so the climber is
         * inside that cell. */
        const int adj[5][2] = { {0,0}, {1,0}, {-1,0}, {0,1}, {0,-1} };
        for (int i = 0; i < 5 && !ladder_adj; i++) {
            int ax = cx + adj[i][0];
            int az = cz + adj[i][1];
            if (craft_world_get(ax, cy_feet, az)  == BLK_LADDER) ladder_adj = true;
            if (craft_world_get(ax, cy_chest, az) == BLK_LADDER) ladder_adj = true;
        }

        bool climb_engage = false;
        float climb_vel = 0.0f;
        if (scheme == CRAFT_SCHEME_CLASSIC || scheme == CRAFT_SCHEME_CLASSIC_FLIP) {
            /* Lockout clears the moment the walk-button is released. */
            if (!walk_btn) p->climb_lockout = false;
            climb_engage = walk_btn && !walk_btn_consumed && !in->menu &&
                           ladder_adj && !p->climb_lockout &&
                           (p->climbing || p->cam.pitch >= 0.10f);
            if (climb_engage)
                climb_vel = (p->cam.pitch >= -0.05f) ? 3.5f : -3.5f;
        } else {
            /* Schemes 3/4: D-pad UP / DOWN engage. Lockout clears
             * when neither UP nor DOWN is held. */
            if (!in->up && !in->down) p->climb_lockout = false;
            bool up_eng   = in->up   && ladder_adj && !p->climb_lockout && !in->menu;
            bool down_eng = in->down && ladder_adj && !p->climb_lockout && !in->menu;
            climb_engage = (p->climbing && (up_eng || down_eng)) ||
                           (!p->climbing && up_eng);
            if (climb_engage)
                climb_vel = in->down ? -3.5f : 3.5f;
        }
        if (climb_engage) {
            p->climbing  = true;
            p->vel.y     = climb_vel;
            p->vel.x     = 0.0f;
            p->vel.z     = 0.0f;
            p->fall_peak_y = p->cam.pos.y;
        } else {
            /* Not engaging this frame — drop the climb flag so the
             * next tick treats this as a fresh grab attempt. */
            if (p->climbing) p->climbing = false;
        }
        /* Jump:
         *   schemes 1, 3, 4 — RB tap
         *   scheme 2        — LB tap (LB is the jump button there)
         *   schemes 5, 6    — A tap (console layout)
         *
         * Suppressed while MENU is held so MENU+LB/RB hotbar chords
         * (or MENU+A fly-toggle) don't accidentally launch the player. */
        bool jump_pressed;
        if (scheme == CRAFT_SCHEME_CLASSIC_FLIP) {
            jump_pressed = in->lb_pressed && !p->_lb_consumed_by_chord;
        } else if (console) {
            jump_pressed = in->a_pressed;
        } else {
            jump_pressed = in->rb_pressed;
        }
        if (jump_pressed && (p->on_ground || ladder_adj) && !in->menu) {
            p->vel.y = JUMP_VEL;
            p->on_ground = false;
        }
    }

    p->_lb_prev = in->lb;
    p->_rb_prev = in->rb;

    /* ----- AABB sweep ------------------------------------------ */
    float dx = p->vel.x * dt;
    float dz = p->vel.z * dt;
    float orig_x = p->cam.pos.x;
    float orig_z = p->cam.pos.z;
    bool stepped = false;
    if (!try_horizontal(p, p->cam.pos.x + dx, p->cam.pos.z, &stepped)) p->vel.x = 0;
    if (!try_horizontal(p, p->cam.pos.x, p->cam.pos.z + dz, &stepped)) p->vel.z = 0;
    /* Sustained-contact timer feeds the auto-step gate inside
     * try_horizontal. Reset whenever the player actually moved (or
     * stepped up), accumulate while trying-but-stuck. */
    bool moved = (p->cam.pos.x != orig_x) || (p->cam.pos.z != orig_z) || stepped;
    if (moved) {
        p->stuck_against_wall_t = 0.0f;
    } else if (walk_active) {
        p->stuck_against_wall_t += dt;
        if (p->stuck_against_wall_t > 1.0f) p->stuck_against_wall_t = 1.0f;
    }

    float feet_y = p->cam.pos.y - PLAYER_EYE;
    float ny_feet = feet_y + p->vel.y * dt;
    bool was_on_ground = p->on_ground;
    if (!aabb_blocked(p->cam.pos.x, ny_feet, p->cam.pos.z)) {
        p->cam.pos.y = ny_feet + PLAYER_EYE;
        p->on_ground = false;
    } else {
        if (p->vel.y < 0) {
            int ground_y = (int)floorf(ny_feet);
            p->cam.pos.y = (float)(ground_y + 1) + PLAYER_EYE;
            p->on_ground = true;
        }
        p->vel.y = 0;
    }
    /* Track peak Y over the current air-time so we can compute fall
     * damage on landing. Walk mode + survival only — fly mode and
     * creative skip the bookkeeping. */
    if (!p->fly_mode) {
        if (!p->on_ground) {
            if (p->cam.pos.y > p->fall_peak_y) p->fall_peak_y = p->cam.pos.y;
        } else {
            if (!was_on_ground) {
                float drop = p->fall_peak_y - p->cam.pos.y;
                /* Slime block underfoot — cancel fall damage and
                 * rebound with ~60% of the impact speed (vanilla-ish),
                 * with a gentle floor so even a small step bounces. */
                int fbx = (int)floorf(p->cam.pos.x);
                int fby = (int)floorf(p->cam.pos.y - PLAYER_EYE - 0.1f);
                int fbz = (int)floorf(p->cam.pos.z);
                if (craft_world_get(fbx, fby, fbz) == BLK_SLIME_BLOCK &&
                    drop > 0.4f) {
                    float impact = sqrtf(2.0f * 22.0f * drop);   /* = -GRAVITY */
                    float bounce = impact * 0.6f;
                    if (bounce > 16.0f) bounce = 16.0f;
                    p->vel.y     = bounce;
                    p->on_ground = false;
                } else {
                    /* 1 HP per block fallen beyond a 10-block grace
                     * (generous — the 128-pixel screen makes vertical
                     * drops feel taller). Clamped to [0, 20]. */
                    int dmg = (int)(drop - 10.0f + 0.5f);
                    if (dmg > 20) dmg = 20;
                    if (dmg > 0) craft_player_take_damage(p, dmg);
                }
            }
            p->fall_peak_y = p->cam.pos.y;
        }
        /* Touched ground while on a ladder — latch the climb lockout
         * so a ladder shaft that bottoms out on a stone floor doesn't
         * keep you stuck. You release LB + represss with upward
         * pitch to re-engage. */
        if (p->on_ground && p->climbing) {
            p->climbing = false;
            p->climb_lockout = true;
        }
    }

    /* World is infinite in X/Z — only Y stays clamped to the buffer
     * height. craft_world_maybe_shift in craft_main keeps the player
     * roughly centred in the loaded window. */
    if (p->cam.pos.y < 1.0f) p->cam.pos.y = 1.0f;
    if (p->cam.pos.y > CRAFT_WORLD_Y - 1.0f) p->cam.pos.y = CRAFT_WORLD_Y - 1.0f;

    /* Footstep SFX removed — the noise-burst layer sounded harsh under
     * the 3× loudness boost and the field reported it as distracting. */
    p->step_acc = 0.0f;

    /* Pressure-pad detection. Player feet sit one block above their
     * cam.pos.y - PLAYER_EYE; the cell their feet stand IN is the
     * pad. Report it (or clear) so the redstone tick can drive
     * neighbours. */
    {
        int pf_y = (int)floorf(p->cam.pos.y - PLAYER_EYE);
        int pf_x = (int)floorf(p->cam.pos.x);
        int pf_z = (int)floorf(p->cam.pos.z);
        BlockId stood = craft_world_get(pf_x, pf_y, pf_z);
        if (stood == BLK_PRESSURE_PAD) {
            craft_redstone_note_pressure(pf_x, pf_y, pf_z);
        } else {
            craft_redstone_note_pressure(0, -1, 0);
        }
    }

    /* ----- Place / break / attack (only when MENU not held) ---- */
    if (!in->menu) {
        /* Console schemes swap the action-button assignments: attack/
         * break + bow-draw move from A to RB, and place moves from B
         * to LB. Compute the effective signals up-front so the rest
         * of this section can stay scheme-agnostic. */
        bool attack_held    = console ? in->rb : in->a;
        bool attack_pressed = console
            ? (in->rb_pressed && !p->_rb_consumed_by_chord)
            : in->a_pressed;
        bool place_pressed  = console
            ? (in->lb_pressed && !p->_lb_consumed_by_chord)
            : in->b_pressed;
        /* Bow handling — attack-button held with a bow + arrows enters
         * "drawing" state, snaps yaw to the nearest hostile mob inside
         * a ±60° cone within 16 blocks, and fires on release.
         * Suppresses the regular break/attack while the player is
         * aiming. */
        bool bow_held = p->hotbar[p->hotbar_idx] == BLK_BOW &&
                        p->inventory[BLK_ARROW] > 0;
        if (bow_held && attack_held) {
            p->bow_drawing = true;
            /* Charge the draw — ~0.4 s to reach full draw. The model
             * uses this to swing the bow up + back; release uses the
             * existing swing animation. */
            p->bow_draw_t += dt / 0.4f;
            if (p->bow_draw_t > 1.0f) p->bow_draw_t = 1.0f;
            /* Pick / refresh target each frame. Hostile mobs only
             * (passive types share their MobType enum positions
             * below MOB_SLIME). */
            float best_d2 = 16.0f * 16.0f;
            int best = -1;
            for (int i = 0; i < CRAFT_MAX_MOBS; i++) {
                CraftMob *m = &craft_mobs[i];
                if (!m->alive) continue;
                if (m->type < MOB_SLIME) continue;
                float dx = m->pos.x - p->cam.pos.x;
                float dz = m->pos.z - p->cam.pos.z;
                float d2 = dx * dx + dz * dz;
                if (d2 > best_d2) continue;
                /* Cone gate around the current yaw (±60°). Once a
                 * target is locked the smooth lerp pulls the yaw
                 * onto it, so the next frame's cone is centred on
                 * the same direction — lock stays sticky. */
                float t_yaw = atan2f(dx, dz);
                float dy = t_yaw - p->cam.yaw;
                while (dy >  3.14159265f) dy -= 6.2831853f;
                while (dy < -3.14159265f) dy += 6.2831853f;
                if (dy > 1.05f || dy < -1.05f) continue;
                best_d2 = d2;
                best = i;
            }
            p->bow_target_mob = best;
            /* Smooth yaw snap (~6 rad/sec, ~0.5 s to swing 180°). */
            if (best >= 0) {
                CraftMob *m = &craft_mobs[best];
                float dx = m->pos.x - p->cam.pos.x;
                float dz = m->pos.z - p->cam.pos.z;
                float t_yaw = atan2f(dx, dz);
                float dy = t_yaw - p->cam.yaw;
                while (dy >  3.14159265f) dy -= 6.2831853f;
                while (dy < -3.14159265f) dy += 6.2831853f;
                float step = 6.0f * dt;
                if (dy >  step) dy =  step;
                if (dy < -step) dy = -step;
                p->cam.yaw += dy;
            }
        }
        /* Release detection — fire on the frame the attack-button
         * transitions from held to released, provided we were
         * drawing. `bow_prev_a` retains its old name but now tracks
         * whichever button is bound to "attack" for the current
         * scheme. */
        bool a_just_released = p->bow_prev_a && !attack_held;
        p->bow_prev_a = attack_held;
        if (p->bow_drawing && a_just_released) {
            p->bow_drawing = false;
            if (bow_held) {
                /* Recompute the firing direction: prefer the locked
                 * mob if it's still alive, otherwise straight forward. */
                Vec3 origin = (Vec3){
                    p->cam.pos.x,
                    p->cam.pos.y - 0.1f,
                    p->cam.pos.z
                };
                /* Camera forward — yaw is already auto-aimed onto the
                 * target by the draw lerp; pitch is whatever the
                 * player set with D-pad. Using camera-forward (not a
                 * point-to-target ray) keeps vertical aim user-
                 * controlled, so arc shots over a wall stay possible. */
                float cy = cosf(p->cam.yaw),  sy = sinf(p->cam.yaw);
                float cp = cosf(p->cam.pitch), sp = sinf(p->cam.pitch);
                Vec3 fwd = (Vec3){ sy * cp, sp, cy * cp };
                const float ARROW_SPEED = 14.0f;
                Vec3 vel = (Vec3){
                    fwd.x * ARROW_SPEED,
                    fwd.y * ARROW_SPEED + 1.0f,
                    fwd.z * ARROW_SPEED
                };
                craft_arrows_spawn(origin, vel, true);
                p->inventory[BLK_ARROW]--;
                p->broke_block = true;
            }
            p->bow_target_mob = -1;
            p->bow_draw_t     = 0.0f;
            goto skip_attack;
        }
        if (p->bow_drawing) {
            /* Still drawing — don't run melee/break code. */
            goto skip_attack;
        }
        if (attack_pressed) {
            /* Try mob hit first — if within attack range, damage it
             * instead of breaking the block behind. */
            int mob_i = craft_mobs_pick(&p->cam, CRAFT_PLAYER_ATTACK_RANGE);
            int torch_i = (mob_i >= 0) ? -1
                          : craft_torches_pick(&p->cam, 8.0f);
            if (mob_i >= 0) {
                /* Sword in active slot boosts damage by tier. */
                BlockId held = p->hotbar[p->hotbar_idx];
                int dmg = CRAFT_PLAYER_ATTACK_DAMAGE;
                if      (held == BLK_SWORD_DIAMOND) dmg = 8;
                else if (held == BLK_SWORD_GOLD)    dmg = 4;
                else if (held == BLK_SWORD_SILVER)  dmg = 4;
                else if (held == BLK_SWORD_IRON)    dmg = 4;
                else if (held == BLK_SWORD_STONE)   dmg = 3;
                else if (held == BLK_SWORD_WOOD)    dmg = 2;
                craft_mob_damage(mob_i, dmg, held);
                craft_audio_break(BLK_DIRT);   /* generic thud */
            } else if (torch_i >= 0) {
                /* Break a torch — convert back to AIR + give to inventory. */
                CraftTorch *t = &craft_torches[torch_i];
                int tx = t->wx, ty = t->wy, tz = t->wz;
                craft_world_set(tx, ty, tz, BLK_AIR);
                p->inventory[BLK_TORCH]++;
                p->broke_block = true;
                p->last_block_touched = BLK_TORCH;
                p->last_action_x = tx;
                p->last_action_y = ty;
                p->last_action_z = tz;
                craft_audio_break(BLK_TORCH);
            } else {
            CraftRayHit h = craft_render_pick(&p->cam);
            if (h.hit && h.distance < 8.0f) {
                BlockId was = craft_world_get(h.bx, h.by, h.bz);
                /* Bedrock — the bottom world layer is indestructible.
                 * Plays the "needs pickaxe" ting so the player gets
                 * feedback that the hit registered but did nothing. */
                if (h.by <= 0) {
                    extern void craft_audio_pickaxe_ting(void);
                    craft_audio_pickaxe_ting();
                    goto break_handled;
                }
                /* Mining tier gating. Player has the highest pickaxe
                 * they own; reject if the block needs a higher tier. */
                int need = craft_block_pickaxe_tier(was);
                int have = 0;
                if      (p->inventory[BLK_PICKAXE_DIAMOND] > 0) have = 3;
                else if (p->inventory[BLK_PICKAXE_GOLD]    > 0) have = 3;
                else if (p->inventory[BLK_PICKAXE_SILVER]  > 0) have = 3;
                else if (p->inventory[BLK_PICKAXE_IRON]    > 0) have = 3;
                else if (p->inventory[BLK_PICKAXE_STONE]   > 0) have = 2;
                else if (p->inventory[BLK_PICKAXE_WOOD]    > 0) have = 1;
                if (need > have) {
                    extern void craft_audio_pickaxe_ting(void);
                    craft_audio_pickaxe_ting();
                    goto break_handled;
                }
                craft_world_set(h.bx, h.by, h.bz, BLK_AIR);
                if (was == BLK_FURNACE) {
                    /* Free the furnace's state slot. Any items still
                     * in input/fuel/output are lost — same as vanilla. */
                    craft_furnace_remove(h.bx, h.by, h.bz);
                }
                if (was == BLK_CHEST) {
                    /* Transfer chest contents to the player's
                     * inventory before freeing the state record —
                     * losing the loot on a stray break feels worse
                     * than vanilla on a tiny screen where chest
                     * UIs are fiddly to open precisely. */
                    CraftChest *c = craft_chest_find(h.bx, h.by, h.bz);
                    if (c) {
                        for (int i = 0; i < CRAFT_CHEST_SLOTS; i++) {
                            BlockId b = (BlockId)c->slots[i].blk;
                            int n = (int)c->slots[i].n;
                            if (b != BLK_AIR && n > 0) {
                                p->inventory[b] += n;
                            }
                        }
                    }
                    craft_chest_remove(h.bx, h.by, h.bz);
                }
                p->broke_block = true;
                p->last_block_touched = was;
                p->last_action_x = h.bx;
                p->last_action_y = h.by;
                p->last_action_z = h.bz;
                if (was != BLK_AIR) {
                    /* Mining-drop table — matches vanilla Minecraft:
                     *   STONE → COBBLE (silk-touch would give stone,
                     *                   we don't have enchants yet)
                     *   GRASS → DIRT   (grass is a surface state,
                     *                   block underneath is dirt)
                     *   everything else → drops itself
                     * Without this, mining stone gave you stone and
                     * the furnace recipe (needs cobble) was unobtainable. */
                    BlockId dropped = was;
                    if      (was == BLK_STONE)         dropped = BLK_COBBLE;
                    else if (was == BLK_GRASS)         dropped = BLK_DIRT;
                    else if (was == BLK_DIAMOND_ORE)   dropped = BLK_DIAMOND;
                    else if (was == BLK_REDSTONE_ORE)  dropped = BLK_REDSTONE;
                    else if (was == BLK_REDSTONE_WIRE ||
                             was == BLK_REDSTONE_WIRE_ON) dropped = BLK_REDSTONE;
                    else if (was == BLK_LEVER_ON)      dropped = BLK_LEVER_OFF;
                    else if (was == BLK_DOOR_ON)       dropped = BLK_DOOR_OFF;
                    else if (was == BLK_TRAPDOOR_ON)   dropped = BLK_TRAPDOOR_OFF;
                    else if (was == BLK_PISTON_ON)     dropped = BLK_PISTON_OFF;
                    else if (was == BLK_PISTON_ARM)    dropped = BLK_AIR;
                    else if (was == BLK_TNT_FUSED)     dropped = BLK_TNT;
                    /* Water at any level drops as the source level so
                     * the inventory item is unified — without this a
                     * pickup from a settled (L7) cell would land in
                     * the inventory at BlockId 70, which fails the
                     * placeable check (it's above the BLK_STICK cap),
                     * while a pickup of an L0 source (BlockId 7,
                     * below the cap) places fine. Same icon, same
                     * stack — same drop. */
                    else if (craft_is_water_id((uint8_t)was)) dropped = BLK_WATER_L0;
                    /* Track inventory counts in BOTH modes — creative
                     * needs them so the crafting picker can know what
                     * the player has mined. Creative just never
                     * decrements on place. */
                    p->inventory[dropped]++;
                    /* Recipe matcher + hotbar still key off the
                     * dropped item, so update `was` for the rest of
                     * this block of code. */
                    was = dropped;
                    /* Both modes: auto-add to hotbar if not present.
                     * In creative, slots only appear as the player
                     * mines a block type for the first time. */
                    bool present = false;
                    for (int i = 0; i < CRAFT_HOTBAR_SLOTS; i++)
                        if (p->hotbar[i] == was) { present = true; break; }
                    if (!present) {
                        for (int i = 0; i < CRAFT_HOTBAR_SLOTS; i++) {
                            bool empty = (p->hotbar[i] == BLK_AIR);
                            bool exhausted =
                                (p->mode == CRAFT_MODE_SURVIVAL) &&
                                p->hotbar[i] != BLK_AIR &&
                                p->inventory[p->hotbar[i]] == 0;
                            if (empty || exhausted) {
                                p->hotbar[i] = was;
                                break;
                            }
                        }
                    }
                }
                craft_audio_break(was);
            }
break_handled: ;
            }
        }
skip_attack: ;
        if (place_pressed) {
            CraftRayHit h = craft_render_pick(&p->cam);
            /* Doors/trapdoors/ladders are non-opaque in the raycaster
             * (so the sprite post-pass owns their pixels) which means
             * craft_render_pick passes through them and reports
             * whatever's BEHIND. Use the torch-list picker — it walks
             * the sprite cuboids directly — and prefer its hit when
             * it's closer than the world-cell hit. */
            int sprite_i = craft_torches_pick(&p->cam, 5.0f);
            if (sprite_i >= 0) {
                CraftTorch *t = &craft_torches[sprite_i];
                BlockId sb = craft_world_get(t->wx, t->wy, t->wz);
                if (sb == BLK_DOOR_OFF || sb == BLK_DOOR_ON ||
                    sb == BLK_TRAPDOOR_OFF || sb == BLK_TRAPDOOR_ON ||
                    sb == BLK_LEVER_OFF || sb == BLK_LEVER_ON) {
                    h.hit = true;
                    h.bx = t->wx; h.by = t->wy; h.bz = t->wz;
                    h.face = t->orient;
                    h.distance = 0.5f;   /* arbitrary, < 5.0 gate below */
                }
            }
            /* Interact: if the targeted block is a furnace or chest,
             * open the appropriate UI instead of placing on the
             * adjacent face. Routed via a player request flag so the
             * menu-open call lives in craft_main where the platform-
             * level menu lifecycle is. */
            if (h.hit && h.distance < 5.0f) {
                BlockId hit_blk = craft_world_get(h.bx, h.by, h.bz);
                if (hit_blk == BLK_FURNACE) {
                    p->request_furnace_open = true;
                    p->furnace_open_x = h.bx;
                    p->furnace_open_y = h.by;
                    p->furnace_open_z = h.bz;
                    goto place_done;
                }
                if (hit_blk == BLK_CHEST) {
                    p->request_chest_open = true;
                    p->chest_open_x = h.bx;
                    p->chest_open_y = h.by;
                    p->chest_open_z = h.bz;
                    goto place_done;
                }
                /* Lever toggle — B on the lever flips its state in
                 * place. The actual circuit-side power flow is driven
                 * by craft_redstone's propagation tick, which reads
                 * lever state next time it runs (5 Hz). */
                if (hit_blk == BLK_LEVER_OFF) {
                    craft_world_set(h.bx, h.by, h.bz, BLK_LEVER_ON);
                    craft_audio_place(BLK_LEVER_ON);
                    goto place_done;
                }
                if (hit_blk == BLK_LEVER_ON) {
                    craft_world_set(h.bx, h.by, h.bz, BLK_LEVER_OFF);
                    craft_audio_place(BLK_LEVER_OFF);
                    goto place_done;
                }
                /* Manual door / trapdoor toggle — same gesture as
                 * Minecraft. Redstone power also drives these via
                 * craft_redstone, but the player can flip them by
                 * hand when no circuit is wired up. */
                if (hit_blk == BLK_DOOR_OFF) {
                    craft_world_set(h.bx, h.by, h.bz, BLK_DOOR_ON);
                    /* Also toggle the other half of the door so both
                     * cells open/close together. */
                    if (craft_world_get(h.bx, h.by + 1, h.bz) == BLK_DOOR_OFF)
                        craft_world_set(h.bx, h.by + 1, h.bz, BLK_DOOR_ON);
                    else if (craft_world_get(h.bx, h.by - 1, h.bz) == BLK_DOOR_OFF)
                        craft_world_set(h.bx, h.by - 1, h.bz, BLK_DOOR_ON);
                    craft_audio_place(BLK_DOOR_ON);
                    goto place_done;
                }
                if (hit_blk == BLK_DOOR_ON) {
                    craft_world_set(h.bx, h.by, h.bz, BLK_DOOR_OFF);
                    if (craft_world_get(h.bx, h.by + 1, h.bz) == BLK_DOOR_ON)
                        craft_world_set(h.bx, h.by + 1, h.bz, BLK_DOOR_OFF);
                    else if (craft_world_get(h.bx, h.by - 1, h.bz) == BLK_DOOR_ON)
                        craft_world_set(h.bx, h.by - 1, h.bz, BLK_DOOR_OFF);
                    craft_audio_place(BLK_DOOR_OFF);
                    goto place_done;
                }
                if (hit_blk == BLK_TRAPDOOR_OFF) {
                    craft_world_set(h.bx, h.by, h.bz, BLK_TRAPDOOR_ON);
                    craft_audio_place(BLK_TRAPDOOR_ON);
                    goto place_done;
                }
                if (hit_blk == BLK_TRAPDOOR_ON) {
                    craft_world_set(h.bx, h.by, h.bz, BLK_TRAPDOOR_OFF);
                    craft_audio_place(BLK_TRAPDOOR_OFF);
                    goto place_done;
                }
                /* Note block — B cycles the pitch through the 2-octave
                 * range (0..23) and previews it. The pitch lives in
                 * the orient hash slot (note blocks have no facing) so
                 * it persists across saves and the redstone tick reads
                 * it back on the rising edge. */
                if (hit_blk == BLK_NOTE_BLOCK || hit_blk == BLK_NOTE_BLOCK_ON) {
                    int pitch = craft_torches_lookup_orient(h.bx, h.by, h.bz);
                    if (pitch < 0 || pitch > 23) pitch = 12;
                    pitch = (pitch + 1) % 24;
                    craft_torches_record_orient(h.bx, h.by, h.bz, pitch);
                    craft_audio_note(pitch);
                    goto place_done;
                }
                /* Repeater (DELAY) — B cycles the delay 1..4 ticks.
                 * The orient byte packs face in bits 0-2 and the delay
                 * setting in bits 3-4, so cycling preserves facing. */
                if (hit_blk == BLK_DELAY || hit_blk == BLK_DELAY_ON) {
                    int o = craft_torches_lookup_orient(h.bx, h.by, h.bz);
                    int face = o & 0x07;
                    int setting = (o >> 3) & 0x03;
                    setting = (setting + 1) & 0x03;
                    craft_torches_record_orient(h.bx, h.by, h.bz,
                                                face | (setting << 3));
                    craft_audio_place(BLK_DELAY);
                    goto place_done;
                }
            }
            /* Ladder-in-own-cell shortcut.
             *
             * When you're inside a 1-block-wide vertical shaft, aiming
             * at a side wall already lands h.fx,fy,fz on your own
             * cell — but the picker can miss when there's nothing to
             * aim at (looking up into open air) or when the closest
             * wall is just above eye level. Make placing a ladder in
             * the current chest cell explicit: holding LADDER + B
             * always places a ladder in the player's own cell (if
             * empty) against the nearest adjacent wall, even when
             * the picker returned no hit. */
            {
                BlockId held = p->hotbar[p->hotbar_idx];
                if (held == BLK_LADDER) {
                    bool affordable_l = (p->mode == CRAFT_MODE_CREATIVE) ||
                                        p->inventory[BLK_LADDER] > 0;
                    int  px = (int)floorf(p->cam.pos.x);
                    int  py = (int)floorf(p->cam.pos.y);   /* chest cell */
                    int  pz = (int)floorf(p->cam.pos.z);
                    /* Pick the wall that's actually adjacent. Preference
                     * order matches the placement orient enum so the
                     * sprite mounts predictably. */
                    int wall_face = -1;
                    if (craft_block_solid(craft_world_get(px + 1, py, pz)))      wall_face = FACE_NX;   /* +X wall → ladder on +X side of cell */
                    else if (craft_block_solid(craft_world_get(px - 1, py, pz))) wall_face = FACE_PX;
                    else if (craft_block_solid(craft_world_get(px, py, pz + 1))) wall_face = FACE_NZ;
                    else if (craft_block_solid(craft_world_get(px, py, pz - 1))) wall_face = FACE_PZ;
                    BlockId here = craft_world_get(px, py, pz);
                    if (affordable_l && wall_face >= 0 &&
                        (here == BLK_AIR || craft_is_water_id((uint8_t)here))) {
                        craft_world_set(px, py, pz, BLK_LADDER);
                        craft_torches_record_orient(px, py, pz, wall_face);
                        craft_torches_rebuild();
                        if (p->mode == CRAFT_MODE_SURVIVAL) p->inventory[BLK_LADDER]--;
                        p->placed_block = true;
                        craft_audio_place(BLK_LADDER);
                        goto place_done;
                    }
                }
            }

            if (h.hit && h.distance < 8.0f) {
                BlockId blk = p->hotbar[p->hotbar_idx];
                /* Tools (pickaxe) are non-placeable; skip silently. */
                if (!craft_block_placeable(blk)) goto place_done;
                /* Survival: must have at least one of this block. */
                bool affordable = (p->mode == CRAFT_MODE_CREATIVE) ||
                                  (blk != BLK_AIR && p->inventory[blk] > 0);
                BlockId cur = craft_world_get(h.fx, h.fy, h.fz);
                if (affordable && (cur == BLK_AIR || craft_is_water_id((uint8_t)cur))) {
                    BlockId stash = cur;
                    /* Place-time conversion: BLK_REDSTONE is an
                     * inventory item; in the world it manifests as
                     * BLK_REDSTONE_WIRE. The inventory column the
                     * player spends is still BLK_REDSTONE — that's
                     * decremented at the end of this branch. */
                    BlockId place_blk = blk;
                    if (place_blk == BLK_REDSTONE) place_blk = BLK_REDSTONE_WIRE;
                    /* Player-placed water always becomes a FLOWING
                     * level-1 cell, never an L=0 "source". Sources
                     * are reserved for procgen lake/ocean cells —
                     * letting the player drop a permanent source
                     * anywhere lets a single bucket flood the whole
                     * world. Placed L=1 decays normally after 6-7
                     * ticks if nothing else feeds it. */
                    if (place_blk == BLK_WATER_L0) place_blk = BLK_WATER_L1;
                    craft_world_set(h.fx, h.fy, h.fz, place_blk);
                    float feet_y2 = p->cam.pos.y - PLAYER_EYE;
                    if (aabb_blocked(p->cam.pos.x, feet_y2, p->cam.pos.z)) {
                        craft_world_set(h.fx, h.fy, h.fz, stash);
                    } else {
                        p->placed_block = true;
                        p->last_block_touched = blk;
                        p->last_action_x = h.fx;
                        p->last_action_y = h.fy;
                        p->last_action_z = h.fz;
                        if (p->mode == CRAFT_MODE_SURVIVAL) p->inventory[blk]--;
                        /* Torches: record orientation so the 3D
                         * render pass can mount them correctly. */
                        if (blk == BLK_TORCH) {
                            craft_torches_record_orient(
                                h.fx, h.fy, h.fz, h.face);
                            /* Rebuild now to pick up the orient. */
                            craft_torches_rebuild();
                        }
                        /* Sprite-based blocks need an orientation
                         * memo so the post-pass knows which wall /
                         * axis to mount against.
                         *
                         *  Ladders:    use the wall face the player
                         *              aimed at (h.face).
                         *  Doors:      use the player's facing
                         *              direction, snapped to the
                         *              nearest cardinal — so the
                         *              door swings around the side
                         *              the player walked toward.
                         *  Trapdoors:  same player-facing rule, so
                         *              the open swing goes toward
                         *              the wall the player faced. */
                        if (place_blk == BLK_LADDER ||
                            place_blk == BLK_DOOR_OFF ||
                            place_blk == BLK_TRAPDOOR_OFF ||
                            place_blk == BLK_PISTON_OFF ||
                            place_blk == BLK_STICKY_PISTON_OFF ||
                            place_blk == BLK_LEVER_OFF ||
                            place_blk == BLK_NOT_GATE ||
                            place_blk == BLK_DELAY ||
                            place_blk == BLK_OBSERVER ||
                            place_blk == BLK_DISPENSER) {
                            int orient_face = h.face;
                            if (place_blk == BLK_DOOR_OFF ||
                                place_blk == BLK_TRAPDOOR_OFF ||
                                place_blk == BLK_NOT_GATE ||
                                place_blk == BLK_DELAY ||
                                place_blk == BLK_OBSERVER ||
                                place_blk == BLK_DISPENSER) {
                                /* Snap player yaw to a cardinal so the
                                 * gate / observer "faces" away from the
                                 * player when placed. Same trick as
                                 * doors. */
                                float yaw = p->cam.yaw;
                                /* Normalise to (-π, π]. */
                                while (yaw >  3.14159265f) yaw -= 6.2831853f;
                                while (yaw < -3.14159265f) yaw += 6.2831853f;
                                const float Q = 0.7853982f;   /* π/4 */
                                if      (yaw >= -Q && yaw <  Q)        orient_face = FACE_PZ;
                                else if (yaw >=  Q && yaw <  Q*3.0f)   orient_face = FACE_PX;
                                else if (yaw < -Q && yaw >= -Q*3.0f)   orient_face = FACE_NX;
                                else                                    orient_face = FACE_NZ;
                            }
                            /* Piston orient: the shaft points AWAY
                             * from the face the player aimed at
                             * (i.e., in the direction +face_normal
                             * from the parent block — same as h.face
                             * value directly). */
                            craft_torches_record_orient(
                                h.fx, h.fy, h.fz, orient_face);
                            if (place_blk == BLK_DOOR_OFF) {
                                craft_torches_record_orient(
                                    h.fx, h.fy + 1, h.fz, orient_face);
                            }
                            craft_torches_rebuild();
                        }
                        /* Note block — seed a default mid-range pitch
                         * (12) into the orient slot so a freshly
                         * placed block sounds reasonable before the
                         * player has cycled it. */
                        if (place_blk == BLK_NOTE_BLOCK) {
                            craft_torches_record_orient(h.fx, h.fy, h.fz, 12);
                        }
                        /* Doors are two cells tall — also place the
                         * top half if the cell above is free. */
                        if (place_blk == BLK_DOOR_OFF) {
                            BlockId above = craft_world_get(h.fx, h.fy + 1, h.fz);
                            if (above == BLK_AIR || craft_is_water_id((uint8_t)above)) {
                                craft_world_set(h.fx, h.fy + 1, h.fz, BLK_DOOR_OFF);
                            }
                        }
                        craft_audio_place(blk);
                    }
                }
            }
place_done: ;
        }
    }

    /* --- Survival: damage cooldown, regen, respawn ------- */
    if (p->mode == CRAFT_MODE_SURVIVAL) {
        if (p->damage_cooldown > 0.0f) p->damage_cooldown -= dt;
        p->no_damage_t += dt;
        p->damage_flash -= dt;
        if (p->damage_flash < 0.0f) p->damage_flash = 0.0f;

        /* Latch the win banner armed by the boss-spider kill, then
         * tick it down so the HUD shows it for ~5 s. */
        if (s_win_pending) {
            p->win_banner_t = 5.0f;
            s_win_pending = false;
        }
        if (p->win_banner_t > 0.0f) {
            p->win_banner_t -= dt;
            if (p->win_banner_t < 0.0f) p->win_banner_t = 0.0f;
        }

        /* Respawn handling. */
        if (p->respawn_timer > 0.0f) {
            p->respawn_timer -= dt;
            if (p->respawn_timer <= 0.0f) {
                p->respawn_timer = 0.0f;
                p->cam.pos = p->spawn_point;
                p->vel     = v3(0, 0, 0);
                p->hp      = CRAFT_PLAYER_MAX_HP;
                p->damage_cooldown = 0.0f;
                p->damage_flash    = 0.0f;
            }
        } else {
            /* HP regen — passive, just needs N seconds without damage. */
            if (p->no_damage_t > CRAFT_PLAYER_REGEN_DELAY &&
                p->hp < CRAFT_PLAYER_MAX_HP) {
                p->regen_acc += dt;
                if (p->regen_acc >= CRAFT_PLAYER_REGEN_INTERVAL) {
                    p->regen_acc = 0.0f;
                    p->hp++;
                }
            } else {
                p->regen_acc = 0.0f;
            }
        }
    }

    /* Hotbar normalisation: any survival-mode slot whose block has
     * been used up to zero inventory becomes empty (BLK_AIR). The HUD
     * skips drawing swatches for empty slots, and craft_render_held_item
     * no-ops on BLK_AIR, so the slot visually clears and the player
     * stops "holding" the depleted item until they pick more up. */
    if (p->mode == CRAFT_MODE_SURVIVAL) {
        for (int i = 0; i < CRAFT_HOTBAR_SLOTS; i++) {
            BlockId b = p->hotbar[i];
            if (b != BLK_AIR && p->inventory[b] <= 0) {
                p->hotbar[i] = BLK_AIR;
            }
        }
    }
}

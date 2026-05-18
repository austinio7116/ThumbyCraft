/*
 * ThumbyCraft — player state, controls, physics.
 *
 * Holds the camera, hotbar selection, game mode + survival inventory,
 * and the per-frame logic that walks the input through movement,
 * look, place/break, gravity, and damage.
 */
#ifndef CRAFT_PLAYER_H
#define CRAFT_PLAYER_H

#include "craft_render.h"
#include "craft_blocks.h"

typedef struct {
    bool up, down, left, right;
    bool a, b, lb, rb, menu;

    /* Edge-trigger flags — set by the platform input layer. */
    bool a_pressed, b_pressed, lb_pressed, rb_pressed, menu_pressed;
    bool a_long;                  /* a held for >= 400 ms (legacy, unused) */
    bool menu_long;               /* menu held >= 400 ms (legacy, unused) */
} CraftInput;

#define CRAFT_HOTBAR_SLOTS 8

typedef enum {
    CRAFT_MODE_CREATIVE = 0,    /* fly + infinite inventory + no HP */
    CRAFT_MODE_SURVIVAL = 1,    /* walk only, earned inventory, takes damage */
} CraftGameMode;

/* HP is shown as 3 hearts in the HUD; each heart subdivides into 4
 * quarters so a hit can take a quarter-, half-, or three-quarter-heart
 * off. MAX_HP=12 gives exact quarter-heart resolution (12 / 3 / 4). */
#define CRAFT_PLAYER_MAX_HP           12
#define CRAFT_PLAYER_DAMAGE_COOLDOWN  1.2f   /* sec between damage ticks */
#define CRAFT_PLAYER_REGEN_DELAY      5.0f   /* sec safe before regen kicks in */
#define CRAFT_PLAYER_REGEN_INTERVAL   2.5f   /* sec between regen ticks */
#define CRAFT_PLAYER_ATTACK_DAMAGE    1
#define CRAFT_PLAYER_ATTACK_RANGE     3.5f

typedef struct {
    CraftCamera cam;
    Vec3   vel;
    bool   on_ground;
    CraftGameMode mode;
    int    hp;                    /* 0..CRAFT_PLAYER_MAX_HP, survival only */
    float  damage_cooldown;
    float  no_damage_t;
    float  regen_acc;
    float  damage_flash;
    float  respawn_timer;         /* >0 = dead, counting down to respawn */
    float  win_banner_t;          /* >0 = boss just slain, banner showing */
    float  fall_peak_y;           /* highest y reached during current air time */
    bool   bow_drawing;           /* true while A is held with bow + arrows */
    int    bow_target_mob;        /* locked mob index for auto-aim, -1 = none */
    bool   bow_prev_a;             /* previous frame's A state (for release detect) */
    float  bow_draw_t;             /* 0..1, charges over BOW_DRAW_TIME while drawing */
    Vec3   spawn_point;
    int    inventory[BLK_COUNT];
    bool   fly_mode;              /* gravity off (creative only) */
    bool   invert_y;              /* D-pad UP pitches DOWN */

    BlockId hotbar[CRAFT_HOTBAR_SLOTS];
    int     hotbar_idx;

    /* Action feedback for HUD/audio. Cleared each tick after read. */
    bool   broke_block;
    bool   placed_block;
    bool   request_menu;
    bool   request_fly_toast;
    /* Player pressed B on an in-world furnace block — main loop
     * opens the furnace UI bound to (x,y,z) and clears the flag. */
    bool   request_furnace_open;
    int    furnace_open_x, furnace_open_y, furnace_open_z;
    /* Same pattern for chests. */
    bool   request_chest_open;
    int    chest_open_x, chest_open_y, chest_open_z;
    BlockId last_block_touched;
    int    last_action_x;
    int    last_action_y;
    int    last_action_z;

    /* Footstep timer — accumulates while walking on ground. */
    float  step_acc;

    /* Auto-step cooldown — set after every auto-step, ticks down to 0.
     * Gates rapid repeat hops on bumpy terrain or short walls. */
    float  autostep_cooldown;

    /* Sustained-contact gate for auto-step. Accumulates dt while the
     * player wanted to move forward but couldn't. Auto-step only fires
     * once this exceeds a threshold so brushing a single 1-cell bump
     * doesn't trigger an immediate hop — the player has to be visibly
     * pressed into the wall. Resets on every successful horizontal
     * move. */
    float  stuck_against_wall_t;

    /* Visual lag between the camera and the logical feet position.
     * After an auto-step the logical Y jumps by +1.0 instantly so
     * collision and forward motion continue uninterrupted, while
     * step_lag is set to 1.0 — the camera then lerps up over ~0.15 s
     * to catch up. Result: smooth elevation change instead of a hop. */
    float  step_lag;

    /* Private — input edge tracking across ticks. Don't poke. */
    bool   _menu_prev;
    bool   _menu_chord_used;
    bool   _lb_prev;
    bool   _lb_consumed_by_chord;

    /* Ladder climb state. `climbing` latches true while the player
     * is actively grabbing the ladder (LB held, adjacent to ladder).
     * `climb_lockout` latches true after the player lands on the
     * ground while climbing — re-engaging needs LB released + a
     * fresh upward press (so descending into a corridor doesn't
     * leave you stuck to the ladder cell). */
    bool   climbing;
    bool   climb_lockout;
} CraftPlayer;

void craft_player_init(CraftPlayer *p, Vec3 spawn);
void craft_player_set_mode(CraftPlayer *p, CraftGameMode mode);

/* Mob/world layers call this to deal HP damage with cooldown enforced. */
void craft_player_take_damage(CraftPlayer *p, int amount);

/* Trigger the green YOU WIN! banner — called when the boss spider
 * dies. The HUD reads win_banner_t and renders the banner for ~5 s. */
void craft_player_signal_win(void);

/* Advance state by dt seconds given current input. */
void craft_player_tick(CraftPlayer *p, const CraftInput *in, float dt);

/* Diagnostic for the "stuck on flat ground" report. Returns true if
 * the player's AABB at their current position currently intersects
 * a solid block — meaning forward motion is blocked. */
bool craft_player_stuck_now(const CraftPlayer *p);

/* Block id directly under the player's foot AABB at the column dx,dz
 * relative to player position. dx,dz in [-1..+1]. Used by HUD to show
 * what's in the cells the player is colliding against. */
int  craft_player_neighbor_block(const CraftPlayer *p, int dx, int dz);

#endif

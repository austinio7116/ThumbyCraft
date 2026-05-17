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
    BlockId last_block_touched;
    int    last_action_x;
    int    last_action_y;
    int    last_action_z;

    /* Footstep timer — accumulates while walking on ground. */
    float  step_acc;

    /* Auto-step cooldown — set after every auto-step, ticks down to 0.
     * Gates rapid repeat hops on bumpy terrain or short walls. */
    float  autostep_cooldown;

    /* Private — input edge tracking across ticks. Don't poke. */
    bool   _menu_prev;
    bool   _menu_chord_used;
    bool   _lb_prev;
    bool   _lb_consumed_by_chord;
} CraftPlayer;

void craft_player_init(CraftPlayer *p, Vec3 spawn);
void craft_player_set_mode(CraftPlayer *p, CraftGameMode mode);

/* Mob/world layers call this to deal HP damage with cooldown enforced. */
void craft_player_take_damage(CraftPlayer *p, int amount);

/* Advance state by dt seconds given current input. */
void craft_player_tick(CraftPlayer *p, const CraftInput *in, float dt);

#endif

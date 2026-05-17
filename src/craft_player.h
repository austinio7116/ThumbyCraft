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

#define CRAFT_PLAYER_MAX_HP           10
#define CRAFT_PLAYER_MAX_HUNGER       10
#define CRAFT_PLAYER_DAMAGE_COOLDOWN  1.2f   /* sec between damage ticks */
#define CRAFT_PLAYER_REGEN_DELAY      5.0f   /* sec safe before regen kicks in */
#define CRAFT_PLAYER_REGEN_INTERVAL   2.5f   /* sec between regen ticks */
#define CRAFT_PLAYER_HUNGER_DECAY     45.0f  /* sec per hunger point */
#define CRAFT_PLAYER_REGEN_MIN_HUNGER 4
#define CRAFT_PLAYER_ATTACK_DAMAGE    1
#define CRAFT_PLAYER_ATTACK_RANGE     3.5f

typedef struct {
    CraftCamera cam;
    Vec3   vel;
    bool   on_ground;
    CraftGameMode mode;
    int    hp;                    /* 0..CRAFT_PLAYER_MAX_HP, survival only */
    int    hunger;                /* 0..CRAFT_PLAYER_MAX_HUNGER */
    int    apples;                /* food items in inventory */
    float  hunger_decay_acc;
    float  damage_cooldown;
    float  no_damage_t;
    float  regen_acc;
    float  damage_flash;
    float  respawn_timer;         /* >0 = dead, counting down to respawn */
    Vec3   spawn_point;
    int    inventory[BLK_COUNT];
    bool   look_sticky;           /* sticky look mode (LB tap toggle) */
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

    /* Private — input edge tracking across ticks. Don't poke. */
    bool   _menu_prev;
    bool   _menu_chord_used;
    bool   _lb_prev;
    bool   _lb_consumed_by_chord;
    float  _lb_hold_t;
    bool   _lb_pitched_this_hold;
} CraftPlayer;

void craft_player_init(CraftPlayer *p, Vec3 spawn);
void craft_player_set_mode(CraftPlayer *p, CraftGameMode mode);

/* Mob/world layers call this to deal HP damage with cooldown enforced. */
void craft_player_take_damage(CraftPlayer *p, int amount);

/* Advance state by dt seconds given current input. */
void craft_player_tick(CraftPlayer *p, const CraftInput *in, float dt);

#endif

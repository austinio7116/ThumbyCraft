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
#include "craft_world.h"
#include "craft_render.h"
#include "craft_audio.h"
#include "craft_mobs.h"
#include "craft_torches.h"

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

static bool aabb_blocked(float px, float feet_y, float pz) {
    int x0 = (int)floorf(px - PLAYER_HALF_W);
    int x1 = (int)floorf(px + PLAYER_HALF_W);
    int z0 = (int)floorf(pz - PLAYER_HALF_W);
    int z1 = (int)floorf(pz + PLAYER_HALF_W);
    int y0 = (int)floorf(feet_y);
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
#define AUTOSTEP_COOLDOWN 0.35f

static bool try_horizontal(CraftPlayer *p, float nx, float nz,
                           bool *stepped_up) {
    float feet_y = p->cam.pos.y - PLAYER_EYE;
    if (!aabb_blocked(nx, feet_y, nz)) {
        p->cam.pos.x = nx;
        p->cam.pos.z = nz;
        return true;
    }
    if (!p->fly_mode && p->on_ground &&
        !*stepped_up &&
        p->autostep_cooldown <= 0.0f &&
        p->vel.y <= 0.001f &&
        !aabb_blocked(nx, feet_y + 1.0f, nz)) {
        p->cam.pos.x = nx;
        p->cam.pos.z = nz;
        p->cam.pos.y += 1.0f;
        *stepped_up = true;
        p->autostep_cooldown = AUTOSTEP_COOLDOWN;
        return true;
    }
    return false;
}

void craft_player_tick(CraftPlayer *p, const CraftInput *in, float dt) {
    p->broke_block = false;
    p->placed_block = false;
    if (p->autostep_cooldown > 0.0f) {
        p->autostep_cooldown -= dt;
        if (p->autostep_cooldown < 0.0f) p->autostep_cooldown = 0.0f;
    }

    /* ----- MENU state machine (open-menu disambiguation) ------- */
    bool menu_just_pressed  = in->menu && !p->_menu_prev;
    bool menu_just_released = !in->menu && p->_menu_prev;
    if (menu_just_pressed) p->_menu_chord_used = false;

    /* ----- LB state (walk button + chord suppression) ---------- */
    bool lb_just_pressed  = in->lb && !p->_lb_prev;
    if (lb_just_pressed) p->_lb_consumed_by_chord = false;

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

    /* ----- Look + turn (D-pad is ALWAYS the look stick) -------- */
    if (in->left)  p->cam.yaw -= TURN_SPEED * dt;
    if (in->right) p->cam.yaw += TURN_SPEED * dt;
    float pitch_sign = p->invert_y ? -1.0f : 1.0f;
    if (in->up)    p->cam.pitch += PITCH_SPEED * dt * pitch_sign;
    if (in->down)  p->cam.pitch -= PITCH_SPEED * dt * pitch_sign;
    const float pmax = 85.0f * 3.14159265f / 180.0f;
    if (p->cam.pitch >  pmax) p->cam.pitch =  pmax;
    if (p->cam.pitch < -pmax) p->cam.pitch = -pmax;
    p->_lb_prev = in->lb;

    /* ----- Camera basis vectors -------------------------------- */
    float cy = cosf(p->cam.yaw),  sy = sinf(p->cam.yaw);
    float cp = cosf(p->cam.pitch), sp = sinf(p->cam.pitch);
    Vec3 fwd_full = v3(sy * cp, sp, cy * cp);
    Vec3 fwd_h    = v3(sy, 0.0f, cy);

    /* ----- Build wish vector — LB held = walk forward ---------- */
    Vec3 wish = v3(0, 0, 0);
    bool walk_active = in->lb && !p->_lb_consumed_by_chord;
    if (walk_active) {
        Vec3 fwd = p->fly_mode ? fwd_full : fwd_h;
        wish = fwd;
    }

    if (p->fly_mode) {
        /* RB held = ascend directly. */
        if (in->rb) wish.y += 1.0f;
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
        /* RB tap on ground = jump. */
        if (in->rb_pressed && p->on_ground) {
            p->vel.y = JUMP_VEL;
            p->on_ground = false;
            craft_audio_jump();
        }
    }

    /* ----- AABB sweep ------------------------------------------ */
    float dx = p->vel.x * dt;
    float dz = p->vel.z * dt;
    bool stepped = false;
    if (!try_horizontal(p, p->cam.pos.x + dx, p->cam.pos.z, &stepped)) p->vel.x = 0;
    if (!try_horizontal(p, p->cam.pos.x, p->cam.pos.z + dz, &stepped)) p->vel.z = 0;

    float feet_y = p->cam.pos.y - PLAYER_EYE;
    float ny_feet = feet_y + p->vel.y * dt;
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

    /* World is infinite in X/Z — only Y stays clamped to the buffer
     * height. craft_world_maybe_shift in craft_main keeps the player
     * roughly centred in the loaded window. */
    if (p->cam.pos.y < 1.0f) p->cam.pos.y = 1.0f;
    if (p->cam.pos.y > CRAFT_WORLD_Y - 1.0f) p->cam.pos.y = CRAFT_WORLD_Y - 1.0f;

    /* ----- Footsteps ---------------------------------------------- */
    if (!p->fly_mode && p->on_ground) {
        float spd2 = p->vel.x * p->vel.x + p->vel.z * p->vel.z;
        if (spd2 > 0.5f) {
            p->step_acc += dt;
            if (p->step_acc > 0.40f) {
                int fx = (int)floorf(p->cam.pos.x);
                int fy = (int)floorf(p->cam.pos.y - PLAYER_EYE - 0.05f);
                int fz = (int)floorf(p->cam.pos.z);
                BlockId under = craft_world_get(fx, fy, fz);
                if (under != BLK_AIR) craft_audio_step_on(under);
                p->step_acc = 0.0f;
            }
        } else {
            p->step_acc = 0.0f;
        }
    } else {
        p->step_acc = 0.0f;
    }

    /* ----- Place / break / attack (only when MENU not held) ---- */
    if (!in->menu) {
        if (in->a_pressed) {
            /* Try mob hit first — if within attack range, damage it
             * instead of breaking the block behind. */
            int mob_i = craft_mobs_pick(&p->cam, CRAFT_PLAYER_ATTACK_RANGE);
            int torch_i = (mob_i >= 0) ? -1
                          : craft_torches_pick(&p->cam, 8.0f);
            if (mob_i >= 0) {
                /* Sword in active slot boosts damage by tier. */
                BlockId held = p->hotbar[p->hotbar_idx];
                int dmg = CRAFT_PLAYER_ATTACK_DAMAGE;
                if      (held == BLK_SWORD_IRON)  dmg = 4;
                else if (held == BLK_SWORD_STONE) dmg = 3;
                else if (held == BLK_SWORD_WOOD)  dmg = 2;
                craft_mob_damage(mob_i, dmg);
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
                /* Mining tier gating. Player has the highest pickaxe
                 * they own; reject if the block needs a higher tier. */
                int need = craft_block_pickaxe_tier(was);
                int have = 0;
                if      (p->inventory[BLK_PICKAXE_IRON]  > 0) have = 3;
                else if (p->inventory[BLK_PICKAXE_STONE] > 0) have = 2;
                else if (p->inventory[BLK_PICKAXE_WOOD]  > 0) have = 1;
                if (need > have) {
                    extern void craft_audio_pickaxe_ting(void);
                    craft_audio_pickaxe_ting();
                    goto break_handled;
                }
                craft_world_set(h.bx, h.by, h.bz, BLK_AIR);
                p->broke_block = true;
                p->last_block_touched = was;
                p->last_action_x = h.bx;
                p->last_action_y = h.by;
                p->last_action_z = h.bz;
                if (was != BLK_AIR) {
                    /* Track inventory counts in BOTH modes — creative
                     * needs them so the crafting picker can know what
                     * the player has mined. Creative just never
                     * decrements on place. */
                    p->inventory[was]++;
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
        if (in->b_pressed) {
            CraftRayHit h = craft_render_pick(&p->cam);
            if (h.hit && h.distance < 8.0f) {
                BlockId blk = p->hotbar[p->hotbar_idx];
                /* Tools (pickaxe) are non-placeable; skip silently. */
                if (!craft_block_placeable(blk)) goto place_done;
                /* Survival: must have at least one of this block. */
                bool affordable = (p->mode == CRAFT_MODE_CREATIVE) ||
                                  (blk != BLK_AIR && p->inventory[blk] > 0);
                BlockId cur = craft_world_get(h.fx, h.fy, h.fz);
                if (affordable && (cur == BLK_AIR || cur == BLK_WATER)) {
                    BlockId stash = cur;
                    craft_world_set(h.fx, h.fy, h.fz, blk);
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
}

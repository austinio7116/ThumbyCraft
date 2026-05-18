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
        /* RB tap on ground = jump. Suppressed while MENU is held —
         * otherwise MENU+RB (hotbar-next chord) launches the player
         * mid-cycle, and MENU+LB users who flick RB by accident
         * get a surprise hop. */
        if (in->rb_pressed && p->on_ground && !in->menu) {
            p->vel.y = JUMP_VEL;
            p->on_ground = false;
        }
    }

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

    /* Footstep SFX removed — the noise-burst layer sounded harsh under
     * the 3× loudness boost and the field reported it as distracting. */
    p->step_acc = 0.0f;

    /* ----- Place / break / attack (only when MENU not held) ---- */
    if (!in->menu) {
        /* Bow short-circuit — A while holding a bow + having arrows in
         * inventory fires an arrow from the camera. Doesn't break /
         * attack at melee range. */
        if (in->a_pressed &&
            p->hotbar[p->hotbar_idx] == BLK_BOW &&
            p->inventory[BLK_ARROW] > 0) {
            float cy = cosf(p->cam.yaw),  sy = sinf(p->cam.yaw);
            float cp = cosf(p->cam.pitch), sp = sinf(p->cam.pitch);
            Vec3 origin = (Vec3){
                p->cam.pos.x,
                p->cam.pos.y - 0.1f,           /* fire from chest, not eyes */
                p->cam.pos.z
            };
            Vec3 fwd = (Vec3){ sy * cp, sp, cy * cp };
            /* Match the skeleton arrow speed so flight time + drop
             * feel consistent for the player. */
            const float ARROW_SPEED = 14.0f;
            Vec3 vel = (Vec3){
                fwd.x * ARROW_SPEED,
                fwd.y * ARROW_SPEED + 1.0f,    /* slight lift compensates for gravity */
                fwd.z * ARROW_SPEED
            };
            craft_arrows_spawn(origin, vel, true);
            p->inventory[BLK_ARROW]--;
            p->broke_block = true;            /* drives swing animation */
            goto skip_attack;
        }
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
                if (was == BLK_FURNACE) {
                    /* Free the furnace's state slot. Any items still
                     * in input/fuel/output are lost — same as vanilla. */
                    craft_furnace_remove(h.bx, h.by, h.bz);
                }
                if (was == BLK_CHEST) {
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
                    if      (was == BLK_STONE) dropped = BLK_COBBLE;
                    else if (was == BLK_GRASS) dropped = BLK_DIRT;
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
        if (in->b_pressed) {
            CraftRayHit h = craft_render_pick(&p->cam);
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
            }
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

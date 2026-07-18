/*
 * ThumbyCraft — 2-player co-op protocol (over the craft_link byte pipe).
 *
 * Modelled on TerraMote's netcode, adapted to ThumbyCraft's world
 * architecture. ThumbyCraft stores its infinite world as procedural
 * baseline (seed) + a diff journal (SRAM mod hash + flash chunk store),
 * and device-to-device peers run bit-identical firmware — so unlike
 * TerraMote there is no full world transfer: the HOST streams its seed
 * plus the complete diff journal on join, and the guest regenerates
 * everything locally. After that both sides simulate their own window
 * and only exchange:
 *
 *   - block edits (symmetric, last-write-wins, echo-suppressed) through
 *     the single craft_world_set choke point. Redstone-batch edits are
 *     only sent when the peer is too far to be simulating that circuit
 *     itself (both near = both sim the same synced inputs).
 *   - player state at 15 Hz (position/yaw/pitch/held/swing)
 *   - chest contents when a chest UI closes
 *   - the world clock (host-authoritative, piggybacked on state)
 *
 * Mobs, drops, particles, water/lava flow and redstone stay local per
 * device — they are driven by the synced block state and converge.
 *
 * Roles are explicit: the HOST invites from the pause menu (their world
 * is the one played); the GUEST joins from the title screen. If the
 * link drops mid-game both sides keep a complete local copy and simply
 * continue solo.
 */
#ifndef CRAFT_NET_H
#define CRAFT_NET_H

#include "craft_types.h"
#include "craft_player.h"

/* --- session lifecycle ------------------------------------------- */

/* Host: invite a friend into the current world. Persists the full diff
 * journal to the chunk store first (one-off flash burst), then starts
 * link discovery. The host keeps playing while it searches/syncs. */
void craft_net_begin_host(void);

/* Guest: join a friend's world. Call after a normal world init (any
 * seed) — the current world keeps rendering behind the sync overlay
 * and is replaced by the host's world when the transfer lands. */
void craft_net_begin_guest(void);

/* Stop the session. notify=true sends a quit message first. */
void craft_net_stop(bool notify);

int  craft_net_active(void);     /* in PLAY state */
int  craft_net_is_host(void);
int  craft_net_started(void);    /* any state except OFF/FAILED */

/* True while the local player should be frozen (guest joining: waiting
 * for link / hello / world transfer). The host is never blocked. */
bool craft_net_blocking(void);

/* --- per-frame --------------------------------------------------- */

/* Pump transport + protocol. Call once per frame from craft_main_tick /
 * craft_main_step BEFORE gameplay, every frame regardless of menu or
 * sync state. `in` may be NULL; when blocking, B cancels the join. */
void craft_net_tick(const CraftInput *in, float dt);

/* Draw the sync overlay / link toasts on top of the finished frame. */
void craft_net_draw(uint16_t *fb);

/* Render the remote player (cuboid model, z-tested). Call after
 * craft_mobs_render. No-op unless in PLAY and the peer is in view. */
void craft_net_render_remote(const CraftCamera *cam, uint16_t *fb);

/* --- capture hooks (called from the engine) ----------------------- */

/* From craft_world_set, right where the mod journal records. `batched`
 * is true inside a begin/end_batch burst (the redstone tick). */
void craft_net_note_set(int wx, int wy, int wz,
                        uint8_t prev, uint8_t blk, bool batched);

/* From craft_torches_record_orient — attaches the mount face to the
 * pending edit for that cell so the peer's torch points the same way. */
void craft_net_note_orient(int wx, int wy, int wz, int face);

/* From the platform save path: host tells the guest a flash save burst
 * is starting/ending so the quiet spell reads as "saving", not lost. */
void craft_net_note_saving(bool on);

/* --- platform hooks ----------------------------------------------- */

/* Optional aggressive-pump hook for the device: called while searching /
 * syncing so USB enumeration advances faster than once per frame. */
void craft_net_set_idle_pump(void (*pump)(void));

#endif /* CRAFT_NET_H */

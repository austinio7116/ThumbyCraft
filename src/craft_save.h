/*
 * ThumbyCraft — world persistence.
 *
 * Format (all little-endian):
 *   magic        u32  'TCFT' = 0x54434654
 *   version      u32  current = 1
 *   seed         u32  for terrain regeneration
 *   block_count  u32  number of delta entries that follow
 *   hotbar_idx   u8
 *   hotbar[8]    u8
 *   cam_pos      3 × f32
 *   cam_yaw      f32
 *   cam_pitch    f32
 *   reserved     u32 (padding)
 *   delta[N]     each: u32 packed_xyz | (block << 24)
 *   crc32        u32  over header + payload
 *
 * Deltas are computed against a freshly regenerated terrain from the
 * same seed — only blocks the player actually changed are written.
 * That keeps saves tiny (a few KB) and fits in one flash sector.
 *
 * The platform layer (host or device) is responsible for the actual
 * read/write of the blob; the engine just hands it bytes via the
 * craft_save_sink interface.
 */
#ifndef CRAFT_SAVE_H
#define CRAFT_SAVE_H

#include <stddef.h>
#include "craft_types.h"
#include "craft_player.h"

#define CRAFT_SAVE_MAGIC   0x54434654u
#define CRAFT_SAVE_VERSION 1u
#define CRAFT_SAVE_MAX_BYTES (4096 - 32)   /* one flash sector minus header */

/* Returns bytes written into `out` (≤ out_cap), or 0 on error.
 * Caller passes the seed used to regenerate the base terrain. */
size_t craft_save_serialise(uint32_t seed,
                            const CraftPlayer *p,
                            uint8_t *out, size_t out_cap);

/* Returns true on success. Re-seeds + regenerates the world, then
 * applies deltas + restores player state. */
bool   craft_save_deserialise(const uint8_t *in, size_t n,
                              uint32_t *out_seed, CraftPlayer *p);

#endif

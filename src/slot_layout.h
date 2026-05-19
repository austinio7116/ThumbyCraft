/*
 * ThumbyCraft — standalone-build flash layout.
 *
 * Authoritative source of truth for where every persistent region
 * lives on the W25Q128 (16 MB) flash chip. Both the chunk store and
 * the save metadata module include this header so a single edit
 * here moves both in lockstep.
 *
 * Layout:
 *   0x000000 .. 0x0FFFFF  code + BS2                 (1 MB max)
 *   0x100000 .. 0x4FFFFF  slot 0..3 chunk regions    (4 × 1 MB)
 *   0x500000 .. 0x5FFFFF  scratch chunk region       (1 MB)
 *   0x600000 .. 0x604FFF  metadata sectors           (5 × 4 KB)
 *   0x605000 .. 0xFFFFFF  free                        (~10 MB)
 *
 * Each chunk region holds CHUNK_REGION_SECTORS = 256 4-KB sectors,
 * addressed by hash(cx, cz) within the region. A region is the
 * full backing store for ONE save slot (or for the scratch space
 * used by unsaved new worlds). Cross-region eviction is impossible
 * by construction — each save slot owns its bytes outright.
 *
 * Metadata sectors hold the save record (player state + seed +
 * thumbnail) for their respective slot.
 *
 * THUMBYONE_SLOT_MODE will replace the chunk-region offsets with
 * a FatFs file backend; the rest of this header still applies
 * (sizes/limits/slot-count don't change). See PLAN.md.
 */
#ifndef THUMBYCRAFT_SLOT_LAYOUT_H
#define THUMBYCRAFT_SLOT_LAYOUT_H

/* --- Slot identifiers ------------------------------------------- */

#define TBC_SLOT_COUNT         4
#define TBC_REGION_SCRATCH     (TBC_SLOT_COUNT)      /* logical id past last slot */
#define TBC_REGION_COUNT       (TBC_SLOT_COUNT + 1)  /* 4 slots + scratch */

/* --- Chunk region geometry -------------------------------------- */

#define TBC_CHUNK_REGION_SECTORS  256                            /* 1 MB per region */
#define TBC_CHUNK_REGION_BYTES    (TBC_CHUNK_REGION_SECTORS * 4096u)

/* --- Flash offsets (standalone build) --------------------------- */

#ifndef THUMBYONE_SLOT_MODE
#  define TBC_CHUNK_REGIONS_BASE   (1u * 1024u * 1024u)            /* 0x100000 */
#  define TBC_METADATA_BASE        (6u * 1024u * 1024u)            /* 0x600000 */
#endif

/* Region offset within flash. region in [0, TBC_REGION_COUNT). */
#define TBC_CHUNK_REGION_OFFSET(region) \
    (TBC_CHUNK_REGIONS_BASE + (uint32_t)(region) * TBC_CHUNK_REGION_BYTES)

/* Metadata sector offset within flash. region in [0, TBC_REGION_COUNT). */
#define TBC_METADATA_OFFSET(region) \
    (TBC_METADATA_BASE + (uint32_t)(region) * 4096u)

#endif

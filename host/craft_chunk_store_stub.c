/*
 * ThumbyCraft — chunk store (host stub, nonce-aware API).
 *
 * Host has no flash. The whole API is a no-op; the host's mod hash
 * survives until the process exits and the next run starts fresh.
 */
#include "craft_chunk_store.h"

static int      s_region = -1;
static uint32_t s_nonce;

void craft_chunk_store_bind(int region, uint32_t nonce) {
    s_region = region; s_nonce = nonce;
}
int      craft_chunk_store_bound(void)       { return s_region; }
uint32_t craft_chunk_store_bound_nonce(void) { return s_nonce; }

int craft_chunk_store_load(int chunk_x, int chunk_z,
                           ChunkMod *out, int max_entries) {
    (void)chunk_x; (void)chunk_z; (void)out; (void)max_entries;
    return 0;
}

bool craft_chunk_store_save(int chunk_x, int chunk_z,
                            const ChunkMod *mods, int n) {
    (void)chunk_x; (void)chunk_z; (void)mods; (void)n;
    return true;
}

/* Nothing to enumerate — the mod hash carries everything on host, so
 * the co-op world transfer's SEC_MODS section covers the whole diff. */
int craft_chunk_store_slots(void) { return 0; }
int craft_chunk_store_read_slot(int slot, int *cx, int *cz,
                                ChunkMod *out, int max_entries) {
    (void)slot; (void)cx; (void)cz; (void)out; (void)max_entries;
    return -1;
}

uint8_t *craft_chunk_store_scratch4k(void) {
    static uint8_t page[4096];
    return page;
}

void craft_chunk_store_erase_region(int region) { (void)region; }
void craft_chunk_store_copy(int src_region, uint32_t src_nonce,
                            int dst_region, uint32_t dst_nonce) {
    (void)src_region; (void)src_nonce; (void)dst_region; (void)dst_nonce;
}

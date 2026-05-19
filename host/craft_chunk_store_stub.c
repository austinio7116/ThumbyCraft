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

void craft_chunk_store_erase_region(int region) { (void)region; }
void craft_chunk_store_copy(int src_region, uint32_t src_nonce,
                            int dst_region, uint32_t dst_nonce) {
    (void)src_region; (void)src_nonce; (void)dst_region; (void)dst_nonce;
}

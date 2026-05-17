/*
 * ThumbyCraft — chunk mod store (host stub).
 *
 * The host build doesn't need flash persistence — we exercise the
 * gameplay against a transient world only. Stub everything out to
 * no-ops so the shared craft_world.c can call the API unconditionally
 * without ifdef noise.
 */
#include "craft_chunk_store.h"

void craft_chunk_store_init(uint32_t world_seed) {
    (void)world_seed;
}

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

void craft_chunk_store_clear(void) {}

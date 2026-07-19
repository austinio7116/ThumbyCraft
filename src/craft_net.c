/*
 * ThumbyCraft — 2-player co-op protocol (see craft_net.h for design).
 *
 * Wire format: every message is [0xC7 magic][type byte][payload]. The
 * receiver resynchronises on the magic byte and derives frame length
 * per-type. The transport (craft_link) is a reliable in-order byte
 * pipe; the only unreliability handled here is a lost cable.
 *
 * Join transfer: the host streams its complete diff journal — every
 * chunk-store sector plus the live SRAM mod hash (newest values last),
 * the torch-orientation blob, and the chest table — as sections inside
 * 'm' chunk messages, FNV-checked end to end. The guest writes chunk
 * sections straight into its own (scratch) chunk store and mod hash,
 * then regenerates the window from the host's seed: identical firmware
 * makes procgen bit-identical, so seed + journal IS the world.
 */
#include "craft_net.h"

#if CRAFT_NET_ENABLED

#include "craft_link.h"
#include "craft_world.h"
#include "craft_main.h"
#include "craft_menu.h"
#include "craft_torches.h"
#include "craft_chests.h"
#include "craft_chunk_store.h"
#include "craft_font.h"
#include "craft_audio.h"
#include "craft_mobs.h"

#include <string.h>
#include <stdio.h>
#include <math.h>

#define NET_MAGIC  0xC7
#define NET_PROTO  1

/* Host-build debug trace (CRAFT_NET_DBG=1 in the environment). */
#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE
#define NET_DBG(...) ((void)0)
#else
#include <stdlib.h>
#define NET_DBG(...) do { \
        if (getenv("CRAFT_NET_DBG")) { printf("[net] " __VA_ARGS__); printf("\n"); fflush(stdout); } \
    } while (0)
#endif

/* Peer "far" threshold for redstone/sim edits: outside this XZ distance
 * the peer's 64-cell window can't contain the cell, so their own sim
 * isn't covering it and we must send the authoritative record. Inside
 * it, both sides sim the same circuit off the same synced inputs and
 * sending would only invite phase-crossfire. */
#define NET_FAR_DIST  40

enum { NS_OFF = 0, NS_WAIT_LINK, NS_HELLO, NS_SYNC, NS_PLAY, NS_FAILED };

static uint8_t s_ns;
static uint8_t s_host;               /* our role (explicit, from UI) */
static float   s_hello_t, s_wait_t;
static uint8_t s_got_hello;
static uint8_t s_applying;           /* suppress echo while applying remote */
static uint8_t s_guest_world_reset;  /* guest: prepare() ran (world dropped) */
static const char *s_phase = "";
static void  (*s_idle_pump)(void);

/* ---- peer view ---------------------------------------------------------- */
static struct {
    uint8_t present;
    float   x, y, z;                 /* smoothed */
    float   tx, ty, tz;              /* latest target */
    float   yaw;
    uint8_t held;
} s_peer;

/* ---- outbound ring ------------------------------------------------------ *
 * 512 B (power of two). Throughput is bounded by how fast the peer
 * drains the CDC FIFO per frame, not by ring depth — the world-transfer
 * pump simply refills it every frame until the FIFO stops accepting. */
static uint8_t  s_txr[512];
static uint16_t s_txh, s_txt;
static int tx_space(void) {
    return (int)sizeof s_txr - 1 - ((uint16_t)(s_txh - s_txt) & (sizeof s_txr - 1));
}
static int tx_send(const uint8_t *m, int n) {
    if (tx_space() < n) return 0;
    for (int i = 0; i < n; i++) { s_txr[s_txh & (sizeof s_txr - 1)] = m[i]; s_txh++; }
    return 1;
}
static void tx_pump(void) {
    while (s_txt != s_txh) {
        int t = s_txt & (sizeof s_txr - 1);
        int run = (int)sizeof s_txr - t;
        int have = (uint16_t)(s_txh - s_txt);
        if (run > have) run = have;
        /* Keep calls small so a partially-full CDC FIFO always makes
         * forward progress (link_send returns honest partial counts). */
        if (run > 128) run = 128;
        int w = craft_link_send(s_txr + t, run);
        if (w <= 0) return;
        s_txt = (uint16_t)(s_txt + w);
    }
}

/* little-endian field helpers */
static void put16(uint8_t *p, int v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void putf(uint8_t *p, float f) { uint32_t u; memcpy(&u, &f, 4); put32(p, u); }
static int      get16(const uint8_t *p) { return (int16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static int32_t get_i32(const uint8_t *p) { return (int32_t)get32(p); }
static float   getf(const uint8_t *p) { uint32_t u = get32(p); float f; memcpy(&f, &u, 4); return f; }

/* ---- edit queue (both directions, echo-suppressed) ----------------------- */
#define EDITQ 96
typedef struct {
    int32_t wx, wz;
    uint8_t wy, blk, orient;         /* orient 0xFF = none */
} NetEdit;
static NetEdit  s_editq[EDITQ];
static uint16_t s_eh, s_et;          /* head/tail indices into power-of-2... */

/* EDITQ isn't a power of two — use plain modular arithmetic. */
static int editq_count(void) { return (s_eh + EDITQ - s_et) % EDITQ; }
static void editq_push(int wx, int wy, int wz, uint8_t blk) {
    /* coalesce: a later write to the same cell replaces the queued one */
    for (int i = s_et; i != s_eh; i = (i + 1) % EDITQ) {
        NetEdit *e = &s_editq[i];
        if (e->wx == wx && e->wz == wz && e->wy == (uint8_t)wy) {
            e->blk = blk; e->orient = 0xFF;
            return;
        }
    }
    if (editq_count() >= EDITQ - 1) return;    /* overflow: drop (rare burst) */
    NetEdit *e = &s_editq[s_eh];
    e->wx = wx; e->wz = wz; e->wy = (uint8_t)wy;
    e->blk = blk; e->orient = 0xFF;
    s_eh = (uint16_t)((s_eh + 1) % EDITQ);
}

/* ---- world transfer ------------------------------------------------------ */
/* Section stream framed inside 'm' messages: [type u8][len u16][payload]. */
#define SEC_CHUNK   1   /* cx i16, cz i16, count u16, count × ChunkMod(4) */
#define SEC_MODS    2   /* count u16, count × (wx i32, wz i32, wy u8, blk u8) */
#define SEC_ORIENTS 3   /* count u16, count × (wx i32, wz i32, wy u8, face u8) */
#define SEC_CHESTS  4   /* craft_chests 180-byte blob */
#define SEC_END     5   /* len 0 */

/* Sized by the largest section: a full chunk record (6 + 340×4 mods).
 * Everything else streams in ≤64-entry sections. Every byte here is
 * BSS the ThumbyOne slot pays for, so keep it tight. */
#define STAGING_MAX 1376
static uint8_t s_staging[STAGING_MAX];

static uint32_t s_wtotal, s_wdone;   /* stream bytes (both directions) */
static uint32_t s_wfnv;              /* running FNV (tx) / expected (rx) */
static uint32_t s_rxfnv;             /* guest: running FNV over received */
static uint16_t s_wseq;              /* 'm' sequence */

/* host tx cursor */
static int s_txphase;                /* 0 store, 1 hash, 2 orients, 3 chests, 4 end, 5 done */
static int s_txslot;                 /* store slot cursor */
static int s_txmod;                  /* hash cursor */
static int s_txorient;               /* orient hash cursor */
static int s_txoff, s_txlen;         /* progress inside staged section */
static uint8_t s_trailer_sent;       /* FNV trailer queued after the stream */
static uint8_t s_peer_acked;

/* guest rx parser */
static uint8_t s_rxhdr[3];
static int s_rxhdrn, s_rxneed, s_rxgot;
static uint8_t s_stream_end, s_sync_done;

/* meta from 'W' */
static uint32_t s_mseed;
static float    s_mtime;
static Vec3     s_mspawn;

static uint32_t fnv_update(uint32_t h, const uint8_t *p, int n) {
    for (int i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

/* ---- host: stage the next transfer section into s_staging ---------------- */
/* Returns section length staged (>0), or 0 when the stream is finished. */
static int stage_next_section(void) {
    uint8_t *o = s_staging;
    for (;;) {
        switch (s_txphase) {
        case 0: {                                    /* chunk store sectors */
            int slots = craft_chunk_store_slots();
            while (s_txslot < slots) {
                int cx, cz;
                int n = craft_chunk_store_read_slot(s_txslot, &cx, &cz,
                        (ChunkMod *)(o + 9), (STAGING_MAX - 9) / 4);
                s_txslot++;
                if (n <= 0) continue;
                int len = 6 + n * 4;
                o[0] = SEC_CHUNK; put16(o + 1, len);
                put16(o + 3, cx); put16(o + 5, cz);
                put16(o + 7, n);
                return 3 + len;
            }
            s_txphase = 1; s_txmod = 0;
            break;
        }
        case 1: {                                    /* live SRAM mod hash */
            int n = 0;
            int32_t wx, wz; int wy; uint8_t blk;
            while (n < 64 &&
                   (s_txmod = craft_world_mod_iter(s_txmod, &wx, &wy, &wz, &blk)) >= 0) {
                uint8_t *e = o + 5 + n * 10;
                put32(e, (uint32_t)wx); put32(e + 4, (uint32_t)wz);
                e[8] = (uint8_t)wy; e[9] = blk;
                n++;
            }
            if (n > 0) {
                int len = 2 + n * 10;
                o[0] = SEC_MODS; put16(o + 1, len);
                put16(o + 3, n);
                if (s_txmod < 0) s_txphase = 2;      /* hash exhausted */
                return 3 + len;
            }
            s_txphase = 2;
            break;
        }
        case 2: {                                    /* torch orientations */
            int n = 0;
            int32_t wx, wz; int wy; uint8_t face;
            while (n < 64 &&
                   (s_txorient = craft_torches_orient_iter(s_txorient, &wx, &wy,
                                                           &wz, &face)) >= 0) {
                uint8_t *e = o + 5 + n * 10;
                put32(e, (uint32_t)wx); put32(e + 4, (uint32_t)wz);
                e[8] = (uint8_t)wy; e[9] = face;
                n++;
            }
            if (n > 0) {
                int len = 2 + n * 10;
                o[0] = SEC_ORIENTS; put16(o + 1, len);
                put16(o + 3, n);
                if (s_txorient < 0) s_txphase = 3;
                return 3 + len;
            }
            s_txphase = 3;
            break;
        }
        case 3: {                                    /* chest table */
            o[0] = SEC_CHESTS; put16(o + 1, CRAFT_CHESTS_BLOB_BYTES);
            craft_chests_serialise(o + 3);
            s_txphase = 4;
            return 3 + CRAFT_CHESTS_BLOB_BYTES;
        }
        case 4:
            o[0] = SEC_END; put16(o + 1, 0);
            s_txphase = 5;
            return 3;
        default:
            return 0;
        }
    }
}

/* Dry-run total: sum of every section's (3 + len) bytes. Mirrors
 * stage_next_section exactly — keep the two in step. */
static uint32_t transfer_total_bytes(void) {
    uint32_t total = 0;
    int slots = craft_chunk_store_slots();
    for (int s = 0; s < slots; s++) {
        int cx, cz;
        int n = craft_chunk_store_read_slot(s, &cx, &cz, NULL, 0);
        if (n > 0) total += 3u + 6u + (uint32_t)n * 4u;
    }
    int mods = craft_world_mod_count();
    while (mods > 0) {
        int n = mods > 64 ? 64 : mods;
        total += 3u + 2u + (uint32_t)n * 10u;
        mods -= n;
    }
    int orients = craft_torches_orient_count();
    while (orients > 0) {
        int n = orients > 64 ? 64 : orients;
        total += 3u + 2u + (uint32_t)n * 10u;
        orients -= n;
    }
    total += 3u + CRAFT_CHESTS_BLOB_BYTES;           /* chests */
    total += 3u;                                     /* end */
    return total;
}

static void world_tx_begin(void) {
    s_wtotal = transfer_total_bytes();
    s_wdone = 0; s_wseq = 0; s_wfnv = 2166136261u;
    s_txphase = 0; s_txslot = 0; s_txmod = 0; s_txorient = 0;
    s_txoff = 0; s_txlen = 0;
    s_trailer_sent = 0;
    s_peer_acked = 0;

    const CraftPlayer *pl = craft_main_player();
    uint8_t m[28];
    m[0] = NET_MAGIC; m[1] = 'W';
    put32(m + 2, craft_main_seed());
    put16(m + 6, (int)(craft_main_world_time() * 65535.0f / 300.0f));
    /* Guest spawns just beside the host so they land in generated,
     * journal-covered terrain. */
    putf(m + 8,  pl->cam.pos.x + 1.5f);
    putf(m + 12, pl->cam.pos.y + 0.5f);
    putf(m + 16, pl->cam.pos.z);
    put32(m + 20, s_wtotal);
    put32(m + 24, 0);                 /* FNV placeholder — sent in 'y'-check via end 'F' */
    tx_send(m, 28);
}

/* Stream as much of the staged journal as the ring will take. */
static void world_tx_pump(void) {
    for (;;) {
        if (s_txoff >= s_txlen) {
            if (s_txphase >= 5) {
                /* Whole stream queued — append the FNV trailer (retry
                 * across frames if the ring happened to be full). */
                if (!s_trailer_sent) {
                    uint8_t f[6] = { NET_MAGIC, 'F' };
                    put32(f + 2, s_wfnv);
                    if (tx_send(f, 6)) s_trailer_sent = 1;
                }
                return;
            }
            s_txlen = stage_next_section();
            s_txoff = 0;
            if (s_txlen <= 0) continue;              /* phase advanced, re-check */
            s_wfnv = fnv_update(s_wfnv, s_staging, s_txlen);
        }
        uint8_t m[5 + 96];
        int n = s_txlen - s_txoff;
        if (n > 96) n = 96;
        memcpy(m + 5, s_staging + s_txoff, (size_t)n);
        m[0] = NET_MAGIC; m[1] = 'm';
        put16(m + 2, s_wseq);
        m[4] = (uint8_t)n;
        if (tx_space() < 5 + n) return;              /* ring full: retry next frame */
        tx_send(m, 5 + n);
        s_txoff += n;
        s_wseq++;
        s_wdone += (uint32_t)n;
    }
}

static void net_fail(const char *msg) {
    NET_DBG("FAIL: %s (link=%d ns=%d wdone=%u/%u)", msg,
            craft_link_status(), s_ns, (unsigned)s_wdone, (unsigned)s_wtotal);
    craft_menu_toast(msg);
    craft_link_stop();
    s_ns = NS_FAILED;
    if (!s_host && s_guest_world_reset) {
        /* The old world was dropped when the transfer began — recover
         * into a fresh solo world rather than a half-ingested one. */
        craft_main_net_guest_abort();
    }
    s_guest_world_reset = 0;
}

/* ---- guest: apply one completed section ---------------------------------- */
static void world_rx_section(void) {
    int type = s_rxhdr[0];
    int len  = s_rxgot;
    const uint8_t *p = s_staging;
    s_applying = 1;             /* never re-broadcast ingested journal data */
    switch (type) {
    case SEC_CHUNK: {
        if (len < 6) break;
        int cx = get16(p), cz = get16(p + 2);
        int n  = (int)(uint16_t)(p[4] | (p[5] << 8));
        if (n < 0 || 6 + n * 4 > len) break;
        /* Wire layout == ChunkMod layout (4 packed bytes). */
        craft_chunk_store_save(cx, cz, (const ChunkMod *)(p + 6), n);
        break;
    }
    case SEC_MODS: {
        if (len < 2) break;
        int n = (int)(uint16_t)(p[0] | (p[1] << 8));
        if (2 + n * 10 > len) break;
        for (int i = 0; i < n; i++) {
            const uint8_t *e = p + 2 + i * 10;
            craft_world_net_ingest_mod(get_i32(e), e[8], get_i32(e + 4), e[9]);
        }
        break;
    }
    case SEC_ORIENTS: {
        if (len < 2) break;
        int n = (int)(uint16_t)(p[0] | (p[1] << 8));
        if (2 + n * 10 > len) break;
        for (int i = 0; i < n; i++) {
            const uint8_t *e = p + 2 + i * 10;
            craft_torches_record_orient(get_i32(e), e[8], get_i32(e + 4), e[9]);
        }
        break;
    }
    case SEC_CHESTS:
        if (len == CRAFT_CHESTS_BLOB_BYTES) craft_chests_deserialise(p);
        break;
    case SEC_END:
        s_stream_end = 1;
        break;
    }
    s_applying = 0;
}

static void world_rx_bytes(const uint8_t *d, int n) {
    s_rxfnv = fnv_update(s_rxfnv, d, n);
    s_wdone += (uint32_t)n;
    while (n > 0) {
        if (s_rxhdrn < 3) {                          /* section header */
            s_rxhdr[s_rxhdrn++] = *d++; n--;
            if (s_rxhdrn == 3) {
                s_rxneed = (int)(uint16_t)(s_rxhdr[1] | (s_rxhdr[2] << 8));
                s_rxgot = 0;
                if (s_rxneed > STAGING_MAX) { net_fail("TRANSFER ERROR"); return; }
                if (s_rxneed == 0) {                 /* empty payload (END) */
                    world_rx_section();
                    s_rxhdrn = 0;
                }
            }
            continue;
        }
        int take = s_rxneed - s_rxgot;
        if (take > n) take = n;
        memcpy(s_staging + s_rxgot, d, (size_t)take);
        s_rxgot += take; d += take; n -= take;
        if (s_rxgot >= s_rxneed) {
            world_rx_section();
            s_rxhdrn = 0;
        }
    }
}

/* ---- tiny senders -------------------------------------------------------- */
static void send_hello(void) {
    uint8_t m[4] = { NET_MAGIC, 'H', NET_PROTO, s_host };
    tx_send(m, 4);
}

static void send_state(void) {
    const CraftPlayer *pl = craft_main_player();
    uint8_t m[20];
    m[0] = NET_MAGIC; m[1] = 'S';
    putf(m + 2,  pl->cam.pos.x);
    putf(m + 6,  pl->cam.pos.y);
    putf(m + 10, pl->cam.pos.z);
    /* yaw wrapped into one byte (2π / 256 ≈ 1.4° resolution). */
    float yw = fmodf(pl->cam.yaw, 6.2831853f);
    if (yw < 0) yw += 6.2831853f;
    m[14] = (uint8_t)(yw * (256.0f / 6.2831853f));
    m[15] = (uint8_t)(int8_t)(pl->cam.pitch * 80.0f);
    m[16] = 0;                                       /* flags (reserved) */
    m[17] = (uint8_t)pl->hotbar[pl->hotbar_idx];
    put16(m + 18, (int)(craft_main_world_time() * 65535.0f / 300.0f));
    tx_send(m, 20);
}

static void flush_edits(void) {
    while (s_et != s_eh) {
        uint8_t m[3 + 8 * 11];
        int n = 0;
        int t = s_et;
        while (t != s_eh && n < 8) {
            NetEdit *e = &s_editq[t];
            uint8_t *p = m + 3 + n * 11;
            put32(p, (uint32_t)e->wx);
            put32(p + 4, (uint32_t)e->wz);
            p[8] = e->wy; p[9] = e->blk; p[10] = e->orient;
            n++; t = (t + 1) % EDITQ;
        }
        m[0] = NET_MAGIC; m[1] = 'e'; m[2] = (uint8_t)n;
        if (!tx_send(m, 3 + n * 11)) return;         /* ring full: keep queued */
        s_et = (uint16_t)t;
    }
}

static void send_chest(int wx, int wy, int wz) {
    CraftChest *c = craft_chest_find(wx, wy, wz);
    uint8_t m[43];
    m[0] = NET_MAGIC; m[1] = 'C';
    put32(m + 2, (uint32_t)wx);
    put32(m + 6, (uint32_t)wz);
    m[10] = (uint8_t)wy;
    for (int i = 0; i < CRAFT_CHEST_SLOTS; i++) {
        m[11 + i * 2]     = c ? c->slots[i].blk : 0;
        m[11 + i * 2 + 1] = c ? c->slots[i].n   : 0;
    }
    tx_send(m, 43);
}

void craft_net_note_saving(bool on) {
    if (s_ns != NS_PLAY) return;
    uint8_t m[3] = { NET_MAGIC, 'v', (uint8_t)(on ? 1 : 0) };
    tx_send(m, 3);
    tx_pump();      /* flush now — the quiet spell starts right after */
}

/* ---- inbound dispatch ---------------------------------------------------- */
static float s_last_rx_t;            /* seconds since last inbound byte */
static float s_saving_grace;         /* peer flash-save tolerance window */
static float s_ka_t;                 /* keepalive send timer */

static void apply_edit(int32_t wx, int wy, int32_t wz, uint8_t blk, uint8_t orient) {
    s_applying = 1;
    if (orient != 0xFF) craft_torches_record_orient(wx, wy, wz, orient);
    int lx = (int)wx - craft_world_origin_x;
    int lz = (int)wz - craft_world_origin_z;
    bool in_win = (unsigned)lx < CRAFT_WORLD_X && (unsigned)lz < CRAFT_WORLD_Z;
    /* Guest mid-sync: the resident window still shows the placeholder
     * world — journal everything and let the adopt regen pick it up. */
    if (s_ns != NS_PLAY) in_win = false;
    if (in_win) {
        uint8_t prev = (uint8_t)craft_world_get(wx, wy, wz);
        if (prev != blk) {
            craft_world_set(wx, wy, wz, (BlockId)blk);
            /* Audible feedback when it happened near us. */
            const CraftPlayer *pl = craft_main_player();
            float dx = (float)wx - pl->cam.pos.x;
            float dz = (float)wz - pl->cam.pos.z;
            if (dx * dx + dz * dz < 20.0f * 20.0f) {
                if (blk == BLK_AIR) craft_audio_break((BlockId)prev);
                else                craft_audio_place((BlockId)blk);
            }
        }
    } else {
        craft_world_net_ingest_mod(wx, wy, wz, blk);
    }
    /* A chest cell overwritten by anything else drops its record so the
     * slot frees up (the breaker's side already looted the contents). */
    if (blk != BLK_CHEST && craft_chest_find(wx, wy, wz)) {
        craft_chest_remove(wx, wy, wz);
    }
    s_applying = 0;
}

static void handle_hello(const uint8_t *m) {
    if (m[2] != NET_PROTO) { net_fail("VERSION MISMATCH"); return; }
    if (m[3] == s_host) {
        net_fail(s_host ? "BOTH HOSTING - ONE MUST JOIN"
                        : "NOBODY HOSTING - NO WORLD");
        return;
    }
    s_got_hello = 1;
}

static void handle_state(const uint8_t *m) {
    float nx = getf(m + 2), ny = getf(m + 6), nz = getf(m + 10);
    if (!s_peer.present ||
        fabsf(nx - s_peer.x) > 24.0f || fabsf(nz - s_peer.z) > 24.0f) {
        s_peer.x = nx; s_peer.y = ny; s_peer.z = nz;   /* snap on spawn/teleport */
    }
    s_peer.present = 1;
    s_peer.tx = nx; s_peer.ty = ny; s_peer.tz = nz;
    s_peer.yaw  = (float)m[14] * (6.2831853f / 256.0f);
    s_peer.held = m[17];
    if (!s_host) {                                   /* host owns the clock */
        craft_main_net_set_world_time(
            (float)(uint16_t)(m[18] | (m[19] << 8)) * (300.0f / 65535.0f));
    }
}

static void handle_msg(const uint8_t *m, int len) {
    (void)len;
    switch (m[1]) {
    case 'H': handle_hello(m); break;
    case 'W':                                        /* guest: transfer meta */
        /* The host's hello and 'W' can land in the same rx batch, before
         * the state machine has advanced us out of NS_HELLO — accept in
         * either state once the hello handshake has happened. */
        if (s_host || !s_got_hello ||
            (s_ns != NS_SYNC && s_ns != NS_HELLO)) break;
        s_mseed  = get32(m + 2);
        s_mtime  = (float)(uint16_t)(m[6] | (m[7] << 8)) * (300.0f / 65535.0f);
        s_mspawn = v3(getf(m + 8), getf(m + 12), getf(m + 16));
        s_wtotal = get32(m + 20);
        s_wdone = 0; s_rxhdrn = 0; s_wseq = 0;
        s_rxfnv = 2166136261u;
        s_stream_end = 0;
        /* Drop the placeholder world NOW — chunk sections start landing
         * in the (rebound, re-nonced) scratch store immediately after. */
        craft_main_net_guest_prepare(s_mseed);
        s_guest_world_reset = 1;
        break;
    case 'm': {
        if (s_host) break;
        uint16_t seq = (uint16_t)(m[2] | (m[3] << 8));
        if (seq != s_wseq) { net_fail("TRANSFER ERROR"); break; }
        s_wseq++;
        world_rx_bytes(m + 5, m[4]);
        break;
    }
    case 'F':                                        /* transfer FNV trailer */
        if (s_host) break;
        if (!s_stream_end || s_rxfnv != get32(m + 2)) {
            net_fail("TRANSFER ERROR");
            break;
        }
        {
            uint8_t y[2] = { NET_MAGIC, 'y' };
            tx_send(y, 2);
        }
        NET_DBG("guest: transfer ok (%u bytes, fnv %08x) — adopting seed %u",
                (unsigned)s_wdone, (unsigned)s_rxfnv, (unsigned)s_mseed);
        craft_main_net_guest_adopt(s_mspawn, s_mtime);
        s_sync_done = 1;
        s_ns = NS_PLAY;
        s_phase = "";
        craft_menu_toast("Joined friend's world");
        break;
    case 'y':
        s_peer_acked = 1;
        if (s_host && s_ns == NS_SYNC) {
            NET_DBG("host: guest acked (%u bytes sent, fnv %08x)",
                    (unsigned)s_wdone, (unsigned)s_wfnv);
            s_ns = NS_PLAY;
            s_phase = "";
            craft_menu_toast("Friend joined");
        }
        break;
    case 'S': handle_state(m); break;
    case 'e':
        for (int i = 0; i < m[2]; i++) {
            const uint8_t *p = m + 3 + i * 11;
            apply_edit(get_i32(p), p[8], get_i32(p + 4), p[9], p[10]);
        }
        break;
    case 'C': {                                      /* chest contents */
        s_applying = 1;
        CraftChest *c = craft_chest_at(get_i32(m + 2), (int)m[10], get_i32(m + 6));
        if (c) {
            for (int i = 0; i < CRAFT_CHEST_SLOTS; i++) {
                c->slots[i].blk = m[11 + i * 2];
                c->slots[i].n   = m[11 + i * 2 + 1];
            }
        }
        s_applying = 0;
        break;
    }
    case 'v':                                        /* peer saving to flash */
        s_saving_grace = m[2] ? 10.0f : 0.0f;
        if (m[2]) craft_menu_toast(s_host ? "Friend is saving..." : "Host is saving...");
        break;
    case 'k': break;                                 /* keepalive */
    case 'Q':
        if (s_ns == NS_PLAY) {
            craft_menu_toast("Friend left - playing solo");
            craft_link_stop();
            s_ns = NS_OFF;
        } else {
            net_fail("PEER LEFT");
        }
        break;
    }
}

/* expected total frame length for a (partially received) message */
static int want_len(const uint8_t *m, int have) {
    switch (m[1]) {
    case 'H': return 4;
    case 'W': return 28;
    case 'm': return have >= 5 ? 5 + m[4] : 5;
    case 'F': return 6;
    case 'y': case 'Q': case 'k': return 2;
    case 'S': return 20;
    case 'e': return have >= 3 ? 3 + m[2] * 11 : 3;
    case 'C': return 43;
    case 'v': return 3;
    }
    return -1;
}

static uint8_t s_msg[128];
static int s_msg_len;

static void rx_pump(void) {
    uint8_t chunk[128];
    int n;
    while ((n = craft_link_recv(chunk, (int)sizeof chunk)) > 0) {
        s_last_rx_t = 0.0f;
        for (int i = 0; i < n; i++) {
            uint8_t b = chunk[i];
            if (s_msg_len == 0) { if (b == NET_MAGIC) s_msg[s_msg_len++] = b; continue; }
            s_msg[s_msg_len++] = b;
            int want = want_len(s_msg, s_msg_len);
            if (want < 0 || want > (int)sizeof s_msg) { s_msg_len = 0; continue; }
            if (s_msg_len < want) continue;
            s_msg_len = 0;
            handle_msg(s_msg, want);
            if (s_ns == NS_OFF || s_ns == NS_FAILED) return;
        }
    }
}

/* ---- session lifecycle ---------------------------------------------------- */
int  craft_net_active(void)   { return s_ns == NS_PLAY; }
int  craft_net_is_host(void)  { return s_ns == NS_PLAY && s_host; }
int  craft_net_started(void)  { return s_ns != NS_OFF && s_ns != NS_FAILED; }
bool craft_net_blocking(void) {
    return !s_host && (s_ns == NS_WAIT_LINK || s_ns == NS_HELLO || s_ns == NS_SYNC);
}

void craft_net_set_idle_pump(void (*pump)(void)) { s_idle_pump = pump; }

static void session_reset(int host) {
    memset(&s_peer, 0, sizeof s_peer);
    s_host = (uint8_t)host;
    s_got_hello = 0; s_hello_t = 0; s_wait_t = 0;
    s_msg_len = 0;
    s_txh = s_txt = 0;
    s_eh = s_et = 0;
    s_wtotal = s_wdone = 0; s_wseq = 0;
    s_stream_end = 0; s_sync_done = 0; s_peer_acked = 0;
    s_rxhdrn = 0;
    s_applying = 0;
    s_guest_world_reset = 0;
    s_last_rx_t = 0; s_saving_grace = 0; s_ka_t = 0;
    s_phase = "SEARCHING FOR FRIEND";
}

void craft_net_begin_host(void) {
    if (s_ns != NS_OFF && s_ns != NS_FAILED) return;
    /* Flush the complete diff journal to the chunk store so the flash
     * walk in the transfer sees everything (one-off flash burst, done
     * before the link opens so it can't starve USB mid-pairing). */
    craft_world_chunks_force_persist_window();
    while (craft_world_dirty_pending() > 0) craft_world_persist_tick();
    session_reset(1);
    craft_link_start();
    s_ns = NS_WAIT_LINK;
    craft_menu_toast("Searching for friend...");
}

void craft_net_begin_guest(void) {
    if (s_ns != NS_OFF && s_ns != NS_FAILED) return;
    session_reset(0);
    craft_link_start();
    s_ns = NS_WAIT_LINK;
}

void craft_net_stop(bool notify) {
    if (s_ns == NS_OFF) return;
    if (notify && craft_link_status() == CRAFT_LINK_CONNECTED) {
        uint8_t m[2] = { NET_MAGIC, 'Q' };
        tx_send(m, 2);
        tx_pump();
    }
    craft_link_stop();
    s_ns = NS_OFF;
    s_guest_world_reset = 0;
}

/* ---- capture hooks -------------------------------------------------------- */
void craft_net_note_set(int wx, int wy, int wz,
                        uint8_t prev, uint8_t blk, bool batched) {
    if (s_ns != NS_PLAY && s_ns != NS_SYNC) return;
    if (s_applying) return;
    if (prev == blk) return;
    /* Redstone-batch and fluid-sim results: both sides simulate them off
     * the same synced inputs whenever the peer is close enough to have
     * the circuit/fluid resident — sending would fight their sim. Only
     * send when the peer is too far to be simulating the cell. */
    bool sim_edit = batched ||
                    craft_is_water_id(prev) || craft_is_lava_id(prev);
    if (sim_edit && s_peer.present) {
        float dx = (float)wx - s_peer.tx;
        float dz = (float)wz - s_peer.tz;
        if (fabsf(dx) < NET_FAR_DIST && fabsf(dz) < NET_FAR_DIST) return;
    }
    editq_push(wx, wy, wz, blk);
}

void craft_net_note_orient(int wx, int wy, int wz, int face) {
    if (s_ns != NS_PLAY && s_ns != NS_SYNC) return;
    if (s_applying) return;
    /* Attach to the pending edit for that cell (place() records orient
     * right after craft_world_set in the same frame). */
    for (int i = s_et; i != s_eh; i = (i + 1) % EDITQ) {
        NetEdit *e = &s_editq[i];
        if (e->wx == wx && e->wz == wz && e->wy == (uint8_t)wy) {
            e->orient = (uint8_t)face;
            return;
        }
    }
}

/* ---- chest watcher -------------------------------------------------------- */
static bool s_chest_was_open;
static int  s_chest_wx, s_chest_wy, s_chest_wz;

static void chest_watch(void) {
    int wx, wy, wz;
    bool open = craft_menu_chest_view(&wx, &wy, &wz);
    if (open) {
        s_chest_wx = wx; s_chest_wy = wy; s_chest_wz = wz;
    } else if (s_chest_was_open) {
        send_chest(s_chest_wx, s_chest_wy, s_chest_wz);
    }
    s_chest_was_open = open;
}

/* ---- per-frame ------------------------------------------------------------ */
void craft_net_tick(const CraftInput *in, float dt) {
    if (s_ns == NS_FAILED) { s_ns = NS_OFF; return; }
    if (s_ns == NS_OFF) return;

    craft_link_task();
    /* Aggressive USB pump while pairing/syncing — enumeration advances
     * one control transfer per task call, and once per game frame is too
     * slow to finish inside a role-flip window. */
    if (s_idle_pump && s_ns != NS_PLAY) s_idle_pump();

    /* Guest can cancel the join at any pre-play stage. */
    if (craft_net_blocking() && in && in->b_pressed) {
        bool world_dropped = s_guest_world_reset != 0;
        craft_net_stop(true);
        if (world_dropped) craft_main_net_guest_abort();
        craft_menu_toast("Join cancelled");
        return;
    }

    if (s_ns != NS_WAIT_LINK && craft_link_status() != CRAFT_LINK_CONNECTED) {
        if (s_ns == NS_PLAY) {
            craft_menu_toast("Link lost - playing solo");
            craft_link_stop();
            s_ns = NS_OFF;
        } else {
            net_fail("LINK LOST");
        }
        return;
    }

    /* Inbound-silence health (only meaningful once both sides talk). */
    if (s_ns >= NS_HELLO) {
        s_last_rx_t += dt;
        if (s_saving_grace > 0.0f) s_saving_grace -= dt;
        float lost_after = 12.0f + (s_saving_grace > 0.0f ? 10.0f : 0.0f);
        if (s_ns == NS_PLAY && s_last_rx_t > lost_after) {
            craft_menu_toast("Link lost - playing solo");
            craft_link_stop();
            s_ns = NS_OFF;
            return;
        }
    }

    tx_pump();
    rx_pump();
    if (s_ns == NS_OFF || s_ns == NS_FAILED) return;

    /* Keepalive from the hello stage on — never let the pipe sit silent
     * longer than 500 ms. In-game this backs the health check; during
     * HELLO/SYNC it matters for the Studio internet bridge, whose splice
     * ends a session on prolonged one-way silence (a guest receiving a
     * big world transfer would otherwise send nothing for its whole
     * duration). Costs 4 B/s on a cable — harmless. */
    if (s_ns >= NS_HELLO) {
        s_ka_t += dt;
        if (s_ka_t >= 0.5f) {
            s_ka_t = 0;
            uint8_t m[2] = { NET_MAGIC, 'k' };
            tx_send(m, 2);
        }
    }

    switch (s_ns) {
    case NS_WAIT_LINK:
        s_phase = "SEARCHING FOR FRIEND";
        s_wait_t += dt;
        if (craft_link_status() == CRAFT_LINK_CONNECTED) {
            s_ns = NS_HELLO;
            s_wait_t = 0;
        } else if (s_wait_t > 90.0f) {
            net_fail("NO LINK FOUND");
        }
        break;
    case NS_HELLO:
        s_phase = "GREETING";
        s_hello_t -= dt;
        if (s_hello_t <= 0) { send_hello(); s_hello_t = 0.45f; }
        s_wait_t += dt;
        /* 120 s: on a cable the peer answers within seconds, but through
         * the Studio internet bridge this stage covers relay matchmaking
         * (the friend may not have opened their side yet). B cancels. */
        if (s_wait_t > 120.0f) { net_fail("NO ANSWER FROM PEER"); break; }
        if (s_got_hello) {
            s_ns = NS_SYNC;
            s_wait_t = 0;
            if (s_host) { world_tx_begin(); s_phase = "SENDING WORLD"; }
            else s_phase = "RECEIVING WORLD";
        }
        break;
    case NS_SYNC: {
        if (s_host) {
            world_tx_pump();
            flush_edits();               /* live edits ride behind the stream */
            if (s_txphase >= 5 && s_txoff >= s_txlen) s_phase = "WAITING FOR FRIEND";
        } else if (s_wtotal == 0) {
            /* We heard the host, but the host may not have heard US: over
             * the Studio internet bridge our pre-pairing hellos are eaten
             * by the bridge's sniffing, and the host's queued hello lands
             * the instant the relay pairs — so we'd go quiet before ever
             * being heard. Keep offering the hello until the host's 'W'
             * proves the handshake completed. Harmless on a cable. */
            s_hello_t -= dt;
            if (s_hello_t <= 0) { send_hello(); s_hello_t = 0.45f; }
        }
        /* Timeout on STALL, not total time — a big journal over the
         * cable can legitimately take minutes. Any forward progress
         * (bytes streamed either direction) resets the clock. */
        static uint32_t s_last_wdone;
        if (s_wdone != s_last_wdone) { s_last_wdone = s_wdone; s_wait_t = 0; }
        s_wait_t += dt;
        if (s_wait_t > 30.0f) net_fail("TRANSFER TIMED OUT");
        break;
    }
    case NS_PLAY: {
        /* smooth the peer toward its latest reported position */
        if (s_peer.present) {
            float k = dt * 10.0f;
            if (k > 1.0f) k = 1.0f;
            s_peer.x += (s_peer.tx - s_peer.x) * k;
            s_peer.y += (s_peer.ty - s_peer.y) * k;
            s_peer.z += (s_peer.tz - s_peer.z) * k;
        }
        static float s_state_t;
        s_state_t += dt;
        if (s_state_t >= 1.0f / 15.0f) { s_state_t = 0; send_state(); }
        flush_edits();
        chest_watch();
        break;
    }
    default: break;
    }
}

/* ---- overlay + remote player render --------------------------------------- */
static void draw_text_centered(uint16_t *fb, const char *s, int y, uint16_t c) {
    int w = craft_font_width(s);
    craft_font_draw(fb, s, (CRAFT_FB_W - w) / 2, y, c);
}

void craft_net_draw(uint16_t *fb) {
    if (s_ns == NS_OFF || s_ns == NS_FAILED) return;

    if (craft_net_blocking()) {
        /* Guest join panel — dim strip + phase + progress. */
        for (int y = 40; y < 88; y++) {
            for (int x = 8; x < CRAFT_FB_W - 8; x++) {
                uint16_t c = fb[y * CRAFT_FB_W + x];
                fb[y * CRAFT_FB_W + x] = (uint16_t)((c >> 2) & 0x39E7);
            }
        }
        draw_text_centered(fb, "LINK PLAY", 45, 0xFFFF);
        draw_text_centered(fb, s_phase, 57, 0xCE79);
        if (s_ns == NS_WAIT_LINK || s_ns == NS_HELLO) {
            /* Diagnostic line: current USB role (should alternate D/H
             * every ~second while searching) + elapsed seconds. */
            char dbg[24];
            snprintf(dbg, sizeof dbg, "usb %c  %ds",
                     craft_link_role_host() ? 'H' : 'D', (int)s_wait_t);
            draw_text_centered(fb, dbg, 67, 0x8410);
        }
        if (s_ns == NS_SYNC && s_wtotal > 0) {
            uint32_t p = s_wdone * 100u / s_wtotal;
            if (p > 100) p = 100;
            char buf[16];
            snprintf(buf, sizeof buf, "%u%%", (unsigned)p);
            draw_text_centered(fb, buf, 67, 0xFFE0);
            int bar_w = (int)((CRAFT_FB_W - 32) * p / 100u);
            for (int x = 0; x < bar_w; x++)
                fb[78 * CRAFT_FB_W + 16 + x] = 0x07E0;
        }
        draw_text_centered(fb, "B: cancel", 82, 0x8410);
        return;
    }

    /* Host-side searching/greeting/sync runs behind normal play — one
     * status line at the top so it doesn't cover gameplay. */
    if (s_host && s_ns != NS_PLAY) {
        static char buf[36];
        const char *txt = s_phase;
        if (s_ns == NS_SYNC && s_wtotal > 0) {
            uint32_t p = s_wdone * 100u / s_wtotal;
            if (p > 100) p = 100;
            snprintf(buf, sizeof buf, "SENDING WORLD %u%%", (unsigned)p);
            txt = buf;
        } else if (s_ns == NS_WAIT_LINK || s_ns == NS_HELLO) {
            snprintf(buf, sizeof buf, "%s %c %ds", s_phase,
                     craft_link_role_host() ? 'H' : 'D', (int)s_wait_t);
            txt = buf;
        }
        draw_text_centered(fb, txt, 2, 0xFFE0);
    } else if (s_ns == NS_PLAY && s_last_rx_t > 3.0f && s_saving_grace <= 0.0f) {
        draw_text_centered(fb, "LINK STALLED...", 2, 0xF800);
    }
}

void craft_net_render_remote(const CraftCamera *cam, uint16_t *fb) {
    if (s_ns != NS_PLAY || !s_peer.present) return;
    Vec3 feet = v3(s_peer.x, s_peer.y - 1.60f, s_peer.z);
    craft_mobs_render_net_player(cam, fb, feet, s_peer.yaw);
}

#endif /* CRAFT_NET_ENABLED */

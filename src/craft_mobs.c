/*
 * ThumbyCraft — passive mobs (Phase 27, 3D rebuild).
 *
 * Each mob is composed of several axis-aligned cuboid parts (body,
 * head, legs, snout, etc) defined in mob-local space. At render time
 * we project the mob's world AABB to find the screen bounding box,
 * then per-pixel cast a ray into the mob's local frame and test
 * against each cuboid using slab intersection. The nearest hit
 * defines the colour + face for shading.
 *
 * This integrates with the existing raycaster's z-buffer so mobs
 * occlude correctly against blocks AND each other — no billboards,
 * no facing-camera cheats. The mob rotates around its yaw axis with
 * proper depth on every side, which is what "blocky 3D" means.
 *
 * Cost: only the pixels inside each mob's screen bbox are walked.
 * For ~6 mobs at typical distances that's a few hundred extra pixel
 * rays per frame, maybe 5% of one core.
 */
#include "craft_mobs.h"
#include "craft_world.h"
#include "craft_blocks.h"
#include "craft_player.h"
#include "craft_particles.h"

#include <string.h>

CraftMob   craft_mobs[CRAFT_MAX_MOBS];
CraftArrow craft_arrows[CRAFT_MAX_ARROWS];

/* --- Model description ------------------------------------------- */

typedef struct {
    float    cx, cy, cz;       /* part centre in local frame */
    float    hx, hy, hz;       /* half-sizes */
    uint16_t color;
} CuboidPart;

/* Bumped 13 → 17 for the detailed skeleton (skull + nose socket + jaw +
 * upper/lower torso + sternum + 3 ribs + 2 eyes + 2 arms + 2 legs = 15)
 * and the detailed creeper (head + base ring + 4 eye cubes + 2-piece
 * T-mouth + torso + 4 mottle spots + 4 legs = 17). Adds ~28 B × 4 slots
 * × 7 mob types ≈ 0.8 KB BSS — sized to keep total RAM under the link
 * ceiling (heap + stack + BSS all fight for the 512 KB region). */
#define MAX_PARTS 17
typedef struct {
    int             n_parts;
    CuboidPart      parts[MAX_PARTS];
    float           radius;    /* horizontal yaw-invariant radius */
    float           height;    /* world height (feet to top) */
} MobModel;

static MobModel s_models[MOB_TYPE_COUNT];

static const float mob_speed[MOB_TYPE_COUNT] = {
    1.0f,    /* sheep */
    1.3f,    /* pig   */
    1.6f,    /* chicken */
    2.2f,    /* slime */
    1.8f,    /* skeleton — strafes / approaches at moderate pace */
    3.3f,    /* spider — fast */
    2.4f,    /* creeper — slightly faster than slime */
};
static const int mob_hp_table[MOB_TYPE_COUNT] = {
    3, 3, 1, 2,
    4,  /* skeleton */
    4,  /* spider   */
    3,  /* creeper  */
};
/* Aggro / contact distances and behavioural tunables.
 *
 * Mob stand-off: hostiles stop closing when they're within this far
 * of the player so they always sit in the ADJACENT world cell rather
 * than shoving into the player's own cell. With player half-width
 * 0.30 and mob radii 0.4-0.6, 1.4 m keeps the mob centre in a
 * different floor cell from the player's centre across all approach
 * angles. Melee mobs still hit from this range (their attack is a
 * contact-tick, not an AABB collision). */
#define MOB_STANDOFF           1.40f
#define SLIME_AGGRO_DIST       12.0f
#define SLIME_CONTACT_DIST     MOB_STANDOFF
#define SKEL_AGGRO_DIST        16.0f
#define SKEL_KEEP_DIST          5.0f   /* stop closing inside this range */
#define SKEL_BACK_DIST          3.0f   /* back off if too close */
#define SKEL_FIRE_GAP           2.0f   /* sec between arrow shots */
#define SKEL_ARROW_SPEED       14.0f
#define SPIDER_AGGRO_DIST      14.0f
#define SPIDER_CONTACT_DIST    MOB_STANDOFF
#define CREEPER_AGGRO_DIST     14.0f
#define CREEPER_FUSE_DIST       1.80f   /* fuse a tad sooner so blast point is adjacent */
#define CREEPER_FUSE_TIME       1.0f
#define CREEPER_BLAST_DIST      2.5f
#define CREEPER_BLAST_DAMAGE    5
#define NIGHT_SPAWN_GAP    4.0f   /* sec between spawn attempts at night */
#define DAY_SPAWN_GAP     20.0f   /* sec between spawn attempts during the day */

static float s_day_night_t;

/* --- RNG ---------------------------------------------------------- */
static uint32_t s_rng = 0xC0FFEE;
static uint32_t xs(void) {
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}
static float frand(void) { return (xs() & 0xFFFF) / 65535.0f; }

/* --- Build the cuboid models ------------------------------------- */
/* Models look "down +Z" in local space (so head is at +Z). The mob's
 * yaw rotates the model around the Y axis into world space at render
 * time. All centres + sizes are in world-unit fractions; legs sit on
 * the ground plane (local y=0 is feet level). */

void craft_mobs_build_sprites(void) {
    /* SHEEP — cream body, grey legs. */
    MobModel *m = &s_models[MOB_SHEEP];
    uint16_t WOOL = rgb565(240, 235, 220);
    uint16_t LEG  = rgb565(110, 100, 90);
    m->parts[0] = (CuboidPart){  0.00f, 0.55f,  0.00f,  0.32f, 0.28f, 0.50f, WOOL };  /* body */
    m->parts[1] = (CuboidPart){  0.00f, 0.65f,  0.55f,  0.22f, 0.22f, 0.22f, WOOL };  /* head */
    m->parts[2] = (CuboidPart){ -0.20f, 0.13f,  0.28f,  0.08f, 0.13f, 0.08f, LEG  };  /* FL leg */
    m->parts[3] = (CuboidPart){  0.20f, 0.13f,  0.28f,  0.08f, 0.13f, 0.08f, LEG  };  /* FR leg */
    m->parts[4] = (CuboidPart){ -0.20f, 0.13f, -0.28f,  0.08f, 0.13f, 0.08f, LEG  };  /* BL leg */
    m->parts[5] = (CuboidPart){  0.20f, 0.13f, -0.28f,  0.08f, 0.13f, 0.08f, LEG  };  /* BR leg */
    m->n_parts = 6;
    m->radius  = 0.78f;
    m->height  = 1.25f;

    /* PIG — pink body, darker snout, pink legs. */
    m = &s_models[MOB_PIG];
    uint16_t PIG  = rgb565(240, 160, 180);
    uint16_t SNOUT= rgb565(200, 110, 140);
    m->parts[0] = (CuboidPart){  0.00f, 0.45f,  0.00f,  0.28f, 0.22f, 0.45f, PIG   };  /* body */
    m->parts[1] = (CuboidPart){  0.00f, 0.48f,  0.50f,  0.20f, 0.18f, 0.15f, PIG   };  /* head */
    m->parts[2] = (CuboidPart){  0.00f, 0.42f,  0.66f,  0.10f, 0.08f, 0.05f, SNOUT };  /* snout */
    m->parts[3] = (CuboidPart){ -0.18f, 0.12f,  0.25f,  0.08f, 0.12f, 0.08f, PIG   };
    m->parts[4] = (CuboidPart){  0.18f, 0.12f,  0.25f,  0.08f, 0.12f, 0.08f, PIG   };
    m->parts[5] = (CuboidPart){ -0.18f, 0.12f, -0.25f,  0.08f, 0.12f, 0.08f, PIG   };
    m->parts[6] = (CuboidPart){  0.18f, 0.12f, -0.25f,  0.08f, 0.12f, 0.08f, PIG   };
    m->n_parts = 7;
    m->radius  = 0.62f;
    m->height  = 1.0f;

    /* CHICKEN — small white body, red comb, yellow beak + legs. */
    m = &s_models[MOB_CHICKEN];
    uint16_t WHT  = rgb565(240, 240, 240);
    uint16_t YEL  = rgb565(240, 220, 60);
    uint16_t RED  = rgb565(220, 50, 50);
    m->parts[0] = (CuboidPart){  0.00f, 0.25f,  0.00f,  0.14f, 0.18f, 0.18f, WHT };
    m->parts[1] = (CuboidPart){  0.00f, 0.48f,  0.10f,  0.11f, 0.10f, 0.10f, WHT };
    m->parts[2] = (CuboidPart){  0.00f, 0.46f,  0.22f,  0.04f, 0.03f, 0.05f, YEL };
    m->parts[3] = (CuboidPart){  0.00f, 0.59f,  0.06f,  0.05f, 0.04f, 0.06f, RED };
    m->parts[4] = (CuboidPart){ -0.07f, 0.07f,  0.00f,  0.04f, 0.07f, 0.04f, YEL };
    m->parts[5] = (CuboidPart){  0.07f, 0.07f,  0.00f,  0.04f, 0.07f, 0.04f, YEL };
    m->n_parts = 6;
    m->radius  = 0.32f;
    m->height  = 0.70f;

    /* SLIME — single bright-green chunky cube + dark spots for eyes. */
    m = &s_models[MOB_SLIME];
    uint16_t GREEN = rgb565(80, 220, 70);
    uint16_t DARK  = rgb565(30,  60, 30);
    m->parts[0] = (CuboidPart){  0.00f, 0.35f,  0.00f,  0.35f, 0.35f, 0.35f, GREEN };  /* body */
    m->parts[1] = (CuboidPart){ -0.15f, 0.45f,  0.34f,  0.05f, 0.04f, 0.02f, DARK  };  /* L eye */
    m->parts[2] = (CuboidPart){  0.15f, 0.45f,  0.34f,  0.05f, 0.04f, 0.02f, DARK  };  /* R eye */
    m->parts[3] = (CuboidPart){  0.00f, 0.20f,  0.34f,  0.18f, 0.02f, 0.02f, DARK  };  /* mouth */
    m->n_parts = 4;
    m->radius  = 0.55f;
    m->height  = 0.70f;

    /* SKELETON — humanoid: skull with sunken eyes + nose cavity + jaw,
     * ribcage with horizontal rib slats and central sternum, arms at
     * sides, narrow legs. ~0.5 wide × 1.6 tall × 0.32 deep envelope. */
    m = &s_models[MOB_SKELETON];
    uint16_t BONE   = rgb565(230, 230, 215);
    uint16_t BONE_D = rgb565(180, 180, 170);
    uint16_t RIB    = rgb565(150, 150, 140);   /* darker bone for ribs */
    uint16_t SOCKET = rgb565(10, 10, 10);
    /* Skull — cubic, ~0.30 across, sits on top. */
    m->parts[0] = (CuboidPart){  0.00f, 1.42f,  0.00f,  0.15f, 0.13f, 0.13f, BONE   };  /* skull */
    /* Eye sockets — recessed deeper into the skull (front face flush,
     * extra depth behind). cz shifted back, hz grown so the socket
     * punches further in but only just protrudes from the skull face. */
    m->parts[1] = (CuboidPart){ -0.07f, 1.46f,  0.11f,  0.04f, 0.04f, 0.04f, SOCKET };  /* L eye socket */
    m->parts[2] = (CuboidPart){  0.07f, 1.46f,  0.11f,  0.04f, 0.04f, 0.04f, SOCKET };  /* R eye socket */
    /* Nose cavity — central dark triangle/socket below the eyes. */
    m->parts[3] = (CuboidPart){  0.00f, 1.38f,  0.11f,  0.02f, 0.04f, 0.04f, SOCKET };  /* nose cavity */
    /* Jaw — narrower chunk hanging below the skull front. */
    m->parts[4] = (CuboidPart){  0.00f, 1.24f,  0.03f,  0.10f, 0.04f, 0.09f, BONE_D };  /* jaw */
    /* Hourglass ribcage: wider upper chest, narrower lower torso. */
    m->parts[5] = (CuboidPart){  0.00f, 1.02f,  0.00f,  0.17f, 0.13f, 0.09f, BONE_D };  /* upper torso */
    m->parts[6] = (CuboidPart){  0.00f, 0.78f,  0.00f,  0.10f, 0.12f, 0.07f, BONE_D };  /* lower torso */
    /* Ribs — 3 horizontal slats across the chest, slightly proud of
     * the upper torso (cz 0.12 > torso front face 0.09). Spaced ~0.10
     * apart vertically across the chest. */
    m->parts[7] = (CuboidPart){  0.00f, 1.13f,  0.12f,  0.10f, 0.02f, 0.03f, RIB    };  /* top rib */
    m->parts[8] = (CuboidPart){  0.00f, 1.02f,  0.12f,  0.10f, 0.02f, 0.03f, RIB    };  /* mid rib */
    m->parts[9] = (CuboidPart){  0.00f, 0.91f,  0.12f,  0.10f, 0.02f, 0.03f, RIB    };  /* low rib */
    /* Sternum — vertical bone ridge running down the centre of the
     * ribs, slightly more proud than the ribs themselves. */
    m->parts[10] = (CuboidPart){  0.00f, 1.02f,  0.13f,  0.018f, 0.13f, 0.025f, BONE };  /* sternum */
    /* Arms hanging at the sides, just outboard of the chest. */
    m->parts[11] = (CuboidPart){ -0.22f, 0.85f,  0.00f,  0.05f, 0.27f, 0.05f, BONE   };  /* L arm */
    m->parts[12] = (CuboidPart){  0.22f, 0.85f,  0.00f,  0.05f, 0.27f, 0.05f, BONE   };  /* R arm */
    /* Legs — narrow bone shafts down to the feet (y=0). */
    m->parts[13] = (CuboidPart){ -0.08f, 0.30f,  0.00f,  0.05f, 0.30f, 0.05f, BONE   };  /* L leg */
    m->parts[14] = (CuboidPart){  0.08f, 0.30f,  0.00f,  0.05f, 0.30f, 0.05f, BONE   };  /* R leg */
    m->n_parts = 15;
    m->radius  = 0.30f;   /* arms set the widest yaw-invariant envelope */
    m->height  = 1.60f;

    /* SPIDER — wide & low arachnid. Front cephalothorax (smaller),
     * rear abdomen (larger) with red hourglass marking on top, two
     * red eye dots on the front, eight splayed legs (two per side
     * front/back, four per side total). ~1.2 wide × 0.5 tall ×
     * 1.0 deep envelope. */
    m = &s_models[MOB_SPIDER];
    uint16_t SPDR   = rgb565(30, 22, 26);   /* abdomen — coldest dark */
    uint16_t SPDR_H = rgb565(55, 42, 46);   /* cephalothorax — slightly lifted */
    uint16_t SPDR_L = rgb565(20, 16, 20);   /* legs — near-black */
    uint16_t EYE_R  = rgb565(230, 30, 30);
    uint16_t MARK_R = rgb565(160, 25, 25);  /* deep crimson marking */
    /* Cephalothorax (front, +Z). */
    m->parts[0] = (CuboidPart){  0.00f, 0.20f,  0.20f,  0.16f, 0.10f, 0.18f, SPDR_H };
    /* Abdomen (rear, -Z) — bigger, slightly higher dome. */
    m->parts[1] = (CuboidPart){  0.00f, 0.22f, -0.20f,  0.22f, 0.13f, 0.24f, SPDR   };
    /* Red marking visible from above on the abdomen. */
    m->parts[2] = (CuboidPart){  0.00f, 0.36f, -0.20f,  0.08f, 0.01f, 0.14f, MARK_R };
    /* Eyes — small red dots on front of cephalothorax. */
    m->parts[3] = (CuboidPart){ -0.06f, 0.22f,  0.38f,  0.03f, 0.025f, 0.02f, EYE_R };
    m->parts[4] = (CuboidPart){  0.06f, 0.22f,  0.38f,  0.03f, 0.025f, 0.02f, EYE_R };
    /* Eight legs — four per side, splayed outward and at four
     * different Z offsets so they read as distinct legs from the
     * side. Each is a thin horizontal cuboid that juts past the
     * body silhouette. Y kept near body height so they read as
     * "legs out to the side" not "feet under". */
    /* Left side: front, mid-front, mid-back, back. */
    m->parts[5]  = (CuboidPart){ -0.42f, 0.16f,  0.28f,  0.22f, 0.025f, 0.03f, SPDR_L };
    m->parts[6]  = (CuboidPart){ -0.44f, 0.16f,  0.09f,  0.22f, 0.025f, 0.03f, SPDR_L };
    m->parts[7]  = (CuboidPart){ -0.44f, 0.16f, -0.12f,  0.22f, 0.025f, 0.03f, SPDR_L };
    m->parts[8]  = (CuboidPart){ -0.42f, 0.16f, -0.32f,  0.22f, 0.025f, 0.03f, SPDR_L };
    /* Right side: front, mid-front, mid-back, back. */
    m->parts[9]  = (CuboidPart){  0.42f, 0.16f,  0.28f,  0.22f, 0.025f, 0.03f, SPDR_L };
    m->parts[10] = (CuboidPart){  0.44f, 0.16f,  0.09f,  0.22f, 0.025f, 0.03f, SPDR_L };
    m->parts[11] = (CuboidPart){  0.44f, 0.16f, -0.12f,  0.22f, 0.025f, 0.03f, SPDR_L };
    m->parts[12] = (CuboidPart){  0.42f, 0.16f, -0.32f,  0.22f, 0.025f, 0.03f, SPDR_L };
    m->n_parts = 13;
    m->radius  = 0.72f;   /* widest splay of legs (corner of outer leg cuboid) */
    m->height  = 0.50f;

    /* CREEPER — iconic Minecraft silhouette: cubic-ish head on top
     * with vanilla 4-quadrant eyes + downturned T-mouth, base ring
     * at the head/torso join, mottled torso, 4 stubby corner legs,
     * NO arms. ~0.65 wide × 1.7 tall × 0.45 deep. */
    m = &s_models[MOB_CREEPER];
    uint16_t CRP   = rgb565(80, 190, 80);    /* bright creeper green */
    uint16_t CRP_D = rgb565(45, 120, 45);    /* darker mottle + base ring */
    uint16_t CRP_F = rgb565(15, 15, 15);     /* face features (black) */
    /* Head — wider/deeper than torso, sits at the top. */
    m->parts[0] = (CuboidPart){  0.00f, 1.50f,  0.00f,  0.22f, 0.20f, 0.22f, CRP   };  /* head */
    /* Base ring — thin darker green band where the head meets the
     * torso, slightly proud so it reads as a defined seam. */
    m->parts[1] = (CuboidPart){  0.00f, 1.31f,  0.00f,  0.225f, 0.015f, 0.225f, CRP_D }; /* base ring */
    /* Eyes — 4 small dark cubes (2 per eye, side-by-side) to suggest
     * the vanilla pixelated 2×2-ish square eye holes high on the head. */
    m->parts[2] = (CuboidPart){ -0.13f, 1.58f,  0.23f,  0.04f, 0.045f, 0.02f, CRP_F }; /* L eye outer */
    m->parts[3] = (CuboidPart){ -0.05f, 1.58f,  0.23f,  0.04f, 0.045f, 0.02f, CRP_F }; /* L eye inner */
    m->parts[4] = (CuboidPart){  0.05f, 1.58f,  0.23f,  0.04f, 0.045f, 0.02f, CRP_F }; /* R eye inner */
    m->parts[5] = (CuboidPart){  0.13f, 1.58f,  0.23f,  0.04f, 0.045f, 0.02f, CRP_F }; /* R eye outer */
    /* Mouth — downturned T: central vertical stem + single horizontal
     * "frown" bar across its top, in dark face colour. (One wider bar
     * instead of two flanking cubes — same silhouette, saves a part.) */
    m->parts[6] = (CuboidPart){  0.00f, 1.39f,  0.23f,  0.03f, 0.07f, 0.02f, CRP_F }; /* stem */
    m->parts[7] = (CuboidPart){  0.00f, 1.44f,  0.23f,  0.13f, 0.02f, 0.02f, CRP_F }; /* frown bar */
    /* Torso — narrower than head front-to-back, tall column. */
    m->parts[8] = (CuboidPart){  0.00f, 0.80f,  0.00f,  0.18f, 0.50f, 0.10f, CRP   };  /* torso */
    /* Mottling — small darker green spots scattered across the torso,
     * each sitting just proud of the surface (front, back, both sides). */
    m->parts[9]  = (CuboidPart){ -0.09f, 1.10f,  0.11f,  0.025f, 0.025f, 0.01f, CRP_D }; /* front upper */
    m->parts[10] = (CuboidPart){  0.07f, 0.55f,  0.11f,  0.025f, 0.025f, 0.01f, CRP_D }; /* front lower */
    m->parts[11] = (CuboidPart){ -0.19f, 0.95f, -0.02f,  0.01f,  0.025f, 0.025f, CRP_D }; /* L side */
    m->parts[12] = (CuboidPart){  0.19f, 0.65f,  0.03f,  0.01f,  0.025f, 0.025f, CRP_D }; /* R side */
    /* Four stubby corner legs. */
    m->parts[13] = (CuboidPart){ -0.10f, 0.15f,  0.06f,  0.08f, 0.15f, 0.05f, CRP   };  /* FL leg */
    m->parts[14] = (CuboidPart){  0.10f, 0.15f,  0.06f,  0.08f, 0.15f, 0.05f, CRP   };  /* FR leg */
    m->parts[15] = (CuboidPart){ -0.10f, 0.15f, -0.06f,  0.08f, 0.15f, 0.05f, CRP   };  /* BL leg */
    m->parts[16] = (CuboidPart){  0.10f, 0.15f, -0.06f,  0.08f, 0.15f, 0.05f, CRP   };  /* BR leg */
    m->n_parts = 17;
    m->radius  = 0.32f;   /* head diagonal sets the yaw-invariant envelope */
    m->height  = 1.70f;

    /* Sort every model's parts by volume descending. The mob renderer
     * iterates parts per pixel inside the screen bbox with a best_t
     * early-out — if we test the big body cube first it tightens
     * best_t immediately, and the dozens of tiny detail cubes (eyes,
     * mouth, ribs, mottling) get rejected by `t_near > best_t` in
     * one compare instead of running the full slab intersection.
     * Insertion sort — N ≤ 17 and only runs once at startup. */
    for (int type = 0; type < MOB_TYPE_COUNT; type++) {
        MobModel *mm = &s_models[type];
        for (int i = 1; i < mm->n_parts; i++) {
            CuboidPart key = mm->parts[i];
            float key_vol = key.hx * key.hy * key.hz;
            int j = i - 1;
            while (j >= 0) {
                float jvol = mm->parts[j].hx * mm->parts[j].hy * mm->parts[j].hz;
                if (jvol >= key_vol) break;
                mm->parts[j + 1] = mm->parts[j];
                j--;
            }
            mm->parts[j + 1] = key;
        }
    }
}

static bool mob_is_hostile(MobType t) {
    return t == MOB_SLIME || t == MOB_SKELETON ||
           t == MOB_SPIDER || t == MOB_CREEPER;
}

/* --- Find a grass/sand/dirt block under (x, z) ------------------- */
static int find_ground(int x, int z) {
    /* World is infinite — bound check is now only Y, handled by
     * craft_world_get returning BLK_AIR for out-of-window coords. */
    for (int y = CRAFT_WORLD_Y - 2; y > 0; y--) {
        BlockId b = craft_world_get(x, y, z);
        if (b == BLK_GRASS || b == BLK_SAND || b == BLK_DIRT) return y;
    }
    return -1;
}

void craft_mobs_spawn_around(Vec3 centre, uint32_t seed) {
    s_rng ^= seed;
    for (int i = 0; i < CRAFT_MAX_MOBS; i++) craft_mobs[i].alive = false;
    craft_arrows_clear();
    int placed = 0;
    for (int tries = 0; tries < 60 && placed < CRAFT_PASSIVE_MAX; tries++) {
        int dx = (int)(xs() & 0x1F) - 16;
        int dz = (int)(xs() & 0x1F) - 16;
        int x  = (int)centre.x + dx;
        int z  = (int)centre.z + dz;
        int y  = find_ground(x, z);
        if (y < 0) continue;
        CraftMob *m = &craft_mobs[placed];
        m->alive    = true;
        m->type     = (MobType)(xs() % MOB_SLIME);
        m->pos      = v3((float)x + 0.5f, (float)(y + 1), (float)z + 0.5f);
        m->yaw      = frand() * 6.2831853f;
        m->vel      = v3(0, 0, 0);
        m->ai_timer = 0.5f + frand() * 2.0f;
        m->hp       = mob_hp_table[m->type];
        m->hurt_flash = 0.0f;
        m->fire_cooldown = 0.0f;
        m->fuse_t        = 0.0f;
        placed++;
    }
    s_day_night_t = 0.0f;
}


/* Stash the last sun_y the day/night tick saw so spawn_hostile can
 * decide whether daytime spawns need to be sky-shielded. */
static float s_last_sun_y = -1.0f;

void craft_mobs_spawn_hostile(CraftPlayer *p, int n) {
    bool is_day = s_last_sun_y > 0.0f;
    int placed = 0;
    for (int tries = 0; tries < 80 && placed < n; tries++) {
        /* Spawn beyond visible range so they emerge instead of popping
         * in. 12-20 blocks from the player. */
        float angle = frand() * 6.2831853f;
        float dist  = 12.0f + frand() * 8.0f;
        int x = (int)(p->cam.pos.x + cosf(angle) * dist);
        int z = (int)(p->cam.pos.z + sinf(angle) * dist);
        int y = find_ground(x, z);
        if (y < 0) continue;
        /* Hostile spawn rule: only allowed if the spawn cell is in
         * shadow or it's currently night. At day, a sky-exposed
         * surface tile is direct sunlight — skip it.
         * `y+1` is the cell the mob's feet occupy (one above the
         * ground block). */
        if (is_day && craft_world_sky_exposed(x, y + 1, z)) continue;
        /* Find a free slot. */
        for (int i = 0; i < CRAFT_MAX_MOBS; i++) {
            if (craft_mobs[i].alive) continue;
            CraftMob *m = &craft_mobs[i];
            /* Pick a random hostile type. SLIME/SKELETON/SPIDER/CREEPER
             * are 4 contiguous enum values starting at MOB_SLIME. */
            MobType t = (MobType)(MOB_SLIME + (xs() & 3));
            m->alive    = true;
            m->type     = t;
            m->pos      = v3((float)x + 0.5f, (float)(y + 1), (float)z + 0.5f);
            m->yaw      = frand() * 6.2831853f;
            m->vel      = v3(0, 0, 0);
            m->ai_timer = 0.5f;
            m->hp       = mob_hp_table[t];
            m->hurt_flash    = 0.0f;
            m->fire_cooldown = 1.0f + frand() * 1.5f;
            m->fuse_t        = 0.0f;
            placed++;
            break;
        }
    }
}

bool craft_mob_damage(int mob_index, int amt) {
    if (mob_index < 0 || mob_index >= CRAFT_MAX_MOBS) return false;
    CraftMob *m = &craft_mobs[mob_index];
    if (!m->alive) return false;
    m->hp -= amt;
    m->hurt_flash = 0.25f;
    /* Knock-back away from current heading. */
    m->vel.y = 4.0f;
    if (m->hp <= 0) {
        m->alive = false;
        return true;
    }
    return false;
}

/* Ray-vs-mob picking. Returns index of closest hit mob within max_dist
 * or -1 if none. Uses the same yaw-invariant AABB the renderer uses. */
int craft_mobs_pick(const CraftCamera *cam, float max_dist) {
    float cy = cosf(cam->yaw),  sye = sinf(cam->yaw);
    float cp = cosf(cam->pitch), sp = sinf(cam->pitch);
    float dx = sye * cp, dy = sp, dz = cy * cp;

    int best = -1;
    float best_t = max_dist;
    for (int i = 0; i < CRAFT_MAX_MOBS; i++) {
        CraftMob *m = &craft_mobs[i];
        if (!m->alive) continue;
        const MobModel *model = &s_models[m->type];
        float bminx = m->pos.x - model->radius;
        float bmaxx = m->pos.x + model->radius;
        float bminy = m->pos.y;
        float bmaxy = m->pos.y + model->height;
        float bminz = m->pos.z - model->radius;
        float bmaxz = m->pos.z + model->radius;
        /* Slab intersect against world-space AABB. */
        float t_near = 0.0f, t_far = best_t;
        float inv;
        bool fail = false;
#define SLAB(o, d, mn, mx)                                          \
        do {                                                        \
            if (d > -1e-6f && d < 1e-6f) {                          \
                if (o < mn || o > mx) { fail = true; break; }       \
            } else {                                                \
                inv = 1.0f / d;                                     \
                float t1 = (mn - o) * inv;                          \
                float t2 = (mx - o) * inv;                          \
                if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; } \
                if (t1 > t_near) t_near = t1;                       \
                if (t2 < t_far)  t_far  = t2;                       \
                if (t_near > t_far) { fail = true; break; }         \
            }                                                       \
        } while (0)
        SLAB(cam->pos.x, dx, bminx, bmaxx);
        if (!fail) SLAB(cam->pos.y, dy, bminy, bmaxy);
        if (!fail) SLAB(cam->pos.z, dz, bminz, bmaxz);
#undef SLAB
        if (fail) continue;
        if (t_near < 0.0f) continue;
        if (t_near < best_t) {
            best_t = t_near;
            best = i;
        }
    }
    return best;
}

void craft_mobs_day_night_tick(float dt, float sun_y, CraftPlayer *p) {
    if (p->mode != CRAFT_MODE_SURVIVAL) return;
    s_last_sun_y = sun_y;
    s_day_night_t += dt;
    /* Top up hostile count toward CRAFT_HOSTILE_MAX. Faster at night
     * (sun below horizon), slower during the day. No daylight
     * despawn — slimes are tough enough to roam in sunlight. */
    bool night = sun_y < -0.10f;
    float gap  = night ? NIGHT_SPAWN_GAP : DAY_SPAWN_GAP;
    if (s_day_night_t < gap) return;
    s_day_night_t = 0.0f;
    int alive_h = 0;
    for (int i = 0; i < CRAFT_MAX_MOBS; i++)
        if (craft_mobs[i].alive && craft_mobs[i].type == MOB_SLIME) alive_h++;
    if (alive_h < CRAFT_HOSTILE_MAX) craft_mobs_spawn_hostile(p, 1);
}

/* --- AI ----------------------------------------------------------- */
static void ai_decide(CraftMob *m) {
    float roll = frand();
    if (roll < 0.35f) {
        m->vel = v3(0, m->vel.y, 0);
        m->ai_timer = 1.5f + frand() * 2.5f;
    } else if (roll < 0.55f) {
        m->yaw += (frand() - 0.5f) * 2.0f;
        m->vel = v3(0, m->vel.y, 0);
        m->ai_timer = 0.5f + frand() * 1.0f;
    } else {
        m->yaw += (frand() - 0.5f) * 1.2f;
        float spd = mob_speed[m->type];
        m->vel.x = sinf(m->yaw) * spd;
        m->vel.z = cosf(m->yaw) * spd;
        m->ai_timer = 1.5f + frand() * 3.0f;
    }
}

/* Slime AI override: chase player when in range, contact-damage on
 * proximity. Falls back to wandering when player is far. */
static void slime_ai(CraftMob *m, CraftPlayer *p, float dt) {
    (void)dt;
    float dx = p->cam.pos.x - m->pos.x;
    float dz = p->cam.pos.z - m->pos.z;
    float dist = sqrtf(dx * dx + dz * dz);
    if (dist > SLIME_AGGRO_DIST) {
        /* Out of range — wander like a passive mob. */
        if (m->ai_timer <= 0.0f) ai_decide(m);
        return;
    }
    /* In range — chase until stand-off distance, then hold. */
    if (dist > 0.001f) {
        m->yaw = atan2f(dx, dz);
        if (dist > MOB_STANDOFF) {
            float spd = mob_speed[MOB_SLIME];
            m->vel.x = (dx / dist) * spd;
            m->vel.z = (dz / dist) * spd;
        } else {
            m->vel.x = 0; m->vel.z = 0;
        }
        m->ai_timer = 0.5f;
    }
    /* Contact damage — fires while sitting at stand-off range. */
    if (dist < SLIME_CONTACT_DIST + 0.10f) {
        craft_player_take_damage(p, 1);
    }
}

/* Rough "line of sight" — true if no solid block lies along the
 * horizontal line from mob eye to player eye. Step in ~0.5-block
 * increments and short-circuit on first solid. */
static bool has_los(Vec3 from, Vec3 to) {
    float dx = to.x - from.x, dy = to.y - from.y, dz = to.z - from.z;
    float dist = sqrtf(dx*dx + dy*dy + dz*dz);
    if (dist < 0.001f) return true;
    int steps = (int)(dist * 2.0f);   /* ~0.5-block step */
    if (steps < 2) steps = 2;
    if (steps > 64) steps = 64;
    float inv = 1.0f / (float)steps;
    for (int i = 1; i < steps; i++) {
        float t = (float)i * inv;
        int bx = (int)floorf(from.x + dx * t);
        int by = (int)floorf(from.y + dy * t);
        int bz = (int)floorf(from.z + dz * t);
        if (craft_block_solid(craft_world_get(bx, by, bz))) return false;
    }
    return true;
}

/* Skeleton AI: walk toward player but hold at SKEL_KEEP_DIST. Fire
 * arrows on a fixed cooldown when in line of sight. */
static void skeleton_ai(CraftMob *m, CraftPlayer *p, float dt) {
    float dx = p->cam.pos.x - m->pos.x;
    float dz = p->cam.pos.z - m->pos.z;
    float dist = sqrtf(dx * dx + dz * dz);
    if (dist > SKEL_AGGRO_DIST) {
        if (m->ai_timer <= 0.0f) ai_decide(m);
        return;
    }
    float spd = mob_speed[MOB_SKELETON];
    if (dist > 0.001f) m->yaw = atan2f(dx, dz);
    if (dist > SKEL_KEEP_DIST) {
        /* Close in. */
        m->vel.x = (dx / dist) * spd;
        m->vel.z = (dz / dist) * spd;
    } else if (dist < SKEL_BACK_DIST) {
        /* Back off. */
        m->vel.x = -(dx / dist) * spd * 0.6f;
        m->vel.z = -(dz / dist) * spd * 0.6f;
    } else {
        m->vel.x = 0; m->vel.z = 0;
    }
    m->ai_timer = 0.3f;

    /* Fire on cooldown. */
    m->fire_cooldown -= dt;
    if (m->fire_cooldown <= 0.0f) {
        m->fire_cooldown = SKEL_FIRE_GAP;
        Vec3 from = v3(m->pos.x, m->pos.y + 1.25f, m->pos.z);
        Vec3 to   = v3(p->cam.pos.x, p->cam.pos.y - 0.2f, p->cam.pos.z);
        if (has_los(from, to)) {
            /* Aim slightly above the player to compensate for gravity
             * over the flight time. Crude — works fine at < 16 blocks. */
            float tx = to.x - from.x;
            float ty = to.y - from.y;
            float tz = to.z - from.z;
            float td = sqrtf(tx*tx + tz*tz);
            float tof = (td > 0.001f) ? (td / SKEL_ARROW_SPEED) : 0.1f;
            /* drop = 0.5 * g * tof^2, gravity tuned to -8 for arrows */
            float lead_y = 0.5f * 8.0f * tof * tof;
            Vec3 dir = v3(tx, ty + lead_y, tz);
            float dl = sqrtf(dir.x*dir.x + dir.y*dir.y + dir.z*dir.z);
            if (dl > 0.001f) {
                float inv = SKEL_ARROW_SPEED / dl;
                Vec3 vel = v3(dir.x * inv, dir.y * inv, dir.z * inv);
                craft_arrows_spawn(from, vel);
            }
        }
    }
}

/* Spider AI: fast melee chase. Behaves like a faster slime. */
static void spider_ai(CraftMob *m, CraftPlayer *p, float dt) {
    (void)dt;
    float dx = p->cam.pos.x - m->pos.x;
    float dz = p->cam.pos.z - m->pos.z;
    float dist = sqrtf(dx * dx + dz * dz);
    if (dist > SPIDER_AGGRO_DIST) {
        if (m->ai_timer <= 0.0f) ai_decide(m);
        return;
    }
    if (dist > 0.001f) {
        m->yaw = atan2f(dx, dz);
        if (dist > MOB_STANDOFF) {
            float spd = mob_speed[MOB_SPIDER];
            m->vel.x = (dx / dist) * spd;
            m->vel.z = (dz / dist) * spd;
        } else {
            m->vel.x = 0; m->vel.z = 0;
        }
        m->ai_timer = 0.3f;
    }
    if (dist < SPIDER_CONTACT_DIST + 0.10f) {
        craft_player_take_damage(p, 2);
    }
}

/* Creeper AI: chase toward player; on entering fuse range, freeze and
 * tint brighter for CREEPER_FUSE_TIME seconds, then explode. */
static void creeper_ai(CraftMob *m, CraftPlayer *p, float dt) {
    float dx = p->cam.pos.x - m->pos.x;
    float dz = p->cam.pos.z - m->pos.z;
    float dist = sqrtf(dx * dx + dz * dz);

    if (m->fuse_t > 0.0f) {
        /* Already fusing — freeze in place and count down. */
        m->vel.x = 0; m->vel.z = 0;
        m->fuse_t -= dt;
        if (m->fuse_t <= 0.0f) {
            /* Boom — damage, particles, block destruction. */
            Vec3 ctr = v3(m->pos.x, m->pos.y + 0.7f, m->pos.z);
            if (dist < CREEPER_BLAST_DIST) {
                craft_player_take_damage(p, CREEPER_BLAST_DAMAGE);
            }
            craft_particles_emit_explosion(ctr);
            /* Destroy solid blocks in spherical radius. We sweep an
             * AABB around the creeper position and null any cell whose
             * centre is within blast radius. Skip water (lets the
             * explosion punch underwater without draining the pool)
             * and BLK_AIR (already empty). The world_set call drops
             * the mod into the chunk store so destruction persists. */
            int r = (int)(CREEPER_BLAST_DIST + 0.5f);
            int cx = (int)floorf(m->pos.x);
            int cy = (int)floorf(m->pos.y + 0.7f);
            int cz = (int)floorf(m->pos.z);
            for (int dy = -r; dy <= r; dy++) {
                for (int dz = -r; dz <= r; dz++) {
                    for (int dxi = -r; dxi <= r; dxi++) {
                        float fx = (float)dxi + 0.5f;
                        float fy = (float)dy  + 0.5f - 0.7f;  /* centre offset back */
                        float fz = (float)dz  + 0.5f;
                        if (fx*fx + fy*fy + fz*fz > CREEPER_BLAST_DIST * CREEPER_BLAST_DIST)
                            continue;
                        int wx = cx + dxi, wy = cy + dy, wz = cz + dz;
                        BlockId b = craft_world_get(wx, wy, wz);
                        if (b == BLK_AIR || b == BLK_WATER) continue;
                        craft_world_set(wx, wy, wz, BLK_AIR);
                    }
                }
            }
            m->alive = false;
        }
        return;
    }

    if (dist > CREEPER_AGGRO_DIST) {
        if (m->ai_timer <= 0.0f) ai_decide(m);
        return;
    }
    if (dist < CREEPER_FUSE_DIST) {
        m->fuse_t = CREEPER_FUSE_TIME;
        m->vel.x = 0; m->vel.z = 0;
        return;
    }
    if (dist > 0.001f) {
        m->yaw = atan2f(dx, dz);
        float spd = mob_speed[MOB_CREEPER];
        m->vel.x = (dx / dist) * spd;
        m->vel.z = (dz / dist) * spd;
        m->ai_timer = 0.3f;
    }
}

static bool foot_solid(float fx, float fy, float fz) {
    int bx = (int)floorf(fx);
    int by = (int)floorf(fy - 0.05f);
    int bz = (int)floorf(fz);
    return craft_block_solid(craft_world_get(bx, by, bz));
}
static bool ahead_solid(float fx, float fy, float fz) {
    int bx = (int)floorf(fx);
    int by = (int)floorf(fy + 0.4f);
    int bz = (int)floorf(fz);
    return craft_block_solid(craft_world_get(bx, by, bz));
}

void craft_mobs_tick(float dt, CraftPlayer *p) {
    if (dt > 0.1f) dt = 0.1f;
    /* Daytime sunlight burn: hostile mobs caught in direct sun take
     * continuous damage and pulse red. Cheap — single sky_height
     * lookup per mob. */
    bool burning_weather = s_last_sun_y > 0.10f;
    for (int i = 0; i < CRAFT_MAX_MOBS; i++) {
        CraftMob *m = &craft_mobs[i];
        if (!m->alive) continue;
        if (m->hurt_flash > 0.0f) m->hurt_flash -= dt;
        if (burning_weather && mob_is_hostile(m->type)) {
            int hx = (int)m->pos.x;
            int hy = (int)(m->pos.y + s_models[m->type].height * 0.7f);
            int hz = (int)m->pos.z;
            if (craft_world_sky_exposed(hx, hy, hz)) {
                m->burn_acc += dt;
                /* Visual flame puff each tick — particle module
                 * rate-limits internally to 1-2 per call so even at
                 * 30 fps the budget stays sane. */
                Vec3 flame_pos = v3(m->pos.x,
                                    m->pos.y + s_models[m->type].height * 0.4f,
                                    m->pos.z);
                craft_particles_emit_flame(flame_pos);
                /* 1 HP per second of direct sun. */
                if (m->burn_acc >= 1.0f) {
                    m->burn_acc -= 1.0f;
                    m->hp -= 1;
                    m->hurt_flash = 0.30f;
                    if (m->hp <= 0) {
                        m->alive = false;
                        continue;
                    }
                }
            } else {
                m->burn_acc = 0.0f;
            }
        } else {
            m->burn_acc = 0.0f;
        }
        m->ai_timer -= dt;
        switch (m->type) {
            case MOB_SLIME:    slime_ai(m, p, dt); break;
            case MOB_SKELETON: skeleton_ai(m, p, dt); break;
            case MOB_SPIDER:   spider_ai(m, p, dt); break;
            case MOB_CREEPER:  creeper_ai(m, p, dt); break;
            default:
                if (m->ai_timer <= 0.0f) ai_decide(m);
                break;
        }
        /* Creeper just exploded itself — skip remaining physics. */
        if (!m->alive) continue;

        m->vel.y -= 22.0f * dt;
        if (m->vel.y < -16.0f) m->vel.y = -16.0f;

        float nx = m->pos.x + m->vel.x * dt;
        float nz = m->pos.z + m->vel.z * dt;

        /* Despawn when too far from the player (infinite world). */
        float dxp = nx - p->cam.pos.x;
        float dzp = nz - p->cam.pos.z;
        if (dxp * dxp + dzp * dzp > 60.0f * 60.0f) {
            m->alive = false;
            continue;
        }

        if (ahead_solid(nx, m->pos.y, nz)) {
            m->vel.x = 0; m->vel.z = 0;
            m->ai_timer = 0.0f;
        } else {
            m->pos.x = nx; m->pos.z = nz;
        }

        m->pos.y += m->vel.y * dt;
        if (m->pos.y < 1.0f) m->pos.y = 1.0f;
        if (foot_solid(m->pos.x, m->pos.y, m->pos.z)) {
            float gy = floorf(m->pos.y - 0.05f) + 1.0f;
            if (m->pos.y < gy) m->pos.y = gy;
            m->vel.y = 0;
        }
    }
}

/* --- 3D rendering -------------------------------------------------- */

/* Ray vs AABB slab intersection. Returns true if the ray hits the box
 * at positive t. t_out is the entry t, face_out identifies which face:
 *   0 = +X (east), 1 = -X (west),
 *   2 = +Y (top),  3 = -Y (bottom),
 *   4 = +Z (front), 5 = -Z (back).
 */
static inline bool ray_aabb(float ox, float oy, float oz,
                            float dx, float dy, float dz,
                            float bminx, float bminy, float bminz,
                            float bmaxx, float bmaxy, float bmaxz,
                            float *t_out, int *face_out) {
    float t_near = -1e30f, t_far = 1e30f;
    int   nf = -1;

    /* X slab */
    if (dx > -1e-6f && dx < 1e-6f) {
        if (ox < bminx || ox > bmaxx) return false;
    } else {
        float inv = 1.0f / dx;
        float t1  = (bminx - ox) * inv;
        float t2  = (bmaxx - ox) * inv;
        int near_face = (dx > 0) ? 1 : 0;
        if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; near_face ^= 1; }
        if (t1 > t_near) { t_near = t1; nf = near_face; }
        if (t2 < t_far)  t_far  = t2;
        if (t_near > t_far) return false;
    }
    /* Y slab */
    if (dy > -1e-6f && dy < 1e-6f) {
        if (oy < bminy || oy > bmaxy) return false;
    } else {
        float inv = 1.0f / dy;
        float t1  = (bminy - oy) * inv;
        float t2  = (bmaxy - oy) * inv;
        int near_face = (dy > 0) ? 3 : 2;
        if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; near_face ^= 1; }
        if (t1 > t_near) { t_near = t1; nf = near_face; }
        if (t2 < t_far)  t_far  = t2;
        if (t_near > t_far) return false;
    }
    /* Z slab */
    if (dz > -1e-6f && dz < 1e-6f) {
        if (oz < bminz || oz > bmaxz) return false;
    } else {
        float inv = 1.0f / dz;
        float t1  = (bminz - oz) * inv;
        float t2  = (bmaxz - oz) * inv;
        int near_face = (dz > 0) ? 5 : 4;
        if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; near_face ^= 1; }
        if (t1 > t_near) { t_near = t1; nf = near_face; }
        if (t2 < t_far)  t_far  = t2;
        if (t_near > t_far) return false;
    }
    if (t_near < 0.0f) return false;
    *t_out = t_near;
    *face_out = nf;
    return true;
}

/* Face shading — analog to block face shade. */
static const uint16_t mob_face_shade[6] = {
    220, 220, 256, 150, 200, 170
};

static inline uint16_t shade565(uint16_t c, int m) {
    int r = ((c >> 11) & 0x1F) * m >> 8;
    int g = ((c >>  5) & 0x3F) * m >> 8;
    int b = ( c        & 0x1F) * m >> 8;
    if (r > 31) r = 31;
    if (g > 63) g = 63;
    if (b > 31) b = 31;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

void craft_mobs_render(const CraftCamera *cam, uint16_t *fb) {
    float cy_c = cosf(cam->yaw),  sy_c = sinf(cam->yaw);
    float cp_c = cosf(cam->pitch), sp_c = sinf(cam->pitch);
    Vec3 fwd   = v3(sy_c * cp_c, sp_c, cy_c * cp_c);
    Vec3 right = v3(cy_c, 0.0f, -sy_c);
    Vec3 up    = v3(
        fwd.y * right.z - fwd.z * right.y,
        fwd.z * right.x - fwd.x * right.z,
        fwd.x * right.y - fwd.y * right.x);
    float tan_h  = tanf(cam->fov * 0.5f);
    float focal_h = (CRAFT_FB_W * 0.5f) / tan_h;
    float focal_v = (CRAFT_FB_H * 0.5f) / tan_h;

    for (int i = 0; i < CRAFT_MAX_MOBS; i++) {
        CraftMob *m = &craft_mobs[i];
        if (!m->alive) continue;
        const MobModel *model = &s_models[m->type];

        /* Yaw-invariant world AABB for screen-bbox computation. */
        float wbmin_x = m->pos.x - model->radius;
        float wbmin_y = m->pos.y;
        float wbmin_z = m->pos.z - model->radius;
        float wbmax_x = m->pos.x + model->radius;
        float wbmax_y = m->pos.y + model->height;
        float wbmax_z = m->pos.z + model->radius;

        /* Project 8 AABB corners → screen bbox. */
        int sx_min = CRAFT_FB_W, sx_max = -1;
        int sy_min = CRAFT_FB_H, sy_max = -1;
        bool any_in_front = false;
        for (int corner = 0; corner < 8; corner++) {
            float cx = (corner & 1) ? wbmax_x : wbmin_x;
            float cyw = (corner & 2) ? wbmax_y : wbmin_y;
            float cz = (corner & 4) ? wbmax_z : wbmin_z;
            float rx = cx - cam->pos.x;
            float ry = cyw - cam->pos.y;
            float rz = cz - cam->pos.z;
            float zf = rx * fwd.x + ry * fwd.y + rz * fwd.z;
            if (zf <= 0.05f) continue;
            any_in_front = true;
            float xs = (rx * right.x + ry * right.y + rz * right.z) / zf;
            float ys = (rx * up.x    + ry * up.y    + rz * up.z   ) / zf;
            int   sx = (int)(CRAFT_FB_W * 0.5f + xs * focal_h);
            int   sy = (int)(CRAFT_FB_H * 0.5f - ys * focal_v);
            if (sx < sx_min) sx_min = sx;
            if (sx > sx_max) sx_max = sx;
            if (sy < sy_min) sy_min = sy;
            if (sy > sy_max) sy_max = sy;
        }
        if (!any_in_front) continue;
        /* Clip + expand by 1 px for safety. */
        sx_min--; sy_min--; sx_max++; sy_max++;
        if (sx_min < 0)            sx_min = 0;
        if (sy_min < 0)            sy_min = 0;
        if (sx_max >= CRAFT_FB_W)  sx_max = CRAFT_FB_W - 1;
        if (sy_max >= CRAFT_FB_H)  sy_max = CRAFT_FB_H - 1;
        if (sx_min > sx_max || sy_min > sy_max) continue;

        /* Pre-transform camera position into mob-local frame. The mob
         * faces +Z in local space; world yaw rotates that around Y.
         * Inverse rotation: -yaw around Y. */
        float my_c = cosf(-m->yaw), my_s = sinf(-m->yaw);
        float rel_x = cam->pos.x - m->pos.x;
        float rel_y = cam->pos.y - m->pos.y;
        float rel_z = cam->pos.z - m->pos.z;
        float lo_x  = rel_x * my_c - rel_z * my_s;
        float lo_y  = rel_y;
        float lo_z  = rel_x * my_s + rel_z * my_c;

        /* Iterate screen-bbox pixels. */
        for (int sy = sy_min; sy <= sy_max; sy++) {
            float ndc_y = -((float)(sy * 2 - CRAFT_FB_H + 1) / (float)CRAFT_FB_H);
            float vy    = ndc_y * tan_h;
            for (int sx = sx_min; sx <= sx_max; sx++) {
                float ndc_x = ((float)(sx * 2 - CRAFT_FB_W + 1) / (float)CRAFT_FB_W);
                float vx    = ndc_x * tan_h;

                /* World ray dir. */
                float wdx = fwd.x + right.x * vx + up.x * vy;
                float wdy = fwd.y + right.y * vx + up.y * vy;
                float wdz = fwd.z + right.z * vx + up.z * vy;
                /* Rotate into local frame. */
                float ldx = wdx * my_c - wdz * my_s;
                float ldy = wdy;
                float ldz = wdx * my_s + wdz * my_c;

                /* Test against every cuboid part — keep nearest hit. */
                float best_t = 1e30f;
                int   best_face = 0;
                uint16_t best_color = 0;
                for (int p = 0; p < model->n_parts; p++) {
                    const CuboidPart *part = &model->parts[p];
                    float bminx = part->cx - part->hx;
                    float bmaxx = part->cx + part->hx;
                    float bminy = part->cy - part->hy;
                    float bmaxy = part->cy + part->hy;
                    float bminz = part->cz - part->hz;
                    float bmaxz = part->cz + part->hz;
                    float t; int face;
                    if (ray_aabb(lo_x, lo_y, lo_z, ldx, ldy, ldz,
                                 bminx, bminy, bminz,
                                 bmaxx, bmaxy, bmaxz, &t, &face)) {
                        if (t < best_t) {
                            best_t = t;
                            best_face = face;
                            best_color = part->color;
                        }
                    }
                }
                if (best_t >= 1e29f) continue;

                /* Hurt-flash tint — momentary red wash. */
                if (m->hurt_flash > 0.0f) {
                    int r = ((best_color >> 11) & 0x1F);
                    int g = ((best_color >>  5) & 0x3F);
                    int b = ( best_color        & 0x1F);
                    r = (r + 31) / 2;
                    g = g / 3;
                    b = b / 3;
                    best_color = (uint16_t)((r << 11) | (g << 5) | b);
                }
                /* Creeper fuse tint — lerp toward white as fuse_t
                 * winds down from CREEPER_FUSE_TIME to 0. */
                if (m->type == MOB_CREEPER && m->fuse_t > 0.0f) {
                    float k = 1.0f - (m->fuse_t / CREEPER_FUSE_TIME);
                    if (k < 0.0f) k = 0.0f;
                    if (k > 1.0f) k = 1.0f;
                    int kk = (int)(k * 256.0f);
                    int r = ((best_color >> 11) & 0x1F);
                    int g = ((best_color >>  5) & 0x3F);
                    int b = ( best_color        & 0x1F);
                    r = r + ((31 - r) * kk >> 8);
                    g = g + ((63 - g) * kk >> 8);
                    b = b + ((31 - b) * kk >> 8);
                    best_color = (uint16_t)((r << 11) | (g << 5) | b);
                }

                /* World distance = t * |world_dir| (rotation preserves
                 * length, so |local_dir| = |world_dir|). */
                float dl2 = wdx*wdx + wdy*wdy + wdz*wdz;
                float dl  = (dl2 > 1.0001f) ? sqrtf(dl2) : 1.0f;
                float world_dist = best_t * dl;
                int q = (int)(world_dist * 255.0f / CRAFT_MAX_DIST_FOR_ZBUF);
                if (q < 0)   q = 0;
                if (q > 254) q = 254;
                int idx = sy * CRAFT_FB_W + sx;
                if (craft_zbuf[idx] > (uint8_t)q) {
                    fb[idx] = shade565(best_color, mob_face_shade[best_face]);
                    craft_zbuf[idx] = (uint8_t)q;
                }
            }
        }
    }
}

/* --- Arrow projectile system ------------------------------------- */

void craft_arrows_clear(void) {
    for (int i = 0; i < CRAFT_MAX_ARROWS; i++) craft_arrows[i].alive = false;
}

void craft_arrows_spawn(Vec3 pos, Vec3 vel) {
    for (int i = 0; i < CRAFT_MAX_ARROWS; i++) {
        if (craft_arrows[i].alive) continue;
        craft_arrows[i].alive    = true;
        craft_arrows[i].pos      = pos;
        craft_arrows[i].vel      = vel;
        craft_arrows[i].lifetime = 3.0f;
        return;
    }
}

#define ARROW_GRAVITY  -8.0f
#define ARROW_HIT_R     0.45f      /* player hit radius squared check */

void craft_arrows_tick(float dt, CraftPlayer *p) {
    if (dt > 0.1f) dt = 0.1f;
    for (int i = 0; i < CRAFT_MAX_ARROWS; i++) {
        CraftArrow *a = &craft_arrows[i];
        if (!a->alive) continue;
        a->lifetime -= dt;
        if (a->lifetime <= 0.0f) { a->alive = false; continue; }
        a->vel.y += ARROW_GRAVITY * dt;
        a->pos.x += a->vel.x * dt;
        a->pos.y += a->vel.y * dt;
        a->pos.z += a->vel.z * dt;
        /* World block hit. */
        int bx = (int)floorf(a->pos.x);
        int by = (int)floorf(a->pos.y);
        int bz = (int)floorf(a->pos.z);
        if (craft_block_solid(craft_world_get(bx, by, bz))) {
            a->alive = false;
            continue;
        }
        /* Player hit (sphere vs player body). Player feet at
         * cam.pos.y - PLAYER_EYE — approximate centre as cam.pos with
         * a generous 0.6 m radius vertically. */
        float dx = a->pos.x - p->cam.pos.x;
        float dy = a->pos.y - (p->cam.pos.y - 0.8f);
        float dz = a->pos.z - p->cam.pos.z;
        if (dx*dx + dz*dz < ARROW_HIT_R && dy > -1.2f && dy < 0.6f) {
            craft_player_take_damage(p, 1);
            a->alive = false;
            continue;
        }
    }
}

/* Project arrow centre to screen, paint a short dark cuboid trail
 * aligned to velocity. Cheap — at most CRAFT_MAX_ARROWS=16 sprites
 * and each draws a thin line of pixels. */
void craft_arrows_render(const CraftCamera *cam, uint16_t *fb) {
    for (int i = 0; i < CRAFT_MAX_ARROWS; i++) {
        const CraftArrow *a = &craft_arrows[i];
        if (!a->alive) continue;
        /* Tail point is one segment back along velocity. */
        float vlen = sqrtf(a->vel.x*a->vel.x + a->vel.y*a->vel.y + a->vel.z*a->vel.z);
        float t = (vlen > 0.001f) ? (0.35f / vlen) : 0.0f;
        Vec3 tail = v3(a->pos.x - a->vel.x * t,
                       a->pos.y - a->vel.y * t,
                       a->pos.z - a->vel.z * t);
        int sx0, sy0, sx1, sy1;
        uint8_t d0, d1;
        float dist0, dist1;
        if (!craft_render_project(cam, a->pos, &sx1, &sy1, &d1, &dist1)) continue;
        if (!craft_render_project(cam, tail,   &sx0, &sy0, &d0, &dist0)) {
            sx0 = sx1; sy0 = sy1; d0 = d1;
        }
        if (dist1 > CRAFT_MAX_DIST_FOR_ZBUF) continue;
        /* Bresenham — z-tested against world. */
        int dx =  (sx1 > sx0) ? (sx1 - sx0) : (sx0 - sx1);
        int dy = -((sy1 > sy0) ? (sy1 - sy0) : (sy0 - sy1));
        int stp_x = (sx0 < sx1) ? 1 : -1;
        int stp_y = (sy0 < sy1) ? 1 : -1;
        int err = dx + dy;
        int steps = (dx > -dy) ? dx : -dy;
        if (steps < 1) steps = 1;
        uint16_t shaft = rgb565(60, 45, 30);
        uint16_t tipc  = rgb565(220, 220, 220);
        int step_i = 0;
        for (;;) {
            if ((unsigned)sx0 < CRAFT_FB_W && (unsigned)sy0 < CRAFT_FB_H) {
                int idx = sy0 * CRAFT_FB_W + sx0;
                /* Interpolate depth from d0 to d1. */
                int depth = d0 + (d1 - d0) * step_i / steps;
                if (depth < 0)   depth = 0;
                if (depth > 254) depth = 254;
                if (craft_zbuf[idx] > (uint8_t)depth) {
                    fb[idx] = (step_i == steps) ? tipc : shaft;
                    craft_zbuf[idx] = (uint8_t)depth;
                }
            }
            if (sx0 == sx1 && sy0 == sy1) break;
            int e2 = 2 * err;
            if (e2 >= dy) { err += dy; sx0 += stp_x; }
            if (e2 <= dx) { err += dx; sy0 += stp_y; }
            step_i++;
            if (step_i > 32) break;  /* safety cap */
        }
    }
}

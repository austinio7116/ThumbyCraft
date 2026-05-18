/*
 * ThumbyCraft — held-item cuboid models.
 *
 * Each model is rebuilt from the canonical Minecraft 2D inventory
 * icon, with one cuboid per icon "pixel cluster" so the silhouette
 * reads correctly from the held-item viewport's idle pose
 * (yaw +26°, pitch -20° — see craft_render_held_item).
 *
 * Local frame: +Y up, +X right, model front faces -Z. Total bounding
 * extent stays inside ~0.5 m so the model fits the virtual frustum.
 *
 * Diagonals (pickaxe handle, sword blade, arrow shaft) are
 * approximated as a staircase of small cuboids stepped along the
 * X+Y direction since the held-item renderer doesn't rotate parts.
 *
 * All part tables are `static const` so they live in flash, not
 * SRAM. Total BSS cost of this module is zero.
 */
#include "craft_tool_models.h"

/* Compile-time RGB565 pack — rgb565() in craft_types.h does runtime
 * clamps and so can't be a constant initialiser. Inputs are
 * literal 0..255 channels; we just truncate to 5/6/5 bits. */
#define C565(r, g, b)  \
    ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

/* ---- Palette --------------------------------------------------- */

/* Pickaxe head tints (light face + darker face / shadow). */
#define COL_PICK_WOOD_LIGHT   C565(160, 110,  60)
#define COL_PICK_WOOD_DARK    C565(115,  80,  40)
#define COL_PICK_STONE_LIGHT  C565(140, 140, 150)
#define COL_PICK_STONE_DARK   C565( 95,  95, 105)
#define COL_PICK_IRON_LIGHT   C565(230, 230, 240)
#define COL_PICK_IRON_DARK    C565(170, 170, 180)

/* Tool handle / haft / grip — always wood, tier-independent. */
#define COL_HAFT_LIGHT        C565(140,  95,  45)
#define COL_HAFT_DARK         C565(110,  70,  30)

/* Sword blades reuse the pickaxe palette so tier reads are consistent. */
#define COL_BLADE_WOOD_LIGHT  COL_PICK_WOOD_LIGHT
#define COL_BLADE_WOOD_DARK   COL_PICK_WOOD_DARK
#define COL_BLADE_STONE_LIGHT COL_PICK_STONE_LIGHT
#define COL_BLADE_STONE_DARK  COL_PICK_STONE_DARK
#define COL_BLADE_IRON_LIGHT  COL_PICK_IRON_LIGHT
#define COL_BLADE_IRON_DARK   COL_PICK_IRON_DARK

/* Sword cross-guard / pommel — small dark detail. */
#define COL_GUARD             C565( 95,  65,  25)
#define COL_POMMEL            C565( 70,  45,  15)

/* Bow + arrow accents. */
#define COL_BOW_LIGHT         C565(140,  95,  45)
#define COL_BOW_DARK          C565(100,  65,  30)
#define COL_STRING            C565(220, 220, 230)
#define COL_SHAFT             C565(140,  95,  50)
#define COL_FLETCH            C565(235, 235, 235)
#define COL_FLETCH_ACCENT     C565(210,  55,  55)
#define COL_TIP               C565( 70,  70,  80)

/* ---- Pickaxe (3 tiers) ---------------------------------------- *
 *
 * Canonical MC pickaxe icon: a 3-5-3 vertical "diamond" head at the
 * upper-right, with a one-pixel-thick handle running diagonally
 * down to the lower-left.
 *
 *     . . . . X X X . .   top row    (3 wide)
 *     . . . X X X X X .   mid row    (5 wide)
 *     . . . . X X X . .   bot row    (3 wide)
 *     . . . X . . . . .   handle starts
 *     . . X . . . . . .
 *     . X . . . . . . .
 *     X . . . . . . . .
 *
 * Head is 3 stacked horizontal cuboids; handle is a 7-step staircase
 * along the -X -Y diagonal toward the lower-left of the bounding
 * box. We tag the top of the head a slightly darker tint so the
 * tilted idle view picks up a fake "shadow" along the top face. */

#define PICKAXE_HEAD_PARTS(LIGHT, DARK)                              \
    /* top row, 3 px wide */                                         \
    {  0.140f,  0.225f,  0.000f,                                     \
       0.045f,  0.025f,  0.030f, (DARK)  },                          \
    /* mid row, 5 px wide — sticks out left + right of the top */    \
    {  0.140f,  0.175f,  0.000f,                                     \
       0.075f,  0.025f,  0.030f, (LIGHT) },                          \
    /* bottom row, 3 px wide */                                      \
    {  0.140f,  0.125f,  0.000f,                                     \
       0.045f,  0.025f,  0.030f, (LIGHT) }

/* 7-step handle staircase from (0.07, 0.085) to (-0.215, -0.215).
 * Each cube ~0.025 in X/Y so neighbours overlap slightly and the
 * silhouette reads as a smooth diagonal. */
#define PICKAXE_HANDLE_PARTS                                         \
    {  0.070f,  0.085f,  0.000f, 0.028f, 0.028f, 0.022f, COL_HAFT_LIGHT }, \
    {  0.020f,  0.035f,  0.000f, 0.028f, 0.028f, 0.022f, COL_HAFT_LIGHT }, \
    { -0.030f, -0.015f,  0.000f, 0.028f, 0.028f, 0.022f, COL_HAFT_LIGHT }, \
    { -0.080f, -0.065f,  0.000f, 0.028f, 0.028f, 0.022f, COL_HAFT_DARK  }, \
    { -0.125f, -0.110f,  0.000f, 0.028f, 0.028f, 0.022f, COL_HAFT_DARK  }, \
    { -0.170f, -0.160f,  0.000f, 0.028f, 0.028f, 0.022f, COL_HAFT_DARK  }, \
    { -0.215f, -0.215f,  0.000f, 0.028f, 0.028f, 0.022f, COL_HAFT_DARK  }

static const CraftToolPart parts_pick_wood[] = {
    PICKAXE_HEAD_PARTS(COL_PICK_WOOD_LIGHT, COL_PICK_WOOD_DARK),
    PICKAXE_HANDLE_PARTS,
};
static const CraftToolPart parts_pick_stone[] = {
    PICKAXE_HEAD_PARTS(COL_PICK_STONE_LIGHT, COL_PICK_STONE_DARK),
    PICKAXE_HANDLE_PARTS,
};
static const CraftToolPart parts_pick_iron[] = {
    PICKAXE_HEAD_PARTS(COL_PICK_IRON_LIGHT, COL_PICK_IRON_DARK),
    PICKAXE_HANDLE_PARTS,
};

/* ---- Sword (3 tiers) ------------------------------------------ *
 *
 * Canonical MC sword icon: a long thin blade running diagonally
 * from lower-left to upper-right, a short cross-guard kicking out
 * perpendicular to the blade at the grip end, then a short grip
 * and a tiny pommel.
 *
 *     . . . . . . . . X .   tip
 *     . . . . . . . X . .
 *     . . . . . . X . . .
 *     . . . . . X . . . .   blade staircase
 *     . . . . X . . . . .
 *     . . . X . . . . . .
 *     . . X . . . . . . .
 *     . X X X . . . . . .   cross-guard (3 px wide perpendicular)
 *     . . X . . . . . . .   grip
 *     . . X . . . . . . .
 *     . . X . . . . . . .   pommel (darker)
 *
 * Blade is a 6-step staircase + a slightly smaller pointed tip
 * cuboid at the top-right. */

#define SWORD_BLADE_PARTS(LIGHT, DARK)                                       \
    /* 6 staircase segments from cross-guard to tip */                       \
    { -0.050f, -0.030f, 0.000f, 0.028f, 0.028f, 0.018f, (LIGHT) },           \
    { -0.005f,  0.015f, 0.000f, 0.028f, 0.028f, 0.018f, (LIGHT) },           \
    {  0.040f,  0.060f, 0.000f, 0.028f, 0.028f, 0.018f, (DARK)  },           \
    {  0.085f,  0.105f, 0.000f, 0.028f, 0.028f, 0.018f, (LIGHT) },           \
    {  0.130f,  0.150f, 0.000f, 0.028f, 0.028f, 0.018f, (DARK)  },           \
    {  0.175f,  0.195f, 0.000f, 0.028f, 0.028f, 0.018f, (LIGHT) },           \
    /* pointed tip — slightly smaller, in the dark tint so the         */    \
    /* highlight on the blade really pops                              */    \
    {  0.215f,  0.235f, 0.000f, 0.020f, 0.020f, 0.014f, (DARK)  }

/* Cross-guard runs perpendicular to the blade — one block on the
 * blade axis plus two flanking blocks offset along the
 * perpendicular (-X+Y / +X-Y) so it kicks out either side. */
#define SWORD_GUARD_PARTS                                                    \
    /* centre of cross-guard, sitting on the blade base */                   \
    { -0.090f, -0.070f, 0.000f, 0.028f, 0.028f, 0.022f, COL_GUARD  },        \
    /* upper-left flange (perpendicular: -along-blade direction) */          \
    { -0.130f, -0.030f, 0.000f, 0.028f, 0.028f, 0.022f, COL_GUARD  },        \
    /* lower-right flange */                                                 \
    { -0.050f, -0.110f, 0.000f, 0.028f, 0.028f, 0.022f, COL_GUARD  }

/* Grip + pommel below the cross-guard, continuing the diagonal. */
#define SWORD_GRIP_PARTS                                                     \
    { -0.130f, -0.115f, 0.000f, 0.024f, 0.024f, 0.022f, COL_HAFT_LIGHT },    \
    { -0.170f, -0.155f, 0.000f, 0.024f, 0.024f, 0.022f, COL_HAFT_DARK  },    \
    { -0.210f, -0.195f, 0.000f, 0.020f, 0.020f, 0.018f, COL_POMMEL     }

static const CraftToolPart parts_sword_wood[] = {
    SWORD_BLADE_PARTS(COL_BLADE_WOOD_LIGHT, COL_BLADE_WOOD_DARK),
    SWORD_GUARD_PARTS,
    SWORD_GRIP_PARTS,
};
static const CraftToolPart parts_sword_stone[] = {
    SWORD_BLADE_PARTS(COL_BLADE_STONE_LIGHT, COL_BLADE_STONE_DARK),
    SWORD_GUARD_PARTS,
    SWORD_GRIP_PARTS,
};
static const CraftToolPart parts_sword_iron[] = {
    SWORD_BLADE_PARTS(COL_BLADE_IRON_LIGHT, COL_BLADE_IRON_DARK),
    SWORD_GUARD_PARTS,
    SWORD_GRIP_PARTS,
};

/* ---- Bow ------------------------------------------------------ *
 *
 * Canonical MC bow icon: a "C" shape opening rightward (toward the
 * viewer's draw arm) with the limbs curving in to small tips at
 * the upper-left and lower-left of the icon, plus a thin vertical
 * string crossing between the two tips on the right side of the
 * body.
 *
 *     . . . . X X . . .   top tip (tucked back to the right)
 *     . . . X . . . . .
 *     . . X . . . . . .
 *     . X . . . . . . .
 *     . X . . . . . . .   left limb (vertical centre)
 *     . X . . . . . . .   string vertical at the right side
 *     . X . . . . . . .
 *     . X . . . . . . .
 *     . X . . . . . . .
 *     . . X . . . . . .
 *     . . . X . . . . .
 *     . . . . X X . . .   bottom tip
 *
 * 5 wood cuboids (top tip, top arch, vertical centre, bottom
 * arch, bottom tip) + 2 string cuboids stacked vertically on the
 * right of the body. */

static const CraftToolPart parts_bow[] = {
    /* top tip — short horizontal cube at upper-right of bow body */
    {  0.020f,  0.225f,  0.000f, 0.035f, 0.022f, 0.025f, COL_BOW_DARK  },
    /* upper arch — diagonal stepping down-left from tip toward body */
    { -0.060f,  0.140f,  0.000f, 0.025f, 0.045f, 0.025f, COL_BOW_LIGHT },
    /* vertical centre — tall thin cube forming the bow's belly */
    { -0.105f,  0.000f,  0.000f, 0.022f, 0.110f, 0.025f, COL_BOW_LIGHT },
    /* lower arch */
    { -0.060f, -0.140f,  0.000f, 0.025f, 0.045f, 0.025f, COL_BOW_LIGHT },
    /* bottom tip */
    {  0.020f, -0.225f,  0.000f, 0.035f, 0.022f, 0.025f, COL_BOW_DARK  },
    /* string — two stacked thin vertical cuboids inside the curve.
     * Placed slightly forward in Z so the string sits in front of
     * the wood when the model tilts. */
    {  0.020f,  0.100f, -0.008f, 0.005f, 0.115f, 0.005f, COL_STRING    },
    {  0.020f, -0.100f, -0.008f, 0.005f, 0.115f, 0.005f, COL_STRING    },
};

/* ---- Arrow ---------------------------------------------------- *
 *
 * Canonical MC arrow icon: a diagonal shaft from upper-right
 * (tip) to lower-left (fletching). Tip is a dark grey wedge, the
 * shaft is brown, and the fletching is white with a small red
 * accent.
 *
 *     . . . . . . . X X .   tip
 *     . . . . . . X X . .
 *     . . . . . X X . . .   shaft staircase
 *     . . . . X . . . . .
 *     . . . X . . . . . .
 *     . . X . . . . . . .
 *     . X X . . . . . . .   fletching (white + red accent)
 *     X X . . . . . . . . */

static const CraftToolPart parts_arrow[] = {
    /* dark tip — two small cubes stacked diagonally at the top-right */
    {  0.220f,  0.225f, 0.000f, 0.022f, 0.022f, 0.018f, COL_TIP    },
    {  0.180f,  0.185f, 0.000f, 0.022f, 0.022f, 0.018f, COL_TIP    },
    /* shaft staircase — 4 brown segments stepping down-left */
    {  0.130f,  0.135f, 0.000f, 0.025f, 0.025f, 0.016f, COL_SHAFT  },
    {  0.075f,  0.080f, 0.000f, 0.025f, 0.025f, 0.016f, COL_SHAFT  },
    {  0.020f,  0.025f, 0.000f, 0.025f, 0.025f, 0.016f, COL_SHAFT  },
    { -0.035f, -0.030f, 0.000f, 0.025f, 0.025f, 0.016f, COL_SHAFT  },
    /* fletching — white V at the lower-left, with one red accent
     * cube tucked between the two flights so the colour pops on
     * the tilted idle view. */
    { -0.090f, -0.085f, 0.000f, 0.028f, 0.028f, 0.020f, COL_FLETCH         },
    { -0.135f, -0.135f, 0.000f, 0.030f, 0.030f, 0.022f, COL_FLETCH         },
    { -0.105f, -0.130f, 0.000f, 0.014f, 0.014f, 0.014f, COL_FLETCH_ACCENT  },
    { -0.180f, -0.185f, 0.000f, 0.028f, 0.028f, 0.020f, COL_FLETCH         },
};

/* Torch — vertical wooden stick with a glowing flame cluster on top.
 * BLK_TORCH is in the "placeable" allow-list so without a tool model
 * the held-item path renders it as a textured cube; that reads as a
 * brown block instead of a torch. The model puts a tall thin stick
 * along Y with a small flame stack at the top. */
#define COL_TORCH_WOOD  0x9341u   /* dark wood brown */
#define COL_TORCH_HEAD  0xFE60u   /* orange-yellow flame */
#define COL_TORCH_TIP   0xFFE0u   /* hot yellow tip */
static const CraftToolPart parts_torch[] = {
    /* Stick — tall thin column running through the centre. */
    {  0.000f, -0.150f, 0.000f, 0.025f, 0.100f, 0.025f, COL_TORCH_WOOD },
    /* Flame stack — two small bright cubes above the stick. */
    {  0.000f,  0.000f, 0.000f, 0.055f, 0.040f, 0.055f, COL_TORCH_HEAD },
    {  0.000f,  0.060f, 0.000f, 0.040f, 0.025f, 0.040f, COL_TORCH_TIP  },
};

#define MODEL(arr)  { sizeof(arr) / sizeof((arr)[0]), (arr) }

void craft_tool_models_init(void) {
    /* No-op today — kept as an explicit init step in case future
     * tool variants need runtime setup. */
}

/* BLK_BOW / BLK_ARROW are referenced by their integer values (22,
 * 23) per the original spec for defensive merge-safety against
 * parallel work that introduced those names. Returns an empty model
 * (n_parts == 0) for ids with no held-view model — the caller
 * (craft_render_held_item) early-outs on that. */
CraftToolModel craft_tool_model(BlockId b) {
    switch ((int)b) {
        case BLK_PICKAXE_WOOD:  return (CraftToolModel)MODEL(parts_pick_wood);
        case BLK_PICKAXE_STONE: return (CraftToolModel)MODEL(parts_pick_stone);
        case BLK_PICKAXE_IRON:  return (CraftToolModel)MODEL(parts_pick_iron);
        case BLK_SWORD_WOOD:    return (CraftToolModel)MODEL(parts_sword_wood);
        case BLK_SWORD_STONE:   return (CraftToolModel)MODEL(parts_sword_stone);
        case BLK_SWORD_IRON:    return (CraftToolModel)MODEL(parts_sword_iron);
        case 22:                return (CraftToolModel)MODEL(parts_bow);   /* BLK_BOW   */
        case 23:                return (CraftToolModel)MODEL(parts_arrow); /* BLK_ARROW */
        case BLK_TORCH:         return (CraftToolModel)MODEL(parts_torch);
        default: {
            CraftToolModel empty = { 0, (const CraftToolPart *)0 };
            return empty;
        }
    }
}

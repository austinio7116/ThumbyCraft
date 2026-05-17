/*
 * ThumbyCraft — held-item cuboid models.
 *
 * Six tools + a bow + an arrow. Each model is a small array of
 * axis-aligned coloured boxes in local space; the held-item renderer
 * walks them per pixel inside a fixed bottom-right viewport, just
 * like craft_mobs_render walks mob parts inside a projected screen
 * bbox.
 *
 * Diagonals (pickaxe handle, blade) are approximated by short stacks
 * of small cubes stepped along the X+Y direction — the renderer can't
 * rotate parts so visual diagonals come from staircase silhouettes.
 *
 * Storage: all part tables are `static const` so they live in flash,
 * not SRAM. The whole module costs ~0 BSS (just the dispatch tables).
 */
#include "craft_tool_models.h"

/* Compile-time RGB565 pack — rgb565() in craft_types.h does runtime
 * clamps and so can't be a constant initialiser. Inputs are
 * literal 0..255 channels; we just truncate to 5/6/5 bits. */
#define C565(r, g, b)  \
    ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

/* Pre-baked colour constants used across the tool table. */
#define COL_HANDLE_WOOD  C565(110, 70, 35)
#define COL_GUARD_GOLD   C565(180, 160, 80)
#define COL_PICK_WOOD    C565(160, 110, 60)
#define COL_PICK_STONE   C565(140, 140, 140)
#define COL_PICK_IRON    C565(220, 220, 230)
#define COL_BLADE_WOOD   C565(160, 110, 60)
#define COL_BLADE_STONE  C565(140, 140, 140)
#define COL_BLADE_IRON   C565(220, 220, 230)
#define COL_STRING       C565(235, 235, 220)
#define COL_SHAFT        C565(140, 95, 50)
#define COL_FLIGHT       C565(230, 60, 60)
#define COL_TIP          C565(80, 80, 90)

/* --- Pickaxe (head colour varies by tier) ---------------------- *
 * Handle: 5 short brown cubes stepped diagonally from lower-left
 * (-x, -y) to upper-right (+x, +y) so the silhouette reads as a
 * tilted rod despite each part being axis-aligned.
 * Head: chevron / wedge at the top of the handle — central block
 * over the top of the rod, plus two narrower wings angled outward
 * so the silhouette reads as a pickaxe head pointing both ways. */
#define PICKAXE_HANDLE_PARTS                                         \
    { -0.16f, -0.20f, 0.00f, 0.030f, 0.040f, 0.030f, COL_HANDLE_WOOD }, \
    { -0.08f, -0.10f, 0.00f, 0.030f, 0.040f, 0.030f, COL_HANDLE_WOOD }, \
    {  0.00f,  0.00f, 0.00f, 0.030f, 0.040f, 0.030f, COL_HANDLE_WOOD }, \
    {  0.08f,  0.10f, 0.00f, 0.030f, 0.040f, 0.030f, COL_HANDLE_WOOD }, \
    {  0.16f,  0.20f, 0.00f, 0.030f, 0.040f, 0.030f, COL_HANDLE_WOOD }

#define PICKAXE_HEAD_PARTS(HEAD_COL)                                 \
    {  0.16f,  0.25f, 0.00f, 0.045f, 0.025f, 0.040f, (HEAD_COL) },   \
    {  0.06f,  0.30f, 0.00f, 0.055f, 0.022f, 0.040f, (HEAD_COL) },   \
    {  0.26f,  0.30f, 0.00f, 0.055f, 0.022f, 0.040f, (HEAD_COL) }

static const CraftToolPart parts_pick_wood[] = {
    PICKAXE_HANDLE_PARTS,
    PICKAXE_HEAD_PARTS(COL_PICK_WOOD),
};
static const CraftToolPart parts_pick_stone[] = {
    PICKAXE_HANDLE_PARTS,
    PICKAXE_HEAD_PARTS(COL_PICK_STONE),
};
static const CraftToolPart parts_pick_iron[] = {
    PICKAXE_HANDLE_PARTS,
    PICKAXE_HEAD_PARTS(COL_PICK_IRON),
};

/* --- Sword (blade colour varies by tier) ----------------------- *
 * Grip at bottom-left, cross-guard above, blade in 3 cuboids
 * stacked along the diagonal toward upper-right so the silhouette
 * reads as a single long, tilted blade. */
#define SWORD_BODY_PARTS                                             \
    { -0.18f, -0.22f, 0.00f, 0.030f, 0.060f, 0.030f, COL_HANDLE_WOOD }, \
    { -0.13f, -0.13f, 0.00f, 0.085f, 0.020f, 0.030f, COL_GUARD_GOLD }

#define SWORD_BLADE_PARTS(BLADE_COL)                                 \
    { -0.05f,  0.00f, 0.00f, 0.035f, 0.060f, 0.020f, (BLADE_COL) },  \
    {  0.05f,  0.12f, 0.00f, 0.035f, 0.060f, 0.020f, (BLADE_COL) },  \
    {  0.15f,  0.24f, 0.00f, 0.035f, 0.060f, 0.020f, (BLADE_COL) }

static const CraftToolPart parts_sword_wood[] = {
    SWORD_BODY_PARTS,
    SWORD_BLADE_PARTS(COL_BLADE_WOOD),
};
static const CraftToolPart parts_sword_stone[] = {
    SWORD_BODY_PARTS,
    SWORD_BLADE_PARTS(COL_BLADE_STONE),
};
static const CraftToolPart parts_sword_iron[] = {
    SWORD_BODY_PARTS,
    SWORD_BLADE_PARTS(COL_BLADE_IRON),
};

/* --- Bow + arrow ----------------------------------------------- *
 * Bow arch opens to the left, like a horizontal C: tips on the
 * upper-left and lower-left, deepest point on the right. String is
 * a single thin vertical bar joining the two tips. */
static const CraftToolPart parts_bow[] = {
    {  0.05f,  0.22f, 0.00f, 0.025f, 0.030f, 0.030f, COL_HANDLE_WOOD }, /* top tip */
    {  0.12f,  0.12f, 0.00f, 0.025f, 0.045f, 0.030f, COL_HANDLE_WOOD }, /* upper arch */
    {  0.16f,  0.00f, 0.00f, 0.025f, 0.060f, 0.030f, COL_HANDLE_WOOD }, /* middle */
    {  0.12f, -0.12f, 0.00f, 0.025f, 0.045f, 0.030f, COL_HANDLE_WOOD }, /* lower arch */
    {  0.05f, -0.22f, 0.00f, 0.025f, 0.030f, 0.030f, COL_HANDLE_WOOD }, /* bottom tip */
    {  0.04f,  0.00f, 0.00f, 0.006f, 0.220f, 0.006f, COL_STRING },      /* bowstring */
};

/* Arrow — shaft along X, flight at -X end (red feathers), tip at
 * +X end (dark grey arrowhead). */
static const CraftToolPart parts_arrow[] = {
    {  0.00f,  0.00f, 0.00f, 0.190f, 0.013f, 0.013f, COL_SHAFT  },
    { -0.20f,  0.00f, 0.00f, 0.030f, 0.035f, 0.025f, COL_FLIGHT },
    {  0.21f,  0.00f, 0.00f, 0.020f, 0.015f, 0.015f, COL_TIP    },
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
        default: {
            CraftToolModel empty = { 0, (const CraftToolPart *)0 };
            return empty;
        }
    }
}

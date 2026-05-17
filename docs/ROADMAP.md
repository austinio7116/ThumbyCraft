# ThumbyCraft v2 roadmap

What "complete game" means at this scale, sequenced by ROI. Each
phase is shippable on its own — pick what you actually want.

Status legend: ✅ shipped, 🟡 in-flight, ⬜ planned.

## Foundations (v1)

| Phase | Description | Status |
|---|---|---|
| 0 | Repo scaffold + drivers | ✅ |
| 1 | Host SDL2 raycaster | ✅ |
| 2 | Device raycaster, dual-core | ✅ |
| 3 | Place / break + hotbar | ✅ |
| 4 | Terrain generation | ✅ |
| 5 | Player physics | ✅ |
| 6 | Flash saves | ✅ |
| 7 | Polish — fog, shading, audio | ✅ |
| 8 | ThumbyOne slot scaffolding | ✅ |
| 9 | Control revamp (v2 scheme + auto-step-up) | ✅ |
| 10 | Performance pass (SRAM renderer + per-column basis + skip-normalize) | ✅ |
| 9b | Control revamp v3 (no strafe, LB look, RB jump, MENU = pause) | ✅ |
| 11 | Day/night cycle (4-min cycle, sky colour shift, brightness scale, stars) | ✅ |
| 18 | Pause menu (Resume / Inventory / Save / Load / Toggle fly / Invert Y / New world / Settings) | ✅ |
| 22 | Toast notifications ("World saved" / "Fly mode ON" / etc) | ✅ |
| 17 | Inventory grid (4×3 picker, A assigns to active hotbar slot) | ✅ |
| 12 | Sun + moon billboard (proper 3D projection, tracks day/night) | ✅ |
| 26 | Z-buffer (16 KB uint8 quantised, populated by render_strip) | ✅ |
| 27 | Passive mobs (sheep / pig / chicken, random walk, 3D cuboid raycaster) | ✅ |
| 34 | Procedural ambient music (C-major pentatonic, I-vi-IV-V chord prog, ADSR, ducking on SFX) | ✅ |
| —  | Music v2 — sine waves, octave-doubled pad, exp ADSR, comb-delay reverb, slower pace | ✅ |
| —  | World-fixed starfield (96 stars, celestial sphere projection, day/night fade) | ✅ |
| 15 | Break particles (8 per break, ballistic, fade, z-tested) | ✅ |
| 16 | Pick outline (wireframe cube of targeted block) | ✅ |
| 20 | Material-aware footsteps (grass/stone/sand/wood/leaves/glass tones) | ✅ |
| 29 | Game modes — Creative / Survival, HP, inventory, slime hostile mob | ✅ |
| 14 | Water animation (per-frame stripe + jitter shift) | ✅ |
| 28 | Day/night hostile spawning (slimes spawn at night, despawn at noon) | ✅ |
| —  | Mob HP + player attack (A hits mob if in attack range, else breaks block) | ✅ |
| —  | Death + respawn (3 s timer, full HP/hunger reset, back to spawn point) | ✅ |
| 30 | Hunger system (decay over time, gates regen, apples drop from leaves auto-eaten) | ✅ |

## Visual & feel (the next thing to ship)

| Phase | Description | Effort | Why |
|---|---|---|---|
| 11 | **Day/night cycle** | S | Sun rotates over 4 min; sky + fog shift colour; stars at night; ambient brightness scales per-face. Massive feel upgrade for ~100 LOC. |
| 12 | **Sun + moon billboard** | XS | Visible disc tracking the day-cycle angle. ~80 LOC. |
| 13 | **Block ambient occlusion** | S | Darker at convex corners — cheap per-face sample of neighbours. ~150 LOC, no extra memory if computed on-the-fly. |
| 14 | **Water animation + waves** | XS | Cycle 4 stripe offsets per second; vertex-displace water surface in raycaster by sin(world_t + x×0.3). |
| 15 | **Break particles** | S | Small 1-px coloured shower at break point, ballistic, fades out. ~150 LOC. |
| 16 | **Place / destroy ghost preview** | XS | Faint outline of the targeted block face on the world render, white when placing, red when about to break. ~50 LOC. |

## Game systems

| Phase | Description | Effort | Why |
|---|---|---|---|
| 17 | **Inventory grid** | M | MENU + long-hold opens 4×4 picker spanning all 11 blocks, D-pad navigates, A confirms into active hotbar slot. ~200 LOC. |
| 18 | **Pause menu** | M | Overlay with Resume / Save / Load / New World / Settings / Exit. Required before save slots. ~200 LOC. |
| 19 | **Settings page** | S | FOV slider, fog toggle, audio gain, render distance. Persists to a separate flash sector. ~150 LOC. |
| 20 | **Material-aware footsteps** | S | Per-step audio whose timbre matches the block under the player's feet (sand whoosh, stone clack, grass rustle). ~80 LOC. |
| 21 | **Save slots — multiple worlds** | M | 4 named worlds in 4 separate sector groups; picker UI on startup; each tracks its own seed + deltas. ~300 LOC. |
| 22 | **Auto-save toast + indicator** | XS | Pulsing icon top-right while saving; "Saved" toast for 1.5 s. ~50 LOC. |

## World scale

| Phase | Description | Effort | Why |
|---|---|---|---|
| 23 | **Chunked world paging (XL)** | XL | 16×16×16 chunks, 64-chunk LRU cache resident in SRAM, dirty chunks flushed to flash. Unlocks **infinite-X-Z** worlds (Y fixed). Big lift — touches gen, render, save. ~500 LOC. |
| 24 | **Faster terrain gen (precomp height per column)** | S | Cache `height[x][z]` once; tree-aware lookup becomes O(1). Needed once chunks page in mid-game. |
| 25 | **Biomes** | M | Beach / forest / mountain / plains driven by a second noise channel; per-biome tree density + surface block. ~150 LOC. |

## Living world

| Phase | Description | Effort | Why |
|---|---|---|---|
| 26 | **Z-buffer for sprite overlays** | S | 32 KB depth buffer (uint16 per pixel). Enables proper sprite vs world occlusion. Required for mobs + future overlays. |
| 27 | **Passive mobs** | L | 8×8 billboarded sprites (sheep, pig, chicken). Simple wandering AI. 4–8 mobs simultaneously. ~400 LOC. |
| 28 | **Day/night mob behaviour** | S | Mobs return to "home" at sundown, scatter at dawn. ~80 LOC. |
| 29 | **Hostile mobs + HP** | L | Slime-style cube monsters spawn at night, deal contact damage. Player HP + low-HP red-tint vignette. ~350 LOC. |
| 30 | **Hunger + food blocks** | M | Bread/apple from inventory restores HP. Hunger ticks slowly. Optional toggle. ~200 LOC. |
| 31 | **Bow + arrow** | M | Ranged break/hit. Arrow physics, sprite render. ~250 LOC. |

## Audio (sound stage)

| Phase | Description | Effort | Why |
|---|---|---|---|
| 34 | **Procedural ambient music (C418-style)** | M | Slow, sparse, Minecraft-Sweden feel. Drives the relaxing tone the rest of the game wants. Detailed spec below. ~400 LOC. |

### Phase 34 detailed spec

The synth engine in `craft_audio.c` already has 4 voices, square / triangle / noise waveforms, and exponential gain decay. Music adds:

1. **ADSR envelope per voice** — replace the single `gain_dec` multiplier with attack-decay-sustain-release. Soft attacks (50 ms) and long releases (400-800 ms) give the "piano in a cavern" feel. ~40 LOC.
2. **Pentatonic note table** — C major pentatonic over 3 octaves (~15 notes). Frequencies precomputed at table-init time. ~20 LOC of data.
3. **Voice budget split**: reserve voices 0-1 for music (melody + pad), voices 2-3 stay for SFX. SFX preempts the round-robin slot only within its own pool. ~30 LOC of refactor.
4. **Chord scheduler** — looped 4-chord progression (i-VI-III-VII in A minor, or I-V-vi-IV in C major). Each chord lasts 8-16 beats at ~60 BPM. Triggers a sustained triangle-wave pad note (root + fifth) on voice 1. ~50 LOC.
5. **Melody scheduler** — random walk over pentatonic degrees, biased toward the current chord's root. Phrase structure: 4-bar phrase, 2-bar rest, repeat with variation. Note durations from a weighted set: half / quarter / dotted-eighth. Triggers a soft triangle pluck on voice 0. ~80 LOC.
6. **Cheap reverb** — single Schroeder allpass + one comb delay. ~2 KB ring buffer of int16 samples. Adds shimmer without devolving into mud. Conditional on `CRAFT_AUDIO_REVERB` so it can be skipped on the device if CPU is tight. ~60 LOC.
7. **Volume blending** — music gain ramps up over 30 s on world load, ducks to 30 % during SFX, ramps back. ~20 LOC.
8. **Track variations** — three or four track "moods" picked by world conditions:
   - Sunny day → bright I-V-vi-IV in C
   - Night → dim i-VI-III-VII in A minor with sparser melody
   - Underwater → low-pass-filtered drone, no melody
   - Cave (player y < 12) → low piano-bell only, very sparse
   ~80 LOC including pickup logic.
9. **API additions** (`craft_audio.h`):
   ```c
   void craft_audio_music_enable(bool on);     /* user setting */
   void craft_audio_music_set_mood(int mood);  /* called per frame */
   ```

**Memory** — adds ~3 KB BSS (reverb ring + note scheduler state). Negligible.

**CPU** — 4 envelopes × 1 mult/sample + reverb (~20 ops/sample) ≈ 300 extra cycles/sample at 22050 Hz = ~6.5 M cycles/sec = 2.3 % of one core. Safe even with everything else running.

**Settings integration** — appears in Phase 19 settings as "Music: On / Off / Quiet" once that ships.

## Multiplayer (long horizon)

| Phase | Description | Effort | Why |
|---|---|---|---|
| 32 | **USB-link 2-player explore mode** | XL | Host + client share a world. Network protocol: position updates 10 Hz, block-change events. Other player rendered as billboard. Requires ThumbyOne wiring. ~600 LOC. |
| 33 | **Co-op survival** | M | Once Phase 32 + 27 ship, co-op survival is mostly UI plumbing. |

## Quality bar checks (recurring, not phases)

- Profile each phase on device — refuse to merge a phase that pushes FPS under 20.
- Resident-SRAM budget — every phase reports its RAM delta in its README/docs entry. Refuse to merge if it pushes the total past 480 KB (40 KB headroom).
- Save format compatibility — bump `CRAFT_SAVE_VERSION` and keep deserialise tolerant.

## Recommended next-one queue

**Phase 38 — Crafting (3×3 shaped grid)** (next key phase)

Real Minecraft-style shaped recipes — the arrangement of items in
the 3×3 grid matters, not just the bag of ingredients. The player
physically places items in cells, and a recipe match shows an output
preview that they can take.

### UI layout (fits in 128×128)

```
+----+----+----+   →   +----+
|    |    |    |       |    |
+----+----+----+       +----+
|    |    |    |       output
+----+----+----+
|    |    |    |
+----+----+----+
   3×3 grid          arrow + result cell
```

- Grid cells 16×16, arrow + output cell on the right, hotbar still
  visible at the bottom so the player can see what they'll place.
- D-pad navigates 10 positions (9 grid cells + 1 output cell), wraps.
- A on a grid cell: places one of the active hotbar block into that
  cell (debits inventory by 1).
- B on a grid cell: removes the block, refunds to inventory.
- A on the output cell: if a recipe matches, consumes the cells'
  contents from inventory (already debited on placement) and adds
  the output to inventory. Clears the grid.
- MENU or B-from-output: close crafting, return remaining cells to
  inventory.

### Recipe representation

```c
typedef struct {
    BlockId pattern[9];   /* 0..8 row-major; BLK_AIR = empty cell    */
    BlockId output;
    uint8_t output_count;
    uint8_t bbox_w, bbox_h;
} CraftRecipe;
```

Matching: the player's grid is reduced to its bounding box (smallest
rectangle containing all non-AIR cells). That bounding rectangle is
compared cell-by-cell to each recipe's `pattern` clipped to its
declared `bbox_w × bbox_h`. So a recipe whose pattern is 2×2 can be
placed anywhere on the 3×3 grid — exactly like Minecraft.

Mirrored recipes can be added as a second entry if needed (e.g. axe
left-handed vs right-handed).

### v1 recipe set

| Recipe | Pattern | Output |
|---|---|---|
| Planks | `[W . .]` `[. . .]` `[. . .]` | 4 PLANK |
| Smooth stone | `[C C]` `[C C]` | 1 STONE |
| Apple from leaves | `[L]` `[L]` `[L]` (vertical) | 1 APPLE |
| Wooden sword | `[. S .]` `[. P .]` `[. P .]` | 1 SWORD |
| Stone pickaxe | `[S S S]` `[. P .]` `[. P .]` | 1 PICKAXE |

(W=wood, P=plank, S=stone, C=cobble, L=leaves)

### New item types

- `BLK_SWORD` — non-placeable inventory item. When held in active
  hotbar slot, player attack damage rises from 1 → 3 (one-shots a
  slime).
- `BLK_PICKAXE` — non-placeable. When held, stone/cobble breaks
  give double drops in survival (proxy for "faster mining" since
  break is already instant in our system).

Both render in inventory + hotbar with their own procedural icons
(stone-blade sword, T-shape pickaxe).

### Effort

- 3×3 grid UI + nav: ~120 LOC (parallel to existing inventory page)
- Recipe matching with bounding box: ~80 LOC
- Recipe table + output preview: ~40 LOC
- BLK_SWORD + BLK_PICKAXE plumbing (textures, names, special-case
  in player attack/break): ~80 LOC
- Menu wiring: ~20 LOC
- **Total ~340 LOC, 1 turn**

Beyond crafting:
- **Phase 21** save slots (multiple worlds)
- **Phase 13** ambient occlusion
- **Phase 25** biomes
- **Tool items + smelting** (extends Phase 38)
- **Phase 32+** USB-link multiplayer

Performance fallback: if on-device FPS is still under 25 after the SRAM-renderer pass already in v1, the cheap next move is interlaced rendering (render half the rows per frame, fill the rest from the previous frame buffer). That halves cost on average for some shimmer on fast yaw — usually a good trade.

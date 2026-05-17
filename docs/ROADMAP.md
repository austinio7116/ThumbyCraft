# ThumbyCraft roadmap

Living plan for ThumbyCraft. Each phase is shippable on its own.

Status legend: ✅ shipped · 🟡 in-flight · ⬜ planned

## What's playable today

A first-person voxel sandbox you can actually play through. Survival
mode is the default; creative is one menu toggle away.

- **World**: infinite sliding-window (64³ resident, partial-regen on
  shift, ~5 ms stutter), procedural terrain with mountain biome,
  trees, water, ore distribution
- **Survival loop**: gather → craft → mine ore → smelt-equivalent →
  upgrade tools → fight slimes at night, eat to regen
- **Tools**: wood / stone / iron pickaxes (tier-gated mining), wood /
  stone / iron swords (tier damage)
- **Crafting**: 3×3 shaped grid with 11 recipes including the full
  tool tier ladder, a recipe-book reference, and torches
- **Mobs**: 3D voxel-cuboid sheep / pig / chicken (passive) and
  slime (hostile, day+night spawn cap, chases player)
- **Audio**: procedural sine-pad + melody music (C-major pentatonic
  with comb-delay reverb), material-specific break sounds, footsteps,
  damage flash, pickaxe-required "ting"
- **Visual**: day/night cycle, world-fixed celestial-sphere
  starfield, sun + moon billboards, water animation, break particles,
  pick-outline with face highlight, fog
- **Menu**: scrolling pause list with sub-pages for Inventory, Craft,
  Recipes, Controls cheatsheet
- **Save**: flash wear-ring on device, file on host

## Shipped phases

### Engine + drivers
- Phase 0-2: Repo, host SDL2 build, device RP2350 dual-core renderer
- Phase 10: Performance pass (SRAM-resident renderer, per-column
  basis, skip per-pixel normalize, column-batched gen)

### World
- Phase 4: Procedural terrain (FBM noise + trees + water)
- Phase 6: Flash saves (delta-vs-base format, 4-sector wear ring)
- Phase 14: Water animation (per-frame stripe shift)
- Phase 23: Sliding-window infinite world (partial regen via memmove
  + new-strip fill; 2048-slot mod hash for player edits across shifts)
- Phase 25 (partial): Mountain biome with low-freq biome noise +
  height boost; mountain peaks become stone-surfaced; ore density
  doubles in mountains

### Player + physics
- Phase 3, 5: Place / break, hotbar, AABB sweep, gravity, auto-step-up
- Phase 9-9b-v4: Control scheme — LB walks, D-pad always
  pitches/turns, RB jumps, A break, B place, MENU chord for hotbar
  and fly toggle

### Modes + survival
- Phase 29: Game modes — Creative / Survival, HP, hunger, contact
  damage, respawn timer
- Phase 30: Hunger system (decay, gates regen, apples drop from
  leaves, auto-eat)

### Combat
- Mob HP + player attack picks mob over block when in range
- Sword tier damage (1 → 2 → 3 → 4 for hand / wood / stone / iron)
- Mining tier gating (`craft_block_pickaxe_tier`: 0=hand, 1=wood,
  2=stone)

### Mobs
- Phase 26: Z-buffer (uint8 quantised, populated by render_strip)
- Phase 27: 3D cuboid mobs (sheep, pig, chicken, slime) with proper
  ray-vs-cuboid hit test, slab intersection, face shading, depth
  sort, hurt flash
- Phase 28: Day/night spawn rate (faster at night, never despawns)

### Crafting
- Phase 17: Inventory grid (4×3 picker)
- Phase 38: Shaped 3×3 crafting grid with output preview
- 11 recipes: planks, sticks, smooth stone, iron ingot, wood/stone/iron
  pickaxes, wood/stone/iron swords, torches
- Recipe-book page (visual catalog)

### Audio
- Phase 34: Procedural ambient music v2 — sine waves, octave-doubled
  pad, exp ADSR, comb-delay reverb, C-major pentatonic with I-vi-IV-V
  chord progression, ducking on SFX
- Phase 20: Material-aware footsteps
- Per-material break + place sounds (layered transient + body)
- Pickaxe-required ting

### Visual
- Phase 11: Day/night cycle (4-min, sky shift, brightness scale)
- Phase 12: Sun + moon billboard
- Phase 15: Break particles
- Phase 16: Pick outline with face highlight (placement face glows)
- World-fixed celestial-sphere starfield

### UI
- Phase 18: Pause menu with all toggles
- Phase 22: Toast notifications
- Scrolling menu (vertical overflow handling)
- Controls cheatsheet page
- Recipe book page
- Settings shown inline: Game mode, Toggle fly, Invert Y, Music

### Platform
- Phase 8: ThumbyOne slot scaffolding

## Open phases — prioritised

### Tier 1 — already discussed with the user, queued

| Phase | Description | Effort | Status |
|---|---|---|---|
| **A — Furnace UI** | Placeable `BLK_FURNACE` (8-cobble ring recipe), B on a placed furnace opens a 3-slot sub-page (input / fuel / output) with a ~5 s smelt timer. Promotes the iron-ingot grid hack to proper smelting; unlocks `sand→glass`, `cobble→stone`, raw meat → cooked meat | ~350 LOC | ⬜ next |
| **B — Chest storage** | Placeable `BLK_CHEST`, B opens 4×4 = 16-slot per-chest inventory. Contents persisted in a sparse coord-keyed table (mod-hash style). Lets the player stockpile beyond the 8 hotbar slots | ~300 LOC | ⬜ |
| **C — Light propagation** | 1-bit lightmap (32 KB BSS) flood-filled from each torch within radius ~6. Renderer reads lightmap to floor block brightness at night, making torches actually useful for caving | ~230 LOC | ⬜ |

### Tier 2 — visual polish & systems

| Phase | Description | Effort |
|---|---|---|
| Phase 13 | **Block ambient occlusion** — darken convex corners via cheap per-face neighbour sample. Big depth-perception upgrade | S |
| Phase 19 | **Settings page** — FOV, fog toggle, music volume, render distance. Persists separately from save | S |
| Phase 21 | **Save slots** — 4 named worlds in 4 sector groups; startup picker | M |
| Phase 25 (rest) | **More biomes** — beach, forest, plains (now have only mountain + default); per-biome tree density + surface block | M |

### Tier 3 — combat, food, drops

| Phase | Description | Effort |
|---|---|---|
| Phase 31 | **Bow + arrow** — ranged combat; arrow projectile + AABB hit + sprite. Needs string (from spider?) + feather (from chicken) — implies mob drops | M |
| — | **Mob loot drops** — sheep → wool, pig → raw pork, chicken → raw chicken + feather, slime → slimeball. Enables cooked-food economy + bow recipe | M |
| — | **Diamond tier** — `BLK_DIAMOND_ORE` (deep stone, iron pick required), `BLK_DIAMOND` item, diamond tools (5 dmg sword, mines anything) | M |
| — | **Hostile variety** — zombies (walk-not-jump slimes that take more hits), maybe a flying or ranged variant | L |

### Tier 4 — building variety

| Phase | Description | Effort |
|---|---|---|
| — | **Doors + ladders** — door needs an `is_open` flag bit per cell; ladder lets you climb vertical walls | S+M |
| — | **Decorative blocks** — bookshelf (bookcase tex), bricks (8 cobble in furnace?), wool from sheep | S |
| — | **Stairs / slabs** — half-block geometry; requires raycaster work since current engine assumes full cubes | XL |

### Tier 5 — multiplayer

| Phase | Description | Effort |
|---|---|---|
| Phase 32 | **USB-link 2-player explore** — host + client share a world via the ThumbyOne USB-link path. Sync block changes + player positions at 10 Hz. Other player drawn as billboard | XL |
| Phase 33 | **Co-op survival** — once 32 + mob drops land, mostly just UI plumbing | M |

## Recommended next-three queue

1. **Slice A — Furnace UI** (biggest gameplay unlock — graduates the
   iron-ingot recipe to real smelting and opens cooked food + glass +
   smooth stone)
2. **Slice B — Chest storage** (overflow problem starts to bite as
   the player accumulates more than 8 block types)
3. **Slice C — Light propagation** (torches earn their keep; sets up
   future cave biome)

## Quality bar (recurring, not phases)

- **Profile each phase on device** — refuse a phase that pushes FPS
  under 20
- **Resident SRAM budget** — current ~415 KB of 520 KB. Refuse a
  phase that pushes past 480 KB (40 KB headroom for stack)
- **Save format** — bump `CRAFT_SAVE_VERSION` and keep deserialise
  tolerant when changing the format
- **Loop discipline** — terrain Y-fill loops must be fused or
  explicitly bounds-checked (see `feedback_thumbycraft_loop_memset`
  memory entry for why)

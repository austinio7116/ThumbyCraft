# ThumbyCraft roadmap

Living plan. Each tier ships independently.

Status legend: ✅ shipped · 🟡 in-flight · ⬜ planned

---

## What's playable today

A first-person voxel sandbox with day/night, real ranged + melee combat,
caves to explore, rivers to bridge, and procedural huts to stumble on.
Survival is the default; creative is one menu toggle away. Buildings
persist to flash automatically.

### World
- ✅ Infinite sliding-window world (64³ resident; partial-regen on shift)
- ✅ Procedural terrain — FBM noise + mountain biome + ore (coal/iron, denser in mountains)
- ✅ **Caves** — 3D value-noise carved into stone; never breaks the surface silhouette
- ✅ **Rivers** — narrow ridge-noise streams in lowlands only (no canyons through hills)
- ✅ **Wooden huts** — rare procedural plank cabins (5×5 footprint, door, roof) in flat lowland grass
- ✅ Day/night cycle (4-min) with day/night-aware sky + brightness
- ✅ Entropic boot seed (pico_rand) — fresh world every power-on

### Trees (Minecraft-faithful)
- ✅ Standard oak (5-tall trunk, 5×5 + 3×3 canopy)
- ✅ Large oak with two side branches + tip leaf clusters
- ✅ Tall pine (8-tall trunk, conical layered skirts) in mountain biome

### Lighting (rewritten)
- ✅ **2-bit gradient lightmap** (64 KB BSS) — flood-filled from each torch, 4 brightness levels with smooth falloff
- ✅ **3D torches** as small wall-mounted / floor-standing cuboids (not full block cubes)
- ✅ **Sky-vs-cave shadow model** via per-column sky_height — tree shadows day/night-aware, cave entrances pick up bleed from sky-open neighbours
- ✅ Procedural texture atlas **baked to flash** (32 KB SRAM freed for the gradient lightmap upgrade)

### Player + physics
- ✅ AABB sweep, gravity, **auto-step with 350 ms cooldown** (no bunny-hop)
- ✅ LB walks, D-pad pitches/turns, RB jumps, A break/attack, B place, MENU chord for hotbar / fly
- ✅ Mining tier gating (hand → wood → stone → iron pick)
- ✅ Sword tier damage (1/2/3/4)

### Survival + UI
- ✅ Creative / Survival modes, contact damage, respawn timer
- ✅ **Hearts HUD** (3 hearts, quarter-resolution) — passive regen 5s after damage; hunger system removed
- ✅ Pause menu with sub-pages (Inventory, Craft, Recipes, Controls)
- ✅ Crafting: shaped 3×3 with 12 recipes (vanilla-accurate for picks + swords)
- ✅ Recipe-book reference page

### Mobs
- ✅ Passive: sheep, pig, chicken (multi-cuboid models)
- ✅ Hostile: **slime, skeleton (arrows), spider (8 legs), creeper (4 stubby legs + face)**
- ✅ **Detailed silhouettes** — skeleton ribcage + nose cavity + recessed sockets; creeper mottled body + 4-quad eyes + T-frown
- ✅ **Arrow projectile system** (16-arrow pool, gravity arcs, AABB hit, block/player despawn)
- ✅ Hostile spawn **only in shadow or at night**
- ✅ Hostiles **catch fire in direct sunlight** (1 HP/sec + rising flame particles)
- ✅ Mob **stand-off** — hostiles always stop in the adjacent cell, never enter the player's block
- ✅ **Creeper explosion** — particles + spherical block destruction (skips water; persists via chunk store)
- ✅ Day/night spawn cadence

### Audio (Music v3 + SFX)
- ✅ **Claire-de-Lune-inspired** pad — true 4-note chord voicings (Fmaj9/Am9/Dm9/Bbmaj7/Csus4/Fmaj7add6), smooth voice leading
- ✅ **Day/night mode switch** — slow contemplative at night, faster + arpeggiated by day
- ✅ **Scale runs** — long sweeping pentatonic figures (8-15 notes) at 16th-note feel
- ✅ 3× loudness boost + ambient hiss removed
- ✅ Per-material break + place sounds
- ✅ Footsteps + jump SFX removed (felt harsh under boost)

### Visual
- ✅ Sun + moon billboards
- ✅ World-fixed celestial-sphere starfield
- ✅ Water animation
- ✅ Block-break particles + **explosion fireballs + rising flame puffs**
- ✅ Pick-outline with face highlight
- ✅ Fog

### Persistence
- ✅ Flash save (delta-vs-base format, 4-sector wear ring, manual via menu)
- ✅ **Chunk-store flash persistence** — every player edit is automatically saved to a 256 KB flash region keyed by chunk hash. Buildings survive walking far away + power cycles.

### Platform
- ✅ ThumbyOne slot scaffolding
- ✅ Host SDL2 build for fast iteration
- ✅ Procedural texture baker as a build-time host tool

---

## Open phases — prioritised

### Tier 1 — gameplay completeness (next-three queue)

| Slice | Description | Effort | Status |
|---|---|---|---|
| **A — More tools** | Axe (faster wood chop), shovel (faster dirt/sand dig), hoe (placeholder — no farming yet), bow (reuses skeleton arrow code) per tier. Adds 9-12 enum entries + textures + recipes + speed/damage hooks | M | ⬜ next |
| **B — Furnace UI** | Placeable `BLK_FURNACE` (8-cobble ring recipe). B on placed furnace opens 3-slot sub-page (input/fuel/output) with a ~5 s smelt timer. Graduates the in-grid "smelt" recipes to proper furnace use; unlocks cooked food | L | ⬜ |
| **C — Chest storage** | Placeable `BLK_CHEST`, B opens 4×4 = 16-slot per-chest inventory. Per-chest contents in a sparse coord-keyed table (mirror of the chunk-store pattern). Lets the player stockpile beyond the 8 hotbar slots | M | ⬜ |

### Tier 2 — combat depth

| Phase | Description | Effort |
|---|---|---|
| **Mob loot drops** | sheep → wool, pig → raw pork, chicken → raw chicken + feather, slime → slimeball, skeleton → bone + arrow. Enables cooked food + bow recipe + decorative wool | M |
| **Diamond tier** | `BLK_DIAMOND_ORE` (deep stone, iron pick required), diamond ingot, diamond tools (5 dmg sword, mines anything) | M |
| **Armour** | Helmet/chest/legs/boots per tier; HUD armour bar; `craft_player_take_damage` reduces by N% per piece. Visual is mechanic-only (first-person — invisible) | M |
| **Zombies** | Walk-not-jump slime variant, more HP, knockback bonus — gives night a different threat profile | M |

### Tier 3 — biomes + structures

| Phase | Description | Effort |
|---|---|---|
| **More biomes** | Beach (sand near water), forest (denser trees), plains (low density). Currently just mountain + default grass | M |
| **Snow / desert biome** | New surface blocks, biome-restricted spawns | M |
| **Villages** | Cluster of huts + path connector. Builds on the hut generator already in place | M |

### Tier 4 — building variety

| Phase | Description | Effort |
|---|---|---|
| Doors + ladders | Door needs an `is_open` flag bit per cell; ladders let you climb vertical walls | S+M |
| Decorative blocks | Bookshelf, bricks, wool from sheep loot | S |
| **Stairs / slabs** | Half-block geometry — requires raycaster work since current engine assumes full cubes | XL |

### Tier 5 — engine polish

| Phase | Description | Effort |
|---|---|---|
| **Block ambient occlusion** | Darken convex corners via cheap per-face neighbour sample. Big depth-perception upgrade | S |
| **Settings page** | FOV, fog toggle, music volume, render distance. Persisted separately from save | S |
| **Save slots** | 4 named worlds in 4 sector groups; startup picker | M |

### Tier 6 — multiplayer

| Phase | Description | Effort |
|---|---|---|
| USB-link 2-player explore | Share world via ThumbyOne USB-link path. Sync block changes + player positions at ~10 Hz. Other player drawn as billboard | XL |
| Co-op survival | Once link + drops land, mostly just UI plumbing | M |

---

## Quality bar (recurring, not phases)

- **Profile each phase on device** — refuse a phase that pushes FPS under 20
- **Resident SRAM budget** — currently ~455 KB of 512 KB main SRAM. Refuse a phase that pushes past ~470 KB (margin for stack + multicore lockout)
- **Save format** — bump `CRAFT_SAVE_VERSION` and keep deserialise tolerant when changing the format
- **Loop discipline** — terrain Y-fill loops must be fused or explicitly bounds-checked (see `feedback_thumbycraft_loop_memset` memory entry)
- **Recipes match vanilla Minecraft** where an equivalent exists; Thumby-specific simplifications (in-grid smelt, no charcoal distinction) are intentional and called out in comments

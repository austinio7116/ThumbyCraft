# ThumbyCraft

A bare-metal Minecraft-style voxel game for the
[Thumby Color](https://thumby.us/) — a credit-card-sized handheld with a
128×128 RGB565 screen, dual-core ARM Cortex-M33, **no GPU**, and 512 KB
of SRAM. Everything you see is rendered by per-pixel CPU raycasting in
real time, with full survival mechanics, mobs, music, and persistent
worlds.

![screenshot — drop one in `docs/` when you have it](docs/screenshot.png)

```
~30 fps  ·  64³ block window over an infinite world  ·  3 hostile + 3 passive mob types
proc audio synth  ·  flash-backed buildings  ·  280 MHz dual-core M33
```

---

## Quick start

A **prebuilt firmware** ships with the repo at the root —
`firmware_thumbycraft.uf2`. To play:

1. Hold **D-pad DOWN** while powering on the Thumby Color to enter
   BOOTSEL mode — it appears as a USB drive `RPI-RP2350`
2. Drag `firmware_thumbycraft.uf2` onto the drive
3. Power-cycle. You're playing.

To build from source, see [Build](#build) below.

For fast iteration on a desktop, see **[Host build](#host-build)** below.

---

# Player guide

## Game modes

- **Survival** (default) — HP, mob threats, mine ore to upgrade tools.
  Three hearts (quarter-resolution); passive regen 5 seconds after
  damage.
- **Creative** — flight, no damage, all blocks free in inventory. Pause
  menu → *Game mode* toggle, or **MENU + A** for fly toggle.

## Controls

| Button | Action |
|---|---|
| **LB** held | Walk forward (gravity on); ascend (fly mode) |
| **RB** tap | Jump |
| **D-pad L/R** | Turn left / right |
| **D-pad U/D** | Pitch camera up / down |
| **A** | Break block / attack mob |
| **B** | Place selected block from hotbar |
| **MENU + LB / RB** | Hotbar previous / next slot |
| **MENU + A** | Toggle fly (creative only) |
| **MENU** (tap & release) | Open pause menu |

The D-pad is **always** the look stick — pitch and turn together
without modal toggles. Walking is on a separate button so you can scan
the horizon while moving forward.

**Auto-jump**: 1-block obstacles in front of you get stepped up
automatically (with a 350 ms cooldown so it doesn't bunny-hop you up
stairs).

## The world

Walk in any direction — it's infinite. A 64×64×64 window slides with
you, regenerating new terrain at the edges from a deterministic seed.
Your edits stay where you left them; walk back a kilometre and the
hole you dug is still there.

### What you'll find

- **Grassland plains** — gentle hills, lakes, occasional small streams
- **Mountain biomes** — taller peaks, stone surfaces, denser ore
- **Trees** — three species (standard oak, large oak with branches,
  pines in mountains)
- **Caves** — naturally carved into stone via 3D noise; entrances often
  visible on hillsides
- **Rivers** — narrow winding streams in lowlands, never carving
  through highlands
- **Wooden huts** — rare 5×5 plank cabins you'll occasionally stumble
  on. Step through the doorway and shelter in one.
- **Coal & iron ore** — denser in mountains; mineable with the right
  pickaxe tier

### Day & night cycle

A full day is **4 minutes**. The sun arcs across the sky, the sky
colour shifts, and stars appear at night. Hostile mobs spawn during
night or in shadows — and **catch fire** in direct sunlight at
~1 HP / sec, complete with rising flame particles. Be inside or
shaded by sunrise.

## Mining & crafting

### Tier-gated mining

| Block | Requires |
|---|---|
| Dirt, sand, wood, leaves | Hands |
| Stone, cobblestone, coal ore | Wooden pickaxe or better |
| Iron ore | Stone pickaxe or better |

Hit a block with **A**. If the active hotbar slot has a sword, its
tier sets your melee damage too (wood / stone / iron = 2 / 3 / 4 HP
per hit, hands = 1).

### Crafting

Open **MENU → Craft**. You get a 3×3 shaped grid. Drop blocks into
cells using the D-pad and **A**, then read the output preview.
Recipes match canonical Minecraft Java where applicable:

| Output | Recipe |
|---|---|
| 4 planks | 1 wood log |
| 4 sticks | 2 planks vertical |
| Wood/Stone/Iron pickaxe | 3 head material across top + 2 sticks centre |
| Wood/Stone/Iron sword | 2 head stacked + 1 stick below |
| 4 torches | 1 coal + 1 stick |
| Iron ingot | 1 iron ore + 1 coal (in-grid "smelt") |
| Glass | 1 sand + 1 coal (in-grid "smelt") |
| Smooth stone | 4 cobble 2×2 |

The recipe book in **MENU → Recipes** shows them all visually.

### Tool & sword ladder

Wood → Stone → Iron is the standard progression. Each tier mines
faster, hits harder, and unlocks the next block type.

## Combat & mobs

### Hostile

- **Slime** — basic chaser, contact damage 1
- **Skeleton** — ranged. Holds at 5 blocks, **fires arrows** with
  gravity arc and rough line-of-sight check
- **Spider** — fast melee, 1.5× slime speed, contact damage 2
- **Creeper** — silent walker; entering 1.8 m range freezes it for a
  **1-second fuse** (visually pulses toward white), then it
  **explodes**: 5 damage within 2.5 m + **destroys nearby blocks**

All hostiles only spawn **in shadow or at night**. They stay one cell
away from you (proper Minecraft-style standoff) — they bite from the
neighbour block, never enter your cell.

### Passive

Sheep, pigs, chickens wander grassland during the day. Currently
no drops; mob loot is a queued feature.

## Lighting & torches

The world has a **gradient lightmap** flood-filled from each torch.
Caves are dark — you'll need torches to see. Place a torch with **B**
when holding one (start with 8 in survival inventory) and the area
brightens in a smooth radial falloff.

Torches mount automatically based on the surface you place them on:

- On a wall → mount horizontally outward
- On the floor → stand vertically

Outdoor brightness tracks the sun: full daylight at noon, dim
moonlight at midnight. Under a tree at noon, you'll see a softer
shadow that fades correctly with the day cycle.

## Building & persistence

Every block you place or break is **persistently saved to flash**,
automatically:

- The 64³ window slides with you; chunks that scroll off-window get
  their player edits written to a 256 KB chunk store on flash
- Walk back later and your build is exactly where you left it
- Survives power-cycles (no need to manually save)

You can still do an explicit save: **MENU → Save world**. New worlds:
**MENU → New world** (generates a fresh seed).

## Tips

- **Mine down for stone & ore.** First wood pickaxe → stone pickaxe
  → iron pickaxe.
- **Light caves before exploring.** Hostiles spawn in unlit cave
  cells day or night.
- **Build a small shelter before sunset.** A 5×5 plank box with a
  torch buys you safety until dawn — or find a hut.
- **Creepers respect water.** Drop into a pond if one's fuse is
  counting — explosions skip water cells.
- **Skeletons are persistent shooters.** Break line of sight or close
  the gap — they stop firing under 3 m and back off.

---

# Technical architecture

This is a CPU-only voxel renderer. There is no GPU on the RP2350; every
pixel drawn to the screen passes through one of the M33 cores.

## Hardware target

- **MCU**: Raspberry Pi RP2350 (dual ARM Cortex-M33 @ 280 MHz, FPU,
  no GPU)
- **SRAM**: 512 KB main + 8 KB scratch X/Y (used for stacks)
- **Flash**: 2 MB (XIP execute-in-place; cached)
- **Display**: GC9107 LCD, 128×128 RGB565, SPI + DMA
- **Audio**: PWM on a single GPIO with IRQ ring buffer, 22050 Hz
- **Input**: GPIO buttons (A, B, LB, RB, D-pad, MENU)

## Memory map (SRAM)

| Item | Size |
|---|---|
| `craft_world_blocks` — resident 64³ window | 256 KB |
| `craft_world_lightmap` — 2-bit gradient | 64 KB |
| `s_mods` — player-edit hash | 24 KB |
| `craft_zbuf` — per-pixel depth | 16 KB |
| `g_fb` — framebuffer | 32 KB |
| `craft_world_skyheight` — per-column sky Y | 4 KB |
| `audio ring + reverb` | 12 KB |
| Mob models, particle pool, torch list, mod buffers | ~10 KB |
| Pico SDK BSS + heap + multicore lockout | ~32 KB |
| **Total resident** | **~456 KB / 512 KB** |

Stacks (core 0 + core 1, 4 KB each) live in scratch X/Y, so they
don't count against main SRAM.

## Rendering: per-pixel DDA raycaster

The core renderer (`src/craft_render.c`) walks one ray per pixel
through the voxel grid using a **3D DDA**:

```
for each pixel (px, py) in 128×128:
    dir = camera_basis(px, py)
    voxel = floor(camera_pos)
    while step_count < 64:
        if t_max_x is smallest: voxel.x += sign(dir.x); idx += signx
        elif t_max_y:           voxel.y += sign(dir.y); idx += signy * WORLD_X * WORLD_Z
        else:                   voxel.z += sign(dir.z); idx += signz * WORLD_X
        if voxel exits window: break
        block = craft_world_blocks[idx]   // direct array read, no fn call
        if block is solid: hit; sample texture; shade; write fb + zbuf; break
```

Key optimisations:

- **Per-frame column basis cache** (`s_col_basis[128]`) — ray direction
  for each screen X is computed once and reused for both top and
  bottom strips
- **Incremental world index** — the local-buffer index is maintained
  by adding ±1 / ±64 / ±4096 per DDA step instead of recomputing from
  voxel coords. Eliminates a function call + bounds compare + multiply
  per step.
- **`.time_critical` SRAM placement** — the hot trace function is
  marked `__attribute__((section(".time_critical.craft")))` so it
  runs from SRAM, not XIP flash (no instruction-cache misses)
- **`-O3 -ffast-math -mfpu=fpv5-sp-d16`** — single-precision FPU,
  aggressive optimisation

### Dual-core split

`device/craft_device_main.c` launches core 1 in a render loop. Each
frame, core 0 runs game logic + the bottom half of the frame, signals
core 1 via a shared volatile flag to render the top half, then waits
for completion before swapping framebuffers. Two cores → ~2× raycast
throughput on the screen.

## World system

### Sliding window

The world is conceptually **infinite** in X/Z, bounded 0..63 in Y. A
64×64×64 buffer (`craft_world_blocks`) is the resident **window**
into that infinite world, tracked by `(origin_x, origin_z)`. When the
player walks within 16 cells of an edge, `craft_world_maybe_shift`:

1. Persists chunks leaving the window to flash (see chunk store)
2. `memmove`s the overlap regions of the buffer
3. Generates the new strip from the deterministic seed
4. Restores chunks entering the window from flash

A single-axis 16-cell shift regenerates only 1024 columns of new
terrain (~7 ms), keeping the frame budget intact.

### Terrain generation (`src/craft_gen.c`)

Pure functions of `(x, y, z, seed)` so the save layer can reconstruct
the base world without holding a second copy:

- **Heightmap**: 4-octave FBM 2D value noise → base elevation
- **Mountain biome**: low-frequency biome noise; ramps add up to
  22 blocks of extra elevation
- **Rivers**: ridge noise (peaks where FBM crosses 0.5); gated by
  natural elevation ≤ water_level + 2 so streams only form in
  lowlands, never canyon through hills
- **Caves**: 3D value noise (stretched Y for horizontal chambers);
  carves stone below dirt + above bedrock; ~5-15 % cell coverage
- **Trees**: three shapes (oak, branched large oak, pine), placed by
  per-position hash, with biome-aware density
- **Huts**: 5×5 plank cabins in flat lowland grass, ~1 per 128×128
  area, with deterministic door orientation

The column generator `craft_gen_column` produces an entire Y stack
for one (x, z) per call, used by the window-load and window-shift
paths. The per-cell `craft_gen_block_at` produces the same answer for
any single coordinate, used by the save diff.

### Lighting (`src/craft_world.c`)

- **2-bit gradient lightmap** (64 KB, 4 levels per cell). Flood-filled
  via BFS from each torch within radius 6, levels decay with distance
- **Per-column sky-height** (4 KB). The Y of the topmost solid block
  per (x, z) column. Lets the renderer distinguish sky-exposed cells
  from cave cells in O(1)
- **Shadow tiers**: in the renderer, the effective brightness for an
  air cell is computed from its column depth below sky:
  - depth ≤ 0: full daylight × current sun factor
  - 1-2: 70 % (tree shadow)
  - 3-5: 50 % (under canopy floor)
  - 6-9: 27 % (upper cave)
  - 10+: deep-cave constant ~16 % (day/night independent)
  - Plus a 3×3 sky-neighbour lift for cave-mouth cells
- **Torch overlay**: lightmap level (1-3) floors the brightness so
  torches glow even in deep caves

### Flash chunk store (`device/craft_chunk_store_flash.c`)

256 KB reserved at flash offset `TOP − 16 KB − 256 KB`. Divided into
64 sector slots × 4 KB each, hash-addressed by `(chunk_x, chunk_z)`
with 4-slot linear probe on collision. Each sector:

```
magic 'TCMK'  · 4 B
world_seed    · 4 B  (invalidates records from prior seeds)
chunk_x, _z   · 4 B + 4 B
mod_count     · 2 B + 2 B pad
mods          · 4 B each (lx, y, lz, blk)  — up to 340
crc32         · 4 B
padding to sector boundary (0xFF)
```

On window shift, chunks leaving the window get their mods scanned out
of the SRAM mod-hash and persisted to flash. Chunks entering have
their mods loaded back into the hash so the gen pass picks them up.

Flash writes use `multicore_lockout_start_blocking` to halt core 1
before erase/program — core 1 calls `multicore_lockout_victim_init()`
at startup to register its IRQ handler.

## Mobs (`src/craft_mobs.c`)

Each mob is a **multi-cuboid model**. The render path projects the
mob's world AABB to a screen bbox, then per pixel inside that bbox
casts a ray into the mob's local frame and tests against every
cuboid part using slab intersection. The nearest hit defines the
colour + face for shading. Parts are sorted by volume at build time
so the big body cuboids hit first, tightening `best_t` for the small
detail cubes (eyes, mouth, ribs, mottling).

### Hostile AI

- **Slime**: chase + melee at standoff
- **Skeleton**: chase, hold at 5 m, back off at 3 m; line-of-sight
  test (stepped block-raycast); fires arrows with gravity-leading aim
- **Spider**: fast chase + melee
- **Creeper**: chase until 1.8 m; freeze + visual fuse 1 s; spherical
  block destruction within 2.5 m + particle blast

### Arrow projectiles

16-arrow pool, gravity -8 m/s², AABB-block hits, AABB-player hits, 3-s
lifetime cap. Rendered as a brown-shaft + white-tip line via
Bresenham with per-pixel z-buffer test.

### Mob spawn rules

Hostiles only spawn where cells are **either not sky-exposed**
(under cover) **or it's currently night**. Caught in direct sunlight
they take 1 HP / sec and emit rising flame particles via
`craft_particles_emit_flame`.

## Audio (`src/craft_audio.c`)

A 4-voice procedural synth running entirely in the audio IRQ:

- **Voices**: 4 total (0 = pad chord, 1 = melody, 2-3 = SFX). Each
  voice supports up to 4 oscillators with one shared envelope, so
  the pad plays a full 4-note chord through one ADSR.
- **Waveforms**: sine (256-entry table + linear interp), square,
  triangle, noise
- **Envelope**: exponential ADSR
- **Reverb**: single comb-delay (~93 ms, wet 0.28, feedback 0.22)

### Music (Claire-de-Lune-inspired)

- **Six-chord modal progression** centred on F major:
  `Fmaj9 → Am9 → Dm9 → Bbmaj7 → Csus4 → Fmaj7add6` with smooth voice
  leading (each chord shares 2-3 notes with the next)
- **Day/night switch** driven by `craft_render_sun_y()`:
  - **Night**: 20 s chords, slow melody every 2.5-5.5 s, 55 % rest,
    sustained single notes
  - **Day**: 12 s chords, melody every 0.9-2.2 s; 60 % of events fire
    long **sequential pentatonic scale runs** (8-15 notes, ascending
    or descending, wrap into next octave) — the Claire-de-Lune
    right-hand cascade

### SFX

Per-material break / place tones layered as transient (noise burst) +
body (square/triangle). Pickaxe-required "ting" two-tone feedback.

### Output stage

3× pre-clamp loudness boost (soft limiter — quiet content brightens,
peaks hit the clamp wall) → int16 sample at 32 000 scale (≈ 98 % of
the 12-bit PWM DAC's range).

## Persistence

Two flash layers:

1. **Chunk store** (256 KB, automatic) — every player edit
2. **Save blob** (16 KB wear-ring, manual) — player state + delta
   summary, written on **MENU → Save**

A new-world action erases the chunk store's records for the old seed
(records key by `world_seed` so a fresh `get_rand_32()` start cleanly
shadows the old).

## Build pipeline

A native host tool **bakes the procedural texture atlas to a flash
const array** at build time (`tools/bake_textures.c`). This freed
~32 KB of BSS that the 2-bit lightmap upgrade then used. The bake
runs via `ExternalProject_Add` in the device CMake so any change to
`craft_blocks.c` re-bakes automatically.

## Source layout

```
src/                         portable engine (host + device share these)
├── craft_types.h            shared types (Vec3, rgb565, CRAFT_FB_W, etc.)
├── craft_blocks.{c,h}       block table + texture atlas (baked at build time)
├── craft_world.{c,h}        sliding-window block storage + mod hash + light + flash bridge
├── craft_chunk_store.h      flash chunk-store API
├── craft_gen.{c,h}          terrain + caves + rivers + trees + huts
├── craft_render.{c,h}       DDA raycaster, mob projection helpers, sky/fog
├── craft_player.{c,h}       camera, controls, AABB physics, hotbar
├── craft_hud.{c,h}          hearts, hotbar, crosshair, toasts
├── craft_menu.{c,h}         pause + craft grid + recipe book
├── craft_mobs.{c,h}         3D mob models + AI + arrows
├── craft_torches.{c,h}      3D torch cuboids + orientation hash + light hooks
├── craft_particles.{c,h}    block-break / explosion / flame puffs
├── craft_audio.{c,h}        synth + music + SFX
├── craft_save.{c,h}         engine-side persistence (diff-vs-base)
├── craft_font.{c,h}         3×5 bitmap font (Pemsa-derived)
└── craft_main.{c,h}         game loop, dispatch, mode toggles

host/                        SDL2 platform layer
├── host_main.c
├── craft_chunk_store_stub.c  (no-op flash on host)
└── CMakeLists.txt

device/                      RP2350 platform layer
├── craft_device_main.c       boot, dual-core dispatch
├── craft_lcd_gc9107.{c,h}    SPI + DMA LCD driver
├── craft_buttons.{c,h}       GPIO + edge detect
├── craft_audio_pwm.{c,h}     PWM + IRQ audio ring
├── craft_save_flash.{c,h}    16 KB wear-ring saves
├── craft_chunk_store_flash.c 256 KB chunk store
└── CMakeLists.txt

tools/                       build-time host tools
└── bake_textures.c           procedural atlas baker
```

## Build

### Host build

For desktop iteration (Linux / macOS):

```bash
sudo apt install libsdl2-dev
cmake -S host -B build_host
cmake --build build_host -j8
./build_host/thumbycraft_host [seed]
```

Keyboard map: arrows / WSAD = D-pad, Z = A, X = B, LShift = LB,
Return = RB, Esc = MENU. F1 toggles fog, F5 saves, F9 loads.

### Device build

```bash
sudo apt install gcc-arm-none-eabi libnewlib-arm-none-eabi cmake
cmake -S device -B build_device -DPICO_SDK_PATH=/path/to/pico-sdk
cmake --build build_device -j8
cp build_device/thumbycraft.uf2 firmware_thumbycraft.uf2
```

Flash: hold **D-pad DOWN** while powering on the Thumby Color
(BOOTSEL mode); the device mounts as `RPI-RP2350`. Drop the `.uf2`
on it. Power-cycle.

### ThumbyOne slot build

ThumbyCraft builds as a slot inside the unified ThumbyOne firmware.
See [`docs/THUMBYONE_INTEGRATION.md`](docs/THUMBYONE_INTEGRATION.md)
for the (small) ThumbyOne-side edits.

## Performance notes

| Hot path | Cost | Mitigation |
|---|---|---|
| Per-pixel DDA | up to 64 steps × 16 384 pixels/frame | incremental idx; SRAM-resident; -O3 |
| Mob render | 17 parts × screen-bbox pixels | parts sorted by volume, slab early-out |
| Window shift | 1024-column regen on a 16-cell slide | ~7 ms, fits in a frame |
| Chunk-store flash write | ~10 ms per sector + ~10 ms erase | only on chunk eviction (every ~16 cells of walking) |
| Light flood | BFS to radius 6 | runs on torch place/break only |
| Audio render | 4 voices × N samples per IRQ | int16 + table sine, soft clamp |

Current frame budget at ~30 fps: ~33 ms / frame, with the raycaster
dominating at ~20 ms across both cores.

## Roadmap

See [`docs/ROADMAP.md`](docs/ROADMAP.md) for the prioritised feature
queue (next-three: more tools incl. bow, furnace UI, chest storage),
the planned biomes / mob loot / armour / multiplayer tiers, and the
quality bar (BSS budget, FPS floor, save-format discipline).

---

## License

The Pemsa-derived font table inherits MIT. The rest is original
(MIT-equivalent, header attribution).

## Acknowledgements

- *Minecraft 4K* (Markus Persson, 2010) — the reference raycaster
  this riffs on
- *Minecraft* (Mojang) — the gameplay vocabulary
- ThumbyDOOM / ThumbyNES — the bare-metal RP2350 driver patterns
  (LCD init, PWM audio, button reader) that ThumbyCraft lifts wholesale
- TinyCircuits — the Thumby Color hardware and engine ecosystem
- Pico SDK — clocks, DMA, multicore, flash APIs

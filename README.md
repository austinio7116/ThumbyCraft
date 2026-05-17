# ThumbyCraft

A bare-metal block-world voxel game for the [Thumby Color](https://thumby.us/)
(RP2350, 128×128 RGB565). Inspired by *Minecraft 4K* — a DDA voxel
raycaster that renders a 64³ block world in real time at ~30 fps on
a 280 MHz dual-core Cortex-M33 with no GPU.

## Status

Working features:

- 64³ block world, 11 block types, procedurally-generated terrain
  (heightmap + trees + ocean + sand beaches) from a 32-bit seed
- DDA voxel raycaster, dual-core split (core1 = top half, core0 =
  bottom half)
- Per-face shading, water transparency, distance fog
- Place / break blocks with reach limit, 8-slot hotbar
- AABB physics: gravity, jump, ground / wall collisions; toggleable
  creative-mode flight
- Procedural audio: distinct break/place tones per block, ambient
  drone, footsteps + jump
- Flash-backed saves: delta-against-base encoding, 4-sector wear
  ring, CRC32-protected; on host saves to `./thumbycraft.sav`
- Host SDL2 build for fast iteration; identical engine sources to
  the device build

## Build

### Host (Linux/macOS — fast iteration)

```bash
sudo apt install libsdl2-dev
cmake -S host -B build_host && cmake --build build_host -j8
./build_host/thumbycraft_host [seed]
```

Keyboard map: arrows / WSAD = D-pad, Z = A, X = B, LShift = LB,
Return = RB, Esc = MENU. F1 toggles fog, F5 saves, F9 loads.

### Device (RP2350 — bare-metal firmware)

```bash
sudo apt install gcc-arm-none-eabi libnewlib-arm-none-eabi
cmake -S device -B build_device \
    -DPICO_SDK_PATH=/path/to/pico-sdk
cmake --build build_device -j8
cp build_device/thumbycraft.uf2 firmware_thumbycraft.uf2
```

Flash: hold DOWN on power-up, drop the `.uf2` onto the `RPI-RP2350`
USB drive.

### ThumbyOne slot

ThumbyCraft already builds as a ThumbyOne slot — see
[`docs/THUMBYONE_INTEGRATION.md`](docs/THUMBYONE_INTEGRATION.md) for
the (small) ThumbyOne-side edits required.

## Controls (v3)

| Button | Action |
|---|---|
| D-pad L/R | Turn left / right |
| D-pad U/D | Walk forward / back (or pitch in look mode) |
| A | Break block at crosshair |
| B | Place selected block |
| RB | Jump (walk mode) / Ascend (fly mode, held) |
| LB (hold) | Look mode — D-pad U/D pitches camera |
| LB (tap) | Toggle sticky look — D-pad keeps pitching until tapped again |
| MENU (release without chord) | Open pause menu |
| MENU + LB / RB | Hotbar prev / next |
| MENU + A | Toggle fly mode (also available from pause menu) |

Starts in **walk mode** (gravity on). Walk mode auto-steps onto
1-block-tall obstacles (no jump needed for that). Fly mode disables
gravity and makes D-pad forward follow the full camera direction —
pitch up + forward = ascend, pitch down + forward = descend.

### Pause menu

- D-pad U/D — navigate
- A — confirm
- B or MENU — close
- Items: Resume / Inventory (stub) / Save world / Load world / Toggle fly /
  New world / Settings (stub)

## Architecture

Source layout:

```
src/                    portable engine (host + device share these)
  craft_types.h         shared types (Vec3, rgb565)
  craft_blocks.{c,h}    block table + procedural texture atlas
  craft_world.{c,h}     monolithic 64³ block array + get/set
  craft_gen.{c,h}       pure-function terrain (heightmap + trees)
  craft_render.{c,h}    DDA voxel raycaster + strip variant
  craft_player.{c,h}    camera, controls, AABB physics, hotbar
  craft_hud.{c,h}       crosshair, hotbar strip, FPS counter
  craft_audio.{c,h}     4-voice procedural synth (square/tri/noise)
  craft_save.{c,h}      diff-vs-base persistence (engine-side)
  craft_font.{c,h}      3×5 bitmap text (lifted from Pemsa via ThumbyDOOM)
  craft_main.{c,h}      game loop glue, exposes tick/render/HUD split

host/                   SDL2 platform layer
  host_main.c
  CMakeLists.txt

device/                 RP2350 bare-metal platform layer
  craft_device_main.c   boot splash, dual-core dispatch, game loop
  craft_lcd_gc9107.{c,h}  SPI + DMA LCD driver
  craft_buttons.{c,h}     GPIO button reader
  craft_audio_pwm.{c,h}   PWM + IRQ audio ring
  craft_save_flash.{c,h}  4-sector wear ring at top of slot
  CMakeLists.txt
```

Memory budget (device):

| Item | Size |
|---|---|
| World (64³ × 1 B) | 256 KB |
| Pico SDK heap | 64 KB |
| Framebuffer | 32 KB |
| Texture atlas (11 blocks × 3 faces × 16² × 2 B) | ~17 KB |
| Audio ring + voices | ~9 KB |
| Pico SDK BSS + .data + 2× stacks | ~30 KB |
| **Total resident** | **~408 KB** |

…out of 520 KB available. Headroom for 16-bit lighting tables, a
larger draw distance, or denser audio later.

## License

The Pemsa-derived font table inherits MIT. The rest is original
(MIT-equivalent, header attribution).

## Acknowledgements

- *Minecraft 4K* (Markus Persson, 2010) — the reference raycaster
  this riffs on
- ThumbyDOOM / ThumbyNES — the bare-metal RP2350 driver patterns
  (LCD init, PWM audio, button reader) that ThumbyCraft lifts
  wholesale
- TinyCircuits — the Thumby Color hardware

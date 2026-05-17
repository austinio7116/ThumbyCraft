# Adding ThumbyCraft as a ThumbyOne slot

ThumbyCraft already builds cleanly in slot mode (`-DTHUMBYONE_SLOT_MODE=1`).
This document captures the exact edits ThumbyOne needs on its side to
pick up that slot. None of these edits touch ThumbyCraft itself.

## 1. Slot enable + ID — `common/slot_layout.h`

```c
#ifndef THUMBYONE_WITH_CRAFT
#  define THUMBYONE_WITH_CRAFT 1
#endif

typedef enum {
    THUMBYONE_SLOT_LOBBY = 0x0,
    THUMBYONE_SLOT_NES   = 0x1,
    THUMBYONE_SLOT_P8    = 0x2,
    THUMBYONE_SLOT_DOOM  = 0x3,
    THUMBYONE_SLOT_MPY   = 0x4,
    THUMBYONE_SLOT_SCUMM = 0x5,
    THUMBYONE_SLOT_CRAFT = 0x6,
    THUMBYONE_SLOT_COUNT = 0x7
} thumbyone_slot_t;
```

Add CRAFT to `thumbyone_slot_partition_id`:

```c
    if (s == THUMBYONE_SLOT_CRAFT) return THUMBYONE_WITH_CRAFT ? idx : -1;
    if (THUMBYONE_WITH_CRAFT) idx++;
```

## 2. Slot size + offset block

ThumbyCraft's measured footprint:
- `.text` ≈ 235 KB
- `.bss`  ≈ 330 KB (256 KB world + 64 KB heap + 32 KB FB + misc)
- UF2    ≈ 80 KB

```c
#define THUMBYONE_CRAFT_SIZE (512u * 1024u)   /* 512 KB headroom */
```

Insert into the "Computed offsets" block right after SCUMM, and add
`SLOT_CRAFT_SAVE_OFFSET` for the save wear-ring (top 16 KB of the
slot region).

## 3. Lobby icon

Drop a 48×48 PNG at `lobby/icons/craft.png` (a green grass block is
the obvious choice). `tools/pack_icons.py` will pick it up
automatically the next build.

## 4. Top-level CMake — pull in the ThumbyCraft slot

In ThumbyOne's top-level `CMakeLists.txt`:

```cmake
if (THUMBYONE_WITH_CRAFT)
    set(THUMBYONE_SLOT_MODE 1)
    add_subdirectory(../ThumbyCraft/device thumbycraft_slot)
    target_link_libraries(thumbycraft_slot_obj_lib
        common_thumbyone
        # ... whatever the existing slots link with
    )
endif()
```

The `thumbycraft_slot` OBJECT library exports the slot sources; the
parent's linker stitches them into the unified UF2 with their own
partition entry.

## 5. Partition table — `tools/gen_pt.py`

Add an entry for the CRAFT slot at index N, named "ThumbyCraft",
sized `THUMBYONE_CRAFT_SIZE`. Mirrors the existing SCUMM entry.

## 6. Lobby dispatch

Add `THUMBYONE_SLOT_CRAFT` to the lobby's tab list with the new icon
and the existing `dispatch_to_slot(THUMBYONE_SLOT_CRAFT, ...)` call.

---

That's the entire integration surface. ThumbyCraft itself needs no
changes beyond what's already there:

- `device/CMakeLists.txt` already has the `THUMBYONE_SLOT_MODE`
  branch that emits the `thumbycraft_slot` OBJECT library.
- `device/craft_device_main.c` already calls
  `thumbyone_slot_init_brightness_and_led` and
  `thumbyone_led_set_rgb` under the same guard.
- `device/craft_lcd_gc9107.c` already uses the shared PIO-PWM
  backlight + `/.brightness` sector under the same guard.
- `device/craft_save_flash.c` already reads
  `SLOT_CRAFT_SAVE_OFFSET` from `slot_layout.h` when in slot mode,
  falling back to a standalone offset otherwise.

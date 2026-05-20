#
# ThumbyCraft — device firmware build (RP2350, Thumby Color).
#
# Standalone produces a UF2 you can drop onto the RPI-RP2350 drive.
# THUMBYONE_SLOT_MODE wraps the same sources as an embedded slot
# inside the unified ThumbyOne firmware.
#
# Build (standalone):
#   cmake -S device -B build_device -DPICO_SDK_PATH=$HOME/pico-sdk
#   cmake --build build_device -j8
#   cp build_device/thumbycraft.uf2 ../firmware_thumbycraft.uf2
#
cmake_minimum_required(VERSION 3.16)

if (NOT THUMBYONE_SLOT_MODE)
    set(PICO_BOARD tinycircuits_thumby_color_rp2350)
    set(PICO_PLATFORM rp2350)
    include(${PICO_SDK_PATH}/pico_sdk_init.cmake)

    project(thumbycraft_device C CXX ASM)
    set(CMAKE_C_STANDARD 11)
    set(CMAKE_CXX_STANDARD 14)

    pico_sdk_init()
endif()

set(CRAFT_ROOT  ${CMAKE_CURRENT_LIST_DIR}/..)
set(SRC         ${CRAFT_ROOT}/src)
set(DEVICE      ${CMAKE_CURRENT_LIST_DIR})

# Bake the procedural texture atlas to const flash data — frees ~32 KB
# of SRAM that the lightmap would otherwise have to fight for. Built
# as a native host tool via a sub-CMake invocation so the cross
# toolchain doesn't apply.
include(ExternalProject)
ExternalProject_Add(thumbycraft_baker
    SOURCE_DIR ${CRAFT_ROOT}/tools
    BINARY_DIR ${CMAKE_BINARY_DIR}/baker
    CMAKE_ARGS
        -DCMAKE_BUILD_TYPE=Release
        -DCMAKE_C_COMPILER=cc
        -DCMAKE_SYSTEM_NAME=
        -DCMAKE_SYSTEM_PROCESSOR=
        -DCMAKE_C_FLAGS=
        -DCMAKE_EXE_LINKER_FLAGS=
    INSTALL_COMMAND ""
    BUILD_ALWAYS 1
    BUILD_BYPRODUCTS ${CMAKE_BINARY_DIR}/baker/bake_textures
)
set(CRAFT_BAKED_C ${CMAKE_BINARY_DIR}/generated/craft_textures_baked.c)
add_custom_command(
    OUTPUT ${CRAFT_BAKED_C}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/generated
    COMMAND ${CMAKE_BINARY_DIR}/baker/bake_textures ${CRAFT_BAKED_C}
    DEPENDS thumbycraft_baker
            ${SRC}/craft_blocks.c
            ${SRC}/craft_blocks.h
    COMMENT "Baking craft_textures atlas to flash"
)
add_custom_target(generate_craft_textures DEPENDS ${CRAFT_BAKED_C})

# Storage backend selection. Standalone uses raw flash regions
# (slot_layout.h offsets); ThumbyOne slot mode uses FatFs files on
# the shared FAT volume so the data survives reflashing the slot.
if (THUMBYONE_SLOT_MODE)
    set(CRAFT_STORAGE_SRC
        ${DEVICE}/craft_save_fatfs.c
        ${DEVICE}/craft_chunk_store_fatfs.c
    )
else()
    set(CRAFT_STORAGE_SRC
        ${DEVICE}/craft_save_flash.c
        ${DEVICE}/craft_chunk_store_flash.c
    )
endif()

set(CRAFT_DEVICE_SOURCES
    ${DEVICE}/craft_device_main.c
    ${DEVICE}/craft_lcd_gc9107.c
    ${DEVICE}/craft_buttons.c
    ${DEVICE}/craft_audio_pwm.c
    ${CRAFT_STORAGE_SRC}

    ${SRC}/craft_world.c
    ${SRC}/craft_blocks.c
    ${SRC}/craft_gen.c
    ${SRC}/craft_render.c
    ${SRC}/craft_player.c
    ${SRC}/craft_hud.c
    ${SRC}/craft_audio.c
    ${SRC}/craft_save.c
    ${SRC}/craft_font.c
    ${SRC}/craft_menu.c
    ${SRC}/craft_mobs.c
    ${SRC}/craft_particles.c
    ${SRC}/craft_torches.c
    ${SRC}/craft_drops.c
    ${SRC}/craft_tool_models.c
    ${SRC}/craft_furnace.c
    ${SRC}/craft_chests.c
    ${SRC}/craft_water.c
    ${SRC}/craft_redstone.c
    ${SRC}/craft_title.c
    ${SRC}/craft_main.c
    ${CRAFT_BAKED_C}
)

# In slot mode, pull in FatFs sources + ThumbyOne's shared helpers
# (FS mount, LED + backlight + settings for the brightness chord).
# Same executable shape as standalone — produces firmware_thumbycraft.uf2
# which the top-level lobby build rebases to THUMBYONE_CRAFT_OFFSET
# and combines into firmware_thumbyone.uf2.
if (THUMBYONE_SLOT_MODE)
    list(APPEND CRAFT_DEVICE_SOURCES
        ${THUMBYONE_COMMON_SRC}          # thumbyone_handoff.c (xip_fast_setup)
        ${THUMBYONE_FATFS_SRC}
        ${THUMBYONE_COMMON_FS_SRC}
        ${THUMBYONE_COMMON_LED_SRC}
        ${THUMBYONE_COMMON_BACKLIGHT_SRC}
        ${THUMBYONE_COMMON_SETTINGS_SRC}
    )
endif()

add_executable(thumbycraft ${CRAFT_DEVICE_SOURCES})

target_include_directories(thumbycraft PRIVATE
    ${SRC}
    ${DEVICE}
)
if (THUMBYONE_SLOT_MODE)
    target_include_directories(thumbycraft PRIVATE
        ${THUMBYONE_FATFS_INCLUDE}
        ${THUMBYONE_COMMON_INCLUDE}
        ${THUMBYONE_COMMON_LED_INCLUDE}
        ${THUMBYONE_COMMON_BACKLIGHT_INCLUDE}
        ${THUMBYONE_COMMON_SETTINGS_INCLUDE}
    )
endif()

target_compile_options(thumbycraft PRIVATE
    -O3 -g
    -Wall -Wextra
    -Wno-unused-parameter -Wno-unused-function -Wno-unused-but-set-variable
    -ffast-math
    -mcpu=cortex-m33 -mthumb -mfloat-abi=softfp -mfpu=fpv5-sp-d16
)

target_link_libraries(thumbycraft
    pico_stdlib
    pico_multicore
    pico_rand
    hardware_spi
    hardware_dma
    hardware_pwm
    hardware_irq
    hardware_flash
    hardware_sync
    hardware_clocks
    hardware_gpio
)
if (THUMBYONE_SLOT_MODE)
    target_link_libraries(thumbycraft hardware_pio hardware_adc)
endif()

if (THUMBYONE_SLOT_MODE)
    # Slot mode: FatFs caches + ~10 KB shared helpers eat into the
    # SRAM budget that standalone gives to the heap. Cut heap to 8 KB —
    # the engine itself barely uses heap; this leaves enough for
    # multicore_lockout's allocation while keeping us under the
    # 520 KB SRAM ceiling.
    set(CRAFT_HEAP_SIZE 0x2000)
else()
    set(CRAFT_HEAP_SIZE 0x10000)
endif()

target_compile_definitions(thumbycraft PRIVATE
    PICO_HEAP_SIZE=${CRAFT_HEAP_SIZE}
    PICO_STACK_SIZE=0x1000
    PICO_CORE1_STACK_SIZE=0x1000
    CRAFT_TEXTURES_BAKED=1
    $<$<BOOL:${THUMBYONE_SLOT_MODE}>:THUMBYONE_SLOT_MODE=1>
)
add_dependencies(thumbycraft generate_craft_textures)

pico_enable_stdio_usb(thumbycraft 0)
pico_enable_stdio_uart(thumbycraft 0)

# Slot mode: switch the UF2 family from the per-board default to
# "absolute" so the lobby's chain_image path can load us regardless
# of where the rebase put us in flash. Standalone keeps the default.
if (THUMBYONE_SLOT_MODE)
    pico_set_uf2_family(thumbycraft "absolute")
endif()

pico_add_extra_outputs(thumbycraft)

# Rename the UF2 to firmware_thumbycraft.uf2 so the parent ThumbyOne
# combiner (which expects this filename in craft_slot/) picks it up.
set_target_properties(thumbycraft PROPERTIES OUTPUT_NAME "firmware_thumbycraft")

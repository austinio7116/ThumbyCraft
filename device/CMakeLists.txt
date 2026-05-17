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

set(CRAFT_DEVICE_SOURCES
    ${DEVICE}/craft_device_main.c
    ${DEVICE}/craft_lcd_gc9107.c
    ${DEVICE}/craft_buttons.c
    ${DEVICE}/craft_audio_pwm.c
    ${DEVICE}/craft_save_flash.c

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
    ${SRC}/craft_main.c
)

# Standalone target. In slot mode, ThumbyOne's top-level CMakeLists
# is responsible for linking our sources into its own executable.
if (NOT THUMBYONE_SLOT_MODE)
    add_executable(thumbycraft ${CRAFT_DEVICE_SOURCES})

    target_include_directories(thumbycraft PRIVATE
        ${SRC}
        ${DEVICE}
    )

    target_compile_options(thumbycraft PRIVATE
        -O3 -g
        -Wall -Wextra
        -Wno-unused-parameter -Wno-unused-function -Wno-unused-but-set-variable
        -ffast-math
        # The raycaster wants the FPU front-and-centre.
        -mcpu=cortex-m33 -mthumb -mfloat-abi=softfp -mfpu=fpv5-sp-d16
    )

    target_link_libraries(thumbycraft
        pico_stdlib
        pico_multicore
        hardware_spi
        hardware_dma
        hardware_pwm
        hardware_irq
        hardware_flash
        hardware_sync
        hardware_clocks
        hardware_gpio
    )

    # Generous heap — the engine itself doesn't malloc much but
    # multicore_lockout uses a chunk and we want headroom for
    # future paged-chunk features.
    target_compile_definitions(thumbycraft PRIVATE
        PICO_HEAP_SIZE=0x10000      # 64 KB
        PICO_STACK_SIZE=0x1000      # 4 KB per core (default 2 KB doubled)
        PICO_CORE1_STACK_SIZE=0x1000
    )

    # USB CDC stdio is unreliable on Thumby Color — leave it off.
    pico_enable_stdio_usb(thumbycraft 0)
    pico_enable_stdio_uart(thumbycraft 0)

    pico_add_extra_outputs(thumbycraft)
else()
    # ThumbyOne slot consumption: expose our sources and include dirs
    # as an OBJECT library named thumbycraft_slot which the parent
    # links into its top-level executable.
    add_library(thumbycraft_slot OBJECT ${CRAFT_DEVICE_SOURCES})
    target_include_directories(thumbycraft_slot PUBLIC
        ${SRC}
        ${DEVICE}
    )
    target_compile_definitions(thumbycraft_slot PUBLIC THUMBYONE_SLOT_MODE=1)
    target_compile_options(thumbycraft_slot PRIVATE
        -O3 -ffast-math
        -Wno-unused-parameter -Wno-unused-function -Wno-unused-but-set-variable
    )
endif()

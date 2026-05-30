#
# ThumbyCraft — host (Linux/macOS/SDL2) build.
#
# Compiles the engine plus an SDL2-backed shell so we can iterate on
# the renderer + gameplay without flashing hardware. The engine
# sources (../src/*.c) are identical between host and device — only
# the platform layer differs.
#
# Build:
#   cmake -B build_host -S host
#   cmake --build build_host -j8
#   ./build_host/thumbycraft_host [seed]
#
cmake_minimum_required(VERSION 3.16)
project(thumbycraft_host C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(SDL2 REQUIRED)

set(CRAFT_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)
set(SRC ${CRAFT_ROOT}/src)

# Bake the procedural texture atlas to const data — mirrors the
# device path so host + device exercise identical code (and host can
# still iterate on texture code by re-running the baker).
include(ExternalProject)
ExternalProject_Add(thumbycraft_baker
    SOURCE_DIR ${CRAFT_ROOT}/tools
    BINARY_DIR ${CMAKE_BINARY_DIR}/baker
    CMAKE_ARGS -DCMAKE_BUILD_TYPE=Release
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
    COMMENT "Baking craft_textures atlas"
)
add_custom_target(generate_craft_textures DEPENDS ${CRAFT_BAKED_C})

add_executable(thumbycraft_host
    ${CMAKE_CURRENT_LIST_DIR}/host_main.c
    ${CMAKE_CURRENT_LIST_DIR}/craft_chunk_store_stub.c
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
    ${SRC}/craft_lava.c
    ${SRC}/craft_redstone.c
    ${SRC}/craft_title.c
    ${SRC}/craft_main.c
    ${CRAFT_BAKED_C}
)
add_dependencies(thumbycraft_host generate_craft_textures)
target_compile_definitions(thumbycraft_host PRIVATE CRAFT_TEXTURES_BAKED=1 CRAFT_HOST=1)

target_include_directories(thumbycraft_host PRIVATE
    ${SRC}
    ${SDL2_INCLUDE_DIRS}
)

# Headless streaming-equivalence harness — same engine sources, no SDL
# window. Swaps host_main.c for the verifier. Run: ./build_host/stream_verify
add_executable(stream_verify
    ${CMAKE_CURRENT_LIST_DIR}/stream_verify.c
    ${CMAKE_CURRENT_LIST_DIR}/craft_chunk_store_stub.c
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
    ${SRC}/craft_lava.c
    ${SRC}/craft_redstone.c
    ${SRC}/craft_title.c
    ${SRC}/craft_main.c
    ${CRAFT_BAKED_C}
)
add_dependencies(stream_verify generate_craft_textures)
target_compile_definitions(stream_verify PRIVATE CRAFT_TEXTURES_BAKED=1)
target_include_directories(stream_verify PRIVATE ${SRC} ${SDL2_INCLUDE_DIRS})
target_compile_options(stream_verify PRIVATE
    -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -ffast-math)
target_link_libraries(stream_verify PRIVATE ${SDL2_LIBRARIES} m)

# Headless render profiler — built with CRAFT_PROFILE so trace_ray keeps
# DDA-step counters. Run: ./build_host/render_bench [seed] [iters]
add_executable(render_bench
    ${CMAKE_CURRENT_LIST_DIR}/render_bench.c
    ${CMAKE_CURRENT_LIST_DIR}/craft_chunk_store_stub.c
    ${SRC}/craft_world.c ${SRC}/craft_blocks.c ${SRC}/craft_gen.c
    ${SRC}/craft_render.c ${SRC}/craft_player.c ${SRC}/craft_hud.c
    ${SRC}/craft_audio.c ${SRC}/craft_save.c ${SRC}/craft_font.c
    ${SRC}/craft_menu.c ${SRC}/craft_mobs.c ${SRC}/craft_particles.c
    ${SRC}/craft_torches.c ${SRC}/craft_drops.c ${SRC}/craft_tool_models.c
    ${SRC}/craft_furnace.c ${SRC}/craft_chests.c ${SRC}/craft_water.c
    ${SRC}/craft_lava.c ${SRC}/craft_redstone.c ${SRC}/craft_title.c
    ${SRC}/craft_main.c ${CRAFT_BAKED_C}
)
add_dependencies(render_bench generate_craft_textures)
target_compile_definitions(render_bench PRIVATE CRAFT_TEXTURES_BAKED=1 CRAFT_PROFILE=1)
target_include_directories(render_bench PRIVATE ${SRC} ${SDL2_INCLUDE_DIRS})
target_compile_options(render_bench PRIVATE
    -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -ffast-math)
target_link_libraries(render_bench PRIVATE ${SDL2_LIBRARIES} m)

# Headless biome screenshot harness — renders one biome to a BMP.
# Run: ./build_host/craft_shot <biome 0-7> [seed] [out.bmp]
add_executable(craft_shot
    ${CMAKE_CURRENT_LIST_DIR}/craft_shot.c
    ${CMAKE_CURRENT_LIST_DIR}/craft_chunk_store_stub.c
    ${SRC}/craft_world.c ${SRC}/craft_blocks.c ${SRC}/craft_gen.c
    ${SRC}/craft_render.c ${SRC}/craft_player.c ${SRC}/craft_hud.c
    ${SRC}/craft_audio.c ${SRC}/craft_save.c ${SRC}/craft_font.c
    ${SRC}/craft_menu.c ${SRC}/craft_mobs.c ${SRC}/craft_particles.c
    ${SRC}/craft_torches.c ${SRC}/craft_drops.c ${SRC}/craft_tool_models.c
    ${SRC}/craft_furnace.c ${SRC}/craft_chests.c ${SRC}/craft_water.c
    ${SRC}/craft_lava.c ${SRC}/craft_redstone.c ${SRC}/craft_title.c
    ${SRC}/craft_main.c ${CRAFT_BAKED_C}
)
add_dependencies(craft_shot generate_craft_textures)
target_compile_definitions(craft_shot PRIVATE CRAFT_TEXTURES_BAKED=1)
target_include_directories(craft_shot PRIVATE ${SRC} ${SDL2_INCLUDE_DIRS})
target_compile_options(craft_shot PRIVATE
    -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -ffast-math)
target_link_libraries(craft_shot PRIVATE ${SDL2_LIBRARIES} m)

# Headless texture-atlas dump — raw block tiles per biome (no render).
# Run: ./build_host/craft_atlas <out_dir>
add_executable(craft_atlas
    ${CMAKE_CURRENT_LIST_DIR}/craft_atlas.c
    ${SRC}/craft_blocks.c ${CRAFT_BAKED_C}
)
add_dependencies(craft_atlas generate_craft_textures)
target_compile_definitions(craft_atlas PRIVATE CRAFT_TEXTURES_BAKED=1)
target_include_directories(craft_atlas PRIVATE ${SRC} ${SDL2_INCLUDE_DIRS})
target_compile_options(craft_atlas PRIVATE
    -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -ffast-math)
target_link_libraries(craft_atlas PRIVATE m)

# Headless greens-chooser — grass-top under candidate biome tints.
add_executable(craft_greens
    ${CMAKE_CURRENT_LIST_DIR}/craft_greens.c
    ${SRC}/craft_blocks.c ${CRAFT_BAKED_C}
)
add_dependencies(craft_greens generate_craft_textures)
target_compile_definitions(craft_greens PRIVATE CRAFT_TEXTURES_BAKED=1)
target_include_directories(craft_greens PRIVATE ${SRC} ${SDL2_INCLUDE_DIRS})
target_compile_options(craft_greens PRIVATE
    -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -ffast-math)
target_link_libraries(craft_greens PRIVATE m)

# Headless frame-phase profiler — times each render phase per scene.
add_executable(craft_profile
    ${CMAKE_CURRENT_LIST_DIR}/craft_profile.c
    ${CMAKE_CURRENT_LIST_DIR}/craft_chunk_store_stub.c
    ${SRC}/craft_world.c ${SRC}/craft_blocks.c ${SRC}/craft_gen.c
    ${SRC}/craft_render.c ${SRC}/craft_player.c ${SRC}/craft_hud.c
    ${SRC}/craft_audio.c ${SRC}/craft_save.c ${SRC}/craft_font.c
    ${SRC}/craft_menu.c ${SRC}/craft_mobs.c ${SRC}/craft_particles.c
    ${SRC}/craft_torches.c ${SRC}/craft_drops.c ${SRC}/craft_tool_models.c
    ${SRC}/craft_furnace.c ${SRC}/craft_chests.c ${SRC}/craft_water.c
    ${SRC}/craft_lava.c ${SRC}/craft_redstone.c ${SRC}/craft_title.c
    ${SRC}/craft_main.c ${CRAFT_BAKED_C}
)
add_dependencies(craft_profile generate_craft_textures)
target_compile_definitions(craft_profile PRIVATE CRAFT_TEXTURES_BAKED=1 CRAFT_PROFILE=1)
target_include_directories(craft_profile PRIVATE ${SRC} ${SDL2_INCLUDE_DIRS})
target_compile_options(craft_profile PRIVATE
    -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -ffast-math)
target_link_libraries(craft_profile PRIVATE ${SDL2_LIBRARIES} m)

# Headless flowering-vine + blossom-leaf chooser.
add_executable(craft_blossom
    ${CMAKE_CURRENT_LIST_DIR}/craft_blossom.c
    ${SRC}/craft_blocks.c ${CRAFT_BAKED_C}
)
add_dependencies(craft_blossom generate_craft_textures)
target_compile_definitions(craft_blossom PRIVATE CRAFT_TEXTURES_BAKED=1)
target_include_directories(craft_blossom PRIVATE ${SRC} ${SDL2_INCLUDE_DIRS})
target_compile_options(craft_blossom PRIVATE
    -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -ffast-math)
target_link_libraries(craft_blossom PRIVATE m)

# Headless grass-top candidate chooser.
add_executable(craft_grass
    ${CMAKE_CURRENT_LIST_DIR}/craft_grass.c
    ${SRC}/craft_blocks.c ${CRAFT_BAKED_C}
)
add_dependencies(craft_grass generate_craft_textures)
target_compile_definitions(craft_grass PRIVATE CRAFT_TEXTURES_BAKED=1)
target_include_directories(craft_grass PRIVATE ${SRC} ${SDL2_INCLUDE_DIRS})
target_compile_options(craft_grass PRIVATE
    -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -ffast-math)
target_link_libraries(craft_grass PRIVATE m)

# Headless leaf-density chooser.
add_executable(craft_leaves
    ${CMAKE_CURRENT_LIST_DIR}/craft_leaves.c
    ${SRC}/craft_blocks.c ${CRAFT_BAKED_C}
)
add_dependencies(craft_leaves generate_craft_textures)
target_compile_definitions(craft_leaves PRIVATE CRAFT_TEXTURES_BAKED=1)
target_include_directories(craft_leaves PRIVATE ${SRC} ${SDL2_INCLUDE_DIRS})
target_compile_options(craft_leaves PRIVATE
    -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -ffast-math)
target_link_libraries(craft_leaves PRIVATE m)

target_compile_options(thumbycraft_host PRIVATE
    -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
    -ffast-math
)

target_link_libraries(thumbycraft_host PRIVATE ${SDL2_LIBRARIES} m)

# Raw Xlib for the WSLg/XWayland mouse-look (XWarpPointer/XQueryPointer).
# Linux only; degrades to a no-op fallback elsewhere.
if(UNIX AND NOT APPLE)
    find_package(X11)
    if(X11_FOUND)
        target_compile_definitions(thumbycraft_host PRIVATE CRAFT_HOST_X11=1)
        target_include_directories(thumbycraft_host PRIVATE ${X11_INCLUDE_DIR})
        target_link_libraries(thumbycraft_host PRIVATE ${X11_LIBRARIES})
    endif()
endif()

add_executable(redstone_test
    ${CMAKE_CURRENT_LIST_DIR}/redstone_test.c
    ${CMAKE_CURRENT_LIST_DIR}/craft_chunk_store_stub.c
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
    ${SRC}/craft_lava.c
    ${SRC}/craft_redstone.c
    ${SRC}/craft_title.c
    ${SRC}/craft_main.c
    ${CRAFT_BAKED_C}
)
add_dependencies(redstone_test generate_craft_textures)
target_compile_definitions(redstone_test PRIVATE CRAFT_TEXTURES_BAKED=1)
target_include_directories(redstone_test PRIVATE ${SRC} ${SDL2_INCLUDE_DIRS})
target_compile_options(redstone_test PRIVATE
    -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -ffast-math)
target_link_libraries(redstone_test PRIVATE ${SDL2_LIBRARIES} m)

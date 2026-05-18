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
    ${SRC}/craft_redstone.c
    ${SRC}/craft_main.c
    ${CRAFT_BAKED_C}
)
add_dependencies(thumbycraft_host generate_craft_textures)
target_compile_definitions(thumbycraft_host PRIVATE CRAFT_TEXTURES_BAKED=1)

target_include_directories(thumbycraft_host PRIVATE
    ${SRC}
    ${SDL2_INCLUDE_DIRS}
)

target_compile_options(thumbycraft_host PRIVATE
    -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
    -ffast-math
)

target_link_libraries(thumbycraft_host PRIVATE ${SDL2_LIBRARIES} m)

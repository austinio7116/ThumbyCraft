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

add_executable(thumbycraft_host
    ${CMAKE_CURRENT_LIST_DIR}/host_main.c
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

target_include_directories(thumbycraft_host PRIVATE
    ${SRC}
    ${SDL2_INCLUDE_DIRS}
)

target_compile_options(thumbycraft_host PRIVATE
    -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
    -ffast-math
)

target_link_libraries(thumbycraft_host PRIVATE ${SDL2_LIBRARIES} m)

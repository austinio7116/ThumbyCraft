#
# Texture-atlas baker — native host tool.
#
# Compiles craft_blocks.c in its unbaked configuration (writable BSS
# craft_textures + working build function) so the baker can run the
# procedural init and dump the result. The output .c file is then
# compiled into the device + host targets as const flash data.
#
# Built as a sub-project via ExternalProject_Add from device/ and
# host/ CMakeLists, with the cross-toolchain overridden so this
# always builds natively.
#
cmake_minimum_required(VERSION 3.16)
project(thumbycraft_baker C)
set(CMAKE_C_STANDARD 11)

set(CRAFT_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)
set(SRC ${CRAFT_ROOT}/src)

add_executable(bake_textures
    bake_textures.c
    ${SRC}/craft_blocks.c
)
target_include_directories(bake_textures PRIVATE ${SRC})
target_compile_options(bake_textures PRIVATE -O2 -Wall)
# Explicitly leave CRAFT_TEXTURES_BAKED undefined here so the baker
# uses the BSS storage path of craft_blocks.c.

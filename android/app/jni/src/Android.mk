LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := main

SDL_PATH := ../SDL
# Engine sources live in the repo's shared src/ (identical to host + device).
ENGINE   := $(LOCAL_PATH)/../../../../src
HOSTDIR  := $(LOCAL_PATH)/../../../../host

LOCAL_C_INCLUDES := \
    $(LOCAL_PATH)/$(SDL_PATH)/include \
    $(ENGINE)

# Mirror the host build: treat the Android shell as a "host" platform layer
# (SDL framebuffer rendering) with the texture atlas baked to const data.
#
# Android has far more headroom than the RP2350. Make the most of it (all of
# these are RP2350-default elsewhere; only this build overrides them):
#   - high-res wide framebuffer 512x256 (2:1 to fill a phone screen; 4x the
#     pixels of the device for a sharper world. NOTE: the engine HUD draws at
#     fixed pixel sizes, so it positions correctly but appears smaller at this
#     resolution; the on-screen touch controls are screen-space so unaffected);
#   - big in-memory world window 576x64x576 (~27 MB vs 256 KB) so a long
#     draw distance has blocks to show;
#   - raycaster reach 60 -> 540 blocks (steps raised to match).
# (World:draw-distance ratio is kept ~ the stock 64:60, just 9x the scale.)
LOCAL_CFLAGS := -DCRAFT_HOST=1 -DCRAFT_TEXTURES_BAKED=1 -DNDEBUG \
                -O3 -ffast-math -flto -std=c11 \
                -DCRAFT_FB_W=512 -DCRAFT_FB_H=256 \
                -DCRAFT_HUD_SCALE=2 \
                -DCRAFT_WORLD_X=576 -DCRAFT_WORLD_Z=576 \
                -DCRAFT_MAX_DIST=540.0f -DCRAFT_MAX_STEPS=1024 \
                -include stdio.h -include stdlib.h -include string.h -include math.h
# Link-time optimisation inlines hot calls across the engine's translation
# units (the raycaster reaches into several files) — needs the flag at link too.
LOCAL_LDFLAGS += -flto -O3
# NOTE: CRAFT_MAX_DIST_FOR_ZBUF is deliberately NOT raised with the draw
# distance. The depth buffer is 8-bit; entity (cuboid) occlusion only matters
# in the near field, so it keeps the stock 60-block range for fine precision.
# Scaling it to the draw distance makes ~2 blocks/level and mobs flicker/tear.

LOCAL_SRC_FILES := \
    android_main.c \
    generated/craft_textures_baked.c \
    $(HOSTDIR)/craft_chunk_store_stub.c \
    $(ENGINE)/craft_world.c \
    $(ENGINE)/craft_blocks.c \
    $(ENGINE)/craft_gen.c \
    $(ENGINE)/craft_render.c \
    $(ENGINE)/craft_player.c \
    $(ENGINE)/craft_hud.c \
    $(ENGINE)/craft_audio.c \
    $(ENGINE)/craft_save.c \
    $(ENGINE)/craft_font.c \
    $(ENGINE)/craft_menu.c \
    $(ENGINE)/craft_mobs.c \
    $(ENGINE)/craft_particles.c \
    $(ENGINE)/craft_torches.c \
    $(ENGINE)/craft_drops.c \
    $(ENGINE)/craft_tool_models.c \
    $(ENGINE)/craft_furnace.c \
    $(ENGINE)/craft_chests.c \
    $(ENGINE)/craft_water.c \
    $(ENGINE)/craft_lava.c \
    $(ENGINE)/craft_redstone.c \
    $(ENGINE)/craft_title.c \
    $(ENGINE)/craft_main.c

LOCAL_SHARED_LIBRARIES := SDL2

LOCAL_LDLIBS := -lGLESv1_CM -lGLESv2 -lOpenSLES -llog -landroid

include $(BUILD_SHARED_LIBRARY)

# ThumbyCraft — Android port

An SDL2 shell around the same shared engine (`../src/*.c`) used by the host and
RP2350 device builds. Renders the engine framebuffer scaled to the phone screen
with on-screen touch controls (left thumb = move stick, right thumb = look,
plus action buttons for mine / place / jump / menu / hotbar).

The engine is unchanged from the device — Android-specific tuning (wide
framebuffer, larger world window, longer draw distance) is applied purely via
compile-time `-D` overrides in `app/jni/src/Android.mk`, so the RP2350 build is
untouched.

## Prerequisites

- Android SDK (platform 35, build-tools 34+), NDK r26+, and a JDK 17+.
- `local.properties` with `sdk.dir=/path/to/android-sdk` (not committed).

## One-time setup

Vendor the SDL2 source (not committed — it's large):

```bash
git clone --depth 1 --branch SDL2 https://github.com/libsdl-org/SDL.git app/jni/SDL
```

## Build

```bash
ANDROID_HOME=/path/to/android-sdk ./gradlew assembleDebug
# → app/build/outputs/apk/debug/app-debug.apk
```

A pre-built debug APK is checked in at the repo root as `ThumbyCraft-debug.apk`
(arm64-v8a). Sideload it onto an arm64 phone to play without building.

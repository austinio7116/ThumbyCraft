#!/usr/bin/env bash
#
# Build a FunKey S / RG Nano OPK using the FunKey SDK.
#
# Usage:
#   FUNKEY_SDK_DIR=/path/to/FunKey-sdk-2.3.0 ./funkey/package-opk.sh
#
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
sdk="${FUNKEY_SDK_DIR:-}"
out_opk="${OPK_OUT:-$root/dist/funkey/ThumbyCraft.opk}"
build="$root/build_funkey"
stage="$build/opk/thumbycraft"
generated="$build/generated/craft_textures_baked.c"

if [[ -z "$sdk" ]]; then
    echo "Set FUNKEY_SDK_DIR=/path/to/FunKey-sdk-2.3.0" >&2
    exit 1
fi
if [[ ! -d "$sdk" ]]; then
    echo "FunKey SDK not found: $sdk" >&2
    exit 1
fi
if [[ -x "$sdk/relocate-sdk.sh" ]]; then
    "$sdk/relocate-sdk.sh"
fi

env_setup=""
if [[ -f "$sdk/environment-setup" ]]; then
    env_setup="$sdk/environment-setup"
else
    env_setup="$(find "$sdk" -maxdepth 2 -type f -name 'environment-setup*' | head -n 1)"
fi
if [[ -z "$env_setup" ]]; then
    echo "Could not find environment-setup* in $sdk" >&2
    exit 1
fi

# shellcheck disable=SC1090
. "$env_setup"

if [[ -z "${CC:-}" ]]; then
    echo "FunKey SDK environment did not set CC" >&2
    exit 1
fi
if ! command -v mksquashfs >/dev/null 2>&1; then
    echo "mksquashfs is required to build OPKs" >&2
    exit 1
fi

sdl_config="${SDL_CONFIG:-}"
if [[ -z "$sdl_config" ]]; then
    sdl_config="$(command -v sdl-config || true)"
fi
if [[ -z "$sdl_config" ]]; then
    echo "Could not find SDK sdl-config" >&2
    exit 1
fi

mkdir -p "$build" "$(dirname "$generated")" "$(dirname "$out_opk")"
env -u AR -u AS -u CC -u CFLAGS -u CPPFLAGS -u CXX -u CXXFLAGS -u LD -u LDFLAGS \
    cmake -B "$build/baker" -S "$root/tools" -DCMAKE_BUILD_TYPE=Release >/dev/null
env -u AR -u AS -u CC -u CFLAGS -u CPPFLAGS -u CXX -u CXXFLAGS -u LD -u LDFLAGS \
    cmake --build "$build/baker" --target bake_textures -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" >/dev/null
"$build/baker/bake_textures" "$generated"

mapfile -t engine_sources < <(find "$root/src" -maxdepth 1 -name '*.c' | sort)

sdl_cflags="$("$sdl_config" --cflags)"
sdl_libs="$("$sdl_config" --libs)"
bin="$build/thumbycraft"

# Use intentional shell word splitting for SDK-provided CC/CFLAGS/LDFLAGS and
# sdl-config output; these are compiler argument strings, not filenames.
# shellcheck disable=SC2086
$CC ${CFLAGS:-} $sdl_cflags \
    -std=gnu11 -Os -ffast-math -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
    -DCRAFT_TEXTURES_BAKED=1 -DCRAFT_FUNKEY=1 \
    -I"$root/src" \
    "$root/funkey/funkey_main.c" \
    "$root/funkey/craft_chunk_store_posix.c" \
    "${engine_sources[@]}" \
    "$generated" \
    -o "$bin" \
    ${LDFLAGS:-} $sdl_libs -lm

if [[ -n "${STRIP:-}" ]]; then
    $STRIP "$bin" 2>/dev/null || true
fi

rm -rf "$stage" "$out_opk"
mkdir -p "$stage/lib"
cp "$bin" "$stage/thumbycraft"
chmod +x "$stage/thumbycraft"

sdl_lib="$sdk/arm-funkey-linux-musleabihf/sysroot/usr/lib/libSDL-1.2.so.0"
if [[ -e "$sdl_lib" ]]; then
    cp -L "$sdl_lib" "$stage/lib/"
fi

cat > "$stage/r.sh" <<'LAUNCH'
#!/bin/sh
APP_DIR=${THUMBYCRAFT_HOME:-/mnt/FunKey/.thumbycraft}
mkdir -p "$APP_DIR"
case "$0" in
    */*) OPK_DIR=${0%/*} ;;
    *) OPK_DIR=. ;;
esac
cd "$OPK_DIR" || exit 126
export LD_LIBRARY_PATH="$OPK_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec ./thumbycraft >> "$APP_DIR/stdout.log" 2>> "$APP_DIR/stderr.log"
LAUNCH
chmod +x "$stage/r.sh"

cat > "$stage/thumbycraft.funkey-s.desktop" <<'DESKTOP'
[Desktop Entry]
Type=Application
Name=ThumbyCraft
Comment=Tiny voxel survival game
Exec=r.sh
Icon=thumbycraft
Categories=games

DESKTOP

if [[ -f "$root/funkey/thumbycraft.png" ]]; then
    cp "$root/funkey/thumbycraft.png" "$stage/thumbycraft.png"
elif [[ -f "$root/android/app/src/main/res/mipmap-xxxhdpi/ic_launcher.png" ]]; then
    cp "$root/android/app/src/main/res/mipmap-xxxhdpi/ic_launcher.png" "$stage/thumbycraft.png"
else
    cp "$root/docs/screenshots/craft-title.jpg" "$stage/thumbycraft.jpg"
fi

mksquashfs "$stage" "$out_opk" -all-root -noappend -no-exports -no-xattrs >/dev/null
echo "OPK: $out_opk"
file "$bin" || true

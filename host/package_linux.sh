#!/usr/bin/env bash
#
# Build a portable Linux bundle of the ThumbyCraft host that others can
# run without building anything or installing SDL2. The game's textures
# are baked into the binary, so the only runtime dependency is SDL2 —
# which we bundle alongside and load via a launcher. Needs a desktop
# Linux with X11 or Wayland (i.e. any normal distro); nothing to install.
#
# Output: dist/thumbycraft-linux/  (+ dist/thumbycraft-linux.tar.gz)
#   thumbycraft_host   the game
#   lib/               bundled libSDL2
#   play.sh            launcher (sets LD_LIBRARY_PATH)
#   README.txt         controls + how to run
#
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
build="$root/build_host"
out="$root/dist/thumbycraft-linux"

cmake -B "$build" -S "$root/host" >/dev/null
cmake --build "$build" --target thumbycraft_host -j8

rm -rf "$out"; mkdir -p "$out/lib"
cp "$build/thumbycraft_host" "$out/"
strip "$out/thumbycraft_host" 2>/dev/null || true

# Bundle the SDL2 shared object the binary actually links.
sdl="$(ldd "$build/thumbycraft_host" | awk '/libSDL2/{print $3}')"
[ -n "$sdl" ] && cp -L "$sdl" "$out/lib/"

cat > "$out/play.sh" <<'LAUNCH'
#!/usr/bin/env bash
here="$(cd "$(dirname "$0")" && pwd)"
cd "$here"   # keep the save file (thumbycraft.sav) next to the game
LD_LIBRARY_PATH="$here/lib:${LD_LIBRARY_PATH:-}" exec "$here/thumbycraft_host" "$@"
LAUNCH
chmod +x "$out/play.sh"

cat > "$out/README.txt" <<'DOC'
ThumbyCraft — Linux build

Run:
    ./play.sh

Controls (Minecraft-style):
    Mouse        look
    W A S D      move / strafe
    Space        jump
    Left click   mine / attack
    Right click  place / use
    Wheel, 1-8   hotbar
    Esc          menu (inventory, save, new world, ...)
    G or `       toggle mouse capture
    F5 / F9      quick save / load     F12 quit

Needs a desktop Linux with X11 or Wayland. No install, no build — the
bundled lib/ provides SDL2. Your world saves to thumbycraft.sav here.
DOC

( cd "$root/dist" && tar -czf thumbycraft-linux.tar.gz thumbycraft-linux )
echo "Bundle:  $out"
echo "Tarball: $root/dist/thumbycraft-linux.tar.gz"
du -h "$root/dist/thumbycraft-linux.tar.gz" | cut -f1 | sed 's/^/Size:    /'

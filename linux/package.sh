#!/bin/bash
# Assemble a small Linux package (runtime + recompiled game code + launcher + notes)
# from an already built folder. It does NOT contain Nintendo game data: copy the
# game's own Metroid Dread_data/ folder next to it, or use --with-data for a
# self-contained (large) copy.
#
# Usage: linux/package.sh [--with-data]
#   SWR_GAME=/path/to/built/folder   default: games/Metroid Dread
#   SWR_DIST=/path/to/output         default: linux/dist
set -euo pipefail
W=${SWR_ROOT:-$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)}
G=${SWR_GAME:-"$W/games/Metroid Dread"}
D=${SWR_DIST:-"$W/linux/dist"}
NAME=${SWR_NAME:-Metroid Dread}
P="$D/MetroidDread-Linux"
WITH_DATA=0
[ "${1:-}" = "--with-data" ] && WITH_DATA=1

[ -x "$G/$NAME" ] || { echo "missing runtime: $G/$NAME (build with linux/build_all.sh)" >&2; exit 1; }
[ -f "$G/${NAME}_code.so" ] || { echo "missing game code: $G/${NAME}_code.so" >&2; exit 1; }

rm -rf "$P"
mkdir -p "$P"
cp "$G/$NAME" "$P/$NAME"
cp "$G/${NAME}_code.so" "$P/${NAME}_code.so"
cp "$G/$NAME.ini" "$P/$NAME.ini" 2>/dev/null || true
cp "$G/$NAME.hints" "$P/$NAME.hints" 2>/dev/null || true
if [ "$WITH_DATA" = 1 ]; then
  [ -d "$G/${NAME}_data" ] || { echo "missing data folder: $G/${NAME}_data" >&2; exit 1; }
  cp -a "$G/${NAME}_data" "$P/${NAME}_data"
fi

# The deployed code does not need the C/objects; keep them only in the build folder.
strip --strip-unneeded "$P/${NAME}_code.so" -o "$P/code.tmp"
mv "$P/code.tmp" "$P/${NAME}_code.so"

cat > "$P/run.sh" <<'EOF'
#!/bin/bash
cd "$(dirname "$(readlink -f "$0")")" || exit 1
if [ ! -d "Metroid Dread_data/romfs" ]; then
  echo "Metroid Dread_data/ is missing. Copy your built game data folder next to this script,"
  echo "or build it with: linux/build_all.sh /path/to/your/dump --out linux/dist"
  exit 1
fi
exec "./Metroid Dread" "$@"
EOF
chmod +x "$P/run.sh" "$P/$NAME"

cat > "$P/README-Linux.txt" <<EOF
Metroid Dread - native Linux build
=================================

This package contains the runtime and the recompiled game code. It does NOT contain
any Nintendo data. Copy your own built "Metroid Dread_data" folder (ExeFS + RomFS +
saves) next to this file, then run:

    ./run.sh
    ./run.sh --fullscreen=1

The game needs a working OpenGL 4.3 driver (Wayland works through XWayland) and,
for controllers, SDL2. With renderer=vulkan_native it needs a Vulkan 1.3 driver and
libvulkan.so.1. F1 or the controller Home button opens the in-game menu with either
renderer; F11/Alt+Enter toggles fullscreen. Settings are in "Metroid Dread.ini".

The game data is compiled from your own Metroid Dread 2.1.0 dump and must not be
redistributed. No game files are included here.

Metroid Dread is (c) Nintendo. This project is not affiliated with or endorsed by Nintendo.
EOF
sed -i 's/\r$//' "$P/run.sh" "$P/README-Linux.txt" "$P/$NAME.ini" 2>/dev/null || true

TAR="$D/MetroidDread-Linux.tar.gz"
tar -C "$D" -czf "$TAR" MetroidDread-Linux

echo "Package: $P"
if [ "$WITH_DATA" = 1 ]; then echo "  (includes game data, $(du -sh "$P" | cut -f1))"; else echo "  (game data not included)"; fi
echo "Archive: $TAR ($(du -h "$TAR" | cut -f1))"
sha256sum "$P/$NAME" "$P/${NAME}_code.so"

#!/bin/bash
# Build the Linux (X11/OpenGL) runtime from this repository with the system GCC.
# Produces bin/runtime_linux_x11. No game files are needed; linux/build_game.py
# compiles the player's own dump into <name>_code.so and creates the runnable folder.
#
# Dependencies (Debian/Ubuntu/SteamOS names):
#   build-essential python3 libx11-dev libgl1-mesa-dev
# Optional at run time: libsdl2-2.0-0 (controller support), libz (zlib runtime).
set -euo pipefail

W=${SWR_ROOT:-$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)}
cd "$W"

BACKEND=${SWR_BACKEND:-x11}
missing=0
for c in python3 gcc g++; do
  if ! command -v "$c" >/dev/null 2>&1; then
    echo "missing: $c" >&2; missing=1
  fi
done
if ! pkg-config --exists x11 2>/dev/null; then
  echo "missing: X11 development files (pkg-config x11; install libx11-dev)" >&2; missing=1
fi
if [ "$BACKEND" = x11 ] && ! pkg-config --exists gl 2>/dev/null; then
  echo "missing: OpenGL development files (pkg-config gl; install libgl1-mesa-dev)" >&2; missing=1
fi
if [ "$missing" != 0 ]; then
  echo "Install the packages above and run this script again." >&2
  exit 1
fi

# Dear ImGui is not vendored into runtime/imgui (it is built there by the Windows
# script). The Linux runtime compiles the OpenGL3 backend's sources directly, so
# only the headers and those five sources have to be in place.
python3 - "$W" <<'PY'
import os, shutil, sys
W = sys.argv[1]
SRC = os.path.join(W, 'third_party', 'imgui-1.92.5')
OUT = os.path.join(W, 'runtime', 'imgui')
os.makedirs(os.path.join(OUT, 'src'), exist_ok=True)
headers = ['imgui.h', 'imconfig.h', 'imgui_internal.h', 'imstb_rectpack.h', 'imstb_textedit.h', 'imstb_truetype.h',
           'LICENSE.txt', 'imgui_impl_opengl3.h', 'imgui_impl_opengl3_loader.h']
for h in headers:
    src = os.path.join(SRC, 'backends', h) if os.path.isfile(os.path.join(SRC, 'backends', h)) else os.path.join(SRC, h)
    if os.path.isfile(src):
        shutil.copy2(src, os.path.join(OUT, os.path.basename(h)))
sources = ['imgui.cpp', 'imgui_draw.cpp', 'imgui_tables.cpp', 'imgui_widgets.cpp', 'backends/imgui_impl_opengl3.cpp']
for f in sources:
    src = os.path.join(SRC, f)
    if not os.path.isfile(src):
        sys.exit('missing ImGui source: ' + src)
    shutil.copy2(src, os.path.join(OUT, 'src', os.path.basename(f)))
print('ImGui sources prepared in runtime/imgui')
PY

echo "Building bin/runtime_linux_${BACKEND} with $(gcc --version | head -1)..."
python3 switchrecomp.py --build-runtime --cc gcc --target linux --backend "$BACKEND" "$@"

echo
echo "Runtime built: bin/runtime_linux_${BACKEND}"
echo "Next: python3 linux/build_game.py /path/to/your/MetroidDread/dump"

# Metroid Dread — native Linux build

This is the **Linux / Steam Deck** build of the SwitchRecomp runtime. It is a native
X11 program with two renderers: OpenGL and ScarletNX's native Vulkan renderer. It is
not the Windows EXE under Wine. The repository contains no game files: you compile
**your own** Metroid Dread **2.1.0** dump (ExeFS **and** RomFS, with the update applied).

The Windows build has a first-run Play screen and setup window. The Linux runtime has
no in-process setup GUI yet, so the build is driven by the scripts below. The in-game
menu (F1 / controller Home) is built for the OpenGL renderer; the native Vulkan
renderer has no menu yet.

## Requirements

A 64-bit Linux desktop with a working OpenGL 4.3 driver (Mesa or the vendor driver;
Wayland desktops work through XWayland). The optional native Vulkan renderer needs a
Vulkan 1.3 driver (Mesa RADV/ANV, NVIDIA, or Mesa's lavapipe in software).

Build tools and development headers:

| distribution | packages |
|---|---|
| Debian / Ubuntu / Linux Mint | `build-essential python3 libx11-dev libgl1-mesa-dev libvulkan-dev` |
| Arch / Manjaro | `base-devel python libx11 mesa vulkan-headers vulkan-icd-loader` |
| Fedora | `gcc gcc-c++ make python3 libX11-devel mesa-libGL-devel vulkan-loader-devel vulkan-headers` |
| SteamOS (Desktop Mode) | build on another Linux PC, or install a build environment; the produced folder runs on the Deck |

At run time you also want:

- `libsdl2-2.0-0` (or your distribution's SDL2 runtime) for controllers. Without it the
  game still runs with the keyboard.
- zlib (`libz.so.1`, normally installed) for the game's decompression patch.
- `libvulkan.so.1` and a Vulkan 1.3 ICD only when using `renderer=vulkan_native`.
- About **6 GB** free for the built game data, plus the generated C/objects (about
  1 GB) and the ~190 MB `Metroid Dread_code.so`.

## Quick start

From the repository root:

```bash
# 1. build the runtime and compile your own dump into a runnable folder
linux/build_all.sh /path/to/your/MetroidDread/dump --jobs 6

# 2. play
cd "games/Metroid Dread"
./Metroid\ Dread
```

The dump folder must contain `exefs/` and `romfs/` (an emulator's extracted dump or a
folder with both is fine). If the base game and the 2.1.0 update are separate dumps:

```bash
linux/build_all.sh /path/to/base/game --update /path/to/2.1.0/update --jobs 6
```

`build_all.sh` runs `linux/build_runtime.sh` and then `linux/build_game.py`. The result
is `games/Metroid Dread/`:

```
Metroid Dread                 the runtime executable
Metroid Dread_code.so         your recompiled game
Metroid Dread_data/           exefs + romfs + saves
Metroid Dread_build/          generated C and objects (kept for fast rebuilds)
Metroid Dread.ini             settings (also editable in the in-game menu)
```

### Faster development builds

The RomFS is 5 GB. By default it is hard-linked when the dump and the output are on
the same filesystem, otherwise copied. To avoid the copy while testing:

```bash
python3 linux/build_game.py /path/to/dump --romfs-mode symlink
```

That links `Metroid Dread_data/romfs` to the dump (do not move the dump afterwards).
Use `--romfs-mode copy` for a self-contained folder.

### Build only the runtime

```bash
linux/build_runtime.sh
# -> bin/runtime_linux_x11
```

`bin/runtime_linux_x11` is also what `build_game.py` copies next to `_code.so`.
You can run it with an existing game folder or with `--help`-style runtime options
(unknown arguments are ignored; `--seconds=N` exits after N seconds with a screenshot
when `--shot=NAME` is given).

### Rebuild only the game code

```bash
python3 linux/build_game.py /path/to/dump --skip-runtime --jobs 6
```

Generated C and `.o` files are cached by compiler flags and source timestamps.
`--force` regenerates the C; a normal rebuild reuses it and only relinks.

## Options

`linux/build_game.py`:

| option | meaning |
|---|---|
| `--update DIR` | separate 2.1.0 update dump (base game dump is the positional argument) |
| `--out DIR` | parent for the output folder (default: `games/`) |
| `--name NAME` | output prefix / window title (default: `Metroid Dread`) |
| `--jobs N` | parallel GCC jobs (default: half the CPU count; lower it on low-memory PCs) |
| `--romfs-mode auto\|copy\|hardlink\|symlink\|none` | how to install the RomFS (default: auto: hardlink when possible, else copy) |
| `--force` | regenerate the C even when the cached generation matches |
| `--skip-runtime` | do not copy the runtime executable |
| `--chunk N` | instructions per generated C file (default 30000; do not change unless debugging the translator) |

## Running

```bash
cd "games/Metroid Dread"
./Metroid\ Dread                 # windowed by default
./Metroid\ Dread --fullscreen=1  # start fullscreen
```

- **F1** or the controller Home/Guide button opens the in-game menu. It has Display,
  Graphics, Gameplay, Audio, Controls and Help pages (OpenGL renderer; see Vulkan below).
- **F11** / Alt+Enter toggles fullscreen, **F3** takes a screenshot, **F4** toggles the
  FPS limit, **F8** takes a full-resolution PNG screenshot, **Esc** quits.
- Keyboard: arrows = D-Pad, WASD = left stick, IJKL = right stick, X/Space = A,
  Z = B, C = X, V = Y, Q/E = L/R, 1/3 = ZL/ZR, Enter = +, Backspace = -.
- Controllers use SDL2 when present. Xbox, DualShock/DualSense, Switch Pro and most
  8BitDo pads work. The `controller_api` setting can force SDL2 or the XInput-like
  direct reader where available.
- Settings live in `Metroid Dread.ini`. The first run expands the short file into the
  documented one; edits are read on the next start and many apply live.

### Native Vulkan renderer

The Linux build now includes ScarletNX's native Vulkan renderer. Select it in
`Metroid Dread.ini`:

```ini
[Graphics]
renderer=vulkan_native
```

`build_runtime.sh` builds `runtime/snxvk/libsnxvk_linux.a` first; the checked-in SPIR-V
means glslang is not needed for a normal build. The renderer uses the Xlib Vulkan
surface and works through XWayland. It needs Vulkan 1.3; if the device/driver cannot
present, the runtime logs the reason and falls back to OpenGL.

Known differences from the OpenGL path on Linux:

- The in-game menu is not drawn by the native Vulkan renderer (the same limitation as
the Windows Vulkan path). F1 is ignored; switch to `renderer=opengl` to change settings
in the menu, or edit the `.ini`.
- HDR output and DLSS are not ported to Linux; there is no DXGI/NGX here.
- Shader/pipeline caches are written under `vulkan_native/` next to the executable.
- `SWR_NO_SNXVK=1 linux/build_runtime.sh` builds an OpenGL-only runtime.

### Steam Deck

1. Build on a PC (or install the build dependencies in Desktop Mode) and copy the
   complete `games/Metroid Dread` folder (including the `_data`, `_build` and `_code.so`
   files) to the Deck.
2. Add `Metroid Dread` as a non-Steam game. Set **Launch Options** if needed:
   `--fullscreen=1`.
3. Start it from Game Mode. Steam Input / the Deck's controls arrive through SDL2.
4. For a smaller folder, delete `Metroid Dread_build/` after a successful build; the game
   only needs the executable, `_code.so`, `_data/` and `.ini`. Keep it if you want fast
   rebuilds.

## Differences from the Windows build

The Linux runtime is the same SwitchRecomp core, but not every Windows-only feature is
available:

- No first-run Play/setup screen and no automatic update checker; use the command line
  scripts above. The game's .ini and the in-game menu cover the player settings.
- OpenGL and native Vulkan rendering are available. DLSS, frame generation (`lsfg`),
  HDR output and the F7 recorder are Windows-only in this source tree. The in-game
  menu is OpenGL-only, as on Windows.
- The in-game menu is built from Dear ImGui's sources (the Windows Play screen is
  Direct3D 11 and is not part of the Linux build).
- Saves/settings are under `Metroid Dread_data/` just like on Windows; no Nintendo
  data is included in this repository or in a source package.
- The Linux runtime loads the system `libz.so.1` for the game's inflate patch instead
  of `zlib1.dll`.

## Troubleshooting

**`No X11 display (DISPLAY is not set)`** — start from a desktop session, not a bare
TTY/SSH shell. On Wayland the game runs through XWayland.

**`Your graphics driver does not support OpenGL 4.3`** — update Mesa/your GPU driver.
For a CPU-only test, Mesa's llvmpipe supports 4.3+ but is slow:

```bash
LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe ./Metroid\ Dread --seconds=30
```

**Vulkan starts, then falls back to OpenGL** — check `Metroid Dread.log` for the
`vulkan_native:` line. The usual causes are no Vulkan 1.3 ICD, a missing
`libvulkan.so.1`, or a driver without the Xlib surface extension. For a CPU-only
Vulkan test use Mesa's lavapipe:

```bash
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json ./Metroid\ Dread --seconds=30
```

**`Cannot find the recompiled code`** — run the executable from its own folder, or keep
`Metroid Dread`, `Metroid Dread_code.so`, `Metroid Dread_data/` and `Metroid Dread.ini`
together.

**`GLIBC_2.xx not found`** — the binary was built on a newer distribution than the
target. Build on the oldest distribution you need to support (or on the Steam Deck
itself). Debian 12/Ubuntu 22.04 or newer is a good baseline.

**Controllers not detected** — install the SDL2 runtime and check that the pad is seen:

```bash
python3 - <<'PY'
import ctypes
s = ctypes.CDLL('libSDL2-2.0.so.0')
s.SDL_Init(0x2000)
print(s.SDL_NumJoysticks())
PY
```

Then set `controller_api=sdl` in the .ini if another API is selected.

**Low FPS / memory pressure** — lower `--jobs` when compiling, use
`resolution=0.75` or `0.5`, turn `shadows=0`, and pick the right `fps_limit`. The log
`Metroid Dread.log` next to the executable records renderer, audio and errors.

## Old WSL test scripts

The other files in this directory (`setup_and_build_runtime.sh`, `build_game_so.sh`,
`make_test_game.sh`, `run_test.sh`, `run_route_linux.sh`, `menu_test.sh`,
`package.sh`, `check_deps.sh`) are the project's original WSL/CI test helpers; they
reference `/opt/swr` and a Windows test installation. You do **not** need WSL for the
native build described above.

#!/bin/bash
# One command for a native Linux / Steam Deck build:
#   linux/build_all.sh /path/to/MetroidDread/dump [--update /path/to/update] [build_game.py options]
# Builds the runtime, compiles the player's own v2.1.0 ExeFS+RomFS into <name>_code.so,
# and writes the runnable folder to games/Metroid Dread (or --out DIR).
set -euo pipefail
W=${SWR_ROOT:-$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)}

if [ $# -lt 1 ]; then
  sed -n '2,5p' "$0" | sed 's/^# \{0,1\}//'
  exit 1
fi

bash "$W/linux/build_runtime.sh"
python3 "$W/linux/build_game.py" "$@"

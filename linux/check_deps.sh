#!/bin/bash
# Linux build: check the produced binaries and the libraries they need at run time.
# Usage: linux/check_deps.sh [GAME_FOLDER]
# Default: games/Metroid Dread (the folder build_game.py writes).
set -euo pipefail
W=${SWR_ROOT:-$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)}
G=${1:-"$W/games/Metroid Dread"}
EXE="$G/Metroid Dread"
SO="$G/Metroid Dread_code.so"

for f in "$EXE" "$SO"; do
  [ -f "$f" ] || { echo "missing: $f (build first with linux/build_all.sh)" >&2; exit 1; }
  echo "== $f"
  echo -n "  newest glibc symbols: "
  objdump -T "$f" 2>/dev/null | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -2 | tr '\n' ' '; echo
  echo "  NEEDED:"
  readelf -d "$f" | sed -n 's/.*Shared library: \[\(.*\)\]/    \1/p'
  echo "  unresolved at run time:"
  if ldd "$f" >/tmp/swr-ldd.$$ 2>&1; then sed 's/^/    /' /tmp/swr-ldd.$$; else sed 's/^/    /' /tmp/swr-ldd.$$; fi
  rm -f /tmp/swr-ldd.$$
done

echo
echo "Build-host glibc: $(ldd --version | head -1)"
echo "If the target (Steam Deck / older distribution) has an older glibc than the newest"
echo "symbol above, build on the oldest distribution you need to support."

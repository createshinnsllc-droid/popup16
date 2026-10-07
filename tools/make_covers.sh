#!/bin/zsh
# Makes a cover for every game in a folder from the game's own opening: plays ~60 s without input
# and keeps the most colourful, least flat frame. Existing covers (including your own art) are kept.
# usage: tools/make_covers.sh <rom folder>      -> <rom folder>/covers/<game>.png
set -e
cd "$(dirname "$0")/.."
ROMS=${1:?usage: make_covers.sh <rom folder>}
mkdir -p "$ROMS/covers"
for rom in "$ROMS"/*.sfc(N) "$ROMS"/*.smc(N); do
  name=${rom:t:r}
  out="$ROMS/covers/$name.png"
  [[ -f "$out" ]] && continue
  frontend/popup16 snes9x/libretro/snes9x_libretro.dylib "$rom" --cover 3600 "$out" 2>/dev/null | grep -o "frame.*" | sed "s|^|$name: |" || echo "$name: failed"
done

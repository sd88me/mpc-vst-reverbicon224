#!/usr/bin/env bash
# Fetch the third-party pieces this port builds on. They are NOT redistributed in this repo (cineolX and Reflexion have no
# licence of their own yet), so every build starts here:
#   - cineolX's native 224 core  -> src/core/          (the reverb engine)
#   - cineolX's key / fader-cap / panel artwork -> vst/art/{keysprite,fadercap,panel}.png   (input to art/make_art.py)
#   - cineolX's pinned Reflexion checkout, patched for building -> tools/cineolX/build/deps/reflexion  (for the ROM importer)
set -euo pipefail
cd "$(dirname "$0")/.."
CINEOLX_COMMIT=65b260c748cf70061e3810c453df56e2a98953b1
if [ ! -d tools/cineolX/.git ]; then git clone https://github.com/temaniak/cineolX.git tools/cineolX; fi
git -C tools/cineolX fetch -q origin
git -C tools/cineolX checkout -q "$CINEOLX_COMMIT"
git -C tools/cineolX submodule update --init deps/reflexion
(cd tools/cineolX && python3 script/prepare_dependency.py reflexion --source deps/reflexion --output build/deps/reflexion)
mkdir -p src/core
cp tools/cineolX/native-hall/core/* src/core/
A=tools/cineolX/native-hall/plugin/assets
cp "$A/panel.png" "$A/fadercap.png" vst/art/
cp "$A/quick-key.png" vst/art/keysprite.png
echo "fetched cineolX $CINEOLX_COMMIT"

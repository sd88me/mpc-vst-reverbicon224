#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
MPC_VST="${MPC_VST:-$HERE/../../mpc-vst-plugins}"
exec "$MPC_VST/tools/build_port.sh" "$HERE/vst.json" "$@"

#!/usr/bin/env bash
# Build the bank helper for x86_64 (offline tests), armv7 (Gen1) and aarch64 (Gen2): tools/bankhelper-<arch>
# (cineolX + its Reflexion overlay in tools/cineolX; see tools/cineolX/script/prepare_dependency.py)
set -euo pipefail
cd "$(dirname "$0")"
SRC="../make_bank.cpp native-hall/import/bank_import.cpp native-hall/core/hall.cpp"
F="-std=gnu++20 -O2 -ffp-contract=off -include cmath -Ibuild/deps/reflexion -Inative-hall -Inative-hall/core -Inative-hall/import"
case "${1:-all}" in
  x86_64|all) (cd cineolX && g++ $F $SRC -o ../bankhelper-x86_64) ;;&
  aarch64|all) docker run --rm --platform linux/arm64 -u "$(id -u):$(id -g)" -v "$PWD":/t -w /t/cineolX arm64v8/gcc:12-bookworm \
      g++ $F -static-libstdc++ -static-libgcc $SRC -o ../bankhelper-aarch64 ;;&
  armv7|all) docker run --rm --platform linux/arm/v7 -u "$(id -u):$(id -g)" -v "$PWD":/t -w /t/cineolX arm32v7/gcc:11-bullseye \
      g++ $F -static-libstdc++ -static-libgcc $SRC -o ../bankhelper-armv7 ;;
esac

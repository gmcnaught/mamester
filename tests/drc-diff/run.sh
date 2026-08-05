#!/usr/bin/env bash
# Probe: does the core reach a running MAME machine with no content?
# See README.md -- this is the foundation the differential harness stands on.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="${LRMAME_SRC:-$REPO/vendor/lrmame}"
CORE="${CORE:-$SRC/drcsh_libretro.so}"
[ -f "$CORE" ] || { echo "no core at $CORE — build one containing a DRC CPU, e.g.
  SOURCES=src/mame/psikyo/psikyosh.cpp SUBTARGET=drcsh tools/build-lrmame.sh" >&2; exit 1; }
command -v qemu-arm-static >/dev/null || { echo "need qemu-user-static" >&2; exit 1; }
OUT=$(mktemp -d); trap 'rm -rf "$OUT"' EXIT
arm-linux-gnueabihf-gcc -O1 -std=gnu99 \
    -I"$SRC/src/osd/libretro/libretro-internal" \
    -o "$OUT/nogame" "$(dirname "$0")/nogame.c" \
    -L"$(dirname "$CORE")" -l:"$(basename "$CORE")" -lm -Wl,-rpath,'$ORIGIN'
cd "$(dirname "$CORE")"
qemu-arm-static -L /usr/arm-linux-gnueabihf ./"$(basename "$OUT")"/nogame 2>/dev/null || \
qemu-arm-static -L /usr/arm-linux-gnueabihf "$OUT/nogame"

#!/usr/bin/env bash
# Probe: does the core reach a running MAME machine with no content?
# See README.md -- this is the foundation the differential harness stands on.
#
#   bash tests/drc-diff/run.sh
#
# Requires a core built with a DRC-backed CPU in it (CPU_INCLUDE_DRC is false
# otherwise and every drcbe* file drops out), plus the armhf cross toolchain and
# qemu-user-static. Runs entirely on an x86_64 host.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
SRC="${LRMAME_SRC:-$REPO/vendor/lrmame}"
CORE="${CORE:-$SRC/drcsh_libretro.so}"
CC="${CC:-arm-linux-gnueabihf-gcc}"

[ -f "$CORE" ] || {
    echo "no core at $CORE — build one containing a DRC CPU:" >&2
    echo "  SOURCES=src/mame/psikyo/psikyosh.cpp SUBTARGET=drcsh tools/build-lrmame.sh" >&2
    exit 1
}
command -v qemu-arm-static >/dev/null || { echo "need qemu-user-static" >&2; exit 2; }
command -v "$CC" >/dev/null || { echo "need $CC" >&2; exit 2; }

COREDIR="$(cd "$(dirname "$CORE")" && pwd)"
CORELIB="$(basename "$CORE")"

# Built INTO the core's directory, not a temp dir: the probe is linked with
# RUNPATH=$ORIGIN (the same arrangement the real host uses), so the loader looks
# for the core beside the binary. Building elsewhere and pointing
# LD_LIBRARY_PATH at the core would test a different linkage than we ship.
BIN="$COREDIR/.drc-diff-nogame"
trap 'rm -f "$BIN"' EXIT

"$CC" -O1 -std=gnu99 -Wall \
    -I"$SRC/src/osd/libretro/libretro-internal" \
    -o "$BIN" "$HERE/nogame.c" \
    -L"$COREDIR" -l:"$CORELIB" -lm -Wl,-rpath,'$ORIGIN'

# MAME chdir()s relative to its own paths and writes cfg/nvram beside the cwd,
# so run from the core's directory.
cd "$COREDIR"
qemu-arm-static -L /usr/arm-linux-gnueabihf "$BIN"

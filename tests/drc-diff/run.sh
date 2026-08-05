#!/usr/bin/env bash
# Differential test of a native UML back-end against drcbe_c.
#
#   tests/drc-diff/run.sh              # run the corpus against the ARM32 core
#   tests/drc-diff/run.sh --host       # run it against the HOST's back-end
#   tests/drc-diff/run.sh alu          # one group (or one case) only
#   tests/drc-diff/run.sh --probe      # just the no-content probe, no diff
#
# --host is the calibration run and it comes first. On x86_64 it diffs drcbe_c
# against drcbe_x64, so every case that fails is the harness's or the corpus's
# fault -- drcbe_x64 has two decades of drivers behind it. Until that run is
# clean, a failure on the ARM32 core does not mean anything, because a
# differential test proves that two things disagree and never whose fault it
# is. tests/a32-asmjit/ already paid for that lesson once.
#
# Both modes need a core built with a DRC-backed CPU in it: CPU_INCLUDE_DRC is
# false when none is present, and then every drcbe* file -- including the one
# under test -- drops out of the build entirely. psikyosh is the SH-2 driver
# used for that, and it is also one of the gap targets the back-end exists for.
#
# Neither mode needs a romset, hardware, or MiSTer. The core is loaded with NULL
# content, MAME starts its ___empty driver, and the hook runs inside the
# resulting live machine -- see README.md and nogame.c.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
SRC="${LRMAME_SRC:-$REPO/vendor/lrmame}"

MODE=arm
FILTER=all
PROBE_ONLY=0
for arg in "$@"; do
    case "$arg" in
    --host)  MODE=host ;;
    --arm)   MODE=arm ;;
    --probe) PROBE_ONLY=1 ;;
    -*)      echo "usage: $0 [--host|--arm] [--probe] [group-or-case]" >&2; exit 2 ;;
    *)       FILTER="$arg" ;;
    esac
done

if [ "$MODE" = host ]; then
    CORE="${CORE:-$SRC/drcdiff_libretro.so}"
    CC="${CC:-gcc}"
    RUNNER=()
    BUILD_HINT="HOST=1 SUBTARGET=drcdiff SOURCES=src/mame/psikyo/psikyosh.cpp tools/build-lrmame.sh"
else
    CORE="${CORE:-$SRC/drcsh_libretro.so}"
    CC="${CC:-arm-linux-gnueabihf-gcc}"
    RUNNER=(qemu-arm-static -L /usr/arm-linux-gnueabihf)
    BUILD_HINT="SUBTARGET=drcsh SOURCES=src/mame/psikyo/psikyosh.cpp tools/build-lrmame.sh"
    command -v qemu-arm-static >/dev/null || { echo "need qemu-user-static" >&2; exit 2; }
fi

[ -f "$CORE" ] || { echo "no core at $CORE — build one with:" >&2; echo "  $BUILD_HINT" >&2; exit 1; }
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

if [ "$PROBE_ONLY" = 1 ]; then
    exec "${RUNNER[@]}" "$BIN"
fi

# MAMESTER_DRC_DIFF is both the switch and the filter: "all" runs everything, a
# group or case name runs just that. The hook terminates the process with the
# test result, so this script's own exit status is the corpus's.
MAMESTER_DRC_DIFF="$FILTER" exec "${RUNNER[@]}" "$BIN"

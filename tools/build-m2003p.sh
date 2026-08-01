#!/usr/bin/env bash
# Build mame2003-plus as a static library for the MiSTer HPS (Cortex-A9).
#
#   tools/build-m2003p.sh                              # ASM CPU cores ON (default)
#   USE_CYCLONE=0 USE_DRZ80=0 tools/build-m2003p.sh    # portable C cores
#   OPT=-O2 tools/build-m2003p.sh                      # upstream's own -O level
#   OUT=mame2003_plus_c.a tools/build-m2003p.sh        # name the archive
#
# Output: vendor/mame2003-plus/<OUT>, linked later by
# tools/mame-frontend/libretro-host/.
#
# Uses the host-native cross container (tools/mister/Dockerfile.cross-armhf), not
# the qemu-emulated one: this is 2097 translation units against mame4all's 1131,
# and the two toolchains were measured equivalent to +0.27% with bit-identical
# frame output (docs/bench-results.md).
#
# Everything is passed on the command line rather than patching upstream, which
# keeps the submodule clean. That works because NEITHER Makefile nor
# Makefile.common contains an `override` directive, so a command-line assignment
# beats every makefile assignment including `+=`. Three of them are load-bearing:
#
#   TARGET=...a    the `platform=unix` block sets TARGET to a .so. STATIC_LINKING
#                  would still `ar rcs` (Makefile:907) but name the archive .so --
#                  an ar archive wearing a shared object's extension.
#   fpic=          clears the -fPIC that `platform=unix` appends to CFLAGS. PIC
#                  costs a register and an indirection on ARM for no benefit in a
#                  statically linked binary.
#   PLATCFLAGS=    replaces the platform block's flags wholesale. Verified that
#                  the unix block never assigns PLATCFLAGS, so nothing is lost.
#
# The ASM CPU cores are the reason this comparison exists: our mame4all build
# runs with BOTH disabled (Cyclone segfaults on entry for every 68000 driver,
# DrZ80 crashes in DrZ80Run for sound CPUs -- tools/mister/patches/
# 0002-default-asm-cores-off.patch), while 2003-plus ships the same two cores and
# enables them on ARM. They must stay switchable, because on/off is a bench arm.
#
# OPT defaults to -O3, not upstream's -O2, and the reason is NEON rather than -O
# in the abstract: gcc 10.2.1 enables -ftree-loop-vectorize and
# -ftree-slp-vectorize at -O3 and NOT at -O2 (they only moved to -O2 in gcc 12,
# under the very-cheap cost model). At -O2 the -mfpu=neon above therefore buys
# little more than instruction selection -- no loop is vectorised, so the NEON
# unit is idle. -O3 is also what mame4all builds at (tools/mister/
# Makefile.mister:42), which removes an -O asymmetry from the engine comparison.
# It goes in PLATCFLAGS because Makefile:927 appends PLATCFLAGS after the -O2 at
# Makefile:851, and the last -O on the command line wins.
#
# Not added, deliberately: -ffast-math. mame4all has it, and on ARMv7 it is what
# additionally unlocks vectorisation of FLOATING-POINT loops (NEON is not
# IEEE-754 compliant there, so gcc refuses without -funsafe-math-optimizations).
# It also changes results, so it is a separate arm rather than part of this one.
# 0.78 is overwhelmingly integer code; -O3 alone should be most of the win.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$REPO/vendor/mame2003-plus"
IMAGE="mamester-cross-armhf"
DOCKERFILE="$REPO/tools/mister/Dockerfile.cross-armhf"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 4)}"
OUT="${OUT:-mame2003_plus_libretro.a}"
OPT="${OPT:--O3}"

PLATCFLAGS="-marm -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard \
-fomit-frame-pointer -fno-math-errno $OPT"

[ -f "$SRC/Makefile" ] || { echo "vendor/mame2003-plus missing — run: git submodule update --init"; exit 1; }

docker image inspect "$IMAGE" >/dev/null 2>&1 || {
    echo "# building image $IMAGE"
    docker build -t "$IMAGE" -f "$DOCKERFILE" "$REPO/tools/mister"
}

# An array, not a string: PLATCFLAGS contains spaces and has to reach make as a
# single argument.
MAKE_VARS=(
    platform=unix
    CC=arm-linux-gnueabihf-gcc
    CXX=arm-linux-gnueabihf-g++
    AR=arm-linux-gnueabihf-ar
    STATIC_LINKING=1
    TARGET_NAME=mame2003_plus
    "TARGET=$OUT"
    fpic=
    "USE_CYCLONE=${USE_CYCLONE:-1}"
    "USE_DRZ80=${USE_DRZ80:-1}"
    "PLATCFLAGS=$PLATCFLAGS"
    HAVE_NEON=1
    ARM=1
    ARCH=arm
    CPU_ARCH=arm
)
SIG=$(printf '%s\n' "${MAKE_VARS[@]}")

# Upstream compiles in-tree (src/foo.o next to src/foo.c) with no per-config
# object directory and no dependency on the makefile, so changing a flag
# rebuilds NOTHING: every .o is already newer than its .c, and the archive is
# just re-`ar`ed from the previous configuration's objects. That produces a
# second "arm" byte-identical to the first and a measured delta of ~0% -- a
# wrong answer indistinguishable from a real one. mame4all avoids this with a
# per-config object directory (tools/build-mame.sh TARGET_NAME); here there is
# nothing to redirect, so the configuration is stamped and a change forces a
# clean. The stamp is written only after the build succeeds, so an interrupted
# build re-cleans rather than leaving a half-converted tree.
STAMP="$SRC/.mister-build-config"
if [ ! -f "$STAMP" ] || [ "$(cat "$STAMP")" != "$SIG" ]; then
    echo "# build configuration changed — cleaning first"
    [ -f "$STAMP" ] && diff "$STAMP" <(printf '%s\n' "$SIG") | sed 's/^/#   /' || true
    rm -f "$STAMP"
    docker run --rm -v "$SRC:/src" -w /src "$IMAGE" make clean "${MAKE_VARS[@]}" >/dev/null
fi

echo "# building $OUT in $IMAGE, -j$JOBS, $OPT, cyclone=${USE_CYCLONE:-1} drz80=${USE_DRZ80:-1}"
docker run --rm -v "$SRC:/src" -w /src "$IMAGE" make -j"$JOBS" "${MAKE_VARS[@]}"

printf '%s\n' "$SIG" > "$STAMP"
ls -lh "$SRC/$OUT"

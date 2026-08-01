#!/usr/bin/env bash
# Build mame2003-plus as a static library for the MiSTer HPS (Cortex-A9).
#
#   tools/build-m2003p.sh                              # ASM CPU cores ON (default)
#   USE_CYCLONE=0 USE_DRZ80=0 tools/build-m2003p.sh    # portable C cores
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
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$REPO/vendor/mame2003-plus"
IMAGE="mamester-cross-armhf"
DOCKERFILE="$REPO/tools/mister/Dockerfile.cross-armhf"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 4)}"
OUT="${OUT:-mame2003_plus_libretro.a}"

[ -f "$SRC/Makefile" ] || { echo "vendor/mame2003-plus missing — run: git submodule update --init"; exit 1; }

docker image inspect "$IMAGE" >/dev/null 2>&1 || {
    echo "# building image $IMAGE"
    docker build -t "$IMAGE" -f "$DOCKERFILE" "$REPO/tools/mister"
}

echo "# building $OUT in $IMAGE, -j$JOBS, cyclone=${USE_CYCLONE:-1} drz80=${USE_DRZ80:-1}"
docker run --rm -v "$SRC:/src" -w /src "$IMAGE" \
    make -j"$JOBS" platform=unix \
        CC=arm-linux-gnueabihf-gcc \
        CXX=arm-linux-gnueabihf-g++ \
        AR=arm-linux-gnueabihf-ar \
        STATIC_LINKING=1 \
        TARGET_NAME=mame2003_plus \
        TARGET="$OUT" \
        fpic= \
        USE_CYCLONE="${USE_CYCLONE:-1}" \
        USE_DRZ80="${USE_DRZ80:-1}" \
        "PLATCFLAGS=-marm -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -fomit-frame-pointer -fno-math-errno" \
        HAVE_NEON=1 ARM=1 ARCH=arm CPU_ARCH=arm

ls -lh "$SRC/$OUT"

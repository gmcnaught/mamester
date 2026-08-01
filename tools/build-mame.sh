#!/usr/bin/env bash
# Build mame4all-pi for the MiSTer HPS (armhf) in the cross-build container.
#
#   tools/build-mame.sh                 # full build -> vendor/mame4all-pi/mame
#   tools/build-mame.sh <make-target>   # e.g. a single object, for a fast check
#
#   CROSS=1 tools/build-mame.sh         # host-native arm-linux-gnueabihf gcc
#                                       # instead of qemu-emulated native gcc.
#                                       # Same gcc version (10.2.1), no emulation.
#   TARGET_NAME=mame-x tools/build-mame.sh   # binary AND object dir suffix
#
# Stages the MiSTer backend + Makefile.mister into the submodule tree, then runs
# make inside the container. Requires the image from
# `docker build -f tools/mister/Dockerfile.mame-build -t mamester-armhf-build`
# (or Dockerfile.cross-armhf -> mamester-cross-armhf when CROSS=1).
#
# TARGET_NAME is not cosmetic. Makefile.mister has OBJ = obj_$(TARGET)_mister and
# the pattern rules are `$(OBJ)/%.o: src/%.c` -- nothing depends on the makefile,
# and there is no clean step. So changing compiler flags, or the compiler itself,
# rebuilds NOTHING: make sees every object newer than its source and relinks the
# stale set. Two configurations built under one object directory silently produce
# identical binaries. Give every configuration you intend to compare its own
# TARGET_NAME, then verify with `cmp` that the outputs actually differ -- sizes
# can match between genuinely different binaries, so a size check proves nothing.
#
# It overrides OBJ and EMULATOR rather than TARGET, because Makefile.mister uses
# TARGET for three unrelated jobs: the object directory, the output binary, and
# `include src/$(TARGET).mak` -- the driver set. Setting TARGET=mame-cross would
# look right and fail on a missing src/mame-cross.mak. OBJDIRS is recursively
# expanded from $(OBJ), so overriding OBJ carries into the directory tree.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$REPO/vendor/mame4all-pi"
TARGET_NAME="${TARGET_NAME:-mame}"

if [ "${CROSS:-0}" = "1" ]; then
    IMAGE="mamester-cross-armhf"
    DOCKERFILE="$REPO/tools/mister/Dockerfile.cross-armhf"
    # No platform pin: the image runs NATIVELY on whatever the host is, and the
    # armhf cross-toolchain inside it is what targets the device. On an arm64
    # host that means zero emulation -- an aarch64-hosted arm-linux-gnueabihf
    # gcc, which is why this path is roughly an order of magnitude faster than
    # the qemu-emulated armhf image. Pinning linux/amd64 here makes `docker run`
    # fail to resolve the locally-built image on an arm64 host.
    PLATFORM=""
    # Override the compiler; every flag in Makefile.mister stays as-is, so the
    # only difference against the qemu arm is which gcc emits the objects.
    MAKE_VARS="CC=arm-linux-gnueabihf-gcc CPP=arm-linux-gnueabihf-g++ LD=arm-linux-gnueabihf-g++ STRIP=arm-linux-gnueabihf-strip"
else
    IMAGE="mamester-armhf-build"
    DOCKERFILE="$REPO/tools/mister/Dockerfile.mame-build"
    PLATFORM="linux/arm/v7"
    MAKE_VARS=""
fi

[ -f "$SRC/Makefile" ] || { echo "submodule missing — run: git submodule update --init"; exit 1; }

# Apply the vendored-source patches (idempotent: skip any already applied).
# These are edits to mame4all's own files, which cannot live in src/mister/
# the way our replacement backends do.
for p in "$REPO"/tools/mister/patches/*.patch; do
    [ -e "$p" ] || continue
    if git -C "$SRC" apply --check "$p" 2>/dev/null; then
        git -C "$SRC" apply "$p"
        echo "# applied $(basename "$p")"
    elif git -C "$SRC" apply --reverse --check "$p" 2>/dev/null; then
        echo "# already applied: $(basename "$p")"
    else
        echo "# WARNING: cannot apply $(basename "$p") -- vendored source drifted" >&2
        exit 1
    fi
done

# Stage the MiSTer present backend, make override, and SDL build script.
mkdir -p "$SRC/src/mister"
cp "$REPO/tools/mame-frontend/mister-backend/"*.cpp "$SRC/src/mister/"
cp "$REPO/tools/mame-frontend/mister-backend/"*.c   "$SRC/src/mister/"
cp "$REPO/tools/mame-frontend/mister-backend/"*.h   "$SRC/src/mister/"
cp "$REPO/tools/mister/Makefile.mister" "$SRC/Makefile.mister"
cp "$REPO/tools/mister/build-sdl12.sh" "$SRC/build-sdl12.sh"

JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 4)}"

# Build the image if it is not present, so a fresh checkout works unattended.
docker image inspect "$IMAGE" >/dev/null 2>&1 || {
    echo "# building image $IMAGE from $(basename "$DOCKERFILE")"
    docker build ${PLATFORM:+--platform "$PLATFORM"} -t "$IMAGE" -f "$DOCKERFILE" "$REPO/tools/mister"
}

echo "# building in $IMAGE (${PLATFORM:-native}), -j$JOBS, out=$TARGET_NAME, target: ${*:-all}"
# Build the lean static SDL 1.2 first (idempotent), then mame against it.
docker run --rm ${PLATFORM:+--platform "$PLATFORM"} -v "$SRC:/src" -w /src "$IMAGE" \
    bash -c "bash build-sdl12.sh && make -f Makefile.mister \
        OBJ=obj_${TARGET_NAME}_mister EMULATOR=$TARGET_NAME \
        $MAKE_VARS -j$JOBS ${*:-all}"

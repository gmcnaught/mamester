#!/usr/bin/env bash
# Build mame4all-pi for the MiSTer HPS (armhf) in the cross-build container.
#
#   tools/build-mame.sh                 # full build -> vendor/mame4all-pi/mame
#   tools/build-mame.sh <make-target>   # e.g. a single object, for a fast check
#
# Stages the MiSTer backend + Makefile.mister into the submodule tree, then runs
# make inside the armhf container (qemu). Requires the image from
# `docker build -f tools/mister/Dockerfile.mame-build -t mame-mister-armhf-build`.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$REPO/vendor/mame4all-pi"
IMAGE="mame-mister-armhf-build"

[ -f "$SRC/Makefile" ] || { echo "submodule missing — run: git submodule update --init"; exit 1; }

# Stage the MiSTer present backend, make override, and SDL build script.
mkdir -p "$SRC/src/mister"
cp "$REPO/tools/mame-frontend/mister-backend/"*.cpp "$SRC/src/mister/"
cp "$REPO/tools/mister/Makefile.mister" "$SRC/Makefile.mister"
cp "$REPO/tools/mister/build-sdl12.sh" "$SRC/build-sdl12.sh"

JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 4)}"
echo "# building in $IMAGE (armhf/qemu), -j$JOBS, target: ${*:-all}"
# Build the lean static SDL 1.2 first (idempotent), then mame against it.
docker run --rm --platform linux/arm/v7 -v "$SRC:/src" -w /src "$IMAGE" \
    bash -c "bash build-sdl12.sh && make -f Makefile.mister -j$JOBS ${*:-all}"

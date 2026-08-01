#!/usr/bin/env bash
# Build mame4all-pi for the MiSTer HPS (armhf) in the cross-build container.
#
#   tools/build-mame.sh                 # full build -> vendor/mame4all-pi/mame
#   tools/build-mame.sh <make-target>   # e.g. a single object, for a fast check
#   tools/build-mame.sh ARCHFLAGS= OPTFLAGS= LDEXTRA= all
#                                       # baseline codegen (see Makefile.mister)
#
# Stages the MiSTer backend + Makefile.mister into the submodule tree, then runs
# make inside the armhf container (qemu). Requires the image from
# `docker build -f tools/mister/Dockerfile.mame-build -t mamester-armhf-build`.
#
# NOT CONCURRENCY-SAFE ON ONE TREE. The staging step below copies
# tools/mame-frontend/mister-backend/*.cpp over $SRC/src/mister/ on EVERY
# invocation, and the Makefile derives OBJ = obj_$(TARGET)_mister, which does not
# vary with build flags. So two builds sharing one $SRC will (a) rewrite each
# other's backend sources underneath a live compile and (b) interleave writes into
# one object directory, silently producing a binary whose objects were built with
# mixed flags. Both happened during the Cortex-A9 flag work: a concurrent build
# swapped mister_video.cpp mid-compile, and left one baseline-codegen object
# (Thumb/VFPv3-D16) in an otherwise ARM/NEON tree, which linked without complaint.
#
# To run a second build concurrently, point it at its own copy of the tree:
#   cp -a vendor/mame4all-pi /tmp/mame-baseline
#   MAME_SRC=/tmp/mame-baseline tools/build-mame.sh ARCHFLAGS= OPTFLAGS= LDEXTRA= all
# To check a tree for mixed codegen, read .ARM.attributes (Tag_FP_arch) and the
# $a/$t mapping symbols in .symtab of every obj_mame_mister/**/*.o -- any $t>0 in
# an ARM-mode (-marm) build is a contaminated object.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${MAME_SRC:-$REPO/vendor/mame4all-pi}"
IMAGE="mamester-armhf-build"

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
cp "$REPO/tools/mame-frontend/mister-backend/"*.h   "$SRC/src/mister/"
cp "$REPO/tools/mister/Makefile.mister" "$SRC/Makefile.mister"
cp "$REPO/tools/mister/build-sdl12.sh" "$SRC/build-sdl12.sh"

JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 4)}"
echo "# building in $IMAGE (armhf/qemu), -j$JOBS, target: ${*:-all}"
# Build the lean static SDL 1.2 first (idempotent), then mame against it.
docker run --rm --platform linux/arm/v7 -v "$SRC:/src" -w /src "$IMAGE" \
    bash -c "bash build-sdl12.sh && make -f Makefile.mister -j$JOBS ${*:-all}"

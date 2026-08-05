#!/usr/bin/env bash
# Build libretro's CURRENT MAME (0.289) for the MiSTer HPS (Cortex-A9, armhf).
#
#   tools/build-lrmame.sh                          # the default driver subset
#   SOURCES=src/mame/pacman/pacman.cpp \
#       tools/build-lrmame.sh                      # the gate build: one driver
#   M16B=1 tools/build-lrmame.sh                   # RGB565 output instead of XRGB8888
#   SUBTARGET=lrmame-gate tools/build-lrmame.sh    # name the build
#
# Output: vendor/lrmame/libretro/mame_<SUBTARGET>_libretro.so, linked later by
# tools/mame-frontend/libretro-host/.
#
# Design: docs/superpowers/specs/2026-08-05-lrmame-engine-design.md.
#
# This is the third engine and the first one whose build is genie/lua rather than
# a flat makefile, so the flags below are not interchangeable with the other two
# build scripts. Six of them are load-bearing and every one was arrived at by a
# failed build rather than by reading documentation:
#
#   OSD=retro          the makefile's OSD default is `sdl` (makefile:455-472).
#                      CONFIG=libretro does NOT imply it -- it only clears
#                      TOOLCHAIN (makefile:563). Without this you get an SDL
#                      build that has no retro_* entry points at all.
#
#   PLATFORM=arm       selects genie's 32-bit ARM configuration (genie.lua:1143).
#
#   PTR64=0            32-bit pointers. Required, and it has a side effect the
#                      next flag has to undo.
#
#   ARCHITECTURE=      MANDATORY, and empty. PTR64=0 sets ARCHITECTURE:=_x86
#                      (makefile:364-368) with no regard for PLATFORM, which
#                      routes the build to the `linux_x86` target and puts -m32
#                      on an ARM compiler:
#                          arm-linux-gnueabihf-g++: error: unrecognized
#                          command-line option '-m32'; did you mean '-mbe32'?
#                      A command-line assignment beats the makefile's `:=`, which
#                      is exactly how upstream's own android-arm block does it
#                      (Makefile.libretro:143-153, `PLATFLAGS += ARCHITECTURE=`).
#
#   FORCE_DRC_C_BACKEND=1
#                      MAME's dynamic recompiler has no ARM32 backend. Every
#                      DRC-backed CPU therefore runs its C interpreter, which is
#                      a large part of why this engine's viability is in doubt at
#                      all -- see the gate in the design doc. Not optional:
#                      without it the DRC-using drivers fail to build.
#
#   CROSS_BUILD=1 + OVERRIDE_CC/CXX/AR
#                      MAME emits host-native code generators (complay.py,
#                      verinfo.py, png2bdc) AND cross-compiles the emulator, so
#                      both toolchains are live at once (makefile:570-590). The
#                      OVERRIDE_* pair names the CROSS compiler; the plain
#                      CC/CXX stay native. Getting this backwards builds x86
#                      generators with the ARM compiler and fails late.
#
# SOURCES is a driver-file filter and it is REQUIRED for anything shippable, not
# an optimisation: a full-set 0.289 build is on the order of a 2 GB link and this
# device has 1 GB of DDR with the upper half owned by the FPGA. Note that it
# filters DRIVERS only -- the layout codegen step still walks all ~2000 .lay
# files and costs several minutes on the first build regardless.
#
# M16B makes the core report RETRO_PIXEL_FORMAT_RGB565 instead of XRGB8888
# (libretro.cpp:771), which is already the DDR format and skips a per-frame
# convert. It is OFF by default here because libretro_shared.h defines
# HAVE_RGB32 unconditionally beside a `FIXME: re-add way to handle 16/32 bit`,
# so the 16-bit path is plausibly bit-rotted upstream -- treat it as a measured
# arm (compare MISTER_FRAME_HASH between the two) rather than a free win.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$REPO/vendor/lrmame"
IMAGE="mamester-cross-armhf-cxx20"
DOCKERFILE="$REPO/tools/mister/Dockerfile.cross-armhf-cxx20"
JOBS="${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}"

# One driver file by default: the gate build from the design doc. Anything
# larger is a deliberate choice made after the gate passes.
SOURCES="${SOURCES:-src/mame/pacman/pacman.cpp}"
SUBTARGET="${SUBTARGET:-lrmame}"

# Cortex-A9 with NEON, matching the other two engines' ARCHOPTS so an engine
# comparison is not also a codegen comparison.
ARCHOPTS="${ARCHOPTS:--marm -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard}"

[ -f "$SRC/makefile" ] || {
    echo "vendor/lrmame missing — run: git submodule update --init vendor/lrmame" >&2
    exit 1
}

# DRC=1 builds the ARM32 dynamic recompiler instead of the UML interpreter.
# Two flags come OFF rather than one going on:
#
#   FORCE_DRC_C_BACKEND  is the obvious one -- it pins every DRC-backed CPU to
#                        drcbec regardless of host.
#   NOASM                is not. It defines MAME_NOASM, which drcuml.cpp's
#                        NATIVE_DRC chain tests, so leaving it set would select
#                        drcbec even with the back-end compiled in. Dropping it
#                        also lets src/osd/eminline.h reach eigccarm.h, which is
#                        an ARM path that has been sitting unused in this build
#                        the whole time.
#
# See docs/superpowers/specs/2026-08-05-drcbearm32-design.md. The back-end is
# incomplete -- unlowered opcodes are a fatalerror -- so this is not the
# shipping configuration yet.
if [ "${DRC:-0}" = "1" ]; then
    echo "# DRC=1: injecting the ARM32 back-end into vendor/lrmame"
    "$REPO/tools/mame-drc-arm32/inject.sh"
    DRC_VARS=()
else
    DRC_VARS=(NOASM=1 FORCE_DRC_C_BACKEND=1)
fi

MAKE_VARS=(
    CONFIG=libretro
    OSD=retro
    TARGET=mame
    "SUBTARGET=$SUBTARGET"
    "SOURCES=$SOURCES"
    PLATFORM=arm
    PTR64=0
    ARCHITECTURE=
    "${DRC_VARS[@]}"
    NO_USE_PORTAUDIO=1
    NO_USE_MIDI=1
    CROSS_BUILD=1
    OVERRIDE_CC=arm-linux-gnueabihf-gcc
    OVERRIDE_CXX=arm-linux-gnueabihf-g++
    OVERRIDE_LD=arm-linux-gnueabihf-g++
    OVERRIDE_AR=arm-linux-gnueabihf-ar
    "ARCHOPTS=$ARCHOPTS"
    PYTHON_EXECUTABLE=python3
    REGENIE=1
    NOWERROR=1
)

# OpenGL must stay absent. With HAVE_OPENGL or HAVE_OPENGLES defined the core
# issues SET_HW_RENDER and RETURNS FALSE FROM retro_load_game when the frontend
# refuses it (libretro.cpp:938-953) -- so an accidental GL build does not fall
# back to software, it fails to load every game. Nothing here defines them; this
# comment is the guard against someone adding one.
if [ "${M16B:-0}" = "1" ]; then
    MAKE_VARS+=("ARCHOPTS=$ARCHOPTS -DM16B")
    echo "# M16B: core will report RGB565 (no per-frame convert)"
fi

# The build configuration is not tracked by genie's dependency graph any better
# than the other engines' were -- changing a flag does not invalidate objects
# built under the old one. Same trap tools/build-m2003p.sh documents, same fix:
# stamp the configuration and force a clean when it moves. The stamp is written
# only on success, so an interrupted build re-cleans.
SIG=$(printf '%s\n' "${MAKE_VARS[@]}")
STAMP="$SRC/.mister-build-config"

run() {
    if command -v arm-linux-gnueabihf-g++ >/dev/null 2>&1; then
        ( cd "$SRC" && "$@" )
    else
        docker image inspect "$IMAGE" >/dev/null 2>&1 || {
            echo "# building image $IMAGE"
            docker build -t "$IMAGE" -f "$DOCKERFILE" "$REPO/tools/mister"
        }
        docker run --rm -v "$SRC:/src" -w /src "$IMAGE" "$@"
    fi
}

if [ ! -f "$STAMP" ] || [ "$(cat "$STAMP")" != "$SIG" ]; then
    if [ -f "$STAMP" ]; then
        echo "# build configuration changed — cleaning first"
        diff "$STAMP" <(printf '%s\n' "$SIG") | sed 's/^/#   /' || true
        rm -f "$STAMP"
        run make -f makefile "${MAKE_VARS[@]}" clean >/dev/null || true
    fi
fi

echo "# building mame_${SUBTARGET} for armhf, -j$JOBS"
echo "#   SOURCES=$SOURCES"
run nice make -f makefile "${MAKE_VARS[@]}" -j"$JOBS"

printf '%s\n' "$SIG" > "$STAMP"

# genie writes `<SUBTARGET>_libretro.so` into the SOURCE ROOT. libretro/bin/
# looks like the output directory in the generated makefiles but is only a -L
# search path, and there is no `mame_` prefix -- both worth stating because the
# host Makefile has to name this file exactly.
ls -lh "$SRC/${SUBTARGET}_libretro.so"

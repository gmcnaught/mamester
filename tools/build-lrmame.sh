#!/usr/bin/env bash
# Build libretro's CURRENT MAME (0.289) for the MiSTer HPS (Cortex-A9, armhf).
#
#   tools/build-lrmame.sh                          # the default driver subset
#   SOURCES=src/mame/pacman/pacman.cpp \
#       tools/build-lrmame.sh                      # the gate build: one driver
#   M16B=1 tools/build-lrmame.sh                   # RGB565 output instead of XRGB8888
#   SUBTARGET=lrmame-gate tools/build-lrmame.sh    # name the build
#   DRC=0 tools/build-lrmame.sh                    # UML interpreter, no ARM32 JIT
#   HOST=1 tools/build-lrmame.sh                   # x86_64 host build, native DRC
#
# HOST=1 is not a way to run games -- it is what tests/drc-diff/ needs. The
# differential harness compares drcbe_c against whatever native back-end the
# host has, and on x86_64 that is drcbe_x64: a back-end two decades of drivers
# have already qualified. Running the harness there first is what separates
# "the harness is wrong" from "the back-end is wrong", and a differential test
# that has not been calibrated that way proves only that two things disagree.
#
# It gets its own BUILDDIR, so the two builds do not clean each other out. The
# cost of that separation is a second run of the layout codegen; the cost of
# not having it is a full rebuild every time you alternate.
#
# Output: vendor/lrmame/libretro/mame_<SUBTARGET>_libretro.so, linked later by
# tools/mame-frontend/libretro-host/.
#
# Design: docs/superpowers/specs/2026-08-05-lrmame-engine-design.md.
#
# This is the third engine and the first one whose build is genie/lua rather than
# a flat makefile, so the flags below are not interchangeable with the other two
# build scripts. Five of them are load-bearing and every one was arrived at by a
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
#   (FORCE_DRC_C_BACKEND was here, and is not any more -- see the DRC block
#    below. Upstream 0.289 ships no ARM32 back-end, which is why it used to be
#    mandatory; this tree adds one.)
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

# The ARM32 dynamic recompiler is ON by default (DRC=0 opts back out to the UML
# interpreter). Operator decision 2026-08-05: the back-end is enabled anywhere
# the AArch64 one would be, which inject.sh implements by folding PLATFORM=arm
# into CPU_INCLUDE_DRC_NATIVE rather than into a parallel flag beside it.
#
# WHAT THAT COSTS TODAY. Only the structural opcodes are lowered; every other
# UML opcode is a fatalerror. So with DRC on, a driver that actually uses a
# DRC-backed CPU -- SH, MIPS, PowerPC, Hyperstone, the DSPs, ~3.2% of drivers,
# tools/lrmame-drc-scan.sh --summary -- ABORTS instead of running slowly through
# drcbec. That is the intended trade while the lowering is built: a hard stop on
# an unlowered opcode is how the remaining work gets found, and a silent wrong
# answer is the thing being avoided. Everything else -- Z80, 6502, 6809, 68000,
# the whole target library and the pacman gate -- never reaches drcuml at all
# and is bit-identical either way. Build with DRC=0 to ship or to bench those
# 3.2% of drivers.
#
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
# See docs/superpowers/specs/2026-08-05-drcbearm32-design.md.
HOST="${HOST:-0}"
BUILDDIR="${BUILDDIR:-$([ "$HOST" = "1" ] && echo build-host || echo build)}"

# The differential harness is injected for every configuration. It compiles into
# the core unconditionally and does nothing unless MAMESTER_DRC_DIFF is set in
# the environment, so this does not make a shipped core a test core.
"$REPO/tests/drc-diff/inject.sh" >/dev/null

if [ "$HOST" = "1" ]; then
    # No PLATFORM, no ARCHITECTURE=, no cross toolchain -- and critically none
    # of NOASM/FORCE_DRC_C_BACKEND, because the whole point is to get the host's
    # NATIVE back-end compiled in for the harness to diff against.
    echo "# HOST=1: x86_64 build with the host's native DRC back-end"
    "$REPO/tools/mame-drc-arm32/inject.sh" --revert >/dev/null 2>&1 || true
    MAKE_VARS=(
        CONFIG=libretro
        OSD=retro
        TARGET=mame
        "SUBTARGET=$SUBTARGET"
        "SOURCES=$SOURCES"
        "BUILDDIR=$BUILDDIR"
        PTR64=1
        NO_USE_PORTAUDIO=1
        NO_USE_MIDI=1
        PYTHON_EXECUTABLE=python3
        REGENIE=1
        NOWERROR=1
    )
elif [ "${DRC:-1}" = "1" ]; then
    echo "# DRC on: injecting the ARM32 back-end into vendor/lrmame"
    "$REPO/tools/mame-drc-arm32/inject.sh"
    echo "#   NOTE: only structural opcodes are lowered — a driver on a DRC-backed"
    echo "#   CPU will fatalerror on the first unlowered opcode. Use DRC=0 for those."
    DRC_VARS=()
else
    echo "# DRC=0: UML interpreter (drcbec), back-end not injected"
    "$REPO/tools/mame-drc-arm32/inject.sh" --revert >/dev/null 2>&1 || true
    DRC_VARS=(NOASM=1 FORCE_DRC_C_BACKEND=1)
fi

if [ "$HOST" != "1" ]; then
MAKE_VARS=(
    CONFIG=libretro
    OSD=retro
    TARGET=mame
    "SUBTARGET=$SUBTARGET"
    "SOURCES=$SOURCES"
    "BUILDDIR=$BUILDDIR"
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
fi

# OpenGL must stay absent. With HAVE_OPENGL or HAVE_OPENGLES defined the core
# issues SET_HW_RENDER and RETURNS FALSE FROM retro_load_game when the frontend
# refuses it (libretro.cpp:938-953) -- so an accidental GL build does not fall
# back to software, it fails to load every game. Nothing here defines them; this
# comment is the guard against someone adding one.
if [ "${M16B:-0}" = "1" ]; then
    if [ "$HOST" = "1" ]; then
        MAKE_VARS+=("ARCHOPTS=-DM16B")
    else
        MAKE_VARS+=("ARCHOPTS=$ARCHOPTS -DM16B")
    fi
    echo "# M16B: core will report RGB565 (no per-frame convert)"
fi

# The build configuration is not tracked by genie's dependency graph any better
# than the other engines' were -- changing a flag does not invalidate objects
# built under the old one. Same trap tools/build-m2003p.sh documents, same fix:
# stamp the configuration and force a clean when it moves. The stamp is written
# only on success, so an interrupted build re-cleans.
#
# The stamp is per-BUILDDIR, so alternating between the armhf and host builds
# does not read as a configuration change and force a clean of the one you are
# not building. That is the whole reason the two have separate build dirs.
SIG=$(printf '%s\n' "${MAKE_VARS[@]}")
STAMP="$SRC/.mister-build-config-$BUILDDIR"

run() {
    # A host build is native by definition -- never the armhf container.
    if [ "$HOST" = "1" ]; then
        ( cd "$SRC" && "$@" )
    elif command -v arm-linux-gnueabihf-g++ >/dev/null 2>&1; then
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

echo "# building mame_${SUBTARGET} for $([ "$HOST" = "1" ] && echo "the host" || echo armhf), -j$JOBS"
echo "#   SOURCES=$SOURCES  BUILDDIR=$BUILDDIR"
run nice make -f makefile "${MAKE_VARS[@]}" -j"$JOBS"

printf '%s\n' "$SIG" > "$STAMP"

# genie writes `<SUBTARGET>_libretro.so` into the SOURCE ROOT. libretro/bin/
# looks like the output directory in the generated makefiles but is only a -L
# search path, and there is no `mame_` prefix -- both worth stating because the
# host Makefile has to name this file exactly.
ls -lh "$SRC/${SUBTARGET}_libretro.so"

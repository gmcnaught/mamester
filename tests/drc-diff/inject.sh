#!/usr/bin/env bash
# Inject the DRC differential harness into vendor/lrmame.
#
#   tests/drc-diff/inject.sh            # copy sources + patch
#   tests/drc-diff/inject.sh --check    # report state, change nothing
#   tests/drc-diff/inject.sh --revert   # undo the patches, keep nothing
#
# Separate from tools/mame-drc-arm32/inject.sh, and separately revertible, for
# a reason that is not tidiness: the harness is useful on a host with NO arm32
# back-end at all. On x86_64 it diffs drcbe_c against drcbe_x64, which is a
# test OF THE HARNESS against a back-end two decades of drivers have already
# qualified -- so a failure there is the harness's fault, and that has to be
# established before the harness is pointed at a back-end being written. It
# therefore gates on CPU_INCLUDE_DRC, not on the arm32 flag.
#
# Three things are injected:
#
#   1. drc_diff.{h,cpp} -> src/devices/cpu/
#
#   2. scripts/src/cpu.lua -- adds them to the CPU_INCLUDE_DRC files block, so
#      they build wherever the UML does.
#
#   3. src/osd/libretro/libretro-internal/libretro.cpp -- one call at the top
#      of retro_run(). mame_machine_manager::instance()->machine() is a global
#      the OSD already uses, so no call-site plumbing is needed to reach a live
#      running_machine and its root_device().
#
# The call is compiled in unconditionally and does nothing unless
# MAMESTER_DRC_DIFF is set in the environment, so a core with the harness in it
# is the same core in normal use.
set -euo pipefail

SENTINEL='MAMESTER-DRC-DIFF'

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
SRC="${LRMAME_SRC:-$REPO/vendor/lrmame}"

MODE="${1:-inject}"

[ -f "$SRC/makefile" ] || {
    echo "vendor/lrmame missing — run: git submodule update --init vendor/lrmame" >&2
    exit 1
}

CPUDIR="$SRC/src/devices/cpu"
CPULUA="$SRC/scripts/src/cpu.lua"
LIBRETRO="$SRC/src/osd/libretro/libretro-internal/libretro.cpp"

case "$MODE" in
--check)
    for f in "$CPUDIR/drc_diff.h" "$CPUDIR/drc_diff.cpp"; do
        [ -f "$f" ] && echo "present: ${f#$SRC/}" || echo "ABSENT:  ${f#$SRC/}"
    done
    grep -q "$SENTINEL" "$CPULUA"   && echo "patched: scripts/src/cpu.lua" || echo "UNPATCHED: scripts/src/cpu.lua"
    grep -q "$SENTINEL" "$LIBRETRO" && echo "patched: src/osd/libretro/libretro-internal/libretro.cpp" || echo "UNPATCHED: src/osd/libretro/libretro-internal/libretro.cpp"
    exit 0
    ;;
--revert)
    rm -f "$CPUDIR/drc_diff.h" "$CPUDIR/drc_diff.cpp"
    python3 - "$CPULUA" "$LIBRETRO" "$SENTINEL" <<'PY'
import re, sys
sentinel = sys.argv[3]
for path in sys.argv[1:3]:
    text = open(path).read()
    text = re.sub(r'[ \t]*// *%s BEGIN.*?// *%s END\n' % (sentinel, sentinel), '', text, flags=re.S)
    text = re.sub(r'[ \t]*-- *%s BEGIN.*?-- *%s END\n' % (sentinel, sentinel), '', text, flags=re.S)
    open(path, 'w').write(text)
PY
    echo "reverted"
    exit 0
    ;;
inject) ;;
*) echo "usage: $0 [--check|--revert]" >&2; exit 2 ;;
esac

install -m 644 "$HERE/drc_diff.h"   "$CPUDIR/drc_diff.h"
install -m 644 "$HERE/drc_diff.cpp" "$CPUDIR/drc_diff.cpp"

python3 - "$CPULUA" "$LIBRETRO" "$SENTINEL" <<'PY'
import sys

cpulua_path, libretro_path, sentinel = sys.argv[1], sys.argv[2], sys.argv[3]

# ---- scripts/src/cpu.lua ----------------------------------------------------
text = open(cpulua_path).read()
if sentinel not in text:
    # Anchored on drcuml.h rather than on the block's opening or closing line:
    # the opening line is shared with other conditions in this file and the
    # closing "end" is not unique at all.
    anchor = '\t\tMAME_DIR .. "src/devices/cpu/drcuml.h",\n'
    if anchor not in text:
        sys.exit("cpu.lua: CPU_INCLUDE_DRC files block not found — upstream layout changed")
    text = text.replace(
        anchor,
        anchor +
        '-- %s BEGIN\n'
        '\t\tMAME_DIR .. "src/devices/cpu/drc_diff.cpp",\n'
        '\t\tMAME_DIR .. "src/devices/cpu/drc_diff.h",\n'
        '-- %s END\n' % (sentinel, sentinel),
        1)
    open(cpulua_path, 'w').write(text)
    print("patched scripts/src/cpu.lua")
else:
    print("scripts/src/cpu.lua already patched")

# ---- libretro.cpp -----------------------------------------------------------
text = open(libretro_path).read()
if sentinel not in text:
    anchor = '#include "libretro_core_options.h"\n'
    if anchor not in text:
        sys.exit("libretro.cpp: include anchor not found — upstream layout changed")
    text = text.replace(
        anchor,
        anchor + '// %s BEGIN\n#include "cpu/drc_diff.h"\n// %s END\n' % (sentinel, sentinel),
        1)

    # First statement in retro_run, so the harness runs before anything else
    # gets a chance to fatalerror on a machine that has no content in it.
    anchor = 'void retro_run(void)\n{\n'
    if anchor not in text:
        sys.exit("libretro.cpp: retro_run not found — upstream layout changed")
    text = text.replace(
        anchor,
        anchor +
        '   // %s BEGIN\n'
        '   // No-op unless MAMESTER_DRC_DIFF is set. root_device() is enough:\n'
        '   // the harness needs a live device_t for machine().options(), not for\n'
        '   // anything it emulates.\n'
        '   //\n'
        '   // The null test on diff_run_once is NOT redundant: cpu.lua compiles\n'
        '   // drc_diff.cpp in the CPU_INCLUDE_DRC block, so a SOURCES subset with\n'
        '   // no DRC-backed CPU (pacman, say) leaves it out entirely and the\n'
        '   // symbol is weak-undefined here. See drc_diff.h.\n'
        '   if (drc::diff_run_once != NULL\n'
        '         && mame_machine_manager::instance() != NULL\n'
        '         && mame_machine_manager::instance()->machine() != NULL)\n'
        '      drc::diff_run_once(mame_machine_manager::instance()->machine()->root_device());\n'
        '   // %s END\n' % (sentinel, sentinel),
        1)
    open(libretro_path, 'w').write(text)
    print("patched src/osd/libretro/libretro-internal/libretro.cpp")
else:
    print("libretro.cpp already patched")
PY

echo "injected into ${SRC#$REPO/}"

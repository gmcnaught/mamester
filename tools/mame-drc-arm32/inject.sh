#!/usr/bin/env bash
# Inject the ARM32 DRC back-end into vendor/lrmame.
#
#   tools/mame-drc-arm32/inject.sh            # copy sources + patch
#   tools/mame-drc-arm32/inject.sh --check    # report state, change nothing
#   tools/mame-drc-arm32/inject.sh --revert   # undo the patches, keep nothing
#
# The submodule stays pristine in git terms -- this is the same arrangement
# tools/build-mame.sh uses to put the MiSTer present back-end into mame4all,
# and for the same reason: a vendored tree that is a submodule cannot carry
# our commits, so our sources live here and are copied in at build time.
#
# Three things are injected, and only the first is a file copy:
#
#   1. arm32emit.h, drcbearm32.{h,cpp}  ->  src/devices/cpu/
#
#   2. src/devices/cpu/drcuml.cpp -- the NATIVE_DRC selection chain. Upstream
#      picks drcbe_x64 for __x86_64__, drcbe_arm64 for __aarch64__ and falls
#      through to drcbe_c for everything else, which is how a Cortex-A9 ends
#      up on the UML interpreter.
#
#   3. scripts/src/cpu.lua -- CPU_INCLUDE_DRC_NATIVE, which gates the native
#      back-end sources on PLATFORM being x86 or arm64. Note this adds a
#      SEPARATE files{} block for arm rather than widening the existing one:
#      the existing block builds drcbex64.cpp and drcbearm64.cpp together, and
#      there is no reason to hand an armhf compiler either of them.
#
# Every patch is idempotent and marked with the sentinel below, which is also
# what --check and --revert look for.
set -euo pipefail

SENTINEL='MAMESTER-ARM32-DRC'

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
SRC="${LRMAME_SRC:-$REPO/vendor/lrmame}"

MODE="${1:-inject}"

[ -f "$SRC/makefile" ] || {
    echo "vendor/lrmame missing — run: git submodule update --init vendor/lrmame" >&2
    exit 1
}

CPUDIR="$SRC/src/devices/cpu"
DRCUML="$CPUDIR/drcuml.cpp"
CPULUA="$SRC/scripts/src/cpu.lua"

case "$MODE" in
--check)
    for f in "$CPUDIR/arm32emit.h" "$CPUDIR/drcbearm32.h" "$CPUDIR/drcbearm32.cpp"; do
        [ -f "$f" ] && echo "present: ${f#$SRC/}" || echo "ABSENT:  ${f#$SRC/}"
    done
    grep -q "$SENTINEL" "$DRCUML" && echo "patched: src/devices/cpu/drcuml.cpp" || echo "UNPATCHED: src/devices/cpu/drcuml.cpp"
    grep -q "$SENTINEL" "$CPULUA" && echo "patched: scripts/src/cpu.lua" || echo "UNPATCHED: scripts/src/cpu.lua"
    exit 0
    ;;
--revert)
    rm -f "$CPUDIR/arm32emit.h" "$CPUDIR/drcbearm32.h" "$CPUDIR/drcbearm32.cpp"
    python3 - "$DRCUML" "$CPULUA" "$SENTINEL" <<'PY'
import re, sys
sentinel = sys.argv[3]
for path in sys.argv[1:3]:
    text = open(path).read()
    # drop every line group fenced by the sentinel markers
    text = re.sub(r'[ \t]*// *%s BEGIN.*?// *%s END\n' % (sentinel, sentinel), '', text, flags=re.S)
    text = re.sub(r'[ \t]*-- *%s BEGIN.*?-- *%s END\n' % (sentinel, sentinel), '', text, flags=re.S)
    text = text.replace(' or (_OPTIONS["PLATFORM"] == "arm") --[[%s]]' % sentinel, '')
    open(path, 'w').write(text)
PY
    echo "reverted"
    exit 0
    ;;
inject) ;;
*) echo "usage: $0 [--check|--revert]" >&2; exit 2 ;;
esac

install -m 644 "$HERE/arm32emit.h"    "$CPUDIR/arm32emit.h"
install -m 644 "$HERE/drcbearm32.h"   "$CPUDIR/drcbearm32.h"
install -m 644 "$HERE/drcbearm32.cpp" "$CPUDIR/drcbearm32.cpp"

python3 - "$DRCUML" "$CPULUA" "$SENTINEL" <<'PY'
import sys

drcuml_path, cpulua_path, sentinel = sys.argv[1], sys.argv[2], sys.argv[3]

# ---- drcuml.cpp -------------------------------------------------------------
text = open(drcuml_path).read()
if sentinel not in text:
    anchor = '#include "drcbearm64.h"\n'
    if anchor not in text:
        sys.exit("drcuml.cpp: include anchor not found — upstream layout changed")
    text = text.replace(
        anchor,
        anchor + '// %s BEGIN\n#include "drcbearm32.h"\n// %s END\n' % (sentinel, sentinel),
        1)

    # Insert ahead of the aarch64 arm, not after it. __arm__ and __aarch64__ are
    # mutually exclusive so the order does not change behaviour, but keeping the
    # 32-bit arm adjacent to the 64-bit one keeps the chain readable.
    anchor = '#elif !defined(MAME_NOASM) && (defined(__aarch64__) || defined(_M_ARM64))\n#define NATIVE_DRC drcbe_arm64\n'
    if anchor not in text:
        sys.exit("drcuml.cpp: NATIVE_DRC chain not found — upstream layout changed")
    text = text.replace(
        anchor,
        anchor +
        '// %s BEGIN\n'
        '#elif !defined(MAME_NOASM) && defined(__arm__)\n'
        '#define NATIVE_DRC drcbe_arm32\n'
        '// %s END\n' % (sentinel, sentinel),
        1)
    open(drcuml_path, 'w').write(text)
    print("patched src/devices/cpu/drcuml.cpp")
else:
    print("src/devices/cpu/drcuml.cpp already patched")

# ---- scripts/src/cpu.lua ----------------------------------------------------
text = open(cpulua_path).read()
if sentinel not in text:
    anchor = 'CPU_INCLUDE_DRC_NATIVE = CPU_INCLUDE_DRC and (not _OPTIONS["FORCE_DRC_C_BACKEND"]) and ((_OPTIONS["PLATFORM"] == "x86") or (_OPTIONS["PLATFORM"] == "arm64"))\n'
    if anchor not in text:
        sys.exit("cpu.lua: CPU_INCLUDE_DRC_NATIVE not found — upstream layout changed")
    text = text.replace(
        anchor,
        anchor +
        '-- %s BEGIN\n'
        'CPU_INCLUDE_DRC_ARM32 = CPU_INCLUDE_DRC and (not _OPTIONS["FORCE_DRC_C_BACKEND"]) and (_OPTIONS["PLATFORM"] == "arm")\n'
        '-- %s END\n' % (sentinel, sentinel),
        1)

    anchor = ('if CPU_INCLUDE_DRC_NATIVE then\n'
              '\tfiles {\n'
              '\t\tMAME_DIR .. "src/devices/cpu/drcbearm64.cpp",\n'
              '\t\tMAME_DIR .. "src/devices/cpu/drcbearm64.h",\n'
              '\t\tMAME_DIR .. "src/devices/cpu/drcbex64.cpp",\n'
              '\t\tMAME_DIR .. "src/devices/cpu/drcbex64.h",\n'
              '\t}\n'
              'end\n')
    if anchor not in text:
        sys.exit("cpu.lua: native files block not found — upstream layout changed")
    text = text.replace(
        anchor,
        anchor +
        '-- %s BEGIN\n'
        'if CPU_INCLUDE_DRC_ARM32 then\n'
        '\tfiles {\n'
        '\t\tMAME_DIR .. "src/devices/cpu/arm32emit.h",\n'
        '\t\tMAME_DIR .. "src/devices/cpu/drcbearm32.cpp",\n'
        '\t\tMAME_DIR .. "src/devices/cpu/drcbearm32.h",\n'
        '\t}\n'
        'end\n'
        '-- %s END\n' % (sentinel, sentinel),
        1)
    open(cpulua_path, 'w').write(text)
    print("patched scripts/src/cpu.lua")
else:
    print("scripts/src/cpu.lua already patched")
PY

echo "injected into ${SRC#$REPO/}"

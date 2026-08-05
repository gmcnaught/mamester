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
#   4. 3rdparty/asmjit -- the AArch32 encoder. MAME vendors asmjit with a64*
#      only; the a32 sources come from vendor/asmjit-a32, pinned to asmjit's
#      unmerged a32_port branch, plus that branch's ~70 lines of core hooks and
#      the local defect fixes in asmjit-a32-fixes.py. scripts/src/3rdparty.lua
#      gates the whole asmjit project on PLATFORM x86/arm64, so that opens too.
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
    [ -f "$SRC/3rdparty/asmjit/asmjit/a32.h" ] && echo "present: 3rdparty/asmjit a32 sources" || echo "ABSENT:  3rdparty/asmjit a32 sources"
    grep -q "$SENTINEL" "$SRC/scripts/src/3rdparty.lua" && echo "patched: scripts/src/3rdparty.lua" || echo "UNPATCHED: scripts/src/3rdparty.lua"
    exit 0
    ;;
--revert)
    rm -f "$CPUDIR/arm32emit.h" "$CPUDIR/drcbearm32.h" "$CPUDIR/drcbearm32.cpp"
    # asmjit is upstream's tree with edits on top, so git restores it exactly
    git -C "$SRC" checkout -- 3rdparty/asmjit scripts/src/3rdparty.lua 2>/dev/null || true
    git -C "$SRC" clean -qfd 3rdparty/asmjit 2>/dev/null || true
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
    # arm32 is folded INTO CPU_INCLUDE_DRC_NATIVE rather than kept in a parallel
    # flag, so that everything upstream gates on "the native DRC is available"
    # turns on for PLATFORM=arm exactly where it turns on for arm64 -- including
    # the i386 disassembler at the bottom of this file, which arm64 already
    # pulls in. A parallel variable would have had to be added to each of those
    # conditions by hand, and any future one would silently miss arm32.
    text = text.replace(
        anchor,
        '-- %s BEGIN\n'
        'CPU_INCLUDE_DRC_NATIVE = CPU_INCLUDE_DRC and (not _OPTIONS["FORCE_DRC_C_BACKEND"]) and ((_OPTIONS["PLATFORM"] == "x86") or (_OPTIONS["PLATFORM"] == "arm64") or (_OPTIONS["PLATFORM"] == "arm"))\n'
        'CPU_INCLUDE_DRC_ARM32 = CPU_INCLUDE_DRC_NATIVE and (_OPTIONS["PLATFORM"] == "arm")\n'
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
    # The FILES stay architecture-split even though the flag is now shared:
    # drcbex64.cpp and drcbearm64.cpp do not compile for a 32-bit ARM target, so
    # "enabled in the same places" must not mean "same source list".
    text = text.replace(
        anchor,
        '-- %s BEGIN\n'
        'if CPU_INCLUDE_DRC_NATIVE and not CPU_INCLUDE_DRC_ARM32 then\n'
        '\tfiles {\n'
        '\t\tMAME_DIR .. "src/devices/cpu/drcbearm64.cpp",\n'
        '\t\tMAME_DIR .. "src/devices/cpu/drcbearm64.h",\n'
        '\t\tMAME_DIR .. "src/devices/cpu/drcbex64.cpp",\n'
        '\t\tMAME_DIR .. "src/devices/cpu/drcbex64.h",\n'
        '\t}\n'
        'end\n'
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

# ---- asmjit AArch32 ---------------------------------------------------------
#
# MAME vendors asmjit with a64* only. The a32 encoder comes from
# vendor/asmjit-a32, a submodule pinned to asmjit's unmerged a32_port branch.
#
# The branch's own changes to core/ and x86/ are ~70 lines across seven files
# and are taken straight from the commit rather than carried here as a stored
# patch, so provenance stays with upstream. They apply to MAME's copy despite
# the two trees sitting at different points on master (16 shared files differ)
# -- both are ASMJIT_LIBRARY_VERSION 1.21.0, which is why this works at all.
#
# asmjit-a32-fixes.py then applies the two defects tests/a32-asmjit/ found.
# Those are OUR patches on upstream's WIP, not integration glue, and they are
# the reason the encoder can be trusted with a shift-by-32.

ASMJIT_A32="$REPO/vendor/asmjit-a32"
MAMEASM="$SRC/3rdparty/asmjit"
LUA3RD="$SRC/scripts/src/3rdparty.lua"

[ -f "$ASMJIT_A32/asmjit/arm/a32assembler.cpp" ] || {
    echo "vendor/asmjit-a32 missing — run: git submodule update --init vendor/asmjit-a32" >&2
    exit 1
}

if [ ! -f "$MAMEASM/asmjit/a32.h" ]; then
    install -m 644 "$ASMJIT_A32/asmjit/a32.h" "$MAMEASM/asmjit/a32.h"
    install -m 644 "$ASMJIT_A32"/asmjit/arm/a32*.h "$ASMJIT_A32"/asmjit/arm/a32*.cpp "$MAMEASM/asmjit/arm/"

    git -C "$ASMJIT_A32" diff HEAD~1 HEAD -- asmjit/core asmjit/x86 asmjit/arm/armformatter.cpp \
        | patch -s -p1 -d "$MAMEASM" --fuzz=5
    echo "installed asmjit a32 sources + upstream core hooks"
fi

python3 "$HERE/asmjit-a32-fixes.py" "$MAMEASM"

python3 - "$LUA3RD" "$SENTINEL" <<'PY2'
import sys

path, sentinel = sys.argv[1], sys.argv[2]
text = open(path).read()
if sentinel in text:
    print("scripts/src/3rdparty.lua already patched")
    sys.exit(0)

# The whole asmjit project is gated on the same x86/arm64 pair cpu.lua uses.
anchor = 'if (not _OPTIONS["FORCE_DRC_C_BACKEND"]) and ((_OPTIONS["PLATFORM"] == "x86") or (_OPTIONS["PLATFORM"] == "arm64")) then\nproject "asmjit"\n'
if anchor not in text:
    sys.exit("3rdparty.lua: asmjit project gate not found — upstream layout changed")
text = text.replace(
    anchor,
    'if (not _OPTIONS["PLATFORM"]) then end -- %s BEGIN\n'
    'if (not _OPTIONS["FORCE_DRC_C_BACKEND"]) and ((_OPTIONS["PLATFORM"] == "x86") or (_OPTIONS["PLATFORM"] == "arm64") or (_OPTIONS["PLATFORM"] == "arm")) then\n'
    '-- %s END\n'
    'project "asmjit"\n' % (sentinel, sentinel),
    1)

# Add the a32 sources to the file list. They are appended rather than slotted
# in alphabetically so the edit stays a single anchored insertion.
anchor = '\t\tMAME_DIR .. "3rdparty/asmjit/asmjit/a64.h",\n'
if anchor not in text:
    sys.exit("3rdparty.lua: asmjit files block not found — upstream layout changed")
a32 = ['a32.h', 'arm/a32archtraits_p.h', 'arm/a32assembler.cpp', 'arm/a32assembler.h',
       'arm/a32builder.cpp', 'arm/a32builder.h', 'arm/a32emithelper.cpp',
       'arm/a32emithelper_p.h', 'arm/a32emitter.h', 'arm/a32formatter.cpp',
       'arm/a32formatter_p.h', 'arm/a32globals.h', 'arm/a32instapi.cpp',
       'arm/a32instapi_p.h', 'arm/a32instdb.cpp', 'arm/a32instdb_p.h',
       'arm/a32operand.h']
block = '-- %s BEGIN\n' % sentinel
block += ''.join('\t\tMAME_DIR .. "3rdparty/asmjit/asmjit/%s",\n' % f for f in a32)
block += '-- %s END\n' % sentinel
text = text.replace(anchor, anchor + block, 1)

open(path, 'w').write(text)
print("patched scripts/src/3rdparty.lua")
PY2

echo "injected into ${SRC#$REPO/}"

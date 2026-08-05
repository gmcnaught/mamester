#!/usr/bin/env bash
# List the MAME driver files that depend on a DRC-capable CPU, so the lrmame
# driver subset can exclude them mechanically instead of by memory.
#
#   tools/lrmame-drc-scan.sh              # one driver .cpp path per line
#   tools/lrmame-drc-scan.sh --summary    # counts, and the per-vendor breakdown
#
# WHY THIS EXISTS
#
# MAME 0.289 ships three DRC backends -- drcbex64, drcbearm64, drcbec -- and
# scripts/src/cpu.lua:24 gates the two native ones to PLATFORM x86 or arm64.
# There is no 32-bit native backend of ANY architecture (x86-32's was retired
# too), and the Cyclone V HPS is a Cortex-A9: ARMv7-A, no 64-bit mode, so
# drcbearm64 can never apply here. Everything below therefore runs drcbec, a
# portable UML interpreter.
#
# That sounds worse than it is, which is the point of measuring it: the affected
# set is ~3% of driver files. Z80, 6502, 6809, 68000/68020 and V60 never touch
# drcuml at all, so FORCE_DRC_C_BACKEND costs the drivers this port actually
# targets exactly nothing -- verified against the Stage-8 gap families
# taito_f3, konamigx, segas24 and kaneko16, none of which include a DRC CPU.
#
# Operator decision (2026-08-05): DESCOPE rather than chase it. Writing a
# drcbearm32 backend is a ~5,700-line project (drcbearm64.cpp's size) made
# harder than the arm64 one by ARMv7 having ~14 usable GPRs against 31 and no
# 64-bit registers for a 64-bit-register IR -- against a slice of the library
# that is mostly unreachable on this silicon for unrelated reasons.
#
# WHAT IS ACTUALLY GIVEN UP
#
# Most of this list is out of scope regardless: SGI workstations, Apple Macs,
# skeleton drivers, Jaguar. The genuine casualties are the SH-2/SH-3 arcade
# boards that would otherwise have been borderline -- psikyosh, stv, feversoc,
# cv1000. Note psikyosh in particular: it is a named gap target in CLAUDE.md,
# and it declares its CPU in psikyosh.h rather than the .cpp, which is why this
# script scans headers too and maps them back to their driver.
#
# The CPU list is the set of src/devices/cpu/ modules that include drcuml.h,
# regenerate with:
#   grep -rl 'drcuml' vendor/lrmame/src/devices/cpu/*/ | xargs -n1 dirname \
#     | sort -u | xargs -n1 basename | sort -u
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${LRMAME_SRC:-$REPO/vendor/lrmame}"
MAMEDIR="$SRC/src/mame"

[ -d "$MAMEDIR" ] || {
    echo "vendor/lrmame missing — run: git submodule update --init vendor/lrmame" >&2
    exit 1
}

DRC_CPUS='sh|mips|powerpc|e132xs|unsp|sharc|dsp16|mb86235|dspp'

# A driver's CPU include may live in either half of a .cpp/.h pair, so both are
# scanned and headers are folded onto their driver before de-duplicating.
list() {
    grep -rlE "#include \"cpu/($DRC_CPUS)/" "$MAMEDIR" \
        --include='*.cpp' --include='*.h' 2>/dev/null \
        | sed 's/\.h$/.cpp/' \
        | sed "s|^$MAMEDIR/||" \
        | sort -u
}

if [ "${1:-}" = "--summary" ]; then
    total=$(find "$MAMEDIR" -name '*.cpp' | wc -l)
    hit=$(list | wc -l)
    printf 'DRC-dependent driver files: %s of %s (%.1f%%)\n' \
        "$hit" "$total" "$(echo "$hit $total" | awk '{print 100*$1/$2}')"
    echo
    echo 'by vendor directory:'
    list | cut -d/ -f1 | sort | uniq -c | sort -rn
else
    list
fi

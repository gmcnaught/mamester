#!/bin/bash
#============================================================================
#
#  build_mame.sh -- Build the OpenBOR MiSTer RBF from the command line.
#
#  Run from fpga/ directory. Requires Quartus Prime Lite 17.0+ in PATH
#  or at the standard Windows location.
#
#  Usage: ./build_mame.sh [output_dir]
#  Default output: ../_Other/
#
#  Copyright (C) 2026 MiSTer Organize -- GPL-3.0
#
#============================================================================

set -e

OUTPUT_DIR="${1:-../_Other}"
PROJECT="MAME"           # matches OpenBOR.qpf (don't change)
# Lean scanout build. NOTE the prefix deliberately contains neither
# "OpenBOR" nor "7533": the MiSTer_Frontier Master_Daemon / _handler.sh
# dispatch the busy-polling OpenBOR ARM engine when EITHER the core setname
# is "OpenBOR" OR the loaded RBF path matches *7533*/*4086*. This lean core
# uses setname "ScanOut" and this neutral filename so neither trigger fires.
RBF_PREFIX="MAME"        # output filename prefix
DATE=$(date +%Y%m%d)

# Locate quartus_sh. Prefer PATH, then the raetro/quartus Docker image (CI),
# then the Windows default.
if command -v quartus_sh >/dev/null 2>&1; then
    QUARTUS_SH=quartus_sh
elif [ -x "/opt/intelFPGA/quartus/bin/quartus_sh" ]; then
    # raetro/quartus:17.0 Docker image (Linux CI) — abs path in case PATH was reset.
    QUARTUS_SH="/opt/intelFPGA/quartus/bin/quartus_sh"
elif [ -x "/c/intelFPGA_lite/17.0/quartus/bin64/quartus_sh.exe" ]; then
    QUARTUS_SH="/c/intelFPGA_lite/17.0/quartus/bin64/quartus_sh.exe"
else
    echo "ERROR: quartus_sh not found in PATH, /opt/intelFPGA, or /c/intelFPGA_lite/17.0/"
    exit 1
fi

echo "============================================"
echo "  MiSTer_MAME -- Quartus Build"
echo "  Quartus: $QUARTUS_SH"
echo "============================================"
echo ""

# Generate build_id.v (date-stamped)
echo "\`define BUILD_DATE \"$(date +%y%m%d)\"" > build_id.v

# Compile
echo ">>> Running quartus_sh --flow compile $PROJECT ..."
"$QUARTUS_SH" --flow compile "$PROJECT" 2>&1 | tee "build_${DATE}.log"

SRC_RBF="output_files/${PROJECT}.rbf"
if [ ! -f "$SRC_RBF" ]; then
    echo ""
    echo "ERROR: RBF not produced. See build_${DATE}.log"
    exit 1
fi

mkdir -p "$OUTPUT_DIR"
DST_RBF="$OUTPUT_DIR/${RBF_PREFIX}_${DATE}.rbf"
cp "$SRC_RBF" "$DST_RBF"
SIZE=$(ls -lh "$DST_RBF" | awk '{print $5}')

echo ""
echo "============================================"
echo "  Build complete"
echo "  $DST_RBF ($SIZE)"
echo "============================================"

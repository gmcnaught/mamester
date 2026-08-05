#!/usr/bin/env bash
# Differential test for tools/mame-drc-arm32/arm32emit.h.
#
#   sh tests/arm32emit/run.sh
#
# Builds gen.cpp natively (the encoder is pure computation -- it never runs the
# instructions it encodes, so it needs no ARM host), then assembles the same
# instructions with arm-linux-gnueabihf-as and compares word for word.
#
# Requires binutils-arm-linux-gnueabihf. If the cross assembler is missing this
# falls back to the build container, which has it.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
WORK="${TMPDIR:-/tmp}/arm32emit-test.$$"
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

AS="${AS:-arm-linux-gnueabihf-as}"
OBJCOPY="${OBJCOPY:-arm-linux-gnueabihf-objcopy}"

command -v "$AS" >/dev/null 2>&1 || {
    echo "$AS not found — install binutils-arm-linux-gnueabihf" >&2
    exit 2
}

g++ -std=c++17 -O1 -Wall -Wextra -Wno-unused-parameter \
    -o "$WORK/gen" "$HERE/gen.cpp"

( cd "$WORK" && ./gen )

"$AS" -march=armv7-a -mfpu=neon -o "$WORK/ref.o" "$WORK/ref.s"
"$OBJCOPY" -O binary --only-section=.text "$WORK/ref.o" "$WORK/ref.bin"

python3 - "$WORK" <<'PY'
import struct, sys, os

work = sys.argv[1]
ours = open(os.path.join(work, "ours.bin"), "rb").read()
ref  = open(os.path.join(work, "ref.bin"), "rb").read()
texts = open(os.path.join(work, "texts.txt")).read().splitlines()

if len(ours) != len(ref):
    print(f"FAIL: length mismatch — encoder {len(ours)//4} words, "
          f"assembler {len(ref)//4} words")
    print("      (an assembler macro expanded to more than one instruction)")
    sys.exit(1)

bad = 0
for i in range(len(ours) // 4):
    a, = struct.unpack_from("<I", ours, i * 4)
    b, = struct.unpack_from("<I", ref,  i * 4)
    if a != b:
        bad += 1
        if bad <= 25:
            print(f"  {texts[i]:<40s} encoder {a:08x}  as {b:08x}  diff {a ^ b:08x}")

n = len(ours) // 4
if bad:
    print(f"FAIL: {bad} of {n} instructions disagree with the assembler")
    sys.exit(1)
print(f"ok: {n} instructions match arm-linux-gnueabihf-as exactly")
PY

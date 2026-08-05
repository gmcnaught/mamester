#!/usr/bin/env bash
# Qualify asmjit's unmerged a32_port branch as a possible encoder for
# drcbearm32, by diffing its output against arm-linux-gnueabihf-as.
#
#   bash tests/a32-asmjit/run.sh
#
# Clones asmjit into a work directory (or reuses ASMJIT_SRC if set), builds the
# a32 back-end natively -- it is a cross-assembler, so an x86 host is fine --
# and runs the corpus in gen.cpp.
#
# Requires: g++, binutils-arm-linux-gnueabihf, network access on first run.
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="${A32_WORK:-${TMPDIR:-/tmp}/a32-asmjit-eval}"
SRC="${ASMJIT_SRC:-$WORK/asmjit}"
AS="${AS:-arm-linux-gnueabihf-as}"
OBJCOPY="${OBJCOPY:-arm-linux-gnueabihf-objcopy}"

command -v "$AS" >/dev/null 2>&1 || { echo "$AS not found" >&2; exit 2; }
mkdir -p "$WORK"

if [ ! -d "$SRC" ]; then
    echo "# cloning asmjit a32_port"
    git clone --filter=blob:none --branch a32_port --single-branch \
        https://github.com/asmjit/asmjit.git "$SRC"
fi

# asmjit's a32 emitter never installs _funcs.format_instruction, so the
# invalid-instruction path in log_instruction_failed() calls through a null
# pointer -- a REFUSED instruction crashes instead of returning its error.
# Guarded here so the harness can report refusals; upstream this is a real
# robustness bug for any JIT that hits an unsupported operand combination.
GUARD="$SRC/asmjit/core/emitterutils.cpp"
if ! grep -q "MAMESTER-GUARD" "$GUARD"; then
    echo "# patching the null-formatter crash in log_instruction_failed()"
    python3 - "$GUARD" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
# Anchor inside log_instruction_failed specifically -- the same call appears
# first in log_instruction_emitted, which has no `err` in scope.
fn = s.index("Error log_instruction_failed(")
call = s.index("  self->_funcs.format_instruction(sb,", fn)
guard = ("  // MAMESTER-GUARD: a32 leaves _funcs.format_instruction null\n"
         "  if (!self->_funcs.format_instruction) { self->reset_state(); return self->report_error(err, sb.data()); }\n")
open(p, 'w').write(s[:call] + guard + s[call:])
PY
fi

echo "# building (first run takes a few minutes)"
g++ -std=c++17 -O1 -I"$SRC" -DASMJIT_STATIC -DASMJIT_NO_COMPILER \
    -o "$WORK/gen" "$HERE/gen.cpp" \
    "$SRC"/asmjit/core/*.cpp "$SRC"/asmjit/arm/*.cpp \
    "$SRC"/asmjit/support/*.cpp "$SRC"/asmjit/x86/*.cpp

cd "$WORK"
./gen

"$AS" -march=armv7-a -mfpu=neon -o ref.o ref.s
"$OBJCOPY" -O binary --only-section=.text ref.o ref.bin

python3 - <<'PY'
import struct, sys
ours = open("ours.bin", "rb").read()
ref  = open("ref.bin", "rb").read()
texts = open("texts.txt").read().splitlines()

if len(ours) != len(ref):
    print(f"FAIL: length mismatch — asmjit {len(ours)//4}, as {len(ref)//4}")
    sys.exit(1)

bad = 0
for i in range(len(ours) // 4):
    a, = struct.unpack_from("<I", ours, i * 4)
    b, = struct.unpack_from("<I", ref,  i * 4)
    if a != b:
        bad += 1
        print(f"  {texts[i]:<32s} asmjit {a:08x}  as {b:08x}")

n = len(ours) // 4
if bad:
    print(f"FAIL: {bad} of {n} encodings disagree with the assembler")
    sys.exit(1)
print(f"ok: {n} encodings match arm-linux-gnueabihf-as exactly")
PY

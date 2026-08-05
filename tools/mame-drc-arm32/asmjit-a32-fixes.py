#!/usr/bin/env python3
"""Local fixes to asmjit's a32_port branch, applied when it is injected.

Both are defects found by tests/a32-asmjit/ and both are upstream bugs rather
than integration friction -- they are kept here as a scripted transformation
rather than a .patch because MAME's vendored asmjit sits at a different point
on master than a32_port's base (16 shared files differ), and anchored edits
survive that drift where context diffs do not.

Run by tools/mame-drc-arm32/inject.sh; idempotent.

    asmjit-a32-fixes.py <path-to-asmjit-root>
"""

import sys
import os

MARKER = "MAMESTER-A32FIX"

SHIFT_HELPER = '''
// ''' + MARKER + ''': A32 encodes an immediate shift amount in five bits, and the
// mapping from "shift by N" to those bits is not the identity:
//
//     LSL   0..31    amount == encoding
//     LSR   1..32    32 encodes as 0  (there is no LSR #0)
//     ASR   1..32    32 encodes as 0  (there is no ASR #0)
//     ROR   1..31    0 would be RRX, which is a different instruction
//
// a32_port validates all four as `amount <= 31`, which is wrong in both
// directions at once. It REJECTS the legal `lsr #32` / `asr #32`, and it
// ACCEPTS `lsr #0` / `asr #0` and silently encodes them as shift-by-32 --
// confirmed against arm-linux-gnueabihf-as, where asmjit's lsr(0) and the
// assembler's `lsr #32` are the same word, e0843025. The silent half is the
// dangerous one.
//
// This is load-bearing for drcbearm32 rather than a corner case: synthesising
// a 64-bit shift on a 32-bit host reaches for shift-by-32 constantly.
//
// Not fixed here, same bug class, outside the UML lowering's path: the pkhbt /
// pkhtb / ssat sites, which have their own per-instruction shift rules.
static inline bool mamester_normalize_shift(uint32_t shift_op, uint64_t& shift_imm) noexcept {
  switch (shift_op) {
    case uint32_t(ShiftOp::kLSL):
      return shift_imm <= 31u;
    case uint32_t(ShiftOp::kLSR):
    case uint32_t(ShiftOp::kASR):
      if (shift_imm == 32u) { shift_imm = 0u; return true; }
      return shift_imm >= 1u && shift_imm <= 31u;
    case uint32_t(ShiftOp::kROR):
      return shift_imm >= 1u && shift_imm <= 31u;
    default:
      return false;
  }
}
'''

OLD_COND = "if (shift_op <= 3 && shiftImm <= 31u) {"
NEW_COND = "if (mamester_normalize_shift(shift_op, shiftImm)) {"


def fix_shift(root):
    path = os.path.join(root, "asmjit/arm/a32assembler.cpp")
    text = open(path).read()
    if MARKER in text:
        return "a32assembler.cpp already fixed"

    anchor = "ASMJIT_BEGIN_SUB_NAMESPACE(a32)\n"
    if anchor not in text:
        sys.exit("a32assembler.cpp: namespace anchor not found")
    text = text.replace(anchor, anchor + SHIFT_HELPER, 1)

    count = text.count(OLD_COND)
    if count != 3:
        sys.exit(f"a32assembler.cpp: expected 3 general shift sites, found {count}")
    text = text.replace(OLD_COND, NEW_COND)

    open(path, "w").write(text)
    return f"a32assembler.cpp: shift validation fixed at {count} sites"


def fix_formatter(root):
    """A rejected instruction must return its error, not dereference null.

    EmitterUtils::log_instruction_failed() calls _funcs.format_instruction,
    which the a32 emitter never installs, so any unsupported operand
    combination segfaults with no diagnostic. For a DRC that turns a lowering
    bug into a crash with nothing to read.
    """
    path = os.path.join(root, "asmjit/core/emitterutils.cpp")
    text = open(path).read()
    if MARKER in text:
        return "emitterutils.cpp already fixed"

    # Anchor inside log_instruction_failed specifically -- the same call
    # appears first in log_instruction_emitted, which has no `err` in scope.
    try:
        fn = text.index("Error log_instruction_failed(")
        call = text.index("  self->_funcs.format_instruction(sb,", fn)
    except ValueError:
        sys.exit("emitterutils.cpp: log_instruction_failed no longer looks like this")

    guard = (
        "  // " + MARKER + ": the a32 emitter never installs a formatter, so\n"
        "  // reporting a rejected instruction would call through null.\n"
        "  if (!self->_funcs.format_instruction) {\n"
        "    self->reset_state();\n"
        "    return self->report_error(err, sb.data());\n"
        "  }\n"
    )
    open(path, "w").write(text[:call] + guard + text[call:])
    return "emitterutils.cpp: null-formatter crash guarded"


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    root = sys.argv[1]
    if not os.path.isfile(os.path.join(root, "asmjit/arm/a32assembler.cpp")):
        sys.exit(f"{root}: not an asmjit tree with a32 sources")
    print("  " + fix_shift(root))
    print("  " + fix_formatter(root))


if __name__ == "__main__":
    main()

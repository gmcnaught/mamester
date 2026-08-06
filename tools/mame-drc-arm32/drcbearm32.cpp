// license:BSD-3-Clause
// copyright-holders:MAMESTer port
// Derived from drcbex86.cpp (Aaron Giles) and drcbearm64.cpp (windyfairy, Vas Crabb)
/***************************************************************************

    drcbearm32.cpp

    32-bit ARM (ARMv7-A, A32) back-end for the universal machine language.

****************************************************************************

    ---------------
    Where this came from
    ---------------

    This is drcbex86 retargeted, not drcbearm64 backported. MAME carried a
    32-bit back-end -- drcbex86 -- through mame0287 and deleted it in
    0.288/0.289, so the hard parts of running a 64-bit-register IR on a
    32-bit host (register pairs, synthesised 64-bit shifts and multiplies,
    flag reconstruction) have an in-tree solution with two decades of driver
    testing behind it. uml.h is byte-identical between 0.287 and 0.289, so the
    IR this lowers is exactly the IR drcbex86 lowered.

    drcbearm64 contributes the ARM-specific part, and it is not a detail: the
    lazy carry state machine below is copied from it rather than re-derived,
    because carry polarity is the single most error-prone piece of an ARM UML
    back-end and that version already works.

    ---------------
    The encoder
    ---------------

    asmjit's AArch32 back-end, from its unmerged a32_port branch, injected into
    MAME's vendored asmjit by tools/mame-drc-arm32/inject.sh. Chosen over the
    hand-written encoder in arm32emit.h for one reason: drcbex86 is itself
    written against asmjit, so retargeting it happens inside the same
    CodeHolder/Label/Mem/relocation machinery rather than across an API
    boundary.

    a32_port is upstream WIP and is treated as such. tests/a32-asmjit/ diffs it
    against arm-linux-gnueabihf-as over the subset this lowering needs -- 130
    encodings, all matching -- and asmjit-a32-fixes.py carries the two defects
    that test found. arm32emit.h is kept as the fallback and as the oracle the
    corpus was built from.

    Three API traps, all of which cost time here:
      * The shift operation lives in the predicate of the LAST operand, so it
        is add(rd, rn, rm, lsr(16)), never add(rd, rn, lsr(rm), imm(16)). The
        wrong form silently encodes as LSL, because LSL is predicate 0.
      * A rejected instruction segfaulted rather than returning its error, as
        the a32 emitter installs no formatter and the reporting path calls
        through it. Fixed locally; do not drop that fix.
      * There is no emitter entry for VMRS/VMSR, and no way to build the
        coprocessor-register operand a32's MRC/MCR path wants -- kOpRegC is
        signature zero and nothing constructs it. FPSCR access is therefore
        embed_uint32() of the literal word, which tests/a32-asmjit/ checks
        against the assembler like everything else.

    Design rationale in
    docs/superpowers/specs/2026-08-05-drcbearm32-design.md.

    ---------------
    ABI/conventions (AAPCS, hard-float)
    ---------------

    Registers:
        r0-r3      - volatile, arguments, r0/r1 return value
        r4-r11     - non-volatile
        r12 (ip)   - volatile, intra-procedure scratch
        r13-r15    - sp, lr, pc

        d0-d7      - volatile, FP arguments
        d8-d15     - non-volatile

    ---------------
    Execution model
    ---------------

    Registers:
        r0-r7      - value scratch
        r8         - helper scratch (flag transfers, shift synthesis)
        r9         - pinned: sp as it was on entry, the frame anchor
        r10        - pinned: the software flags register (bit 0 = C, bit 4 = U)
        r11        - pinned: &m_state, the UML register file base
        r12        - address materialisation and the call trampoline

        d0-d7      - float scratch

    The three pinned registers are all callee-saved under AAPCS, which is what
    lets generated code call into C (CALLC, the memory accessors, the divide
    helpers) without spilling them.

    UML registers are NOT mapped to host registers in this version. Every
    I-register access is a [r11, #offset] load or store and every 64-bit value
    occupies a register pair for the duration of one operation, so
    get_info() reports direct_iregs = 0.

    That is slower than drcbex86, which pins I0-I3's low halves. It is
    deliberately the starting point: register mapping is an optimisation that
    can be layered onto a lowering that already passes the differential
    harness, whereas a lowering built around register mapping cannot be tested
    until it is finished. It also makes every be_parameter either an immediate
    or a memory address, which collapses drcbex86's whole select_register
    family into nothing.

    The pinned state base pays for itself immediately: drcuml_machine_state is
    168 bytes, so every UML register, the exception register, fmod and the
    flags byte are all inside one ldr/str displacement, where an absolute
    address would cost a movw/movt pair before every access.

    Entry point:
        uint32_t entry(void *codeptr) -- sets up the pinned registers and
        calls into generated code.

    Calls between generated blocks use bl/blx with lr, so any block that calls
    (CALLH, EXH, the hashjmp miss path) saves lr around the call, and a handle
    entered through its code pointer pushes lr in a one-instruction prologue
    that fall-through entry jumps over. That is drcbearm64's arrangement; the
    x86 original got the same effect for free from call/ret pushing to the
    stack.

    ---------------
    Flags
    ---------------

    UML carries C V Z S U; ARM's APSR has N Z C V. Two of the five do not fit:

      * U (unordered) has no ARM equivalent at all.
      * C does not mean the same thing. ARM sets C to NOT-borrow after a
        subtract; UML, following x86, defines it as borrow.

    So the model is drcbearm64's: N/Z/V live in APSR as UML S/Z/V, while C and
    U live in bits 0 and 4 of the software flags register, which is
    authoritative. m_carry_state records what the hardware C currently is
    relative to the UML carry -- POISON, CANONICAL or LOGICAL -- so a consumer
    that needs C in the flags reloads it only when the polarity is wrong.

    Everything that is not computing flags must therefore avoid the
    S-suffixed forms: materialising constants (movw/movt), loads, stores and
    address arithmetic all leave APSR alone, which is what lets flags survive
    across a MOV or a LOAD as the UML requires.

    Two ARM shift/flag details the lowering respects, neither of which
    drcbex86 can warn about:
      * a shift by zero leaves C untouched rather than defined, and C after a
        shift is the last bit shifted out
      * there is no ROL (use ROR #(32-n)) and no RCL/RCR, so ROLC/RORC are
        synthesised -- RRX is the only path for carry INTO a shift

    ---------------
    What is lowered, and what is a helper call
    ---------------

    Everything in uml.h is lowered. Six families call a C helper in this file
    instead of being synthesised inline, and the reason is the same for all of
    them -- they are cold, and an inline version would be the most intricate
    code here for no measurable return:

      * DIVU/DIVS in both widths. The Cortex-A9 has no integer divide
        instruction at all (SDIV/UDIV are ARMv7-R/M, or ARMv7-A with the idiv
        extension, and the A9 has neither), so a divide is a call whatever
        happens; making it a call into C rather than into libgcc's __aeabi
        entry points keeps the divide-by-zero semantics and the UML flag byte
        in one readable place.
      * 64x64 multiplies, 64-bit shifts and rotates, and the 64-bit
        integer/float conversions. These are the cases where the synthesis is
        long and the frequency is low; a 32-bit CPU core (the SH-2/SH-4 boards
        this back-end exists for) does not emit them.

    The helpers write UML registers directly, because in this version a UML
    register IS memory, so "pass the destination" is just passing a pointer.

***************************************************************************/

#include "emu.h"
#include "drcbearm32.h"

#include "drcbeut.h"

#include "debug/debugcpu.h"
#include "emuopts.h"

#include "mfpresolve.h"

#include "asmjit/asmjit/a32.h"

#include <cmath>
#include <cstddef>
#include <vector>


//**************************************************************************
//  MACROS
//**************************************************************************

// same shape as drcbex86's, and for the same reason: these are the invariants
// the UML guarantees per opcode, and asserting them is how a malformed block
// is caught in a debug build instead of miscompiled in a release one
#define assert_no_condition(inst)   assert((inst).condition() == uml::COND_ALWAYS)
#define assert_any_condition(inst)  assert((inst).condition() == uml::COND_ALWAYS || ((inst).condition() >= uml::COND_Z && (inst).condition() < uml::COND_MAX))
#define assert_no_flags(inst)       assert((inst).flags() == 0)
#define assert_flags(inst, valid)   assert(((inst).flags() & ~(valid)) == 0)


namespace drc {

namespace {

using namespace uml;
using namespace asmjit;
using namespace asmjit::a32;


//**************************************************************************
//  CONSTANTS
//**************************************************************************

// parameter type masks, as drcbex86 spells them
const uint32_t PTYPE_M   = 1 << parameter::PTYPE_MEMORY;
const uint32_t PTYPE_I   = 1 << parameter::PTYPE_IMMEDIATE;
const uint32_t PTYPE_R   = 1 << parameter::PTYPE_INT_REGISTER;
const uint32_t PTYPE_F   = 1 << parameter::PTYPE_FLOAT_REGISTER;
const uint32_t PTYPE_MR  = PTYPE_M | PTYPE_R;
const uint32_t PTYPE_MRI = PTYPE_M | PTYPE_R | PTYPE_I;
const uint32_t PTYPE_MF  = PTYPE_M | PTYPE_F;

// r0-r7 carry values, and nothing else may live in them across an opcode
const Gp REG_V0 = r0, REG_V1 = r1, REG_V2 = r2, REG_V3 = r3;
const Gp REG_V4 = r4, REG_V5 = r5, REG_V6 = r6, REG_V7 = r7;

// helper scratch: flag transfers and shift synthesis. Never holds a UML value
// across an emitter helper, which is what makes it safe for those helpers to
// use without being told.
const Gp REG_TMP = r8;

// sp as it was when generated code was entered. HASHJMP unwinds to it so a
// jump out of a call chain does not grow the stack, and RECOVER reads the
// outermost saved lr through it.
const Gp REG_FRAME = r9;

// software flags: bit 0 is the UML carry, bit 4 the UML unordered flag
const Gp REG_FLAGS = r10;

// &m_state, the UML register file base
const Gp REG_STATE = r11;

// address materialisation and the call trampoline. Volatile across calls, and
// deliberately the same register for both jobs -- an address and a call target
// are never live at the same time.
const Gp REG_ADDR = r12;

// float scratch
const Vec VD0 = d0, VD1 = d1, VD2 = d2;
const Vec VS0 = s0, VS1 = s1, VS2 = s2;
// s6/s7 alias d3, kept apart from the d0-d2 used for double work
const Vec VS6 = s6;

// bytes of scratch reserved below sp inside generated code. AAPCS wants sp
// 8-byte aligned at every public interface.
constexpr uint32_t STACK_SCRATCH = 32;

// APSR bit positions
constexpr uint32_t APSR_N = 1u << 31;
constexpr uint32_t APSR_Z = 1u << 30;
constexpr uint32_t APSR_C = 1u << 29;
constexpr uint32_t APSR_V = 1u << 28;

// mrs Rd, APSR / msr APSR_nzcvq, Rn -- see the a32 notes above, the immediate
// is the register bank selector and the field mask respectively
constexpr uint32_t MRS_APSR = 0;
constexpr uint32_t MSR_NZCVQ = 2;

// VMRS/VMSR have no emitter entry; these are the literal words, qualified
// against arm-linux-gnueabihf-as in tests/a32-asmjit/.
constexpr uint32_t VMRS_FPSCR_BASE = 0xEEF10A10u;   // vmrs Rt, fpscr
constexpr uint32_t VMSR_FPSCR_BASE = 0xEEE10A10u;   // vmsr fpscr, Rt

// UML condition -> the ARM condition that is true exactly when the UML
// condition is, ASSUMING the hardware carry is in the LOGICAL (inverted)
// state. That assumption is the whole reason m_carry_state exists: the
// unsigned conditions are the ones it applies to, and emit_skip()/op_jmp()
// either arrange for it or use the opposite sense.
const CondCode condition_map[uml::COND_MAX - uml::COND_Z] =
{
	CondCode::kEQ,    // COND_Z      requires Z
	CondCode::kNE,    // COND_NZ     requires Z
	CondCode::kMI,    // COND_S      requires S
	CondCode::kPL,    // COND_NS     requires S
	CondCode::kLO,    // COND_C      requires C
	CondCode::kHS,    // COND_NC     requires C
	CondCode::kVS,    // COND_V      requires V
	CondCode::kVC,    // COND_NV     requires V
	CondCode::kAL,    // COND_U      requires U (software flag, handled apart)
	CondCode::kAL,    // COND_NU     requires U (software flag, handled apart)
	CondCode::kHI,    // COND_A      requires CZ
	CondCode::kLS,    // COND_BE     requires CZ
	CondCode::kGT,    // COND_G      requires SVZ
	CondCode::kLE,    // COND_LE     requires SVZ
	CondCode::kLT,    // COND_L      requires SV
	CondCode::kGE,    // COND_GE     requires SV
};

#define ARM_CONDITION(cond)     (condition_map[(cond) - uml::COND_Z])
#define ARM_NOT_CONDITION(cond) (negate_cond(condition_map[(cond) - uml::COND_Z]))


class ThrowableErrorHandler : public ErrorHandler
{
public:
	virtual void handle_error(Error err, const char *message, BaseEmitter *origin) override
	{
		throw emu_fatalerror("drcbearm32: asmjit error %u: %s", std::underlying_type_t<Error>(err), message);
	}
};


//**************************************************************************
//  SMALL FREE HELPERS
//**************************************************************************

inline uint32_t rotl32(uint32_t v, unsigned n)
{
	n &= 31;
	return n ? ((v << n) | (v >> (32 - n))) : v;
}

// true if the value is an ARM "modified immediate": an 8-bit value rotated
// right by an even amount. Everything else needs movw/movt.
inline bool is_arm_imm(uint32_t v)
{
	for (unsigned rot = 0; rot < 32; rot += 2)
		if (rotl32(v, rot) <= 0xff)
			return true;
	return false;
}

inline uint32_t flags_nz32(uint32_t v)
{
	return (v ? 0 : FLAG_Z) | ((v & 0x80000000u) ? FLAG_S : 0);
}

inline uint32_t flags_nz64(uint64_t v)
{
	return (v ? 0 : FLAG_Z) | ((v & 0x8000000000000000ull) ? FLAG_S : 0);
}


//**************************************************************************
//  HELPERS CALLED FROM GENERATED CODE
//**************************************************************************

// These exist for the cold, intricate cases -- see the note at the top of the
// file. They take pointers to the UML destinations because a UML register is
// memory in this back-end, and they return the UML flag byte so the caller can
// hand it straight to set_flags() when the instruction asked for flags.

uint32_t arm32_divu(uint32_t *quot, uint32_t *rem, uint32_t num, uint32_t den)
{
	// UML leaves both destinations untouched on a divide by zero, and reports
	// overflow -- copying drcbec exactly rather than approximating it, because
	// a CPU core can and does branch on that V.
	if (!den)
		return FLAG_V;

	uint32_t const q = num / den;
	uint32_t const r = num % den;
	*quot = q;
	*rem = r;
	return flags_nz32(q);
}

uint32_t arm32_divs(int32_t *quot, int32_t *rem, int32_t num, int32_t den)
{
	if (!den)
		return FLAG_V;

	// INT_MIN / -1 traps on some hosts and is undefined in C++; UML has no
	// opinion, so give the two's-complement answer the interpreter gives.
	if ((den == -1) && (num == std::numeric_limits<int32_t>::min()))
	{
		*quot = num;
		*rem = 0;
		return flags_nz32(uint32_t(num));
	}

	int32_t const q = num / den;
	int32_t const r = num % den;
	*quot = q;
	*rem = r;
	return flags_nz32(uint32_t(q));
}

uint32_t arm32_ddivu(uint64_t *quot, uint64_t *rem, uint64_t const *num, uint64_t const *den)
{
	if (!*den)
		return FLAG_V;

	uint64_t const q = *num / *den;
	uint64_t const r = *num % *den;
	*quot = q;
	*rem = r;
	return flags_nz64(q);
}

uint32_t arm32_ddivs(int64_t *quot, int64_t *rem, int64_t const *num, int64_t const *den)
{
	if (!*den)
		return FLAG_V;

	if ((*den == -1) && (*num == std::numeric_limits<int64_t>::min()))
	{
		*quot = *num;
		*rem = 0;
		return flags_nz64(uint64_t(*num));
	}

	int64_t const q = *num / *den;
	int64_t const r = *num % *den;
	*quot = q;
	*rem = r;
	return flags_nz64(uint64_t(q));
}

// 64x64 = 128. dsthi may be null, which is the MULULW/MULSLW form: the low
// 64 bits are the result and the overflow flag still reflects the full one.
uint32_t arm32_dmulu(uint64_t *dstlo, uint64_t *dsthi, uint64_t const *src1, uint64_t const *src2)
{
	uint64_t const a = *src1, b = *src2;
	uint64_t const al = uint32_t(a), ah = a >> 32;
	uint64_t const bl = uint32_t(b), bh = b >> 32;

	uint64_t const ll = al * bl;
	uint64_t const lh = al * bh;
	uint64_t const hl = ah * bl;
	uint64_t const hh = ah * bh;

	uint64_t const mid = (ll >> 32) + uint32_t(lh) + uint32_t(hl);
	uint64_t const lo = (mid << 32) | uint32_t(ll);
	uint64_t const hi = hh + (lh >> 32) + (hl >> 32) + (mid >> 32);

	if (dsthi)
	{
		*dstlo = lo;
		*dsthi = hi;
		// the wide form's sign is bit 127 and its zero is over both halves;
		// overflow is "the high half is not just sign extension", which for
		// unsigned means any bit set at all
		return ((hi & 0x8000000000000000ull) ? FLAG_S : 0)
				| ((!lo && !hi) ? FLAG_Z : 0)
				| (hi ? FLAG_V : 0);
	}

	*dstlo = lo;
	return flags_nz64(lo) | (hi ? FLAG_V : 0);
}

uint32_t arm32_dmuls(uint64_t *dstlo, uint64_t *dsthi, uint64_t const *src1, uint64_t const *src2)
{
	// signed 128-bit product built from the unsigned one, then corrected --
	// the standard two's-complement adjustment, and cheaper to read than four
	// signed partial products
	uint64_t lo, hi;
	{
		uint64_t const a = *src1, b = *src2;
		uint64_t const al = uint32_t(a), ah = a >> 32;
		uint64_t const bl = uint32_t(b), bh = b >> 32;

		uint64_t const ll = al * bl;
		uint64_t const lh = al * bh;
		uint64_t const hl = ah * bl;
		uint64_t const hh = ah * bh;

		uint64_t const mid = (ll >> 32) + uint32_t(lh) + uint32_t(hl);
		lo = (mid << 32) | uint32_t(ll);
		hi = hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
	}
	if (int64_t(*src1) < 0)
		hi -= *src2;
	if (int64_t(*src2) < 0)
		hi -= *src1;

	uint64_t const sext = uint64_t(int64_t(lo) >> 63);

	if (dsthi)
	{
		*dstlo = lo;
		*dsthi = hi;
		return ((hi & 0x8000000000000000ull) ? FLAG_S : 0)
				| ((!lo && !hi) ? FLAG_Z : 0)
				| ((hi != sext) ? FLAG_V : 0);
	}

	*dstlo = lo;
	return flags_nz64(lo) | ((hi != sext) ? FLAG_V : 0);
}

// 64-bit shifts and rotates. `op` is the UML opcode; `inflags` carries the UML
// carry in for ROLC/RORC. Returns the UML flag byte.
uint32_t arm32_shift64(uint64_t *dst, uint64_t const *src, uint32_t count, uint32_t op_and_inflags)
{
	uint32_t const op = op_and_inflags & 0xff;
	uint32_t const inflags = (op_and_inflags >> 8) & FLAGS_ALL;
	uint64_t const v = *src;
	uint32_t const shift = count & 63;
	uint64_t result;
	uint32_t carry;

	switch (op)
	{
	case OP_SHL:
		result = v << shift;
		carry = shift ? uint32_t((v << (shift - 1)) >> 63) & 1 : 0;
		break;
	case OP_SHR:
		result = v >> shift;
		carry = shift ? uint32_t(v >> (shift - 1)) & 1 : 0;
		break;
	case OP_SAR:
		result = uint64_t(int64_t(v) >> shift);
		carry = shift ? uint32_t(v >> (shift - 1)) & 1 : 0;
		break;
	case OP_ROL:
		result = shift ? ((v << shift) | (v >> (64 - shift))) : v;
		carry = shift ? uint32_t((v << (shift - 1)) >> 63) & 1 : 0;
		break;
	case OP_ROR:
		result = shift ? ((v >> shift) | (v << (64 - shift))) : v;
		carry = shift ? uint32_t(v >> (shift - 1)) & 1 : 0;
		break;
	case OP_ROLC:
		if (shift > 1)
			result = (v << shift) | (uint64_t(inflags & FLAG_C) << (shift - 1)) | (v >> (65 - shift));
		else if (shift == 1)
			result = (v << shift) | (inflags & FLAG_C);
		else
			result = v;
		carry = shift ? (uint32_t(v >> (64 - shift)) & 1) : (inflags & FLAG_C);
		break;
	case OP_RORC:
		if (shift > 1)
			result = (v >> shift) | (uint64_t(inflags & FLAG_C) << (64 - shift)) | (v << (65 - shift));
		else if (shift == 1)
			result = (v >> shift) | (uint64_t(inflags & FLAG_C) << 63);
		else
			result = v;
		carry = shift ? (uint32_t(v >> (shift - 1)) & 1) : (inflags & FLAG_C);
		break;
	default:
		result = v;
		carry = 0;
		break;
	}

	*dst = result;
	return flags_nz64(result) | (carry ? FLAG_C : 0);
}

// 32-bit ROLC/RORC with a register count. The immediate-count cases are
// synthesised inline; this is the one that would need a branchy sequence.
uint32_t arm32_shift32c(uint32_t *dst, uint32_t src, uint32_t count, uint32_t op_and_inflags)
{
	uint32_t const op = op_and_inflags & 0xff;
	uint32_t const inflags = (op_and_inflags >> 8) & FLAGS_ALL;
	uint32_t const shift = count & 31;
	uint32_t result;
	uint32_t carry;

	if (op == OP_ROLC)
	{
		if (shift > 1)
			result = (src << shift) | ((inflags & FLAG_C) << (shift - 1)) | (src >> (33 - shift));
		else if (shift == 1)
			result = (src << shift) | (inflags & FLAG_C);
		else
			result = src;
		carry = shift ? ((src >> (32 - shift)) & 1) : (inflags & FLAG_C);
	}
	else
	{
		if (shift > 1)
			result = (src >> shift) | ((inflags & FLAG_C) << (32 - shift)) | (src << (33 - shift));
		else if (shift == 1)
			result = (src >> shift) | ((inflags & FLAG_C) << 31);
		else
			result = src;
		carry = shift ? ((src >> (shift - 1)) & 1) : (inflags & FLAG_C);
	}

	*dst = result;
	return flags_nz32(result) | (carry ? FLAG_C : 0);
}

// float/64-bit-integer conversions. VFP on ARMv7-A converts to and from 32-bit
// integers only, so anything with a 64-bit integer side is a call.
void arm32_dtoi64(int64_t *dst, double const *src, uint32_t rounding)
{
	double const v = *src;
	switch (rounding)
	{
	case ROUND_TRUNC: *dst = int64_t(v); break;
	case ROUND_ROUND: *dst = int64_t(std::llround(v)); break;
	case ROUND_CEIL:  *dst = int64_t(std::ceil(v)); break;
	case ROUND_FLOOR: *dst = int64_t(std::floor(v)); break;
	default:          *dst = int64_t(std::nearbyint(v)); break;
	}
}

void arm32_stoi64(int64_t *dst, float const *src, uint32_t rounding)
{
	float const v = *src;
	switch (rounding)
	{
	case ROUND_TRUNC: *dst = int64_t(v); break;
	case ROUND_ROUND: *dst = int64_t(std::llround(v)); break;
	case ROUND_CEIL:  *dst = int64_t(std::ceil(v)); break;
	case ROUND_FLOOR: *dst = int64_t(std::floor(v)); break;
	default:          *dst = int64_t(std::nearbyint(v)); break;
	}
}

void arm32_i64tod(double *dst, int64_t const *src) { *dst = double(*src); }
void arm32_i64tos(float *dst, int64_t const *src)  { *dst = float(*src); }


//**************************************************************************
//  BACK-END CLASS
//**************************************************************************

class drcbe_arm32 : public drcbe_interface
{
	using arm32_entry_point_func = uint32_t (*)(void *entry);

public:
	drcbe_arm32(drcuml_state &drcuml, device_t &device, drc_cache &cache, uint32_t flags, int modes, int addrbits, int ignorebits);
	virtual ~drcbe_arm32();

	virtual void reset() override;
	virtual int execute(uml::code_handle &entry) override;
	virtual void generate(drcuml_block &block, const uml::instruction *instlist, uint32_t numinst) override;
	virtual bool hash_exists(uint32_t mode, uint32_t pc) const noexcept override;
	virtual void hash_invalidate_range(uint32_t pcstart, uint32_t pcend) noexcept override;
	virtual void get_info(drcbe_info &info) const noexcept override;
	virtual bool logging() const noexcept override { return false; }

private:
	// See the flags note in the file comment: after a subtract the hardware
	// carry is the inverse of the UML carry, so what the hardware currently
	// holds has to be tracked rather than assumed.
	enum class carry_state
	{
		POISON,     // does not correspond to the UML carry flag at all
		CANONICAL,  // corresponds directly
		LOGICAL     // is the inverse
	};

	// A be_parameter is a uml::parameter reduced to what this back-end can
	// address. Because no UML register is pinned to a host register, that is
	// only two things: an immediate, or an address. drcbex86's whole
	// select_register family exists to choose between a register and a spill
	// slot, and there is nothing here to choose.
	class be_parameter
	{
	public:
		enum be_parameter_type
		{
			PTYPE_NONE = 0,
			PTYPE_IMMEDIATE,
			PTYPE_MEMORY,
			PTYPE_MAX
		};

		be_parameter() : m_type(PTYPE_NONE), m_value(0) { }
		be_parameter(uint64_t val) : m_type(PTYPE_IMMEDIATE), m_value(val) { }
		be_parameter(drcbe_arm32 &drcbe, const uml::parameter &param, uint32_t allowed);
		be_parameter(const be_parameter &param) = default;

		static be_parameter make_memory(void *base) { return be_parameter(PTYPE_MEMORY, uint64_t(uintptr_t(base))); }
		static be_parameter make_memory(const void *base) { return be_parameter(PTYPE_MEMORY, uint64_t(uintptr_t(const_cast<void *>(base)))); }

		bool operator==(be_parameter const &rhs) const { return (m_type == rhs.m_type) && (m_value == rhs.m_value); }
		bool operator!=(be_parameter const &rhs) const { return !(*this == rhs); }

		be_parameter_type type() const { return m_type; }
		uint64_t immediate() const { assert(m_type == PTYPE_IMMEDIATE); return m_value; }
		uint32_t immediate_lo() const { assert(m_type == PTYPE_IMMEDIATE); return uint32_t(m_value); }
		uint32_t immediate_hi() const { assert(m_type == PTYPE_IMMEDIATE); return uint32_t(m_value >> 32); }
		void *memory(uint32_t offset = 0) const { assert(m_type == PTYPE_MEMORY); return reinterpret_cast<void *>(uintptr_t(m_value + offset)); }

		bool is_immediate() const { return m_type == PTYPE_IMMEDIATE; }
		bool is_memory() const { return m_type == PTYPE_MEMORY; }
		bool is_immediate_value(uint64_t value) const { return (m_type == PTYPE_IMMEDIATE) && (m_value == value); }

	private:
		be_parameter(be_parameter_type type, uint64_t value) : m_type(type), m_value(value) { }

		be_parameter_type   m_type;
		uint64_t            m_value;
	};

	// near-cache block: everything generated code reaches by absolute address
	struct near_state
	{
		uint64_t    tmp[6];         // scratch for the C helpers' pointer arguments
		uint32_t    host_fpscr;     // FPSCR as the host had it when we were entered
		uint32_t    drc_fpscr;      // FPSCR parked across a call out to C
		uint32_t    hashstacksave;  // unused padding kept for alignment clarity
		uint32_t    pad;
	};

	size_t emit(CodeHolder &ch, bool invariant);

	// address and constant materialisation
	void emit_mov_reg_imm(Assembler &a, Gp const &reg, uint32_t val) const;
	Mem emit_abs_mem(Assembler &a, void const *addr) const;
	void emit_call(Assembler &a, void const *target) const;
	void emit_call_saving_lr(Assembler &a, void const *target) const;

	// parameter access
	void mov_reg_param(Assembler &a, Gp const &dst, be_parameter const &param) const;
	void mov_reg_param_pair(Assembler &a, Gp const &lo, Gp const &hi, be_parameter const &param) const;
	void mov_param_reg(Assembler &a, be_parameter const &param, Gp const &src) const;
	void mov_param_reg_pair(Assembler &a, be_parameter const &param, Gp const &lo, Gp const &hi) const;
	void mov_mem_param(Assembler &a, void *dst, be_parameter const &param) const;
	Gp emit_param_ptr(Assembler &a, Gp const &reg, be_parameter const &param, int tmpslot) const;

	// flags
	void store_carry(Assembler &a, bool inverted);
	void store_carry_bit(Assembler &a, Gp const &reg, unsigned bit);
	void clear_carry(Assembler &a);
	void load_carry(Assembler &a, bool inverted);
	void set_flags(Assembler &a, Gp const &reg);
	void get_flags(Assembler &a, Gp const &dst, uint32_t mask);
	void emit_flags_sz32(Assembler &a, Gp const &value);
	void emit_flags_sz64(Assembler &a, Gp const &lo, Gp const &hi);
	void emit_combine_z(Assembler &a, Gp const &lo, Gp const &hi);
	void emit_skip(Assembler &a, uml::condition_t cond, Label &skip);
	CondCode emit_cond_setup(Assembler &a, uml::condition_t cond);

	// FPSCR
	void emit_vmrs(Assembler &a, Gp const &reg) const;
	void emit_vmsr(Assembler &a, Gp const &reg) const;
	void emit_set_rounding(Assembler &a, uint32_t umlmode);

	void generate_one(Assembler &a, const uml::instruction &inst);
	[[noreturn]] void end_of_block() const;
	[[noreturn]] void unimplemented(const uml::instruction &inst) const;

	// structural
	void op_handle(Assembler &a, const uml::instruction &inst);
	void op_hash(Assembler &a, const uml::instruction &inst);
	void op_label(Assembler &a, const uml::instruction &inst);
	void op_comment(Assembler &a, const uml::instruction &inst);
	void op_mapvar(Assembler &a, const uml::instruction &inst);

	// control flow
	void op_nop(Assembler &a, const uml::instruction &inst);
	void op_break(Assembler &a, const uml::instruction &inst);
	void op_debug(Assembler &a, const uml::instruction &inst);
	void op_exit(Assembler &a, const uml::instruction &inst);
	void op_hashjmp(Assembler &a, const uml::instruction &inst);
	void op_jmp(Assembler &a, const uml::instruction &inst);
	void op_exh(Assembler &a, const uml::instruction &inst);
	void op_callh(Assembler &a, const uml::instruction &inst);
	void op_ret(Assembler &a, const uml::instruction &inst);
	void op_callc(Assembler &a, const uml::instruction &inst);
	void op_recover(Assembler &a, const uml::instruction &inst);

	// internal register
	void op_setfmod(Assembler &a, const uml::instruction &inst);
	void op_getfmod(Assembler &a, const uml::instruction &inst);
	void op_getexp(Assembler &a, const uml::instruction &inst);
	void op_getflgs(Assembler &a, const uml::instruction &inst);
	void op_setflgs(Assembler &a, const uml::instruction &inst);
	void op_save(Assembler &a, const uml::instruction &inst);
	void op_restore(Assembler &a, const uml::instruction &inst);

	// integer
	void op_load(Assembler &a, const uml::instruction &inst);
	void op_loads(Assembler &a, const uml::instruction &inst);
	void op_store(Assembler &a, const uml::instruction &inst);
	void op_read(Assembler &a, const uml::instruction &inst);
	void op_readm(Assembler &a, const uml::instruction &inst);
	void op_write(Assembler &a, const uml::instruction &inst);
	void op_writem(Assembler &a, const uml::instruction &inst);
	void op_carry(Assembler &a, const uml::instruction &inst);
	void op_set(Assembler &a, const uml::instruction &inst);
	void op_mov(Assembler &a, const uml::instruction &inst);
	void op_sext(Assembler &a, const uml::instruction &inst);
	void op_bfx(Assembler &a, const uml::instruction &inst, bool sign);
	void op_roland(Assembler &a, const uml::instruction &inst);
	void op_rolins(Assembler &a, const uml::instruction &inst);
	void op_addsub(Assembler &a, const uml::instruction &inst);
	void op_cmp(Assembler &a, const uml::instruction &inst);
	void op_mul(Assembler &a, const uml::instruction &inst, bool sign, bool wide);
	void op_div(Assembler &a, const uml::instruction &inst, bool sign);
	void op_logic(Assembler &a, const uml::instruction &inst);
	void op_test(Assembler &a, const uml::instruction &inst);
	void op_lzcnt(Assembler &a, const uml::instruction &inst);
	void op_tzcnt(Assembler &a, const uml::instruction &inst);
	void op_bswap(Assembler &a, const uml::instruction &inst);
	void op_shift(Assembler &a, const uml::instruction &inst);

	// float
	void op_fload(Assembler &a, const uml::instruction &inst);
	void op_fstore(Assembler &a, const uml::instruction &inst);
	void op_fread(Assembler &a, const uml::instruction &inst);
	void op_fwrite(Assembler &a, const uml::instruction &inst);
	void op_fmov(Assembler &a, const uml::instruction &inst);
	void op_ftoint(Assembler &a, const uml::instruction &inst);
	void op_ffrint(Assembler &a, const uml::instruction &inst);
	void op_ffrflt(Assembler &a, const uml::instruction &inst);
	void op_frnds(Assembler &a, const uml::instruction &inst);
	void op_fbinary(Assembler &a, const uml::instruction &inst);
	void op_funary(Assembler &a, const uml::instruction &inst);
	void op_fcmp(Assembler &a, const uml::instruction &inst);
	void op_fcopyi(Assembler &a, const uml::instruction &inst);
	void op_icopyf(Assembler &a, const uml::instruction &inst);

	// float helpers
	Vec fparam_reg(int size, Vec const &dreg, Vec const &sreg) const { return (size == 4) ? sreg : dreg; }
	void mov_freg_param(Assembler &a, int size, Vec const &dst, be_parameter const &param) const;
	void mov_param_freg(Assembler &a, int size, be_parameter const &param, Vec const &src) const;

	struct memory_accessors
	{
		resolved_memory_accessors resolved;
	};

	drc_hash_table      m_hash;
	drc_map_variables   m_map;
	near_state &        m_near;

	carry_state         m_carry_state;
	bool                m_invariant_block;

	arm32_entry_point_func m_entry;
	drccodeptr          m_exit;
	drccodeptr          m_nocode;
	drccodeptr          m_endofblock;
	drccodeptr          m_save;
	drccodeptr          m_restore;

	resolved_member_function m_drcmap_get_value;
	resolved_member_function m_debug_cpu_instruction_hook;
	std::vector<memory_accessors> m_memory_accessors;
};


//**************************************************************************
//  BE_PARAMETER
//**************************************************************************

drcbe_arm32::be_parameter::be_parameter(drcbe_arm32 &drcbe, parameter const &param, uint32_t allowed)
{
	switch (param.type())
	{
	case parameter::PTYPE_IMMEDIATE:
		assert(allowed & PTYPE_I);
		*this = param.immediate();
		break;

	case parameter::PTYPE_MEMORY:
		assert(allowed & PTYPE_M);
		*this = make_memory(param.memory());
		break;

	// no UML register is pinned to a host register in this version, so every
	// register parameter is its slot in the machine state
	case parameter::PTYPE_INT_REGISTER:
		assert(allowed & PTYPE_R);
		*this = make_memory(&drcbe.m_state.r[param.ireg() - REG_I0]);
		break;

	case parameter::PTYPE_FLOAT_REGISTER:
		assert(allowed & PTYPE_F);
		*this = make_memory(&drcbe.m_state.f[param.freg() - REG_F0]);
		break;

	default:
		fatalerror("drcbearm32: unexpected parameter type\n");
	}
}


//**************************************************************************
//  CONSTRUCTION
//**************************************************************************

drcbe_arm32::drcbe_arm32(drcuml_state &drcuml, device_t &device, drc_cache &cache, uint32_t flags, int modes, int addrbits, int ignorebits)
	: drcbe_interface(drcuml, cache, device)
	, m_hash(cache, modes, addrbits, ignorebits, drcuml.max_sequence_length())
	, m_map(cache, 0xaaaaaaaa)
	, m_near(*cache.alloc_near<near_state>())
	, m_carry_state(carry_state::POISON)
	, m_invariant_block(false)
	, m_entry(nullptr)
	, m_exit(nullptr)
	, m_nocode(nullptr)
	, m_endofblock(nullptr)
	, m_save(nullptr)
	, m_restore(nullptr)
{
	std::fill_n((uint8_t *)&m_near, sizeof(m_near), 0);

	// The UML register file must be reachable from the pinned base with a
	// single displacement. drcuml_machine_state is well inside that, but
	// assert rather than discover it as corruption later.
	static_assert(sizeof(drcuml_machine_state) <= 256,
			"UML machine state no longer fits the 8-bit displacement forms from the pinned base");

	// resolve the member functions generated code calls
	m_drcmap_get_value.set(m_map, &drc_map_variables::get_value);
	if (!m_drcmap_get_value)
		throw emu_fatalerror("drcbearm32: error resolving map variable get value function!\n");

	m_memory_accessors.resize(m_space.size());
	for (int space = 0; m_space.size() > space; ++space)
		if (m_space[space])
			m_memory_accessors[space].resolved.set(*m_space[space]);

	// ---- the invariant code ----
	//
	// Generated once, into the invariant part of the cache, exactly as
	// drcbex86 and drcbearm64 both do it. reset() must NOT regenerate it: a
	// cache flush keeps invariant allocations, so regenerating would leak a
	// copy per reset.
	uint8_t *const dst = (uint8_t *)m_cache.top();

	CodeHolder ch;
	ch.init(Environment::host(), uint64_t(dst));
	ThrowableErrorHandler e;
	ch.set_error_handler(&e);

	Assembler a(&ch);

	// ten registers, not nine, so sp stays 8-byte aligned for the C ABI --
	// r12 is saved for no reason other than being the tenth
	GpList const saved({ r4, r5, r6, r7, r8, r9, r10, r11, r12, r14 });

	// ---- entry point: uint32_t entry(void *codeptr) ----
	uint64_t const entry_offs = a.offset();
	a.push(saved);
	a.sub(sp, sp, imm(STACK_SCRATCH));

	emit_mov_reg_imm(a, REG_STATE, uint32_t(uintptr_t(&m_state)));
	a.mov(REG_FLAGS, imm(0));
	a.mov(REG_FRAME, sp);

	// Take the host's rounding mode as the initial UML fmod, the way
	// drcbearm64 does: the two encodings differ by one modulo four (UML
	// numbers TRUNC first, ARM numbers nearest first), so a single add and
	// extract converts.
	emit_vmrs(a, r1);
	a.str(r1, emit_abs_mem(a, &m_near.host_fpscr));
	a.add(r1, r1, imm(1u << 22));
	a.ubfx(r1, r1, imm(22), imm(2));
	a.strb(r1, emit_abs_mem(a, &m_state.fmod));

	// call rather than jump, so the nocode handler is a plain return -- this
	// is drcbex86's arrangement and the stubs only make sense together
	a.blx(r0);

	// falls straight through into the exit point, which is the whole reason
	// exit is generated here rather than somewhere more convenient
	// ---- exit point: return value already in r0 ----
	uint64_t const exit_offs = a.offset();
	a.mov(sp, REG_FRAME);
	a.ldr(r1, emit_abs_mem(a, &m_near.host_fpscr));
	emit_vmsr(a, r1);
	a.add(sp, sp, imm(STACK_SCRATCH));
	a.pop(saved);
	a.bx(r14);

	// ---- nocode handler: the hash table's default target ----
	// A hashjmp that misses lands here and returns to the miss path the
	// hashjmp itself emitted.
	uint64_t const nocode_offs = a.offset();
	a.bx(r14);

	// ---- end-of-block handler ----
	// Falling off the end of a block is a UML bug, not a runtime condition.
	uint64_t const endofblock_offs = a.offset();
	{
		auto const [entrypoint, adjusted] = util::resolve_member_function(&drcbe_arm32::end_of_block, *this);
		emit_mov_reg_imm(a, r0, uint32_t(uintptr_t(adjusted)));
		emit_call(a, (void const *)entrypoint);
	}

	// ---- SAVE / RESTORE ----
	//
	// These move a whole drcuml_machine_state, so they are two subroutines
	// rather than ninety instructions at every call site -- drcbex86 draws the
	// same line in the same place. The register file is a straight copy off
	// the pinned base; the flag byte is the only part that is not, because the
	// live flags are in APSR and the software flags register, so SAVE takes it
	// as an argument and RESTORE hands it back.
	constexpr uint32_t STATE_WORDS = offsetof(drcuml_machine_state, exp) / 4;
	constexpr uint32_t OFFS_EXP = offsetof(drcuml_machine_state, exp);
	constexpr uint32_t OFFS_FMOD = offsetof(drcuml_machine_state, fmod);
	constexpr uint32_t OFFS_FLAGS = offsetof(drcuml_machine_state, flags);

	// save: r0 = destination state, r1 = the UML flag byte
	uint64_t const save_offs = a.offset();
	for (uint32_t i = 0; i < STATE_WORDS; i++)
	{
		a.ldr(r2, ptr(REG_STATE, int32_t(i * 4)));
		a.str(r2, ptr(r0, int32_t(i * 4)));
	}
	a.ldr(r2, ptr(REG_STATE, int32_t(OFFS_EXP)));
	a.str(r2, ptr(r0, int32_t(OFFS_EXP)));
	a.ldrb(r2, ptr(REG_STATE, int32_t(OFFS_FMOD)));
	a.strb(r2, ptr(r0, int32_t(OFFS_FMOD)));
	a.strb(r1, ptr(r0, int32_t(OFFS_FLAGS)));
	a.bx(r14);

	// restore: r0 = source state, returns the masked UML flag byte in r0
	uint64_t const restore_offs = a.offset();
	for (uint32_t i = 0; i < STATE_WORDS; i++)
	{
		a.ldr(r2, ptr(r0, int32_t(i * 4)));
		a.str(r2, ptr(REG_STATE, int32_t(i * 4)));
	}
	a.ldr(r2, ptr(r0, int32_t(OFFS_EXP)));
	a.str(r2, ptr(REG_STATE, int32_t(OFFS_EXP)));

	// RESTORE masks fmod and the flags as it loads them, and the rounding mode
	// follows fmod -- drcbec does the fesetround here too, and a driver that
	// restores state mid-sequence would otherwise keep the wrong one.
	a.ldrb(r2, ptr(r0, int32_t(OFFS_FMOD)));
	a.and_(r2, r2, imm(3));
	a.strb(r2, ptr(REG_STATE, int32_t(OFFS_FMOD)));
	a.sub(r3, r2, imm(1));
	a.and_(r3, r3, imm(3));
	emit_vmrs(a, r12);
	a.bic(r12, r12, imm(3u << 22));
	a.orr(r12, r12, r3, lsl(22));
	emit_vmsr(a, r12);

	a.ldrb(r0, ptr(r0, int32_t(OFFS_FLAGS)));
	a.and_(r0, r0, imm(FLAGS_ALL));
	a.strb(r0, ptr(REG_STATE, int32_t(OFFS_FLAGS)));
	a.bx(r14);

	a.finalize();
	if (!emit(ch, true))
		throw emu_fatalerror("drcbearm32: out of cache space generating the entry stubs\n");

	m_entry = (arm32_entry_point_func)(uintptr_t(dst) + entry_offs);
	m_exit = drccodeptr(uintptr_t(dst) + exit_offs);
	m_nocode = drccodeptr(uintptr_t(dst) + nocode_offs);
	m_endofblock = drccodeptr(uintptr_t(dst) + endofblock_offs);
	m_save = drccodeptr(uintptr_t(dst) + save_offs);
	m_restore = drccodeptr(uintptr_t(dst) + restore_offs);

	m_hash.set_default_codeptr(m_nocode);
}

drcbe_arm32::~drcbe_arm32()
{
}


//**************************************************************************
//  REQUIRED OVERRIDES
//**************************************************************************

void drcbe_arm32::reset()
{
	m_carry_state = carry_state::POISON;
	m_hash.reset();
	m_hash.set_default_codeptr(m_nocode);
}


int drcbe_arm32::execute(uml::code_handle &entry)
{
	m_cache.codegen_complete();
	return (*m_entry)(entry.codeptr());
}


void drcbe_arm32::generate(drcuml_block &block, const uml::instruction *instlist, uint32_t numinst)
{
	// device.debug() is not initialised at construction time, so the hook is
	// resolved on first use rather than in the constructor
	if (!m_debug_cpu_instruction_hook && (m_device.machine().debug_flags & DEBUG_FLAG_ENABLED) && m_device.debug())
	{
		m_debug_cpu_instruction_hook.set(*m_device.debug(), &device_debug::instruction_hook);
		if (!m_debug_cpu_instruction_hook)
			throw emu_fatalerror("drcbearm32: error resolving debugger instruction hook member function!\n");
	}

	m_hash.block_begin(block, instlist, numinst);
	m_map.block_begin(block);
	m_invariant_block = block.invariant();

	CodeHolder ch;
	ch.init(Environment::host(), uint64_t(m_cache.top()));
	ThrowableErrorHandler e;
	ch.set_error_handler(&e);

	Assembler a(&ch);
	m_carry_state = carry_state::POISON;

	for (uint32_t inum = 0; inum < numinst; inum++)
	{
		assert(instlist[inum].size() == 4 || instlist[inum].size() == 8);
		generate_one(a, instlist[inum]);
	}

	// catch falling off the end of a block
	emit_mov_reg_imm(a, REG_ADDR, uint32_t(uintptr_t(m_endofblock)));
	a.bx(REG_ADDR);

	a.finalize();
	if (!emit(ch, false))
		block.abort();

	m_map.block_end(block);
	m_hash.block_end(block);
}


bool drcbe_arm32::hash_exists(uint32_t mode, uint32_t pc) const noexcept
{
	return m_hash.code_exists(mode, pc);
}


void drcbe_arm32::hash_invalidate_range(uint32_t pcstart, uint32_t pcend) noexcept
{
	m_hash.invalidate_range(pcstart, pcend);
}


void drcbe_arm32::get_info(drcbe_info &info) const noexcept
{
	// No UML register is pinned to a host register in this version -- see the
	// execution model note at the top. Reporting anything else here would tell
	// CPU front-ends to keep values live that this back-end immediately spills.
	info.direct_iregs = 0;
	info.direct_fregs = 0;
}


[[noreturn]] void drcbe_arm32::end_of_block() const
{
	osd_printf_error("drcbearm32: fell off the end of a generated code block!\n");
	std::fflush(stdout);
	std::fflush(stderr);
	abort();
}


[[noreturn]] void drcbe_arm32::unimplemented(const uml::instruction &inst) const
{
	// Deliberately fatal. A back-end that silently skipped an opcode would
	// produce a game that runs and is wrong, which is far more expensive to
	// diagnose than one that refuses to start.
	fatalerror("drcbearm32: UML opcode %d size %d is not lowered\n",
			int(inst.opcode()), int(inst.size()));
}


//**************************************************************************
//  EMITTER HELPERS
//**************************************************************************

// Copy a finished CodeHolder into the DRC cache at the address it was
// assembled for, and make the instruction cache agree. asmjit assembles
// against a fixed base address, so the allocation has to land at or below that
// address rather than wherever the allocator would otherwise put it.
size_t drcbe_arm32::emit(CodeHolder &ch, bool invariant)
{
	size_t const alignment = ch.base_address() - uint64_t(m_cache.top());
	size_t const code_size = ch.code_size();

	auto space = invariant
			? m_cache.alloc_invariant(alignment + code_size, std::align_val_t(1))
			: m_cache.alloc_transient(alignment + code_size, std::align_val_t(1));
	if (!space)
		return 0;

	assert(uintptr_t(space) <= ch.base_address());
	Error const err = ch.copy_flattened_data(drccodeptr(ch.base_address()), code_size, CopySectionFlags::kPadTargetBuffer);
	if (err != Error::kOk)
		throw emu_fatalerror("drcbearm32: CodeHolder::copy_flattened_data() error %u", std::underlying_type_t<Error>(err));

	osd::invalidate_instruction_cache(drccodeptr(ch.base_address()), code_size);

	return code_size;
}


// Materialise a 32-bit constant, never touching the flags. Targeting ARMv7
// rather than v5/v6 is load-bearing here: movw/movt means there is no literal
// pool, so no pool placement, no mid-sequence drain and no PC-relative reach
// limit inside the code cache.
void drcbe_arm32::emit_mov_reg_imm(Assembler &a, Gp const &reg, uint32_t val) const
{
	if (is_arm_imm(val))
		a.mov(reg, imm(val));
	else if (is_arm_imm(~val))
		a.mvn(reg, imm(~val));
	else
	{
		a.movw(reg, imm(val & 0xffff));
		if (val >> 16)
			a.movt(reg, imm(val >> 16));
	}
}


// An absolute address as a Mem, for the 12-bit displacement forms (ldr, str,
// ldrb, strb). Anything inside the machine state -- which is every UML
// register -- comes out as a displacement off the pinned base and costs
// nothing; anything else costs a movw/movt into the address scratch.
Mem drcbe_arm32::emit_abs_mem(Assembler &a, void const *addr) const
{
	intptr_t const off = intptr_t(addr) - intptr_t(&m_state);
	if ((off >= -4095) && (off <= 4095))
		return ptr(REG_STATE, int32_t(off));

	emit_mov_reg_imm(a, REG_ADDR, uint32_t(uintptr_t(addr)));
	return ptr(REG_ADDR);
}


// A call to an arbitrary C function. Deliberately not BL: the code cache and
// libc are not guaranteed to be within BL's +/-32 MB of each other, and a call
// that is nearly always in range is worse than one that never is.
void drcbe_arm32::emit_call(Assembler &a, void const *target) const
{
	emit_mov_reg_imm(a, REG_ADDR, uint32_t(uintptr_t(target)));
	a.blx(REG_ADDR);
}


// The same, but preserving this block's own return address. Generated code has
// no prologue, so lr is live for the whole block and any call has to park it.
// The pair keeps sp 8-byte aligned, which the C ABI requires at the call.
void drcbe_arm32::emit_call_saving_lr(Assembler &a, void const *target) const
{
	// r12 is the pair's filler rather than a live register: emit_call is about
	// to overwrite it with the call target anyway
	a.push(GpList({ r12, r14 }));
	emit_call(a, target);
	a.pop(GpList({ r12, r14 }));
}


//**************************************************************************
//  PARAMETER ACCESS
//**************************************************************************

void drcbe_arm32::mov_reg_param(Assembler &a, Gp const &dst, be_parameter const &param) const
{
	if (param.is_immediate())
		emit_mov_reg_imm(a, dst, param.immediate_lo());
	else
		a.ldr(dst, emit_abs_mem(a, param.memory()));
}


void drcbe_arm32::mov_reg_param_pair(Assembler &a, Gp const &lo, Gp const &hi, be_parameter const &param) const
{
	assert(lo.id() != hi.id());

	if (param.is_immediate())
	{
		emit_mov_reg_imm(a, lo, param.immediate_lo());
		emit_mov_reg_imm(a, hi, param.immediate_hi());
	}
	else
	{
		Mem const memlo = emit_abs_mem(a, param.memory());
		Mem memhi(memlo);
		memhi.add_offset(4);
		a.ldr(lo, memlo);
		a.ldr(hi, memhi);
	}
}


void drcbe_arm32::mov_param_reg(Assembler &a, be_parameter const &param, Gp const &src) const
{
	assert(param.is_memory());
	a.str(src, emit_abs_mem(a, param.memory()));
}


void drcbe_arm32::mov_param_reg_pair(Assembler &a, be_parameter const &param, Gp const &lo, Gp const &hi) const
{
	assert(param.is_memory());

	Mem const memlo = emit_abs_mem(a, param.memory());
	Mem memhi(memlo);
	memhi.add_offset(4);
	a.str(lo, memlo);
	a.str(hi, memhi);
}


void drcbe_arm32::mov_mem_param(Assembler &a, void *dst, be_parameter const &param) const
{
	mov_reg_param(a, REG_TMP, param);
	a.str(REG_TMP, emit_abs_mem(a, dst));
}


// Put a pointer to a 64-bit value in `reg`, for the helpers that take one.
// A memory parameter already has an address; an immediate is parked in the
// near cache first, which is safe because a helper is a leaf and cannot
// re-enter generated code.
Gp drcbe_arm32::emit_param_ptr(Assembler &a, Gp const &reg, be_parameter const &param, int tmpslot) const
{
	if (param.is_memory())
	{
		emit_mov_reg_imm(a, reg, uint32_t(uintptr_t(param.memory())));
	}
	else
	{
		emit_mov_reg_imm(a, REG_TMP, param.immediate_lo());
		emit_mov_reg_imm(a, reg, uint32_t(uintptr_t(&m_near.tmp[tmpslot])));
		a.str(REG_TMP, ptr(reg));
		emit_mov_reg_imm(a, REG_TMP, param.immediate_hi());
		a.str(REG_TMP, ptr(reg, 4));
	}
	return reg;
}


//**************************************************************************
//  FLAGS
//**************************************************************************

// Capture the hardware carry into the software flags register. `inverted` says
// the hardware flag is NOT-borrow, which is what ARM leaves after a subtract
// and the opposite of what UML calls carry. The two predicated forms touch no
// flags, so the rest of NZCV survives.
void drcbe_arm32::store_carry(Assembler &a, bool inverted)
{
	a.bic(REG_FLAGS, REG_FLAGS, imm(FLAG_C));
	a.orr(inverted ? CondCode::kCC : CondCode::kCS, REG_FLAGS, REG_FLAGS, imm(FLAG_C));
	m_carry_state = inverted ? carry_state::LOGICAL : carry_state::CANONICAL;
}


// Carry from a bit of a value rather than from the hardware flag -- the shifts
// compute it that way, because the ARM shift that produced the result did not
// necessarily leave the bit UML wants in C.
void drcbe_arm32::store_carry_bit(Assembler &a, Gp const &reg, unsigned bit)
{
	if (bit == 0)
	{
		a.bfi(REG_FLAGS, reg, imm(0), imm(1));
	}
	else
	{
		a.ubfx(REG_TMP, reg, imm(bit), imm(1));
		a.bfi(REG_FLAGS, REG_TMP, imm(0), imm(1));
	}
	m_carry_state = carry_state::POISON;
}


void drcbe_arm32::clear_carry(Assembler &a)
{
	a.bic(REG_FLAGS, REG_FLAGS, imm(FLAG_C));
	m_carry_state = carry_state::POISON;
}


// Put the software carry back into the hardware flag, in the polarity the
// consumer wants, and only when it is not already there. This is the whole
// point of tracking carry_state.
void drcbe_arm32::load_carry(Assembler &a, bool inverted)
{
	carry_state const desired = inverted ? carry_state::LOGICAL : carry_state::CANONICAL;
	if (desired == m_carry_state)
		return;

	m_carry_state = desired;
	a.mrs(REG_TMP, imm(MRS_APSR));
	a.bfi(REG_TMP, REG_FLAGS, imm(29), imm(1));
	if (inverted)
		a.eor(REG_TMP, REG_TMP, imm(APSR_C));
	a.msr(imm(MSR_NZCVQ), REG_TMP);
}


// Load the whole UML flag byte from a register: S/Z/V into APSR, C/U into the
// software register.
void drcbe_arm32::set_flags(Assembler &a, Gp const &reg)
{
	// bits 2 and 3 of the UML byte are Z and S; shifted up by 28 they land on
	// APSR's Z (30) and N (31) exactly, and V moves from bit 1 to bit 28
	a.and_(REG_TMP, reg, imm(FLAG_Z | FLAG_S));
	a.mov(REG_TMP, REG_TMP, lsl(28));
	a.and_(REG_ADDR, reg, imm(FLAG_V));
	a.orr(REG_TMP, REG_TMP, REG_ADDR, lsl(27));
	a.msr(imm(MSR_NZCVQ), REG_TMP);

	a.and_(REG_FLAGS, reg, imm(FLAG_C | FLAG_U));

	// the hardware carry has just been written as zero, which says nothing
	// about the UML carry now sitting in the software register
	m_carry_state = carry_state::POISON;
}


// The reverse: build the UML flag byte, masked to what the caller asked for.
void drcbe_arm32::get_flags(Assembler &a, Gp const &dst, uint32_t mask)
{
	a.mrs(REG_TMP, imm(MRS_APSR));
	a.mov(REG_TMP, REG_TMP, lsr(28));       // V=bit0 C=bit1 Z=bit2 N=bit3

	a.and_(dst, REG_TMP, imm(FLAG_Z | FLAG_S));
	a.and_(REG_TMP, REG_TMP, imm(1));       // V
	a.orr(dst, dst, REG_TMP, lsl(1));

	a.and_(REG_TMP, REG_FLAGS, imm(FLAG_C | FLAG_U));
	a.orr(dst, dst, REG_TMP);

	if (mask != FLAGS_ALL)
	{
		if (is_arm_imm(mask))
		{
			a.and_(dst, dst, imm(mask));
		}
		else
		{
			emit_mov_reg_imm(a, REG_TMP, mask);
			a.and_(dst, dst, REG_TMP);
		}
	}
}


// S and Z from a 32-bit result. MOVS with no shift leaves C and V alone, which
// matters because the UML opcodes that define only SZ must not disturb a carry
// a later ADDC is going to consume.
void drcbe_arm32::emit_flags_sz32(Assembler &a, Gp const &value)
{
	a.movs(value, value);
}


void drcbe_arm32::emit_flags_sz64(Assembler &a, Gp const &lo, Gp const &hi)
{
	a.movs(hi, hi);
	emit_combine_z(a, lo, hi);
}


// Fold the low word's zero-ness into a Z that a high-word operation set. ARM
// has no 64-bit compare, so this is the seam every 64-bit flag-setting
// operation has to cross. Saving and restoring NZCV around the compare is what
// keeps C and V -- which the high-word operation got right -- intact.
void drcbe_arm32::emit_combine_z(Assembler &a, Gp const &lo, Gp const &hi)
{
	a.mrs(REG_TMP, imm(MRS_APSR));
	a.orr(REG_ADDR, lo, hi);
	a.bic(REG_TMP, REG_TMP, imm(APSR_Z));
	a.cmp(REG_ADDR, imm(0));
	a.orr(CondCode::kEQ, REG_TMP, REG_TMP, imm(APSR_Z));
	a.msr(imm(MSR_NZCVQ), REG_TMP);
}


// Branch around a conditional instruction's body when the condition is not
// met. Every conditional UML opcode goes through this.
void drcbe_arm32::emit_skip(Assembler &a, uml::condition_t cond, Label &skip)
{
	if (cond == uml::COND_ALWAYS)
		return;

	skip = a.new_label();

	switch (cond)
	{
	case uml::COND_U:
	case uml::COND_NU:
		{
			// U has no hardware flag, so testing it means borrowing NZCV --
			// and the opcodes that can be conditional on U are also opcodes
			// UML says do not modify flags, so it has to be given back on
			// both paths.
			Label const body = a.new_label();
			a.mrs(REG_TMP, imm(MRS_APSR));
			a.tst(REG_FLAGS, imm(FLAG_U));
			a.b((cond == uml::COND_U) ? CondCode::kNE : CondCode::kEQ, body);
			a.msr(imm(MSR_NZCVQ), REG_TMP);
			a.b(skip);
			a.bind(body);
			a.msr(imm(MSR_NZCVQ), REG_TMP);
		}
		break;

	case uml::COND_C:
	case uml::COND_NC:
		switch (m_carry_state)
		{
		case carry_state::CANONICAL:
			// the table assumes an inverted hardware carry, so under a
			// canonical one its entry is already the negation we want
			a.b(ARM_CONDITION(cond), skip);
			break;
		case carry_state::LOGICAL:
			a.b(ARM_NOT_CONDITION(cond), skip);
			break;
		default:
			load_carry(a, true);
			a.b(ARM_NOT_CONDITION(cond), skip);
			break;
		}
		break;

	case uml::COND_A:
	case uml::COND_BE:
		load_carry(a, true);
		[[fallthrough]];
	default:
		a.b(ARM_NOT_CONDITION(cond), skip);
		break;
	}
}


// The ARM condition under which a predicated instruction should execute for
// the given UML condition, arranging the carry polarity if it is involved.
// COND_U/COND_NU have no answer and must be handled by the caller.
CondCode drcbe_arm32::emit_cond_setup(Assembler &a, uml::condition_t cond)
{
	assert((cond != uml::COND_U) && (cond != uml::COND_NU));

	switch (cond)
	{
	case uml::COND_C:
	case uml::COND_NC:
	case uml::COND_A:
	case uml::COND_BE:
		load_carry(a, true);
		break;
	default:
		break;
	}

	return ARM_CONDITION(cond);
}


//**************************************************************************
//  FPSCR
//**************************************************************************

// VMRS/VMSR have no a32 emitter entry and its MRC/MCR path wants a
// coprocessor-register operand nothing can construct, so these are the literal
// words. tests/a32-asmjit/ diffs all three against the assembler.
void drcbe_arm32::emit_vmrs(Assembler &a, Gp const &reg) const
{
	a.embed_uint32(VMRS_FPSCR_BASE | (reg.id() << 12));
}


void drcbe_arm32::emit_vmsr(Assembler &a, Gp const &reg) const
{
	a.embed_uint32(VMSR_FPSCR_BASE | (reg.id() << 12));
}


// Program the VFP rounding mode from a UML rounding mode. UML numbers TRUNC
// first and ARM numbers nearest first, so the two differ by one modulo four.
void drcbe_arm32::emit_set_rounding(Assembler &a, uint32_t umlmode)
{
	uint32_t const armmode = (umlmode - 1) & 3;
	emit_vmrs(a, REG_TMP);
	a.bic(REG_TMP, REG_TMP, imm(3u << 22));
	if (armmode)
		a.orr(REG_TMP, REG_TMP, imm(armmode << 22));
	emit_vmsr(a, REG_TMP);
}


//**************************************************************************
//  DISPATCH
//**************************************************************************

void drcbe_arm32::generate_one(Assembler &a, const uml::instruction &inst)
{
	switch (inst.opcode())
	{
	// structural
	case OP_HANDLE:  op_handle(a, inst);  break;
	case OP_HASH:    op_hash(a, inst);    break;
	case OP_LABEL:   op_label(a, inst);   break;
	case OP_COMMENT: op_comment(a, inst); break;
	case OP_MAPVAR:  op_mapvar(a, inst);  break;

	// control flow
	case OP_NOP:     op_nop(a, inst);     break;
	case OP_BREAK:   op_break(a, inst);   break;
	case OP_DEBUG:   op_debug(a, inst);   break;
	case OP_EXIT:    op_exit(a, inst);    break;
	case OP_HASHJMP: op_hashjmp(a, inst); break;
	case OP_JMP:     op_jmp(a, inst);     break;
	case OP_EXH:     op_exh(a, inst);     break;
	case OP_CALLH:   op_callh(a, inst);   break;
	case OP_RET:     op_ret(a, inst);     break;
	case OP_CALLC:   op_callc(a, inst);   break;
	case OP_RECOVER: op_recover(a, inst); break;

	// internal register
	case OP_SETFMOD: op_setfmod(a, inst); break;
	case OP_GETFMOD: op_getfmod(a, inst); break;
	case OP_GETEXP:  op_getexp(a, inst);  break;
	case OP_GETFLGS: op_getflgs(a, inst); break;
	case OP_SETFLGS: op_setflgs(a, inst); break;
	case OP_SAVE:    op_save(a, inst);    break;
	case OP_RESTORE: op_restore(a, inst); break;

	// integer
	case OP_LOAD:    op_load(a, inst);    break;
	case OP_LOADS:   op_loads(a, inst);   break;
	case OP_STORE:   op_store(a, inst);   break;
	case OP_READ:    op_read(a, inst);    break;
	case OP_READM:   op_readm(a, inst);   break;
	case OP_WRITE:   op_write(a, inst);   break;
	case OP_WRITEM:  op_writem(a, inst);  break;
	case OP_CARRY:   op_carry(a, inst);   break;
	case OP_SET:     op_set(a, inst);     break;
	case OP_MOV:     op_mov(a, inst);     break;
	case OP_SEXT:    op_sext(a, inst);    break;
	case OP_BFXU:    op_bfx(a, inst, false); break;
	case OP_BFXS:    op_bfx(a, inst, true);  break;
	case OP_ROLAND:  op_roland(a, inst);  break;
	case OP_ROLINS:  op_rolins(a, inst);  break;
	case OP_ADD:
	case OP_ADDC:
	case OP_SUB:
	case OP_SUBB:    op_addsub(a, inst);  break;
	case OP_CMP:     op_cmp(a, inst);     break;
	case OP_MULU:    op_mul(a, inst, false, true);  break;
	case OP_MULULW:  op_mul(a, inst, false, false); break;
	case OP_MULS:    op_mul(a, inst, true, true);   break;
	case OP_MULSLW:  op_mul(a, inst, true, false);  break;
	case OP_DIVU:    op_div(a, inst, false); break;
	case OP_DIVS:    op_div(a, inst, true);  break;
	case OP_AND:
	case OP_OR:
	case OP_XOR:     op_logic(a, inst);   break;
	case OP_TEST:    op_test(a, inst);    break;
	case OP_LZCNT:   op_lzcnt(a, inst);   break;
	case OP_TZCNT:   op_tzcnt(a, inst);   break;
	case OP_BSWAP:   op_bswap(a, inst);   break;
	case OP_SHL:
	case OP_SHR:
	case OP_SAR:
	case OP_ROL:
	case OP_ROLC:
	case OP_ROR:
	case OP_RORC:    op_shift(a, inst);   break;

	// float
	case OP_FLOAD:   op_fload(a, inst);   break;
	case OP_FSTORE:  op_fstore(a, inst);  break;
	case OP_FREAD:   op_fread(a, inst);   break;
	case OP_FWRITE:  op_fwrite(a, inst);  break;
	case OP_FMOV:    op_fmov(a, inst);    break;
	case OP_FTOINT:  op_ftoint(a, inst);  break;
	case OP_FFRINT:  op_ffrint(a, inst);  break;
	case OP_FFRFLT:  op_ffrflt(a, inst);  break;
	case OP_FRNDS:   op_frnds(a, inst);   break;
	case OP_FADD:
	case OP_FSUB:
	case OP_FMUL:
	case OP_FDIV:    op_fbinary(a, inst); break;
	case OP_FNEG:
	case OP_FABS:
	case OP_FSQRT:
	case OP_FRECIP:
	case OP_FRSQRT:  op_funary(a, inst);  break;
	case OP_FCMP:    op_fcmp(a, inst);    break;
	case OP_FCOPYI:  op_fcopyi(a, inst);  break;
	case OP_ICOPYF:  op_icopyf(a, inst);  break;

	default:
		unimplemented(inst);
	}
}


//**************************************************************************
//  STRUCTURAL OPCODES
//**************************************************************************

void drcbe_arm32::op_handle(Assembler &a, const uml::instruction &inst)
{
	assert_no_condition(inst);
	assert_no_flags(inst);
	assert(inst.numparams() == 1);
	assert(inst.param(0).is_code_handle());

	m_carry_state = carry_state::POISON;

	// The handle's code pointer is a call target, so it needs a prologue that
	// parks lr -- generated code has no other place to keep it. Code that
	// falls into the handle from above is not a call and jumps the prologue,
	// which is drcbearm64's arrangement.
	Label const skip = a.new_label();
	a.b(skip);

	inst.param(0).handle().set_codeptr(drccodeptr(a.code()->base_address() + a.offset()));

	a.push(GpList({ r12, r14 }));
	a.bind(skip);
}


void drcbe_arm32::op_hash(Assembler &a, const uml::instruction &inst)
{
	assert_no_condition(inst);
	assert_no_flags(inst);
	assert(inst.numparams() == 2);
	assert(inst.param(0).is_immediate());
	assert(inst.param(1).is_immediate());

	m_carry_state = carry_state::POISON;
	m_hash.set_codeptr(inst.param(0).immediate(), inst.param(1).immediate(), drccodeptr(a.code()->base_address() + a.offset()));
}


void drcbe_arm32::op_label(Assembler &a, const uml::instruction &inst)
{
	assert_no_condition(inst);
	assert_no_flags(inst);
	assert(inst.numparams() == 1);
	assert(inst.param(0).is_code_label());

	m_carry_state = carry_state::POISON;

	std::string const name = util::string_format("PC$%x", inst.param(0).label());
	Label label = a.label_by_name(name.c_str());
	if (!label.is_valid())
		label = a.new_named_label(name.c_str());
	a.bind(label);
}


void drcbe_arm32::op_comment(Assembler &a, const uml::instruction &inst)
{
	assert_no_condition(inst);
	assert_no_flags(inst);
	assert(inst.numparams() == 1);
	assert(inst.param(0).is_string());
}


void drcbe_arm32::op_mapvar(Assembler &a, const uml::instruction &inst)
{
	assert_no_condition(inst);
	assert_no_flags(inst);
	assert(inst.numparams() == 2);
	assert(inst.param(0).is_mapvar());
	assert(inst.param(1).is_immediate());

	m_map.set_value(drccodeptr(a.code()->base_address() + a.offset()), inst.param(0).mapvar(), inst.param(1).immediate());
}


//**************************************************************************
//  CONTROL FLOW
//**************************************************************************

void drcbe_arm32::op_nop(Assembler &a, const uml::instruction &inst)
{
}


void drcbe_arm32::op_break(Assembler &a, const uml::instruction &inst)
{
	assert_no_condition(inst);
	assert_no_flags(inst);

	static char const *const message = "break from drc";
	emit_mov_reg_imm(a, r0, uint32_t(uintptr_t(message)));
	emit_call_saving_lr(a, (void const *)&osd_break_into_debugger);
	m_carry_state = carry_state::POISON;
}


void drcbe_arm32::op_debug(Assembler &a, const uml::instruction &inst)
{
	assert(inst.size() == 4);
	assert_no_condition(inst);
	assert_no_flags(inst);

	if (m_device.machine().debug_flags & DEBUG_FLAG_ENABLED)
	{
		m_carry_state = carry_state::POISON;

		be_parameter pcp(*this, inst.param(0), PTYPE_MRI);

		Label const done = a.new_label();

		a.ldr(REG_TMP, emit_abs_mem(a, &m_device.machine().debug_flags));
		a.tst(REG_TMP, imm(2));         // DEBUG_FLAG_CALL_HOOK
		a.b(CondCode::kEQ, done);

		mov_reg_param(a, r1, pcp);
		emit_mov_reg_imm(a, r0, uint32_t(uintptr_t(m_debug_cpu_instruction_hook.obj)));
		emit_call_saving_lr(a, (void const *)m_debug_cpu_instruction_hook.func);

		a.bind(done);
	}
}


void drcbe_arm32::op_exit(Assembler &a, const uml::instruction &inst)
{
	assert(inst.size() == 4);
	assert_any_condition(inst);
	assert_no_flags(inst);

	be_parameter retp(*this, inst.param(0), PTYPE_MRI);

	Label skip;
	emit_skip(a, inst.condition(), skip);

	// The exit stub is a far absolute address, so this cannot be a predicated
	// branch -- hence the skip above rather than a conditional jump. Loading
	// the return value uses only movw/movt/ldr, none of which touch the flags
	// the condition was just tested on.
	mov_reg_param(a, r0, retp);
	emit_mov_reg_imm(a, REG_ADDR, uint32_t(uintptr_t(m_exit)));
	a.bx(REG_ADDR);

	if (inst.condition() != uml::COND_ALWAYS)
		a.bind(skip);
}


void drcbe_arm32::op_hashjmp(Assembler &a, const uml::instruction &inst)
{
	assert(inst.size() == 4);
	assert_no_condition(inst);
	assert_no_flags(inst);

	be_parameter modep(*this, inst.param(0), PTYPE_MRI);
	be_parameter pcp(*this, inst.param(1), PTYPE_MRI);
	parameter const &exp = inst.param(2);
	assert(exp.is_code_handle());

	// unwind to the depth generated code was entered at: a hash jump abandons
	// whatever call chain it was reached through, and without this the stack
	// would grow for as long as the CPU runs
	a.mov(sp, REG_FRAME);

	if (modep.is_immediate() && m_hash.populate_mode(modep.immediate()))
	{
		if (pcp.is_immediate() && !m_invariant_block)
		{
			uint32_t const l1val = (pcp.immediate() >> m_hash.l1shift()) & m_hash.l1mask();
			uint32_t const l2val = (pcp.immediate() >> m_hash.l2shift()) & m_hash.l2mask();
			a.ldr(REG_ADDR, emit_abs_mem(a, &m_hash.base()[modep.immediate()][l1val][l2val]));
		}
		else
		{
			emit_mov_reg_imm(a, r1, uint32_t(uintptr_t(m_hash.base()[modep.immediate()])));
			mov_reg_param(a, r2, pcp);
			a.ubfx(r3, r2, imm(m_hash.l1shift()), imm(m_hash.l1bits()));
			a.ldr(r1, ptr(r1, r3, lsl(2)));
			a.ubfx(r3, r2, imm(m_hash.l2shift()), imm(m_hash.l2bits()));
			a.ldr(REG_ADDR, ptr(r1, r3, lsl(2)));
		}
	}
	else
	{
		mov_reg_param(a, r0, modep);
		emit_mov_reg_imm(a, r1, uint32_t(uintptr_t(m_hash.base())));
		a.ldr(r1, ptr(r1, r0, lsl(2)));
		mov_reg_param(a, r2, pcp);
		a.ubfx(r3, r2, imm(m_hash.l1shift()), imm(m_hash.l1bits()));
		a.ldr(r1, ptr(r1, r3, lsl(2)));
		a.ubfx(r3, r2, imm(m_hash.l2shift()), imm(m_hash.l2bits()));
		a.ldr(REG_ADDR, ptr(r1, r3, lsl(2)));
	}

	// Call rather than jump: the default hash entry is the nocode stub, whose
	// entire body is a return, so a miss comes back here to raise the
	// exception. A hit never returns -- it exits or hash-jumps again.
	a.blx(REG_ADDR);

	mov_mem_param(a, &m_state.exp, pcp);

	drccodeptr *const targetptr = exp.handle().codeptr_addr();
	if (*targetptr != nullptr)
	{
		emit_mov_reg_imm(a, REG_ADDR, uint32_t(uintptr_t(*targetptr)));
	}
	else
	{
		emit_mov_reg_imm(a, REG_ADDR, uint32_t(uintptr_t(targetptr)));
		a.ldr(REG_ADDR, ptr(REG_ADDR));
	}
	a.blx(REG_ADDR);

	m_carry_state = carry_state::POISON;
}


void drcbe_arm32::op_jmp(Assembler &a, const uml::instruction &inst)
{
	assert(inst.size() == 4);
	assert_any_condition(inst);
	assert_no_flags(inst);

	parameter const &labelp = inst.param(0);
	assert(labelp.is_code_label());

	std::string const name = util::string_format("PC$%x", labelp.label());
	Label target = a.label_by_name(name.c_str());
	if (!target.is_valid())
		target = a.new_named_label(name.c_str());

	if (inst.condition() == uml::COND_ALWAYS)
	{
		a.b(target);
		return;
	}

	if ((inst.condition() == uml::COND_U) || (inst.condition() == uml::COND_NU))
	{
		// as in emit_skip: testing U costs the flags, and JMP is not allowed
		// to modify them, so both paths give them back
		Label const taken = a.new_label();
		Label const fall = a.new_label();
		a.mrs(REG_TMP, imm(MRS_APSR));
		a.tst(REG_FLAGS, imm(FLAG_U));
		a.b((inst.condition() == uml::COND_U) ? CondCode::kNE : CondCode::kEQ, taken);
		a.msr(imm(MSR_NZCVQ), REG_TMP);
		a.b(fall);
		a.bind(taken);
		a.msr(imm(MSR_NZCVQ), REG_TMP);
		a.b(target);
		a.bind(fall);
		return;
	}

	switch (inst.condition())
	{
	case uml::COND_C:
	case uml::COND_NC:
		switch (m_carry_state)
		{
		case carry_state::CANONICAL:
			a.b(ARM_NOT_CONDITION(inst.condition()), target);
			break;
		case carry_state::LOGICAL:
			a.b(ARM_CONDITION(inst.condition()), target);
			break;
		default:
			load_carry(a, true);
			a.b(ARM_CONDITION(inst.condition()), target);
			break;
		}
		break;

	case uml::COND_A:
	case uml::COND_BE:
		load_carry(a, true);
		[[fallthrough]];
	default:
		a.b(ARM_CONDITION(inst.condition()), target);
		break;
	}
}


void drcbe_arm32::op_exh(Assembler &a, const uml::instruction &inst)
{
	assert(inst.size() == 4);
	assert_any_condition(inst);
	assert_no_flags(inst);

	parameter const &handp = inst.param(0);
	assert(handp.is_code_handle());
	be_parameter exp(*this, inst.param(1), PTYPE_MRI);

	Label no_exception;
	emit_skip(a, inst.condition(), no_exception);

	mov_mem_param(a, &m_state.exp, exp);

	drccodeptr *const targetptr = handp.handle().codeptr_addr();
	a.push(GpList({ r12, r14 }));
	if (*targetptr != nullptr)
	{
		emit_mov_reg_imm(a, REG_ADDR, uint32_t(uintptr_t(*targetptr)));
	}
	else
	{
		emit_mov_reg_imm(a, REG_ADDR, uint32_t(uintptr_t(targetptr)));
		a.ldr(REG_ADDR, ptr(REG_ADDR));
	}
	a.blx(REG_ADDR);
	a.pop(GpList({ r12, r14 }));

	if (inst.condition() != uml::COND_ALWAYS)
		a.bind(no_exception);

	m_carry_state = carry_state::POISON;
}


void drcbe_arm32::op_callh(Assembler &a, const uml::instruction &inst)
{
	assert(inst.size() == 4);
	assert_any_condition(inst);
	assert_no_flags(inst);

	parameter const &handp = inst.param(0);
	assert(handp.is_code_handle());

	Label skip;
	emit_skip(a, inst.condition(), skip);

	drccodeptr *const targetptr = handp.handle().codeptr_addr();
	a.push(GpList({ r12, r14 }));
	if (*targetptr != nullptr)
	{
		emit_mov_reg_imm(a, REG_ADDR, uint32_t(uintptr_t(*targetptr)));
	}
	else
	{
		emit_mov_reg_imm(a, REG_ADDR, uint32_t(uintptr_t(targetptr)));
		a.ldr(REG_ADDR, ptr(REG_ADDR));
	}
	a.blx(REG_ADDR);
	a.pop(GpList({ r12, r14 }));

	if (inst.condition() != uml::COND_ALWAYS)
		a.bind(skip);

	m_carry_state = carry_state::POISON;
}


void drcbe_arm32::op_ret(Assembler &a, const uml::instruction &inst)
{
	assert(inst.size() == 4);
	assert_any_condition(inst);
	assert_no_flags(inst);
	assert(inst.numparams() == 0);

	Label skip;
	emit_skip(a, inst.condition(), skip);

	// undo the prologue op_handle put at the handle's entry point
	a.pop(GpList({ r12, r14 }));
	a.bx(r14);

	if (inst.condition() != uml::COND_ALWAYS)
		a.bind(skip);
}


void drcbe_arm32::op_callc(Assembler &a, const uml::instruction &inst)
{
	assert(inst.size() == 4);
	assert_any_condition(inst);
	assert_no_flags(inst);

	parameter const &funcp = inst.param(0);
	assert(funcp.is_c_function());
	be_parameter paramp(*this, inst.param(1), PTYPE_M);

	Label skip;
	emit_skip(a, inst.condition(), skip);

	// C code expects the host's rounding mode, not whatever SETFMOD left in
	// FPSCR. drcbec brackets its own calls the same way.
	emit_vmrs(a, r1);
	a.str(r1, emit_abs_mem(a, &m_near.drc_fpscr));
	a.ldr(r1, emit_abs_mem(a, &m_near.host_fpscr));
	emit_vmsr(a, r1);

	emit_mov_reg_imm(a, r0, uint32_t(uintptr_t(paramp.memory())));
	emit_call_saving_lr(a, (void const *)funcp.cfunc());

	a.ldr(r1, emit_abs_mem(a, &m_near.drc_fpscr));
	emit_vmsr(a, r1);

	if (inst.condition() != uml::COND_ALWAYS)
		a.bind(skip);

	m_carry_state = carry_state::POISON;
}


void drcbe_arm32::op_recover(Assembler &a, const uml::instruction &inst)
{
	assert(inst.size() == 4);
	assert_no_condition(inst);
	assert_no_flags(inst);

	m_carry_state = carry_state::POISON;

	be_parameter dstp(*this, inst.param(0), PTYPE_MR);

	// The map value wanted is the one at the call site of the OUTERMOST call
	// into generated code, which is the lr the first handle prologue pushed --
	// and that sits immediately below the frame anchor. Minus one because the
	// map is keyed on an address inside the calling instruction.
	emit_mov_reg_imm(a, r0, uint32_t(uintptr_t(m_drcmap_get_value.obj)));
	a.ldr(r1, ptr(REG_FRAME, -4));
	a.sub(r1, r1, imm(1));
	emit_mov_reg_imm(a, r2, inst.param(1).mapvar());
	emit_call_saving_lr(a, (void const *)m_drcmap_get_value.func);

	if (inst.size() == 4)
		mov_param_reg(a, dstp, r0);
	else
	{
		a.mov(r1, imm(0));
		mov_param_reg_pair(a, dstp, r0, r1);
	}
}


//**************************************************************************
//  INTERNAL REGISTER OPCODES
//**************************************************************************

void drcbe_arm32::op_setfmod(Assembler &a, const uml::instruction &inst)
{
	assert(inst.size() == 4);
	assert_no_condition(inst);
	assert_no_flags(inst);

	be_parameter srcp(*this, inst.param(0), PTYPE_MRI);

	if (srcp.is_immediate())
	{
		uint32_t const mode = srcp.immediate_lo() & 3;
		emit_mov_reg_imm(a, REG_ADDR, mode);
		a.strb(REG_ADDR, emit_abs_mem(a, &m_state.fmod));
		emit_set_rounding(a, mode);
	}
	else
	{
		mov_reg_param(a, r0, srcp);
		a.and_(r0, r0, imm(3));
		a.strb(r0, emit_abs_mem(a, &m_state.fmod));

		// UML numbers TRUNC first and ARM numbers nearest first, so the VFP
		// mode is the UML one less one, modulo four
		a.sub(r0, r0, imm(1));
		a.and_(r0, r0, imm(3));
		emit_vmrs(a, REG_TMP);
		a.bic(REG_TMP, REG_TMP, imm(3u << 22));
		a.orr(REG_TMP, REG_TMP, r0, lsl(22));
		emit_vmsr(a, REG_TMP);
	}
}


void drcbe_arm32::op_getfmod(Assembler &a, const uml::instruction &inst)
{
	assert(inst.size() == 4);
	assert_no_condition(inst);
	assert_no_flags(inst);

	be_parameter dstp(*this, inst.param(0), PTYPE_MR);

	a.ldrb(r0, emit_abs_mem(a, &m_state.fmod));
	if (inst.size() == 4)
	{
		mov_param_reg(a, dstp, r0);
	}
	else
	{
		a.mov(r1, imm(0));
		mov_param_reg_pair(a, dstp, r0, r1);
	}
}


void drcbe_arm32::op_getexp(Assembler &a, const uml::instruction &inst)
{
	assert(inst.size() == 4);
	assert_no_condition(inst);
	assert_no_flags(inst);

	be_parameter dstp(*this, inst.param(0), PTYPE_MR);

	a.ldr(r0, emit_abs_mem(a, &m_state.exp));
	if (inst.size() == 4)
	{
		mov_param_reg(a, dstp, r0);
	}
	else
	{
		a.mov(r1, imm(0));
		mov_param_reg_pair(a, dstp, r0, r1);
	}
}


void drcbe_arm32::op_getflgs(Assembler &a, const uml::instruction &inst)
{
	assert(inst.size() == 4 || inst.size() == 8);
	assert_no_condition(inst);
	assert_no_flags(inst);

	be_parameter dstp(*this, inst.param(0), PTYPE_MR);
	parameter const &maskp = inst.param(1);
	assert(maskp.is_immediate());

	get_flags(a, r0, uint32_t(maskp.immediate()) & FLAGS_ALL);

	if (inst.size() == 4)
	{
		mov_param_reg(a, dstp, r0);
	}
	else
	{
		a.mov(r1, imm(0));
		mov_param_reg_pair(a, dstp, r0, r1);
	}
}


void drcbe_arm32::op_setflgs(Assembler &a, const uml::instruction &inst)
{
	assert(inst.size() == 4);
	assert_no_condition(inst);

	be_parameter srcp(*this, inst.param(0), PTYPE_MRI);

	mov_reg_param(a, r0, srcp);
	set_flags(a, r0);
}


void drcbe_arm32::op_save(Assembler &a, const uml::instruction &inst)
{
	assert(inst.size() == 4);
	assert_no_condition(inst);
	assert_no_flags(inst);

	be_parameter dstp(*this, inst.param(0), PTYPE_M);

	// The stub copies the register file; the flag byte cannot be copied
	// because the live flags are not in the machine state -- they are in APSR
	// and the software flags register -- so it is assembled here and passed in.
	get_flags(a, r1, FLAGS_ALL);
	emit_mov_reg_imm(a, r0, uint32_t(uintptr_t(dstp.memory())));
	emit_call_saving_lr(a, m_save);
}


void drcbe_arm32::op_restore(Assembler &a, const uml::instruction &inst)
{
	assert(inst.size() == 4);
	assert_no_condition(inst);

	be_parameter srcp(*this, inst.param(0), PTYPE_M);

	emit_mov_reg_imm(a, r0, uint32_t(uintptr_t(srcp.memory())));
	emit_call_saving_lr(a, m_restore);

	// the stub returns the masked flag byte, which is the only part of the
	// state that does not simply live in memory
	set_flags(a, r0);
}


//**************************************************************************
//  INTEGER: LOAD / STORE
//**************************************************************************

// The address is base + (index << scale), always materialised into the address
// scratch. The scaled addressing modes would save an instruction, but only the
// word and byte loads have them -- ldrh and the signed forms take an unscaled
// register offset only -- and one form for all five sizes is worth more here
// than that instruction.

void drcbe_arm32::op_load(Assembler &a, const uml::instruction &inst)
{
	assert(inst.size() == 4 || inst.size() == 8);
	assert_no_condition(inst);
	assert_no_flags(inst);

	be_parameter dstp(*this, inst.param(0), PTYPE_MR);
	be_parameter basep(*this, inst.param(1), PTYPE_M);
	be_parameter indp(*this, inst.param(2), PTYPE_MRI);
	parameter const &scalesizep = inst.param(3);
	assert(scalesizep.is_size_scale());
	int const size = scalesizep.size();
	int const scale = scalesizep.scale();

	if (indp.is_immediate())
	{
		emit_mov_reg_imm(a, REG_ADDR, uint32_t(uintptr_t(basep.memory())) + (uint32_t(indp.immediate()) << scale));
	}
	else
	{
		mov_reg_param(a, r1, indp);
		emit_mov_reg_imm(a, REG_ADDR, uint32_t(uintptr_t(basep.memory())));
		if (scale)
			a.add(REG_ADDR, REG_ADDR, r1, lsl(scale));
		else
			a.add(REG_ADDR, REG_ADDR, r1);
	}

	switch (size)
	{
	case SIZE_BYTE:  a.ldrb(r0, ptr(REG_ADDR)); break;
	case SIZE_WORD:  a.ldrh(r0, ptr(REG_ADDR)); break;
	case SIZE_DWORD: a.ldr(r0, ptr(REG_ADDR)); break;
	case SIZE_QWORD: a.ldr(r0, ptr(REG_ADDR)); a.ldr(r1, ptr(REG_ADDR, 4)); break;
	}

	if (inst.size() == 4)
	{
		mov_param_reg(a, dstp, r0);
	}
	else
	{
		if (size != SIZE_QWORD)
			a.mov(r1, imm(0));
		mov_param_reg_pair(a, dstp, r0, r1);
	}
}


void drcbe_arm32::op_loads(Assembler &a, const uml::instruction &inst)
{
	assert(inst.size() == 4 || inst.size() == 8);
	assert_no_condition(inst);
	assert_no_flags(inst);

	be_parameter dstp(*this, inst.param(0), PTYPE_MR);
	be_parameter basep(*this, inst.param(1), PTYPE_M);
	be_parameter indp(*this, inst.param(2), PTYPE_MRI);
	parameter const &scalesizep = inst.param(3);
	assert(scalesizep.is_size_scale());
	int const size = scalesizep.size();
	int const scale = scalesizep.scale();

	if (indp.is_immediate())
	{
		emit_mov_reg_imm(a, REG_ADDR, uint32_t(uintptr_t(basep.memory())) + (uint32_t(indp.immediate()) << scale));
	}
	else
	{
		mov_reg_param(a, r1, indp);
		emit_mov_reg_imm(a, REG_ADDR, uint32_t(uintptr_t(basep.memory())));
		if (scale)
			a.add(REG_ADDR, REG_ADDR, r1, lsl(scale));
		else
			a.add(REG_ADDR, REG_ADDR, r1);
	}

	switch (size)
	{
	case SIZE_BYTE:  a.ldrsb(r0, ptr(REG_ADDR)); break;
	case SIZE_WORD:  a.ldrsh(r0, ptr(REG_ADDR)); break;
	case SIZE_DWORD: a.ldr(r0, ptr(REG_ADDR)); break;
	case SIZE_QWORD: a.ldr(r0, ptr(REG_ADDR)); a.ldr(r1, ptr(REG_ADDR, 4)); break;
	}

	if (inst.size() == 4)
	{
		mov_param_reg(a, dstp, r0);
	}
	else
	{
		if (size != SIZE_QWORD)
			a.mov(r1, r0, asr(31));
		mov_param_reg_pair(a, dstp, r0, r1);
	}
}


void drcbe_arm32::op_store(Assembler &a, const uml::instruction &inst)
{
	assert(inst.size() == 4 || inst.size() == 8);
	assert_no_condition(inst);
	assert_no_flags(inst);

	be_parameter basep(*this, inst.param(0), PTYPE_M);
	be_parameter indp(*this, inst.param(1), PTYPE_MRI);
	be_parameter srcp(*this, inst.param(2), PTYPE_MRI);
	parameter const &scalesizep = inst.param(3);
	assert(scalesizep.is_size_scale());
	int const size = scalesizep.size();
	int const scale = scalesizep.scale();

	if (size == SIZE_QWORD)
		mov_reg_param_pair(a, r2, r3, srcp);
	else
		mov_reg_param(a, r2, srcp);

	if (indp.is_immediate())
	{
		emit_mov_reg_imm(a, REG_ADDR, uint32_t(uintptr_t(basep.memory())) + (uint32_t(indp.immediate()) << scale));
	}
	else
	{
		mov_reg_param(a, r1, indp);
		emit_mov_reg_imm(a, REG_ADDR, uint32_t(uintptr_t(basep.memory())));
		if (scale)
			a.add(REG_ADDR, REG_ADDR, r1, lsl(scale));
		else
			a.add(REG_ADDR, REG_ADDR, r1);
	}

	switch (size)
	{
	case SIZE_BYTE:  a.strb(r2, ptr(REG_ADDR)); break;
	case SIZE_WORD:  a.strh(r2, ptr(REG_ADDR)); break;
	case SIZE_DWORD: a.str(r2, ptr(REG_ADDR)); break;
	case SIZE_QWORD: a.str(r2, ptr(REG_ADDR)); a.str(r3, ptr(REG_ADDR, 4)); break;
	}
}


//**************************************************************************
//  INTEGER: EMULATED MEMORY
//**************************************************************************

void drcbe_arm32::op_read(Assembler &a, const uml::instruction &inst)
{
	assert(inst.size() == 4 || inst.size() == 8);
	assert_no_condition(inst);
	assert_no_flags(inst);

	be_parameter dstp(*this, inst.param(0), PTYPE_MR);
	be_parameter addrp(*this, inst.param(1), PTYPE_MRI);
	parameter const &spacesizep = inst.param(2);
	assert(spacesizep.is_size_space());

	auto const &accessors = m_memory_accessors[spacesizep.space()].resolved;
	resolved_member_function const &func =
			(spacesizep.size() == SIZE_BYTE) ? accessors.read_byte :
			(spacesizep.size() == SIZE_WORD) ? accessors.read_word :
			(spacesizep.size() == SIZE_DWORD) ? accessors.read_dword :
			accessors.read_qword;

	mov_reg_param(a, r1, addrp);
	emit_mov_reg_imm(a, r0, uint32_t(uintptr_t(func.obj)));
	emit_call_saving_lr(a, (void const *)func.func);

	if (inst.size() == 4)
	{
		mov_param_reg(a, dstp, r0);
	}
	else
	{
		if (spacesizep.size() != SIZE_QWORD)
			a.mov(r1, imm(0));
		mov_param_reg_pair(a, dstp, r0, r1);
	}

	m_carry_state = carry_state::POISON;
}


void drcbe_arm32::op_readm(Assembler &a, const uml::instruction &inst)
{
	assert(inst.size() == 4 || inst.size() == 8);
	assert_no_condition(inst);
	assert_no_flags(inst);

	be_parameter dstp(*this, inst.param(0), PTYPE_MR);
	be_parameter addrp(*this, inst.param(1), PTYPE_MRI);
	be_parameter maskp(*this, inst.param(2), PTYPE_MRI);
	parameter const &spacesizep = inst.param(3);
	assert(spacesizep.is_size_space());

	auto const &accessors = m_memory_accessors[spacesizep.space()].resolved;
	resolved_member_function const &func =
			(spacesizep.size() == SIZE_BYTE) ? accessors.read_byte_masked :
			(spacesizep.size() == SIZE_WORD) ? accessors.read_word_masked :
			(spacesizep.size() == SIZE_DWORD) ? accessors.read_dword_masked :
			accessors.read_qword_masked;

	// a 64-bit mask is an aligned register pair, which is why it goes in
	// r2/r3 and the 32-bit one in r2 alone
	if (spacesizep.size() == SIZE_QWORD)
		mov_reg_param_pair(a, r2, r3, maskp);
	else
		mov_reg_param(a, r2, maskp);
	mov_reg_param(a, r1, addrp);
	emit_mov_reg_imm(a, r0, uint32_t(uintptr_t(func.obj)));
	emit_call_saving_lr(a, (void const *)func.func);

	if (inst.size() == 4)
	{
		mov_param_reg(a, dstp, r0);
	}
	else
	{
		if (spacesizep.size() != SIZE_QWORD)
			a.mov(r1, imm(0));
		mov_param_reg_pair(a, dstp, r0, r1);
	}

	m_carry_state = carry_state::POISON;
}


void drcbe_arm32::op_write(Assembler &a, const uml::instruction &inst)
{
	assert(inst.size() == 4 || inst.size() == 8);
	assert_no_condition(inst);
	assert_no_flags(inst);

	be_parameter addrp(*this, inst.param(0), PTYPE_MRI);
	be_parameter srcp(*this, inst.param(1), PTYPE_MRI);
	parameter const &spacesizep = inst.param(2);
	assert(spacesizep.is_size_space());

	auto const &accessors = m_memory_accessors[spacesizep.space()].resolved;
	resolved_member_function const &func =
			(spacesizep.size() == SIZE_BYTE) ? accessors.write_byte :
			(spacesizep.size() == SIZE_WORD) ? accessors.write_word :
			(spacesizep.size() == SIZE_DWORD) ? accessors.write_dword :
			accessors.write_qword;

	if (spacesizep.size() == SIZE_QWORD)
		mov_reg_param_pair(a, r2, r3, srcp);
	else
		mov_reg_param(a, r2, srcp);
	mov_reg_param(a, r1, addrp);
	emit_mov_reg_imm(a, r0, uint32_t(uintptr_t(func.obj)));
	emit_call_saving_lr(a, (void const *)func.func);

	m_carry_state = carry_state::POISON;
}


void drcbe_arm32::op_writem(Assembler &a, const uml::instruction &inst)
{
	assert(inst.size() == 4 || inst.size() == 8);
	assert_no_condition(inst);
	assert_no_flags(inst);

	be_parameter addrp(*this, inst.param(0), PTYPE_MRI);
	be_parameter srcp(*this, inst.param(1), PTYPE_MRI);
	be_parameter maskp(*this, inst.param(2), PTYPE_MRI);
	parameter const &spacesizep = inst.param(3);
	assert(spacesizep.is_size_space());

	auto const &accessors = m_memory_accessors[spacesizep.space()].resolved;
	resolved_member_function const &func =
			(spacesizep.size() == SIZE_BYTE) ? accessors.write_byte_masked :
			(spacesizep.size() == SIZE_WORD) ? accessors.write_word_masked :
			(spacesizep.size() == SIZE_DWORD) ? accessors.write_dword_masked :
			accessors.write_qword_masked;

	if (spacesizep.size() == SIZE_QWORD)
	{
		// (this, addr, data, mask): the two 64-bit values need aligned pairs,
		// so data lands in r2/r3 and mask goes on the stack
		mov_reg_param_pair(a, r2, r3, srcp);
		mov_reg_param_pair(a, r4, r5, maskp);
		mov_reg_param(a, r1, addrp);
		emit_mov_reg_imm(a, r0, uint32_t(uintptr_t(func.obj)));
		a.push(GpList({ r4, r5 }));
		a.push(GpList({ r12, r14 }));
		emit_mov_reg_imm(a, REG_ADDR, uint32_t(uintptr_t(func.func)));
		a.blx(REG_ADDR);
		a.pop(GpList({ r12, r14 }));
		a.add(sp, sp, imm(8));
	}
	else
	{
		mov_reg_param(a, r3, maskp);
		mov_reg_param(a, r2, srcp);
		mov_reg_param(a, r1, addrp);
		emit_mov_reg_imm(a, r0, uint32_t(uintptr_t(func.obj)));
		emit_call_saving_lr(a, (void const *)func.func);
	}

	m_carry_state = carry_state::POISON;
}


//**************************************************************************
//  INTEGER: FLAG AND MOVE OPCODES
//**************************************************************************

void drcbe_arm32::op_carry(Assembler &a, const uml::instruction &inst)
{
	assert(inst.size() == 4 || inst.size() == 8);
	assert_no_condition(inst);
	assert_flags(inst, FLAG_C);

	be_parameter srcp(*this, inst.param(0), PTYPE_MRI);
	be_parameter bitp(*this, inst.param(1), PTYPE_MRI);

	// CARRY defines C and nothing else -- drcbec explicitly preserves the rest
	// -- so nothing below is allowed to be an S-form or a compare.
	if (bitp.is_immediate())
	{
		unsigned const bit = bitp.immediate() & ((inst.size() == 4) ? 31 : 63);
		if ((inst.size() == 8) && (bit >= 32))
		{
			Mem const memhi = srcp.is_memory()
					? emit_abs_mem(a, (uint8_t *)srcp.memory() + 4)
					: Mem();
			if (srcp.is_memory())
				a.ldr(r0, memhi);
			else
				emit_mov_reg_imm(a, r0, srcp.immediate_hi());
			store_carry_bit(a, r0, bit - 32);
		}
		else
		{
			mov_reg_param(a, r0, srcp);
			store_carry_bit(a, r0, bit);
		}
		return;
	}

	mov_reg_param(a, r2, bitp);
	if (inst.size() == 4)
	{
		mov_reg_param(a, r0, srcp);
		a.and_(r2, r2, imm(31));
		a.lsr(r0, r0, r2);
	}
	else
	{
		mov_reg_param_pair(a, r0, r1, srcp);
		a.and_(r2, r2, imm(63));

		// select the half without a compare, which would clobber the flags
		// this opcode has to leave alone: bit 5 of the count, smeared
		a.mov(r3, r2, lsl(26));
		a.mov(r3, r3, asr(31));
		a.and_(r1, r1, r3);
		a.bic(r0, r0, r3);
		a.orr(r0, r0, r1);

		a.and_(r2, r2, imm(31));
		a.lsr(r0, r0, r2);
	}
	store_carry_bit(a, r0, 0);
}


void drcbe_arm32::op_set(Assembler &a, const uml::instruction &inst)
{
	assert(inst.size() == 4 || inst.size() == 8);
	assert_any_condition(inst);
	assert_no_flags(inst);

	be_parameter dstp(*this, inst.param(0), PTYPE_MR);
	uml::condition_t const cond = inst.condition();

	if ((cond == uml::COND_U) || (cond == uml::COND_NU))
	{
		// no hardware flag for this one, and SET must not disturb the flags,
		// so it is a bit extract rather than a predicated move
		a.ubfx(r0, REG_FLAGS, imm(FLAG_BIT_U), imm(1));
		if (cond == uml::COND_NU)
			a.eor(r0, r0, imm(1));
	}
	else
	{
		CondCode const cc = emit_cond_setup(a, cond);
		a.mov(r0, imm(0));
		a.mov(cc, r0, imm(1));
	}

	if (inst.size() == 4)
	{
		mov_param_reg(a, dstp, r0);
	}
	else
	{
		a.mov(r1, imm(0));
		mov_param_reg_pair(a, dstp, r0, r1);
	}
}


void drcbe_arm32::op_mov(Assembler &a, const uml::instruction &inst)
{
	assert(inst.size() == 4 || inst.size() == 8);
	assert_any_condition(inst);
	assert_no_flags(inst);

	be_parameter dstp(*this, inst.param(0), PTYPE_MR);
	be_parameter srcp(*this, inst.param(1), PTYPE_MRI);

	if (dstp == srcp)
		return;

	Label skip;
	emit_skip(a, inst.condition(), skip);

	if (inst.size() == 4)
	{
		mov_reg_param(a, r0, srcp);
		mov_param_reg(a, dstp, r0);
	}
	else
	{
		mov_reg_param_pair(a, r0, r1, srcp);
		mov_param_reg_pair(a, dstp, r0, r1);
	}

	if (inst.condition() != uml::COND_ALWAYS)
		a.bind(skip);
}


void drcbe_arm32::op_sext(Assembler &a, const uml::instruction &inst)
{
	assert(inst.size() == 4 || inst.size() == 8);
	assert_no_condition(inst);
	assert_flags(inst, FLAG_S | FLAG_Z);

	be_parameter dstp(*this, inst.param(0), PTYPE_MR);
	be_parameter srcp(*this, inst.param(1), PTYPE_MRI);
	parameter const &sizep = inst.param(2);
	assert(sizep.is_size());

	if (sizep.size() == SIZE_QWORD)
		mov_reg_param_pair(a, r0, r1, srcp);
	else
		mov_reg_param(a, r0, srcp);

	switch (sizep.size())
	{
	case SIZE_BYTE:  a.sxtb(r0, r0); break;
	case SIZE_WORD:  a.sxth(r0, r0); break;
	default: break;
	}

	if (inst.size() == 4)
	{
		if (inst.flags())
			emit_flags_sz32(a, r0);
		mov_param_reg(a, dstp, r0);
	}
	else
	{
		if (sizep.size() != SIZE_QWORD)
			a.mov(r1, r0, asr(31));
		if (inst.flags())
			emit_flags_sz64(a, r0, r1);
		mov_param_reg_pair(a, dstp, r0, r1);
	}
}


// BFXU/BFXS -- extract `width` bits starting at `shift`. drcbec spells it as a
// rotate followed by a shift rather than as a field extract, and that spelling
// is copied verbatim here because it is what defines the edge cases: a width
// of zero and a width of the full register both fall out of it, where ubfx
// would have to special-case them.
void drcbe_arm32::op_bfx(Assembler &a, const uml::instruction &inst, bool sign)
{
	assert(inst.size() == 4 || inst.size() == 8);
	assert_no_condition(inst);
	assert_flags(inst, FLAG_S | FLAG_Z);

	be_parameter dstp(*this, inst.param(0), PTYPE_MR);
	be_parameter srcp(*this, inst.param(1), PTYPE_MRI);
	be_parameter shiftp(*this, inst.param(2), PTYPE_MRI);
	be_parameter widthp(*this, inst.param(3), PTYPE_MRI);

	if (inst.size() == 8)
	{
		// cold and intricate: a 64-bit rotate followed by a 64-bit shift.
		// The width has to outlive the first call, so it sits in r4, which
		// AAPCS makes the callee's problem rather than ours.
		mov_reg_param(a, r4, widthp);
		emit_param_ptr(a, r1, srcp, 0);
		emit_mov_reg_imm(a, r0, uint32_t(uintptr_t(&m_near.tmp[2])));
		mov_reg_param(a, r2, shiftp);
		a.add(r2, r2, r4);
		a.and_(r2, r2, imm(63));
		emit_mov_reg_imm(a, r3, OP_ROR);
		emit_call_saving_lr(a, (void const *)&arm32_shift64);

		a.rsb(r2, r4, imm(0));
		a.and_(r2, r2, imm(63));
		emit_mov_reg_imm(a, r3, sign ? OP_SAR : OP_SHR);
		emit_mov_reg_imm(a, r1, uint32_t(uintptr_t(&m_near.tmp[2])));
		emit_mov_reg_imm(a, r0, uint32_t(uintptr_t(&m_near.tmp[2])));
		emit_call_saving_lr(a, (void const *)&arm32_shift64);

		emit_mov_reg_imm(a, REG_ADDR, uint32_t(uintptr_t(&m_near.tmp[2])));
		a.ldr(r2, ptr(REG_ADDR));
		a.ldr(r3, ptr(REG_ADDR, 4));
		if (inst.flags())
			emit_flags_sz64(a, r2, r3);
		mov_param_reg_pair(a, dstp, r2, r3);
		m_carry_state = carry_state::POISON;
		return;
	}

	mov_reg_param(a, r0, srcp);

	if (shiftp.is_immediate() && widthp.is_immediate())
	{
		unsigned const rot = (shiftp.immediate() + widthp.immediate()) & 31;
		unsigned const shr = (0u - widthp.immediate()) & 31;
		if (rot)
			a.mov(r0, r0, ror(rot));
		if (shr)
			a.mov(r0, r0, sign ? asr(shr) : lsr(shr));
	}
	else
	{
		mov_reg_param(a, r2, shiftp);
		mov_reg_param(a, r3, widthp);
		a.add(r2, r2, r3);
		a.and_(r2, r2, imm(31));
		a.ror(r0, r0, r2);
		a.rsb(r3, r3, imm(0));
		a.and_(r3, r3, imm(31));
		if (sign)
			a.asr(r0, r0, r3);
		else
			a.lsr(r0, r0, r3);
	}

	if (inst.flags())
		emit_flags_sz32(a, r0);
	mov_param_reg(a, dstp, r0);
}


void drcbe_arm32::op_roland(Assembler &a, const uml::instruction &inst)
{
	assert(inst.size() == 4 || inst.size() == 8);
	assert_no_condition(inst);
	assert_flags(inst, FLAG_S | FLAG_Z);

	be_parameter dstp(*this, inst.param(0), PTYPE_MR);
	be_parameter srcp(*this, inst.param(1), PTYPE_MRI);
	be_parameter countp(*this, inst.param(2), PTYPE_MRI);
	be_parameter maskp(*this, inst.param(3), PTYPE_MRI);

	if (inst.size() == 8)
	{
		emit_param_ptr(a, r1, srcp, 0);
		emit_mov_reg_imm(a, r0, uint32_t(uintptr_t(&m_near.tmp[2])));
		mov_reg_param(a, r2, countp);
		a.and_(r2, r2, imm(63));
		emit_mov_reg_imm(a, r3, OP_ROL);
		emit_call_saving_lr(a, (void const *)&arm32_shift64);

		emit_mov_reg_imm(a, REG_ADDR, uint32_t(uintptr_t(&m_near.tmp[2])));
		a.ldr(r0, ptr(REG_ADDR));
		a.ldr(r1, ptr(REG_ADDR, 4));
		mov_reg_param_pair(a, r2, r3, maskp);
		a.and_(r0, r0, r2);
		a.and_(r1, r1, r3);
		if (inst.flags())
			emit_flags_sz64(a, r0, r1);
		mov_param_reg_pair(a, dstp, r0, r1);
		m_carry_state = carry_state::POISON;
		return;
	}

	mov_reg_param(a, r0, srcp);

	// there is no ROL on ARM; a left rotate by n is a right rotate by -n, and
	// the register form gets that for free because a rotate by 32 is identity
	if (countp.is_immediate())
	{
		unsigned const rot = (0u - countp.immediate()) & 31;
		if (rot)
			a.mov(r0, r0, ror(rot));
	}
	else
	{
		mov_reg_param(a, r2, countp);
		a.rsb(r2, r2, imm(0));
		a.and_(r2, r2, imm(31));
		a.ror(r0, r0, r2);
	}

	if (maskp.is_immediate() && is_arm_imm(maskp.immediate_lo()))
	{
		a.and_(r0, r0, imm(maskp.immediate_lo()));
	}
	else
	{
		mov_reg_param(a, r2, maskp);
		a.and_(r0, r0, r2);
	}

	if (inst.flags())
		emit_flags_sz32(a, r0);
	mov_param_reg(a, dstp, r0);
}


void drcbe_arm32::op_rolins(Assembler &a, const uml::instruction &inst)
{
	assert(inst.size() == 4 || inst.size() == 8);
	assert_no_condition(inst);
	assert_flags(inst, FLAG_S | FLAG_Z);

	be_parameter dstp(*this, inst.param(0), PTYPE_MR);
	be_parameter srcp(*this, inst.param(1), PTYPE_MRI);
	be_parameter countp(*this, inst.param(2), PTYPE_MRI);
	be_parameter maskp(*this, inst.param(3), PTYPE_MRI);

	if (inst.size() == 8)
	{
		emit_param_ptr(a, r1, srcp, 0);
		emit_mov_reg_imm(a, r0, uint32_t(uintptr_t(&m_near.tmp[2])));
		mov_reg_param(a, r2, countp);
		a.and_(r2, r2, imm(63));
		emit_mov_reg_imm(a, r3, OP_ROL);
		emit_call_saving_lr(a, (void const *)&arm32_shift64);

		emit_mov_reg_imm(a, REG_ADDR, uint32_t(uintptr_t(&m_near.tmp[2])));
		a.ldr(r0, ptr(REG_ADDR));
		a.ldr(r1, ptr(REG_ADDR, 4));
		mov_reg_param_pair(a, r2, r3, maskp);
		a.and_(r0, r0, r2);
		a.and_(r1, r1, r3);
		mov_reg_param_pair(a, r4, r5, dstp);
		a.bic(r4, r4, r2);
		a.bic(r5, r5, r3);
		a.orr(r0, r0, r4);
		a.orr(r1, r1, r5);
		if (inst.flags())
			emit_flags_sz64(a, r0, r1);
		mov_param_reg_pair(a, dstp, r0, r1);
		m_carry_state = carry_state::POISON;
		return;
	}

	mov_reg_param(a, r0, srcp);

	if (countp.is_immediate())
	{
		unsigned const rot = (0u - countp.immediate()) & 31;
		if (rot)
			a.mov(r0, r0, ror(rot));
	}
	else
	{
		mov_reg_param(a, r2, countp);
		a.rsb(r2, r2, imm(0));
		a.and_(r2, r2, imm(31));
		a.ror(r0, r0, r2);
	}

	mov_reg_param(a, r2, maskp);
	mov_reg_param(a, r1, dstp);
	a.and_(r0, r0, r2);
	a.bic(r1, r1, r2);
	a.orr(r0, r0, r1);

	if (inst.flags())
		emit_flags_sz32(a, r0);
	mov_param_reg(a, dstp, r0);
}


//**************************************************************************
//  INTEGER: ARITHMETIC
//**************************************************************************

void drcbe_arm32::op_addsub(Assembler &a, const uml::instruction &inst)
{
	assert(inst.size() == 4 || inst.size() == 8);
	assert_no_condition(inst);
	assert_flags(inst, FLAGS_ALL);

	be_parameter dstp(*this, inst.param(0), PTYPE_MR);
	be_parameter src1p(*this, inst.param(1), PTYPE_MRI);
	be_parameter src2p(*this, inst.param(2), PTYPE_MRI);

	bool const sub = (inst.opcode() == OP_SUB) || (inst.opcode() == OP_SUBB);
	bool const carryin = (inst.opcode() == OP_ADDC) || (inst.opcode() == OP_SUBB);
	bool const flags = inst.flags() != 0;

	// ADDC consumes the UML carry as ARM's carry; SUBB consumes it as ARM's
	// NOT-borrow, because SBC subtracts (1 - C). Getting this pair backwards
	// is the classic ARM UML bug and the reason carry_state exists.
	if (carryin)
		load_carry(a, sub);

	if (inst.size() == 4)
	{
		mov_reg_param(a, r0, src1p);

		bool const imm2 = src2p.is_immediate() && is_arm_imm(src2p.immediate_lo());
		if (!imm2)
			mov_reg_param(a, r1, src2p);
		Imm const i2 = imm(imm2 ? src2p.immediate_lo() : 0);

		if (carryin)
		{
			if (sub)
			{
				if (imm2) a.sbcs(r0, r0, i2); else a.sbcs(r0, r0, r1);
			}
			else
			{
				if (imm2) a.adcs(r0, r0, i2); else a.adcs(r0, r0, r1);
			}
		}
		else if (sub)
		{
			if (imm2) a.subs(r0, r0, i2); else a.subs(r0, r0, r1);
		}
		else
		{
			if (imm2) a.adds(r0, r0, i2); else a.adds(r0, r0, r1);
		}

		if (flags)
			store_carry(a, sub);
		else
			m_carry_state = carry_state::POISON;

		mov_param_reg(a, dstp, r0);
	}
	else
	{
		mov_reg_param_pair(a, r0, r1, src1p);
		mov_reg_param_pair(a, r2, r3, src2p);

		// The low half must be an S-form even when no flags were asked for,
		// because the high half consumes its carry.
		if (carryin)
		{
			if (sub) a.sbcs(r0, r0, r2); else a.adcs(r0, r0, r2);
		}
		else
		{
			if (sub) a.subs(r0, r0, r2); else a.adds(r0, r0, r2);
		}
		if (sub) a.sbcs(r1, r1, r3); else a.adcs(r1, r1, r3);

		if (flags)
		{
			// N and V come out of the high half and are already right for the
			// 64-bit value; only Z has to be widened.
			store_carry(a, sub);
			emit_combine_z(a, r0, r1);
		}
		else
		{
			m_carry_state = carry_state::POISON;
		}

		mov_param_reg_pair(a, dstp, r0, r1);
	}
}


void drcbe_arm32::op_cmp(Assembler &a, const uml::instruction &inst)
{
	assert(inst.size() == 4 || inst.size() == 8);
	assert_no_condition(inst);
	assert_flags(inst, FLAGS_ALL);

	be_parameter src1p(*this, inst.param(0), PTYPE_MRI);
	be_parameter src2p(*this, inst.param(1), PTYPE_MRI);

	if (inst.size() == 4)
	{
		mov_reg_param(a, r0, src1p);
		if (src2p.is_immediate() && is_arm_imm(src2p.immediate_lo()))
		{
			a.cmp(r0, imm(src2p.immediate_lo()));
		}
		else
		{
			mov_reg_param(a, r1, src2p);
			a.cmp(r0, r1);
		}
		store_carry(a, true);
	}
	else
	{
		mov_reg_param_pair(a, r0, r1, src1p);
		mov_reg_param_pair(a, r2, r3, src2p);
		a.subs(r4, r0, r2);
		a.sbcs(r5, r1, r3);
		store_carry(a, true);
		emit_combine_z(a, r4, r5);
	}
}


void drcbe_arm32::op_mul(Assembler &a, const uml::instruction &inst, bool sign, bool wide)
{
	assert(inst.size() == 4 || inst.size() == 8);
	assert_no_condition(inst);
	assert_flags(inst, FLAG_S | FLAG_Z | FLAG_V);

	be_parameter dstp(*this, inst.param(0), PTYPE_MR);
	be_parameter edstp(*this, inst.param(wide ? 1 : 0), PTYPE_MR);
	be_parameter src1p(*this, inst.param(wide ? 2 : 1), PTYPE_MRI);
	be_parameter src2p(*this, inst.param(wide ? 3 : 2), PTYPE_MRI);

	if (inst.size() == 8)
	{
		// 64x64 is four 32x32 partial products plus the carry chain, and the
		// signed correction on top of that -- long, cold, and clearer in C
		emit_param_ptr(a, r2, src1p, 0);
		emit_param_ptr(a, r3, src2p, 1);
		emit_mov_reg_imm(a, r0, uint32_t(uintptr_t(dstp.memory())));
		if (wide)
			emit_mov_reg_imm(a, r1, uint32_t(uintptr_t(edstp.memory())));
		else
			a.mov(r1, imm(0));
		emit_call_saving_lr(a, sign ? (void const *)&arm32_dmuls : (void const *)&arm32_dmulu);

		if (inst.flags())
			set_flags(a, r0);
		else
			m_carry_state = carry_state::POISON;
		return;
	}

	mov_reg_param(a, r2, src1p);
	mov_reg_param(a, r3, src2p);
	if (sign)
		a.smull(r0, r1, r2, r3);
	else
		a.umull(r0, r1, r2, r3);

	if (inst.flags())
	{
		// The UML flags describe the FULL product even when only the low half
		// is stored, so all three come off the register pair rather than off
		// whatever the multiply happened to leave in APSR.
		a.mov(r4, imm(0));

		if (wide)
		{
			a.orr(r5, r0, r1);
			a.cmp(r5, imm(0));
		}
		else
		{
			a.cmp(r0, imm(0));
		}
		a.orr(CondCode::kEQ, r4, r4, imm(FLAG_Z));

		a.cmp(wide ? r1 : r0, imm(0));
		a.orr(CondCode::kMI, r4, r4, imm(FLAG_S));

		if (sign)
			a.cmp(r1, r0, asr(31));
		else
			a.cmp(r1, imm(0));
		a.orr(CondCode::kNE, r4, r4, imm(FLAG_V));

		set_flags(a, r4);
	}
	else
	{
		m_carry_state = carry_state::POISON;
	}

	// drcbec writes the high half first, so an aliased pair keeps the low one
	if (wide)
		mov_param_reg(a, edstp, r1);
	mov_param_reg(a, dstp, r0);
}


void drcbe_arm32::op_div(Assembler &a, const uml::instruction &inst, bool sign)
{
	assert(inst.size() == 4 || inst.size() == 8);
	assert_no_condition(inst);
	assert_flags(inst, FLAG_S | FLAG_Z | FLAG_V);

	be_parameter dstp(*this, inst.param(0), PTYPE_MR);
	be_parameter edstp(*this, inst.param(1), PTYPE_MR);
	be_parameter src1p(*this, inst.param(2), PTYPE_MRI);
	be_parameter src2p(*this, inst.param(3), PTYPE_MRI);

	// The Cortex-A9 has no divide instruction -- SDIV/UDIV are ARMv7-R/M or
	// ARMv7-A with the idiv extension and it has neither -- so this is a call
	// however it is written. Writing it as a call into this file keeps the
	// divide-by-zero rule (leave both destinations alone, report overflow) in
	// one place instead of spread across a branchy sequence.
	if (inst.size() == 8)
	{
		emit_param_ptr(a, r2, src1p, 0);
		emit_param_ptr(a, r3, src2p, 1);
		emit_mov_reg_imm(a, r0, uint32_t(uintptr_t(dstp.memory())));
		emit_mov_reg_imm(a, r1, uint32_t(uintptr_t(edstp.memory())));
		emit_call_saving_lr(a, sign ? (void const *)&arm32_ddivs : (void const *)&arm32_ddivu);
	}
	else
	{
		mov_reg_param(a, r2, src1p);
		mov_reg_param(a, r3, src2p);
		emit_mov_reg_imm(a, r0, uint32_t(uintptr_t(dstp.memory())));
		emit_mov_reg_imm(a, r1, uint32_t(uintptr_t(edstp.memory())));
		emit_call_saving_lr(a, sign ? (void const *)&arm32_divs : (void const *)&arm32_divu);
	}

	if (inst.flags())
		set_flags(a, r0);
	else
		m_carry_state = carry_state::POISON;
}


//**************************************************************************
//  INTEGER: LOGIC
//**************************************************************************

void drcbe_arm32::op_logic(Assembler &a, const uml::instruction &inst)
{
	assert(inst.size() == 4 || inst.size() == 8);
	assert_no_condition(inst);
	assert_flags(inst, FLAG_S | FLAG_Z);

	be_parameter dstp(*this, inst.param(0), PTYPE_MR);
	be_parameter src1p(*this, inst.param(1), PTYPE_MRI);
	be_parameter src2p(*this, inst.param(2), PTYPE_MRI);

	opcode_t const op = inst.opcode();
	bool const flags = inst.flags() != 0;

	if (inst.size() == 4)
	{
		mov_reg_param(a, r0, src1p);

		bool const imm2 = src2p.is_immediate() && is_arm_imm(src2p.immediate_lo());
		if (!imm2)
			mov_reg_param(a, r1, src2p);
		Imm const i2 = imm(imm2 ? src2p.immediate_lo() : 0);

		switch (op)
		{
		case OP_AND:
			if (flags) { if (imm2) a.ands(r0, r0, i2); else a.ands(r0, r0, r1); }
			else       { if (imm2) a.and_(r0, r0, i2); else a.and_(r0, r0, r1); }
			break;
		case OP_OR:
			if (flags) { if (imm2) a.orrs(r0, r0, i2); else a.orrs(r0, r0, r1); }
			else       { if (imm2) a.orr(r0, r0, i2);  else a.orr(r0, r0, r1); }
			break;
		default:
			if (flags) { if (imm2) a.eors(r0, r0, i2); else a.eors(r0, r0, r1); }
			else       { if (imm2) a.eor(r0, r0, i2);  else a.eor(r0, r0, r1); }
			break;
		}

		// a data-processing immediate with a non-zero rotation writes the
		// shifter carry out to C, so the hardware flag can no longer be
		// assumed to match the software one
		if (flags)
			m_carry_state = carry_state::POISON;

		mov_param_reg(a, dstp, r0);
	}
	else
	{
		mov_reg_param_pair(a, r0, r1, src1p);
		mov_reg_param_pair(a, r2, r3, src2p);

		switch (op)
		{
		case OP_AND: a.and_(r0, r0, r2); a.and_(r1, r1, r3); break;
		case OP_OR:  a.orr(r0, r0, r2);  a.orr(r1, r1, r3);  break;
		default:     a.eor(r0, r0, r2);  a.eor(r1, r1, r3);  break;
		}

		if (flags)
			emit_flags_sz64(a, r0, r1);
		mov_param_reg_pair(a, dstp, r0, r1);
	}
}


void drcbe_arm32::op_test(Assembler &a, const uml::instruction &inst)
{
	assert(inst.size() == 4 || inst.size() == 8);
	assert_no_condition(inst);
	assert_flags(inst, FLAG_S | FLAG_Z);

	be_parameter src1p(*this, inst.param(0), PTYPE_MRI);
	be_parameter src2p(*this, inst.param(1), PTYPE_MRI);

	if (inst.size() == 4)
	{
		mov_reg_param(a, r0, src1p);
		if (src2p.is_immediate() && is_arm_imm(src2p.immediate_lo()))
		{
			a.tst(r0, imm(src2p.immediate_lo()));
		}
		else
		{
			mov_reg_param(a, r1, src2p);
			a.tst(r0, r1);
		}
		m_carry_state = carry_state::POISON;
	}
	else
	{
		mov_reg_param_pair(a, r0, r1, src1p);
		mov_reg_param_pair(a, r2, r3, src2p);
		a.and_(r0, r0, r2);
		a.and_(r1, r1, r3);
		emit_flags_sz64(a, r0, r1);
	}
}


void drcbe_arm32::op_lzcnt(Assembler &a, const uml::instruction &inst)
{
	assert(inst.size() == 4 || inst.size() == 8);
	assert_no_condition(inst);
	assert_flags(inst, FLAG_S | FLAG_Z);

	be_parameter dstp(*this, inst.param(0), PTYPE_MR);
	be_parameter srcp(*this, inst.param(1), PTYPE_MRI);

	if (inst.size() == 4)
	{
		mov_reg_param(a, r0, srcp);
		a.clz(r0, r0);
		if (inst.flags())
			emit_flags_sz32(a, r0);
		mov_param_reg(a, dstp, r0);
	}
	else
	{
		mov_reg_param_pair(a, r0, r1, srcp);
		a.clz(r2, r1);
		a.clz(r3, r0);
		a.cmp(r1, imm(0));
		a.add(CondCode::kEQ, r2, r3, imm(32));
		a.mov(r0, r2);
		a.mov(r1, imm(0));
		if (inst.flags())
			emit_flags_sz64(a, r0, r1);
		else
			m_carry_state = carry_state::POISON;
		mov_param_reg_pair(a, dstp, r0, r1);
	}
}


void drcbe_arm32::op_tzcnt(Assembler &a, const uml::instruction &inst)
{
	assert(inst.size() == 4 || inst.size() == 8);
	assert_no_condition(inst);
	assert_flags(inst, FLAG_S | FLAG_Z);

	be_parameter dstp(*this, inst.param(0), PTYPE_MR);
	be_parameter srcp(*this, inst.param(1), PTYPE_MRI);

	// ARM has no count-trailing-zeros, so it is a bit reversal followed by a
	// count-leading-zeros -- both single instructions on ARMv6T2 and later.
	if (inst.size() == 4)
	{
		mov_reg_param(a, r0, srcp);
		a.rbit(r0, r0);
		a.clz(r0, r0);
	}
	else
	{
		mov_reg_param_pair(a, r0, r1, srcp);
		a.rbit(r2, r0);
		a.clz(r2, r2);
		a.rbit(r3, r1);
		a.clz(r3, r3);
		a.cmp(r0, imm(0));
		a.add(CondCode::kEQ, r2, r3, imm(32));
		a.mov(r0, r2);
		a.mov(r1, imm(0));
	}

	if (inst.flags())
	{
		// TZCNT is the one counting opcode whose flags are not FLAGS_NZ: the
		// interpreter reports zero only for the all-zero input, and never
		// reports sign. Copying that exactly matters, because the natural
		// reading -- "flags from the result" -- gets Z backwards.
		a.mov(r4, imm(0));
		a.cmp(r0, imm((inst.size() == 4) ? 32 : 64));
		a.orr(CondCode::kEQ, r4, r4, imm(FLAG_Z));
		set_flags(a, r4);
	}
	else
	{
		m_carry_state = carry_state::POISON;
	}

	if (inst.size() == 4)
		mov_param_reg(a, dstp, r0);
	else
		mov_param_reg_pair(a, dstp, r0, r1);
}


void drcbe_arm32::op_bswap(Assembler &a, const uml::instruction &inst)
{
	assert(inst.size() == 4 || inst.size() == 8);
	assert_no_condition(inst);
	assert_flags(inst, FLAG_S | FLAG_Z);

	be_parameter dstp(*this, inst.param(0), PTYPE_MR);
	be_parameter srcp(*this, inst.param(1), PTYPE_MRI);

	if (inst.size() == 4)
	{
		mov_reg_param(a, r0, srcp);
		a.rev(r0, r0);
		if (inst.flags())
			emit_flags_sz32(a, r0);
		mov_param_reg(a, dstp, r0);
	}
	else
	{
		mov_reg_param_pair(a, r0, r1, srcp);
		a.rev(r2, r1);
		a.rev(r3, r0);
		if (inst.flags())
			emit_flags_sz64(a, r2, r3);
		mov_param_reg_pair(a, dstp, r2, r3);
	}
}


//**************************************************************************
//  INTEGER: SHIFTS AND ROTATES
//**************************************************************************

void drcbe_arm32::op_shift(Assembler &a, const uml::instruction &inst)
{
	assert(inst.size() == 4 || inst.size() == 8);
	assert_no_condition(inst);
	assert_flags(inst, FLAG_S | FLAG_Z | FLAG_C);

	be_parameter dstp(*this, inst.param(0), PTYPE_MR);
	be_parameter srcp(*this, inst.param(1), PTYPE_MRI);
	be_parameter countp(*this, inst.param(2), PTYPE_MRI);

	opcode_t const op = inst.opcode();
	bool const carryop = (op == OP_ROLC) || (op == OP_RORC);
	bool const flags = inst.flags() != 0;

	// ---- 64-bit: always the helper ----
	//
	// Synthesising a 64-bit shift is five instructions plus a seam term, and
	// the carry-out bit is a second, different extraction on either side of
	// the same seam. The CPU cores this back-end exists for are 32-bit and do
	// not emit these at all.
	if (inst.size() == 8)
	{
		if (carryop)
		{
			get_flags(a, r4, FLAG_C);
			emit_mov_reg_imm(a, REG_TMP, op);
			a.orr(r4, REG_TMP, r4, lsl(8));
		}
		else
		{
			emit_mov_reg_imm(a, r4, op);
		}

		mov_reg_param(a, r2, countp);
		emit_param_ptr(a, r1, srcp, 0);
		emit_mov_reg_imm(a, r0, uint32_t(uintptr_t(dstp.memory())));
		a.mov(r3, r4);
		emit_call_saving_lr(a, (void const *)&arm32_shift64);

		if (flags)
			set_flags(a, r0);
		else
			m_carry_state = carry_state::POISON;
		return;
	}

	// ---- 32-bit rotate through carry ----
	if (carryop)
	{
		uint32_t const c = countp.is_immediate() ? (countp.immediate() & 31) : 0;

		if (countp.is_immediate() && (c == 0))
		{
			// a rotate of zero is a move, and leaves the carry alone
			mov_reg_param(a, r0, srcp);
			if (flags)
				emit_flags_sz32(a, r0);
			mov_param_reg(a, dstp, r0);
			return;
		}

		if (countp.is_immediate() && (c == 1))
		{
			// The single-bit case is the one real CPUs actually emit (SH-2's
			// ROTCL/ROTCR, the 68000's ROXL/ROXR), and ARM does it in one
			// instruction each: ADC of a value with itself shifts left through
			// carry, and RRX is a right shift through carry by definition.
			mov_reg_param(a, r0, srcp);
			load_carry(a, false);
			if (op == OP_ROLC)
				a.adcs(r0, r0, r0);
			else
				a.rrxs(r0, r0);
			if (flags)
				store_carry(a, false);
			else
				m_carry_state = carry_state::POISON;
			mov_param_reg(a, dstp, r0);
			return;
		}

		get_flags(a, r4, FLAG_C);
		emit_mov_reg_imm(a, REG_TMP, op);
		a.orr(r4, REG_TMP, r4, lsl(8));
		mov_reg_param(a, r2, countp);
		mov_reg_param(a, r1, srcp);
		emit_mov_reg_imm(a, r0, uint32_t(uintptr_t(dstp.memory())));
		a.mov(r3, r4);
		emit_call_saving_lr(a, (void const *)&arm32_shift32c);

		if (flags)
			set_flags(a, r0);
		else
			m_carry_state = carry_state::POISON;
		return;
	}

	// ---- 32-bit plain shifts and rotates ----
	mov_reg_param(a, r0, srcp);

	if (countp.is_immediate())
	{
		uint32_t const c = countp.immediate() & 31;

		if (c == 0)
		{
			a.mov(r1, r0);
		}
		else
		{
			switch (op)
			{
			case OP_SHL: a.mov(r1, r0, lsl(c)); break;
			case OP_SHR: a.mov(r1, r0, lsr(c)); break;
			case OP_SAR: a.mov(r1, r0, asr(c)); break;
			case OP_ROL: a.mov(r1, r0, ror(32 - c)); break;
			default:     a.mov(r1, r0, ror(c)); break;
			}
		}

		if (flags)
		{
			emit_flags_sz32(a, r1);
			if (c == 0)
			{
				// the interpreter defines a shift of zero as clearing the
				// carry rather than preserving it
				clear_carry(a);
			}
			else
			{
				unsigned const bit = ((op == OP_SHL) || (op == OP_ROL)) ? (32 - c) : (c - 1);
				store_carry_bit(a, r0, bit);
			}
		}
	}
	else
	{
		mov_reg_param(a, r2, countp);
		a.and_(r2, r2, imm(31));

		switch (op)
		{
		case OP_SHL: a.lsl(r1, r0, r2); break;
		case OP_SHR: a.lsr(r1, r0, r2); break;
		case OP_SAR: a.asr(r1, r0, r2); break;
		case OP_ROL: a.rsb(r3, r2, imm(32)); a.ror(r1, r0, r3); break;
		default:     a.ror(r1, r0, r2); break;
		}

		if (flags)
		{
			emit_flags_sz32(a, r1);

			// A register-specified shift uses the low BYTE of the amount, so
			// an amount of 32 or more produces zero -- which is exactly the
			// "carry is clear when the count is zero" rule, for free, in both
			// directions: 32 - 0 is 32, and 0 - 1 is 255.
			if ((op == OP_SHL) || (op == OP_ROL))
				a.rsb(r3, r2, imm(32));
			else
				a.sub(r3, r2, imm(1));
			a.lsr(r3, r0, r3);
			store_carry_bit(a, r3, 0);
		}
	}

	mov_param_reg(a, dstp, r1);
}


//**************************************************************************
//  FLOAT
//**************************************************************************

// VLDR/VSTR take an 8-bit offset scaled by four, which is a shorter reach than
// the integer forms and is why float access does its own address arithmetic
// rather than going through emit_abs_mem. Every UML float register is inside
// that reach; anything else materialises an address.

void drcbe_arm32::mov_freg_param(Assembler &a, int size, Vec const &dst, be_parameter const &param) const
{
	assert(param.is_memory());
	intptr_t const off = intptr_t(param.memory()) - intptr_t(&m_state);
	if ((off >= 0) && (off <= 1020) && !(off & 3))
	{
		if (size == 4)
			a.vldr_32(dst, ptr(REG_STATE, int32_t(off)));
		else
			a.vldr_64(dst, ptr(REG_STATE, int32_t(off)));
	}
	else
	{
		emit_mov_reg_imm(a, REG_ADDR, uint32_t(uintptr_t(param.memory())));
		if (size == 4)
			a.vldr_32(dst, ptr(REG_ADDR));
		else
			a.vldr_64(dst, ptr(REG_ADDR));
	}
}


void drcbe_arm32::mov_param_freg(Assembler &a, int size, be_parameter const &param, Vec const &src) const
{
	assert(param.is_memory());
	intptr_t const off = intptr_t(param.memory()) - intptr_t(&m_state);
	if ((off >= 0) && (off <= 1020) && !(off & 3))
	{
		if (size == 4)
			a.vstr_32(src, ptr(REG_STATE, int32_t(off)));
		else
			a.vstr_64(src, ptr(REG_STATE, int32_t(off)));
	}
	else
	{
		emit_mov_reg_imm(a, REG_ADDR, uint32_t(uintptr_t(param.memory())));
		if (size == 4)
			a.vstr_32(src, ptr(REG_ADDR));
		else
			a.vstr_64(src, ptr(REG_ADDR));
	}
}


void drcbe_arm32::op_fload(Assembler &a, const uml::instruction &inst)
{
	assert(inst.size() == 4 || inst.size() == 8);
	assert_no_condition(inst);
	assert_no_flags(inst);

	be_parameter dstp(*this, inst.param(0), PTYPE_MF);
	be_parameter basep(*this, inst.param(1), PTYPE_M);
	be_parameter indp(*this, inst.param(2), PTYPE_MRI);

	int const scale = (inst.size() == 4) ? 2 : 3;

	if (indp.is_immediate())
	{
		emit_mov_reg_imm(a, REG_ADDR, uint32_t(uintptr_t(basep.memory())) + (uint32_t(indp.immediate()) << scale));
	}
	else
	{
		mov_reg_param(a, r1, indp);
		emit_mov_reg_imm(a, REG_ADDR, uint32_t(uintptr_t(basep.memory())));
		a.add(REG_ADDR, REG_ADDR, r1, lsl(scale));
	}

	if (inst.size() == 4)
	{
		a.vldr_32(VS0, ptr(REG_ADDR));
		mov_param_freg(a, 4, dstp, VS0);
	}
	else
	{
		a.vldr_64(VD0, ptr(REG_ADDR));
		mov_param_freg(a, 8, dstp, VD0);
	}
}


void drcbe_arm32::op_fstore(Assembler &a, const uml::instruction &inst)
{
	assert(inst.size() == 4 || inst.size() == 8);
	assert_no_condition(inst);
	assert_no_flags(inst);

	be_parameter basep(*this, inst.param(0), PTYPE_M);
	be_parameter indp(*this, inst.param(1), PTYPE_MRI);
	be_parameter srcp(*this, inst.param(2), PTYPE_MF);

	int const scale = (inst.size() == 4) ? 2 : 3;

	if (inst.size() == 4)
		mov_freg_param(a, 4, VS0, srcp);
	else
		mov_freg_param(a, 8, VD0, srcp);

	if (indp.is_immediate())
	{
		emit_mov_reg_imm(a, REG_ADDR, uint32_t(uintptr_t(basep.memory())) + (uint32_t(indp.immediate()) << scale));
	}
	else
	{
		mov_reg_param(a, r1, indp);
		emit_mov_reg_imm(a, REG_ADDR, uint32_t(uintptr_t(basep.memory())));
		a.add(REG_ADDR, REG_ADDR, r1, lsl(scale));
	}

	if (inst.size() == 4)
		a.vstr_32(VS0, ptr(REG_ADDR));
	else
		a.vstr_64(VD0, ptr(REG_ADDR));
}


void drcbe_arm32::op_fread(Assembler &a, const uml::instruction &inst)
{
	assert(inst.size() == 4 || inst.size() == 8);
	assert_no_condition(inst);
	assert_no_flags(inst);

	be_parameter dstp(*this, inst.param(0), PTYPE_MF);
	be_parameter addrp(*this, inst.param(1), PTYPE_MRI);
	parameter const &spacesizep = inst.param(2);
	assert(spacesizep.is_size_space());

	auto const &accessors = m_memory_accessors[spacesizep.space()].resolved;
	resolved_member_function const &func = (inst.size() == 4) ? accessors.read_dword : accessors.read_qword;

	mov_reg_param(a, r1, addrp);
	emit_mov_reg_imm(a, r0, uint32_t(uintptr_t(func.obj)));
	emit_call_saving_lr(a, (void const *)func.func);

	// the value comes back in the integer return registers and has to be moved
	// across rather than reinterpreted in place
	emit_mov_reg_imm(a, REG_ADDR, uint32_t(uintptr_t(dstp.memory())));
	a.str(r0, ptr(REG_ADDR));
	if (inst.size() == 8)
		a.str(r1, ptr(REG_ADDR, 4));

	m_carry_state = carry_state::POISON;
}


void drcbe_arm32::op_fwrite(Assembler &a, const uml::instruction &inst)
{
	assert(inst.size() == 4 || inst.size() == 8);
	assert_no_condition(inst);
	assert_no_flags(inst);

	be_parameter addrp(*this, inst.param(0), PTYPE_MRI);
	be_parameter srcp(*this, inst.param(1), PTYPE_MF);
	parameter const &spacesizep = inst.param(2);
	assert(spacesizep.is_size_space());

	auto const &accessors = m_memory_accessors[spacesizep.space()].resolved;
	resolved_member_function const &func = (inst.size() == 4) ? accessors.write_dword : accessors.write_qword;

	emit_mov_reg_imm(a, REG_ADDR, uint32_t(uintptr_t(srcp.memory())));
	a.ldr(r2, ptr(REG_ADDR));
	if (inst.size() == 8)
		a.ldr(r3, ptr(REG_ADDR, 4));
	mov_reg_param(a, r1, addrp);
	emit_mov_reg_imm(a, r0, uint32_t(uintptr_t(func.obj)));
	emit_call_saving_lr(a, (void const *)func.func);

	m_carry_state = carry_state::POISON;
}


void drcbe_arm32::op_fmov(Assembler &a, const uml::instruction &inst)
{
	assert(inst.size() == 4 || inst.size() == 8);
	assert_any_condition(inst);
	assert_no_flags(inst);

	be_parameter dstp(*this, inst.param(0), PTYPE_MF);
	be_parameter srcp(*this, inst.param(1), PTYPE_MF);

	if (dstp == srcp)
		return;

	Label skip;
	emit_skip(a, inst.condition(), skip);

	// an integer move, not a VFP one: nothing is being computed, and going
	// through the FPU would risk normalising a signalling NaN
	emit_mov_reg_imm(a, REG_ADDR, uint32_t(uintptr_t(srcp.memory())));
	a.ldr(r0, ptr(REG_ADDR));
	if (inst.size() == 8)
		a.ldr(r1, ptr(REG_ADDR, 4));
	emit_mov_reg_imm(a, REG_ADDR, uint32_t(uintptr_t(dstp.memory())));
	a.str(r0, ptr(REG_ADDR));
	if (inst.size() == 8)
		a.str(r1, ptr(REG_ADDR, 4));

	if (inst.condition() != uml::COND_ALWAYS)
		a.bind(skip);
}


void drcbe_arm32::op_ftoint(Assembler &a, const uml::instruction &inst)
{
	assert(inst.size() == 4 || inst.size() == 8);
	assert_no_condition(inst);
	assert_no_flags(inst);

	be_parameter dstp(*this, inst.param(0), PTYPE_MR);
	be_parameter srcp(*this, inst.param(1), PTYPE_MF);
	parameter const &sizep = inst.param(2);
	assert(sizep.is_size());
	parameter const &roundp = inst.param(3);
	assert(roundp.is_rounding());

	// A 64-bit integer result has no VFP instruction on ARMv7-A at all --
	// VCVT converts to and from 32-bit integers only -- so that side is a call.
	if (sizep.size() == SIZE_QWORD)
	{
		emit_mov_reg_imm(a, r2, roundp.rounding());
		emit_mov_reg_imm(a, r1, uint32_t(uintptr_t(srcp.memory())));
		emit_mov_reg_imm(a, r0, uint32_t(uintptr_t(dstp.memory())));
		emit_call_saving_lr(a, (inst.size() == 4) ? (void const *)&arm32_stoi64 : (void const *)&arm32_dtoi64);
		m_carry_state = carry_state::POISON;
		return;
	}

	// VCVTR honours the FPSCR rounding mode and VCVT always truncates, and
	// a32 has no two-operand VCVT for the double case -- so every rounding
	// mode goes through VCVTR with the mode programmed around it. ROUND_DEFAULT
	// is the one that needs no programming, because it IS the current mode.
	bool const setmode = (roundp.rounding() != ROUND_DEFAULT);
	if (setmode)
	{
		emit_vmrs(a, REG_TMP);
		a.bic(REG_ADDR, REG_TMP, imm(3u << 22));
		uint32_t const armmode = (uint32_t(roundp.rounding()) - 1) & 3;
		if (armmode)
			a.orr(REG_ADDR, REG_ADDR, imm(armmode << 22));
		emit_vmsr(a, REG_ADDR);
	}

	if (inst.size() == 4)
	{
		mov_freg_param(a, 4, VS0, srcp);
		a.vcvtr_s32_f32(VS6, VS0);
	}
	else
	{
		mov_freg_param(a, 8, VD0, srcp);
		a.vcvtr_s32_f64(VS6, VD0);
	}

	if (setmode)
		emit_vmsr(a, REG_TMP);

	a.vmov(r0, VS6);
	mov_param_reg(a, dstp, r0);
}


void drcbe_arm32::op_ffrint(Assembler &a, const uml::instruction &inst)
{
	assert(inst.size() == 4 || inst.size() == 8);
	assert_no_condition(inst);
	assert_no_flags(inst);

	be_parameter dstp(*this, inst.param(0), PTYPE_MF);
	be_parameter srcp(*this, inst.param(1), PTYPE_MRI);
	parameter const &sizep = inst.param(2);
	assert(sizep.is_size());

	if (sizep.size() == SIZE_QWORD)
	{
		emit_param_ptr(a, r1, srcp, 0);
		emit_mov_reg_imm(a, r0, uint32_t(uintptr_t(dstp.memory())));
		emit_call_saving_lr(a, (inst.size() == 4) ? (void const *)&arm32_i64tos : (void const *)&arm32_i64tod);
		m_carry_state = carry_state::POISON;
		return;
	}

	mov_reg_param(a, r0, srcp);
	a.vmov(VS6, r0);
	if (inst.size() == 4)
	{
		a.vcvt_f32_s32(VS0, VS6);
		mov_param_freg(a, 4, dstp, VS0);
	}
	else
	{
		a.vcvt_f64_s32(VD0, VS6);
		mov_param_freg(a, 8, dstp, VD0);
	}
}


void drcbe_arm32::op_ffrflt(Assembler &a, const uml::instruction &inst)
{
	assert(inst.size() == 4 || inst.size() == 8);
	assert_no_condition(inst);
	assert_no_flags(inst);

	be_parameter dstp(*this, inst.param(0), PTYPE_MF);
	be_parameter srcp(*this, inst.param(1), PTYPE_MF);
	[[maybe_unused]] parameter const &sizep = inst.param(2);
	assert(sizep.is_size());

	// FFRFLT converts BETWEEN widths, so the source size is always the other
	// one; a size-matched form is an invalid instruction rather than a no-op.
	if (inst.size() == 8)
	{
		assert(sizep.size() == SIZE_DWORD);
		mov_freg_param(a, 4, VS0, srcp);
		a.vcvt_f64_f32(VD1, VS0);
		mov_param_freg(a, 8, dstp, VD1);
	}
	else
	{
		assert(sizep.size() == SIZE_QWORD);
		mov_freg_param(a, 8, VD0, srcp);
		a.vcvt_f32_f64(VS6, VD0);
		mov_param_freg(a, 4, dstp, VS6);
	}
}


void drcbe_arm32::op_frnds(Assembler &a, const uml::instruction &inst)
{
	assert(inst.size() == 8);
	assert_no_condition(inst);
	assert_no_flags(inst);

	be_parameter dstp(*this, inst.param(0), PTYPE_MF);
	be_parameter srcp(*this, inst.param(1), PTYPE_MF);

	mov_freg_param(a, 8, VD0, srcp);
	a.vcvt_f32_f64(VS6, VD0);
	a.vcvt_f64_f32(VD1, VS6);
	mov_param_freg(a, 8, dstp, VD1);
}


void drcbe_arm32::op_fbinary(Assembler &a, const uml::instruction &inst)
{
	assert(inst.size() == 4 || inst.size() == 8);
	assert_no_condition(inst);
	assert_no_flags(inst);

	be_parameter dstp(*this, inst.param(0), PTYPE_MF);
	be_parameter src1p(*this, inst.param(1), PTYPE_MF);
	be_parameter src2p(*this, inst.param(2), PTYPE_MF);

	if (inst.size() == 4)
	{
		mov_freg_param(a, 4, VS0, src1p);
		mov_freg_param(a, 4, VS1, src2p);
		switch (inst.opcode())
		{
		case OP_FADD: a.vadd_f32(VS2, VS0, VS1); break;
		case OP_FSUB: a.vsub_f32(VS2, VS0, VS1); break;
		case OP_FMUL: a.vmul_f32(VS2, VS0, VS1); break;
		default:      a.vdiv_f32(VS2, VS0, VS1); break;
		}
		mov_param_freg(a, 4, dstp, VS2);
	}
	else
	{
		mov_freg_param(a, 8, VD0, src1p);
		mov_freg_param(a, 8, VD1, src2p);
		switch (inst.opcode())
		{
		case OP_FADD: a.vadd_f64(VD2, VD0, VD1); break;
		case OP_FSUB: a.vsub_f64(VD2, VD0, VD1); break;
		case OP_FMUL: a.vmul_f64(VD2, VD0, VD1); break;
		default:      a.vdiv_f64(VD2, VD0, VD1); break;
		}
		mov_param_freg(a, 8, dstp, VD2);
	}
}


void drcbe_arm32::op_funary(Assembler &a, const uml::instruction &inst)
{
	assert(inst.size() == 4 || inst.size() == 8);
	assert_no_condition(inst);
	assert_no_flags(inst);

	be_parameter dstp(*this, inst.param(0), PTYPE_MF);
	be_parameter srcp(*this, inst.param(1), PTYPE_MF);
	opcode_t const op = inst.opcode();

	if (inst.size() == 4)
	{
		mov_freg_param(a, 4, VS0, srcp);
		switch (op)
		{
		case OP_FNEG:  a.vneg_f32(VS2, VS0); break;
		case OP_FABS:  a.vabs_f32(VS2, VS0); break;
		case OP_FSQRT: a.vsqrt_f32(VS2, VS0); break;
		default:
			// no reciprocal instruction with VFP accuracy: the estimate
			// instructions are NEON and are not exact, and UML wants the
			// exact value
			if (op == OP_FRSQRT)
				a.vsqrt_f32(VS0, VS0);
			emit_mov_reg_imm(a, r0, 0x3f800000);
			a.vmov(VS1, r0);
			a.vdiv_f32(VS2, VS1, VS0);
			break;
		}
		mov_param_freg(a, 4, dstp, VS2);
	}
	else
	{
		mov_freg_param(a, 8, VD0, srcp);
		switch (op)
		{
		case OP_FNEG:  a.vneg_f64(VD2, VD0); break;
		case OP_FABS:  a.vabs_f64(VD2, VD0); break;
		case OP_FSQRT: a.vsqrt_f64(VD2, VD0); break;
		default:
			if (op == OP_FRSQRT)
				a.vsqrt_f64(VD0, VD0);
			a.mov(r0, imm(0));
			emit_mov_reg_imm(a, r1, 0x3ff00000);
			a.vmov(VD1, r0, r1);
			a.vdiv_f64(VD2, VD1, VD0);
			break;
		}
		mov_param_freg(a, 8, dstp, VD2);
	}
}


void drcbe_arm32::op_fcmp(Assembler &a, const uml::instruction &inst)
{
	assert(inst.size() == 4 || inst.size() == 8);
	assert_no_condition(inst);
	assert_flags(inst, FLAG_U | FLAG_Z | FLAG_C);

	be_parameter src1p(*this, inst.param(0), PTYPE_MF);
	be_parameter src2p(*this, inst.param(1), PTYPE_MF);

	if (inst.size() == 4)
	{
		mov_freg_param(a, 4, VS0, src1p);
		mov_freg_param(a, 4, VS1, src2p);
		a.vcmp_f32(VS0, VS1);
	}
	else
	{
		mov_freg_param(a, 8, VD0, src1p);
		mov_freg_param(a, 8, VD1, src2p);
		a.vcmp_f64(VD0, VD1);
	}

	// VFP leaves its answer in FPSCR, in the same NZCV positions APSR uses but
	// with a different meaning: less-than is N, unordered is V, and C is set
	// for both greater-than and unordered. UML wants less-than in C, equal in
	// Z and unordered in U, so this is a re-encode rather than a copy.
	emit_vmrs(a, REG_TMP);
	a.ubfx(r0, REG_TMP, imm(31), imm(1));       // N -> UML C
	a.ubfx(r1, REG_TMP, imm(30), imm(1));       // Z
	a.orr(r0, r0, r1, lsl(FLAG_BIT_Z));
	a.ubfx(r1, REG_TMP, imm(28), imm(1));       // V -> UML U
	a.orr(r0, r0, r1, lsl(FLAG_BIT_U));
	set_flags(a, r0);
}


void drcbe_arm32::op_fcopyi(Assembler &a, const uml::instruction &inst)
{
	assert(inst.size() == 4 || inst.size() == 8);
	assert_no_condition(inst);
	assert_no_flags(inst);

	be_parameter dstp(*this, inst.param(0), PTYPE_MF);
	be_parameter srcp(*this, inst.param(1), PTYPE_MR);

	if (inst.size() == 4)
	{
		mov_reg_param(a, r0, srcp);
		emit_mov_reg_imm(a, REG_ADDR, uint32_t(uintptr_t(dstp.memory())));
		a.str(r0, ptr(REG_ADDR));
	}
	else
	{
		mov_reg_param_pair(a, r0, r1, srcp);
		emit_mov_reg_imm(a, REG_ADDR, uint32_t(uintptr_t(dstp.memory())));
		a.str(r0, ptr(REG_ADDR));
		a.str(r1, ptr(REG_ADDR, 4));
	}
}


void drcbe_arm32::op_icopyf(Assembler &a, const uml::instruction &inst)
{
	assert(inst.size() == 4 || inst.size() == 8);
	assert_no_condition(inst);
	assert_no_flags(inst);

	be_parameter dstp(*this, inst.param(0), PTYPE_MR);
	be_parameter srcp(*this, inst.param(1), PTYPE_MF);

	emit_mov_reg_imm(a, REG_ADDR, uint32_t(uintptr_t(srcp.memory())));
	if (inst.size() == 4)
	{
		a.ldr(r0, ptr(REG_ADDR));
		mov_param_reg(a, dstp, r0);
	}
	else
	{
		a.ldr(r0, ptr(REG_ADDR));
		a.ldr(r1, ptr(REG_ADDR, 4));
		mov_param_reg_pair(a, dstp, r0, r1);
	}
}

} // anonymous namespace


std::unique_ptr<drcbe_interface> make_drcbe_arm32(
		drcuml_state &drcuml,
		device_t &device,
		drc_cache &cache,
		uint32_t flags,
		int modes,
		int addrbits,
		int ignorebits)
{
	return std::unique_ptr<drcbe_interface>(
			new drcbe_arm32(drcuml, device, cache, flags, modes, addrbits, ignorebits));
}

} // namespace drc

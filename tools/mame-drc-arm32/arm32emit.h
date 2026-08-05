// license:BSD-3-Clause
// copyright-holders:MAMESTer port
/***************************************************************************

    arm32emit.h

    ARMv7-A (A32) instruction encoder for the MAME UML back-end.

****************************************************************************

    WHY THIS FILE EXISTS

    MAME's two native DRC back-ends both emit through asmjit, and asmjit has
    no AArch32 code generator. Its Arch enumeration lists kARM and kThumb,
    which makes this easy to get wrong, but 3rdparty/asmjit/asmjit/arm/ ships
    a64* and nothing else. So the ARM32 back-end has to carry its own encoder.

    SCOPE

    ARMv7-A, A32 encoding only -- no Thumb, no ARMv8, no ARMv5/v6 fallbacks.
    That is not a limitation to work around later, it is load-bearing:

      * movw/movt (ARMv7) put any 32-bit constant or absolute address in two
        instructions, so there is no literal pool. No pool means no pool
        placement, no pool drain in the middle of a generated sequence, and no
        PC-relative reach limit inside the code cache -- an entire class of
        problem the back-end above never has to model.
      * ubfx/sbfx/bfi (ARMv6T2) make UML's ROLAND/ROLINS bitfield work direct.
      * The target is a Cortex-A9. It is ARMv7-A and it will not change.

    Notably absent, and absent on purpose: SDIV/UDIV. Integer divide is
    ARMv7-R/M, or ARMv7-A *with* the idiv extension (Cortex-A15 and later).
    The A9 has neither, so there is nothing to encode and DIVU/DIVS lower to
    a call.

    CORRECTNESS

    Every encoding here is checked word-for-word against
    arm-linux-gnueabihf-as by tests/arm32emit/. A mis-encoded instruction is
    not a compile error -- it is a wrong answer inside a game, minutes in --
    so the encoder is written as a standalone header specifically so that a
    real assembler can be the oracle. Do not add an entry point here without
    adding its cases there.

***************************************************************************/
#ifndef MAME_CPU_ARM32EMIT_H
#define MAME_CPU_ARM32EMIT_H

#pragma once

#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>


namespace arm32 {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s32 = std::int32_t;


//**************************************************************************
//  CONDITION CODES
//**************************************************************************

enum condition : u32
{
	COND_EQ = 0,  COND_NE = 1,
	COND_CS = 2,  COND_CC = 3,   // aka HS / LO
	COND_MI = 4,  COND_PL = 5,
	COND_VS = 6,  COND_VC = 7,
	COND_HI = 8,  COND_LS = 9,
	COND_GE = 10, COND_LT = 11,
	COND_GT = 12, COND_LE = 13,
	COND_AL = 14
};

constexpr condition COND_HS = COND_CS;
constexpr condition COND_LO = COND_CC;

// invert a condition -- every A32 condition except AL has its complement as
// its LSB-flipped neighbour, which is why this is arithmetic and not a table
constexpr condition invert(condition c) { return condition(u32(c) ^ 1); }


//**************************************************************************
//  REGISTERS
//**************************************************************************

struct gpr
{
	u32 n;
	constexpr bool operator==(gpr const &o) const { return n == o.n; }
	constexpr bool operator!=(gpr const &o) const { return n != o.n; }
};

constexpr gpr r0{0},  r1{1},  r2{2},  r3{3},  r4{4},  r5{5},  r6{6},  r7{7};
constexpr gpr r8{8},  r9{9},  r10{10}, r11{11}, r12{12}, r13{13}, r14{14}, r15{15};
constexpr gpr sp = r13, lr = r14, pc = r15, ip = r12;

// VFP/NEON single- and double-precision registers. Held apart at the type
// level because the D/N/M bit of an encoding lands in a different place for
// each -- a single's register number is split high-4:low-1, a double's is
// low-4:high-1, and mixing them up produces a valid instruction on the wrong
// register rather than an error.
struct vreg_s { u32 n; };
struct vreg_d { u32 n; };

constexpr vreg_s s0{0}, s1{1}, s2{2}, s3{3}, s4{4}, s5{5}, s6{6}, s7{7};
constexpr vreg_s s8{8}, s9{9}, s10{10}, s11{11}, s12{12}, s13{13}, s14{14}, s15{15};
constexpr vreg_d d0{0}, d1{1}, d2{2}, d3{3}, d4{4}, d5{5}, d6{6}, d7{7};
constexpr vreg_d d8{8}, d9{9}, d10{10}, d11{11}, d12{12}, d13{13}, d14{14}, d15{15};


//**************************************************************************
//  SHIFTS AND OPERAND-2
//**************************************************************************

enum shift_type : u32 { SHIFT_LSL = 0, SHIFT_LSR = 1, SHIFT_ASR = 2, SHIFT_ROR = 3 };

// The second source of a data-processing instruction: either a rotated 8-bit
// immediate (I=1) or a shifted register (I=0). Constructed through the free
// functions below rather than directly.
struct operand2
{
	u32 enc;    // bits 11..0
	u32 i;      // bit 25
};

// Try to express `value` as A32's ror(imm8, 2*rot). Returns false when it does
// not fit, which is the normal case for arbitrary constants -- callers use
// assembler::mov32() into a scratch register instead.
inline bool encode_imm(u32 value, operand2 &out)
{
	for (u32 rot = 0; rot < 16; rot++)
	{
		u32 const shift = rot * 2;
		u32 const rotated = (value << shift) | (shift ? (value >> (32 - shift)) : 0);
		if (rotated <= 0xff)
		{
			out.enc = (rot << 8) | rotated;
			out.i = 1;
			return true;
		}
	}
	return false;
}

inline bool is_imm_encodable(u32 value) { operand2 o; return encode_imm(value, o); }

// asserting form, for call sites that have already checked or that use a
// constant known to fit
inline operand2 imm(u32 value)
{
	operand2 o;
	bool const ok = encode_imm(value, o);
	assert(ok); (void)ok;
	return o;
}

inline operand2 reg(gpr rm) { return operand2{ rm.n, 0 }; }

// register with an immediate shift. Note the two encoding quirks A32 inherits
// from having only five bits for the amount: LSR #32 and ASR #32 are encoded
// as amount 0 (there is no "shift by nothing" for those two), and ROR #0 is
// not ROR at all -- it is RRX, which is why rrx() is separate below.
inline operand2 reg(gpr rm, shift_type type, u32 amount)
{
	assert(amount <= 32);
	if ((type == SHIFT_LSR || type == SHIFT_ASR) && amount == 32)
		amount = 0;
	else if (type == SHIFT_LSL || type == SHIFT_ROR)
		assert(amount < 32);
	else
		assert(amount >= 1 && amount < 32);
	return operand2{ (amount << 7) | (u32(type) << 5) | rm.n, 0 };
}

inline operand2 reg(gpr rm, shift_type type, gpr rs)
{
	return operand2{ (rs.n << 8) | (u32(type) << 5) | (1 << 4) | rm.n, 0 };
}

// rotate right with extend: a 33-bit rotate through the carry flag, and the
// only way to get carry *into* a shift on ARM. UML's RORC lowers to this.
inline operand2 rrx(gpr rm) { return operand2{ (u32(SHIFT_ROR) << 5) | rm.n, 0 }; }


//**************************************************************************
//  MEMORY OPERANDS
//**************************************************************************

// Addressing mode for the load/store family. `imm` selects between the two
// A32 offset forms; the extra load/store family (halfword, signed, doubleword)
// accepts a narrower immediate range than the word/byte family, which is
// checked at emit time rather than here.
struct mem
{
	gpr base;
	s32 offset;     // immediate form
	gpr index;      // register form
	shift_type type;
	u32 amount;
	bool imm;
	bool preindex;
	bool writeback;
};

inline mem ptr(gpr base, s32 offset = 0)
{
	return mem{ base, offset, r0, SHIFT_LSL, 0, true, true, false };
}

inline mem ptr(gpr base, gpr index, shift_type type = SHIFT_LSL, u32 amount = 0)
{
	return mem{ base, 0, index, type, amount, false, true, false };
}

// post-indexed: the access uses [base], then base += offset
inline mem ptr_post(gpr base, s32 offset)
{
	return mem{ base, offset, r0, SHIFT_LSL, 0, true, false, false };
}

// pre-indexed with writeback: base += offset, then the access uses [base]
inline mem ptr_pre(gpr base, s32 offset)
{
	return mem{ base, offset, r0, SHIFT_LSL, 0, true, true, true };
}


//**************************************************************************
//  ASSEMBLER
//**************************************************************************

// Labels are indices into the assembler's own table. A label may be branched
// to before it is bound; the branch records a fixup and finalize() patches it.
struct label
{
	u32 id;
	static constexpr u32 INVALID = ~u32(0);
};

class assembler
{
public:
	// Emits at `base`, which is the address the code will actually execute
	// from -- MAME's drc_cache hands out final addresses, and this encoder
	// relies on that for absolute addressing via movw/movt. There is no
	// relocation step and no position-independent mode.
	assembler(u8 *base, u8 *limit) : m_base(base), m_cur(base), m_limit(limit) { }

	u8 *base() const { return m_base; }
	u8 *pc() const { return m_cur; }
	bool overflowed() const { return m_cur > m_limit; }
	size_t size() const { return size_t(m_cur - m_base); }

	void emit(u32 word)
	{
		if (m_cur + 4 <= m_limit)
			std::memcpy(m_cur, &word, 4);
		m_cur += 4;
	}

	//----------------------------------------------------------------------
	//  data processing
	//----------------------------------------------------------------------

	enum dp_op : u32
	{
		DP_AND = 0,  DP_EOR = 1,  DP_SUB = 2,  DP_RSB = 3,
		DP_ADD = 4,  DP_ADC = 5,  DP_SBC = 6,  DP_RSC = 7,
		DP_TST = 8,  DP_TEQ = 9,  DP_CMP = 10, DP_CMN = 11,
		DP_ORR = 12, DP_MOV = 13, DP_BIC = 14, DP_MVN = 15
	};

	void dp(dp_op op, bool s, gpr rd, gpr rn, operand2 o, condition c = COND_AL)
	{
		emit((u32(c) << 28) | (o.i << 25) | (u32(op) << 21) | (u32(s) << 20)
				| (rn.n << 16) | (rd.n << 12) | o.enc);
	}

#define ARM32_DP3(name, op) \
	void name(gpr rd, gpr rn, operand2 o, condition c = COND_AL) { dp(op, false, rd, rn, o, c); } \
	void name##s(gpr rd, gpr rn, operand2 o, condition c = COND_AL) { dp(op, true, rd, rn, o, c); }

	ARM32_DP3(and_, DP_AND)
	ARM32_DP3(eor,  DP_EOR)
	ARM32_DP3(sub,  DP_SUB)
	ARM32_DP3(rsb,  DP_RSB)
	ARM32_DP3(add,  DP_ADD)
	ARM32_DP3(adc,  DP_ADC)
	ARM32_DP3(sbc,  DP_SBC)
	ARM32_DP3(rsc,  DP_RSC)
	ARM32_DP3(orr,  DP_ORR)
	ARM32_DP3(bic,  DP_BIC)
#undef ARM32_DP3

	// single-source forms: Rn is unused and must be zero
	void mov(gpr rd, operand2 o, condition c = COND_AL)  { dp(DP_MOV, false, rd, r0, o, c); }
	void movs(gpr rd, operand2 o, condition c = COND_AL) { dp(DP_MOV, true,  rd, r0, o, c); }
	void mvn(gpr rd, operand2 o, condition c = COND_AL)  { dp(DP_MVN, false, rd, r0, o, c); }
	void mvns(gpr rd, operand2 o, condition c = COND_AL) { dp(DP_MVN, true,  rd, r0, o, c); }

	// comparison forms: S is implicit and Rd is unused and must be zero
	void tst(gpr rn, operand2 o, condition c = COND_AL) { dp(DP_TST, true, r0, rn, o, c); }
	void teq(gpr rn, operand2 o, condition c = COND_AL) { dp(DP_TEQ, true, r0, rn, o, c); }
	void cmp(gpr rn, operand2 o, condition c = COND_AL) { dp(DP_CMP, true, r0, rn, o, c); }
	void cmn(gpr rn, operand2 o, condition c = COND_AL) { dp(DP_CMN, true, r0, rn, o, c); }

	//----------------------------------------------------------------------
	//  constant materialisation
	//----------------------------------------------------------------------

	void movw(gpr rd, u32 imm16, condition c = COND_AL)
	{
		assert(imm16 <= 0xffff);
		emit((u32(c) << 28) | 0x03000000 | ((imm16 & 0xf000) << 4) | (rd.n << 12) | (imm16 & 0xfff));
	}

	void movt(gpr rd, u32 imm16, condition c = COND_AL)
	{
		assert(imm16 <= 0xffff);
		emit((u32(c) << 28) | 0x03400000 | ((imm16 & 0xf000) << 4) | (rd.n << 12) | (imm16 & 0xfff));
	}

	// any 32-bit constant, in one instruction where possible and two where
	// not. This is the whole reason the encoder needs no literal pool.
	void mov32(gpr rd, u32 value, condition c = COND_AL)
	{
		operand2 o;
		if (encode_imm(value, o))
			mov(rd, o, c);
		else if (encode_imm(~value, o))
			mvn(rd, o, c);
		else
		{
			movw(rd, value & 0xffff, c);
			if (value >> 16)
				movt(rd, value >> 16, c);
		}
	}

	void mov32(gpr rd, void const *ptr, condition c = COND_AL)
	{
		mov32(rd, u32(uintptr_t(ptr)), c);
	}

	//----------------------------------------------------------------------
	//  load/store -- word and byte
	//----------------------------------------------------------------------

private:
	void ls_wb(bool load, bool byte, gpr rt, mem const &m, condition c)
	{
		u32 word = (u32(c) << 28) | (1 << 26) | (u32(m.preindex) << 24)
				| (u32(byte) << 22) | (u32(m.writeback) << 21) | (u32(load) << 20)
				| (m.base.n << 16) | (rt.n << 12);
		if (m.imm)
		{
			u32 const mag = u32(m.offset < 0 ? -m.offset : m.offset);
			assert(mag <= 0xfff);
			word |= (u32(m.offset >= 0) << 23) | mag;
		}
		else
		{
			assert(m.amount < 32);
			word |= (1 << 25) | (1 << 23) | (m.amount << 7) | (u32(m.type) << 5) | m.index.n;
		}
		emit(word);
	}

	// halfword, signed and doubleword transfers use a different encoding with
	// a split 8-bit immediate -- ±255 rather than ±4095
	void ls_extra(bool load, u32 sh, gpr rt, mem const &m, condition c)
	{
		u32 word = (u32(c) << 28) | (u32(m.preindex) << 24) | (u32(m.writeback) << 21)
				| (u32(load) << 20) | (m.base.n << 16) | (rt.n << 12)
				| (1 << 7) | (sh << 5) | (1 << 4);
		if (m.imm)
		{
			u32 const mag = u32(m.offset < 0 ? -m.offset : m.offset);
			assert(mag <= 0xff);
			word |= (1 << 22) | (u32(m.offset >= 0) << 23) | ((mag & 0xf0) << 4) | (mag & 0x0f);
		}
		else
		{
			assert(m.amount == 0 && m.type == SHIFT_LSL);
			word |= (1 << 23) | m.index.n;
		}
		emit(word);
	}

public:
	void ldr(gpr rt, mem const &m, condition c = COND_AL)   { ls_wb(true,  false, rt, m, c); }
	void str(gpr rt, mem const &m, condition c = COND_AL)   { ls_wb(false, false, rt, m, c); }
	void ldrb(gpr rt, mem const &m, condition c = COND_AL)  { ls_wb(true,  true,  rt, m, c); }
	void strb(gpr rt, mem const &m, condition c = COND_AL)  { ls_wb(false, true,  rt, m, c); }

	void ldrh(gpr rt, mem const &m, condition c = COND_AL)  { ls_extra(true,  1, rt, m, c); }
	void strh(gpr rt, mem const &m, condition c = COND_AL)  { ls_extra(false, 1, rt, m, c); }
	void ldrsb(gpr rt, mem const &m, condition c = COND_AL) { ls_extra(true,  2, rt, m, c); }
	void ldrsh(gpr rt, mem const &m, condition c = COND_AL) { ls_extra(true,  3, rt, m, c); }

	// Rt must be even and the pair is Rt:Rt+1. Note the L bit is inverted
	// relative to every other load/store: for the doubleword pair, load is
	// encoded as a "store" with sh=2 and store as sh=3.
	void ldrd(gpr rt, mem const &m, condition c = COND_AL)
	{
		assert((rt.n & 1) == 0 && rt.n <= 12);
		ls_extra(false, 2, rt, m, c);
	}

	void strd(gpr rt, mem const &m, condition c = COND_AL)
	{
		assert((rt.n & 1) == 0 && rt.n <= 12);
		ls_extra(false, 3, rt, m, c);
	}

	//----------------------------------------------------------------------
	//  block transfer
	//----------------------------------------------------------------------

	// mask is a bitmap of registers, LSB = r0
	void push(u32 mask, condition c = COND_AL)
	{
		emit((u32(c) << 28) | 0x092d0000 | (mask & 0xffff));
	}

	void pop(u32 mask, condition c = COND_AL)
	{
		emit((u32(c) << 28) | 0x08bd0000 | (mask & 0xffff));
	}

	static constexpr u32 rmask() { return 0; }
	template <typename... T> static constexpr u32 rmask(gpr r, T... rest) { return (1u << r.n) | rmask(rest...); }

	//----------------------------------------------------------------------
	//  branches
	//----------------------------------------------------------------------

	label new_label()
	{
		m_labels.push_back(~u32(0));
		return label{ u32(m_labels.size() - 1) };
	}

	void bind(label l)
	{
		assert(l.id < m_labels.size());
		m_labels[l.id] = u32(m_cur - m_base);
	}

	void b(label l, condition c = COND_AL)   { branch_label(l, false, c); }
	void bl(label l, condition c = COND_AL)  { branch_label(l, true, c); }

	void b(u8 *target, condition c = COND_AL)  { branch_abs(target, false, c); }
	void bl(u8 *target, condition c = COND_AL) { branch_abs(target, true, c); }

	void bx(gpr rm, condition c = COND_AL)  { emit((u32(c) << 28) | 0x012fff10 | rm.n); }
	void blx(gpr rm, condition c = COND_AL) { emit((u32(c) << 28) | 0x012fff30 | rm.n); }

	// A call to an arbitrary C function. Deliberately does NOT use BL: the
	// code cache and libc are not guaranteed to be within BL's ±32 MB, and a
	// call that is nearly always in range is worse than one that never is.
	void call(void const *target, gpr scratch = ip, condition c = COND_AL)
	{
		mov32(scratch, target, c);
		blx(scratch, c);
	}

	//----------------------------------------------------------------------
	//  multiply
	//----------------------------------------------------------------------

	void mul(gpr rd, gpr rn, gpr rm, bool s = false, condition c = COND_AL)
	{
		emit((u32(c) << 28) | (u32(s) << 20) | (rd.n << 16) | (rm.n << 8) | 0x90 | rn.n);
	}

	void mla(gpr rd, gpr rn, gpr rm, gpr ra, bool s = false, condition c = COND_AL)
	{
		emit((u32(c) << 28) | 0x00200000 | (u32(s) << 20) | (rd.n << 16) | (ra.n << 12) | (rm.n << 8) | 0x90 | rn.n);
	}

private:
	void mull(u32 op, gpr rdlo, gpr rdhi, gpr rn, gpr rm, bool s, condition c)
	{
		emit((u32(c) << 28) | op | (u32(s) << 20) | (rdhi.n << 16) | (rdlo.n << 12) | (rm.n << 8) | 0x90 | rn.n);
	}

public:
	void umull(gpr rdlo, gpr rdhi, gpr rn, gpr rm, bool s = false, condition c = COND_AL) { mull(0x00800000, rdlo, rdhi, rn, rm, s, c); }
	void umlal(gpr rdlo, gpr rdhi, gpr rn, gpr rm, bool s = false, condition c = COND_AL) { mull(0x00a00000, rdlo, rdhi, rn, rm, s, c); }
	void smull(gpr rdlo, gpr rdhi, gpr rn, gpr rm, bool s = false, condition c = COND_AL) { mull(0x00c00000, rdlo, rdhi, rn, rm, s, c); }
	void smlal(gpr rdlo, gpr rdhi, gpr rn, gpr rm, bool s = false, condition c = COND_AL) { mull(0x00e00000, rdlo, rdhi, rn, rm, s, c); }

	//----------------------------------------------------------------------
	//  bit manipulation
	//----------------------------------------------------------------------

	void clz(gpr rd, gpr rm, condition c = COND_AL)   { emit((u32(c) << 28) | 0x016f0f10 | (rd.n << 12) | rm.n); }
	void rbit(gpr rd, gpr rm, condition c = COND_AL)  { emit((u32(c) << 28) | 0x06ff0f30 | (rd.n << 12) | rm.n); }
	void rev(gpr rd, gpr rm, condition c = COND_AL)   { emit((u32(c) << 28) | 0x06bf0f30 | (rd.n << 12) | rm.n); }
	void rev16(gpr rd, gpr rm, condition c = COND_AL) { emit((u32(c) << 28) | 0x06bf0fb0 | (rd.n << 12) | rm.n); }

	void ubfx(gpr rd, gpr rn, u32 lsb, u32 width, condition c = COND_AL)
	{
		assert(width >= 1 && lsb + width <= 32);
		emit((u32(c) << 28) | 0x07e00050 | ((width - 1) << 16) | (rd.n << 12) | (lsb << 7) | rn.n);
	}

	void sbfx(gpr rd, gpr rn, u32 lsb, u32 width, condition c = COND_AL)
	{
		assert(width >= 1 && lsb + width <= 32);
		emit((u32(c) << 28) | 0x07a00050 | ((width - 1) << 16) | (rd.n << 12) | (lsb << 7) | rn.n);
	}

	void bfi(gpr rd, gpr rn, u32 lsb, u32 width, condition c = COND_AL)
	{
		assert(width >= 1 && lsb + width <= 32);
		emit((u32(c) << 28) | 0x07c00010 | ((lsb + width - 1) << 16) | (rd.n << 12) | (lsb << 7) | rn.n);
	}

	void bfc(gpr rd, u32 lsb, u32 width, condition c = COND_AL)
	{
		assert(width >= 1 && lsb + width <= 32);
		emit((u32(c) << 28) | 0x07c0001f | ((lsb + width - 1) << 16) | (rd.n << 12) | (lsb << 7));
	}

	void uxtb(gpr rd, gpr rm, u32 rot = 0, condition c = COND_AL) { extend(0x06ef0070, rd, rm, rot, c); }
	void uxth(gpr rd, gpr rm, u32 rot = 0, condition c = COND_AL) { extend(0x06ff0070, rd, rm, rot, c); }
	void sxtb(gpr rd, gpr rm, u32 rot = 0, condition c = COND_AL) { extend(0x06af0070, rd, rm, rot, c); }
	void sxth(gpr rd, gpr rm, u32 rot = 0, condition c = COND_AL) { extend(0x06bf0070, rd, rm, rot, c); }

	//----------------------------------------------------------------------
	//  status register access
	//----------------------------------------------------------------------

	// The UML flag word is built from and restored to NZCV through these.
	void mrs(gpr rd, condition c = COND_AL)  { emit((u32(c) << 28) | 0x010f0000 | (rd.n << 12)); }
	void msr(gpr rm, condition c = COND_AL)  { emit((u32(c) << 28) | 0x0128f000 | rm.n); }

	//----------------------------------------------------------------------
	//  VFP
	//----------------------------------------------------------------------

	// A double's number splits as low-4 into Vd and high-1 into D; a single's
	// splits the other way round. Encoded here once so no call site has to
	// remember which is which.
	static u32 vd_d(vreg_d v) { return ((v.n & 0xf) << 12) | ((v.n >> 4) << 22); }
	static u32 vn_d(vreg_d v) { return ((v.n & 0xf) << 16) | ((v.n >> 4) << 7); }
	static u32 vm_d(vreg_d v) { return (v.n & 0xf) | ((v.n >> 4) << 5); }
	static u32 vd_s(vreg_s v) { return ((v.n >> 1) << 12) | ((v.n & 1) << 22); }
	static u32 vn_s(vreg_s v) { return ((v.n >> 1) << 16) | ((v.n & 1) << 7); }
	static u32 vm_s(vreg_s v) { return (v.n >> 1) | ((v.n & 1) << 5); }

#define ARM32_VFP3(name, base) \
	void name##_f64(vreg_d vd, vreg_d vn, vreg_d vm, condition c = COND_AL) \
	{ emit((u32(c) << 28) | (base) | 0x00000100 | vd_d(vd) | vn_d(vn) | vm_d(vm)); } \
	void name##_f32(vreg_s vd, vreg_s vn, vreg_s vm, condition c = COND_AL) \
	{ emit((u32(c) << 28) | (base) | vd_s(vd) | vn_s(vn) | vm_s(vm)); }

	ARM32_VFP3(vadd, 0x0e300a00)
	ARM32_VFP3(vsub, 0x0e300a40)
	ARM32_VFP3(vmul, 0x0e200a00)
	ARM32_VFP3(vdiv, 0x0e800a00)
#undef ARM32_VFP3

#define ARM32_VFP2(name, base) \
	void name##_f64(vreg_d vd, vreg_d vm, condition c = COND_AL) \
	{ emit((u32(c) << 28) | (base) | 0x00000100 | vd_d(vd) | vm_d(vm)); } \
	void name##_f32(vreg_s vd, vreg_s vm, condition c = COND_AL) \
	{ emit((u32(c) << 28) | (base) | vd_s(vd) | vm_s(vm)); }

	ARM32_VFP2(vmov,  0x0eb00a40)
	ARM32_VFP2(vabs,  0x0eb00ac0)
	ARM32_VFP2(vneg,  0x0eb10a40)
	ARM32_VFP2(vsqrt, 0x0eb10ac0)
	ARM32_VFP2(vcmp,  0x0eb40a40)
	ARM32_VFP2(vcmpe, 0x0eb40ac0)
#undef ARM32_VFP2

	// VFP comparisons land in FPSCR, not APSR; this is the move that makes
	// them branchable, and it must follow every vcmp before any conditional.
	void vmrs_apsr(condition c = COND_AL) { emit((u32(c) << 28) | 0x0ef1fa10); }

	void vcvt_f64_f32(vreg_d vd, vreg_s vm, condition c = COND_AL) { emit((u32(c) << 28) | 0x0eb70ac0 | vd_d(vd) | vm_s(vm)); }
	void vcvt_f32_f64(vreg_s vd, vreg_d vm, condition c = COND_AL) { emit((u32(c) << 28) | 0x0eb70bc0 | vd_s(vd) | vm_d(vm)); }

	// float to integer, round toward zero (the "r"-less VCVT form) -- which is
	// what UML's FTOINT with ROUND_TRUNC wants, and the only rounding mode
	// available without going through FPSCR
	void vcvt_s32_f64(vreg_s vd, vreg_d vm, condition c = COND_AL) { emit((u32(c) << 28) | 0x0ebd0bc0 | vd_s(vd) | vm_d(vm)); }
	void vcvt_u32_f64(vreg_s vd, vreg_d vm, condition c = COND_AL) { emit((u32(c) << 28) | 0x0ebc0bc0 | vd_s(vd) | vm_d(vm)); }
	void vcvt_s32_f32(vreg_s vd, vreg_s vm, condition c = COND_AL) { emit((u32(c) << 28) | 0x0ebd0ac0 | vd_s(vd) | vm_s(vm)); }
	void vcvt_u32_f32(vreg_s vd, vreg_s vm, condition c = COND_AL) { emit((u32(c) << 28) | 0x0ebc0ac0 | vd_s(vd) | vm_s(vm)); }

	void vcvt_f64_s32(vreg_d vd, vreg_s vm, condition c = COND_AL) { emit((u32(c) << 28) | 0x0eb80bc0 | vd_d(vd) | vm_s(vm)); }
	void vcvt_f64_u32(vreg_d vd, vreg_s vm, condition c = COND_AL) { emit((u32(c) << 28) | 0x0eb80b40 | vd_d(vd) | vm_s(vm)); }
	void vcvt_f32_s32(vreg_s vd, vreg_s vm, condition c = COND_AL) { emit((u32(c) << 28) | 0x0eb80ac0 | vd_s(vd) | vm_s(vm)); }
	void vcvt_f32_u32(vreg_s vd, vreg_s vm, condition c = COND_AL) { emit((u32(c) << 28) | 0x0eb80a40 | vd_s(vd) | vm_s(vm)); }

	// VFP load/store immediates are word-scaled and unsigned-with-a-sign-bit,
	// so the reach is ±1020 in steps of 4
	void vldr(vreg_d vd, gpr base, s32 offset = 0, condition c = COND_AL) { vls(true,  true,  vd_d(vd), base, offset, c); }
	void vstr(vreg_d vd, gpr base, s32 offset = 0, condition c = COND_AL) { vls(false, true,  vd_d(vd), base, offset, c); }
	void vldr(vreg_s vd, gpr base, s32 offset = 0, condition c = COND_AL) { vls(true,  false, vd_s(vd), base, offset, c); }
	void vstr(vreg_s vd, gpr base, s32 offset = 0, condition c = COND_AL) { vls(false, false, vd_s(vd), base, offset, c); }

	// GPR <-> VFP moves. The two-GPR form is how a UML 64-bit integer register
	// becomes a double and back, and it is the only transfer that does not go
	// through memory.
	void vmov_to_s(vreg_s vn, gpr rt, condition c = COND_AL)   { emit((u32(c) << 28) | 0x0e000a10 | vn_s(vn) | (rt.n << 12)); }
	void vmov_from_s(gpr rt, vreg_s vn, condition c = COND_AL) { emit((u32(c) << 28) | 0x0e100a10 | vn_s(vn) | (rt.n << 12)); }

	void vmov_to_d(vreg_d vm, gpr rt, gpr rt2, condition c = COND_AL)
	{ emit((u32(c) << 28) | 0x0c400b10 | (rt2.n << 16) | (rt.n << 12) | vm_d(vm)); }

	void vmov_from_d(gpr rt, gpr rt2, vreg_d vm, condition c = COND_AL)
	{ emit((u32(c) << 28) | 0x0c500b10 | (rt2.n << 16) | (rt.n << 12) | vm_d(vm)); }

	//----------------------------------------------------------------------
	//  fixups
	//----------------------------------------------------------------------

	// Must be called once every label is bound and before the buffer is
	// executed. Returns false if any branch was out of range or any label was
	// left unbound.
	bool finalize()
	{
		bool ok = true;
		for (fixup const &f : m_fixups)
		{
			if (m_labels[f.target] == ~u32(0)) { ok = false; continue; }
			if (!patch_branch(m_base + f.position, m_base + m_labels[f.target]))
				ok = false;
		}
		m_fixups.clear();
		return ok && !overflowed();
	}

private:
	struct fixup { u32 position; u32 target; };

	void extend(u32 op, gpr rd, gpr rm, u32 rot, condition c)
	{
		assert(rot == 0 || rot == 8 || rot == 16 || rot == 24);
		emit((u32(c) << 28) | op | (rd.n << 12) | ((rot / 8) << 10) | rm.n);
	}

	void vls(bool load, bool dbl, u32 vd, gpr base, s32 offset, condition c)
	{
		u32 const mag = u32(offset < 0 ? -offset : offset);
		assert((mag & 3) == 0 && mag <= 1020);
		emit((u32(c) << 28) | 0x0d000a00 | (u32(load) << 20) | (u32(offset >= 0) << 23)
				| (u32(dbl) << 8) | vd | (base.n << 16) | (mag >> 2));
	}

	void branch_label(label l, bool link, condition c)
	{
		assert(l.id < m_labels.size());
		m_fixups.push_back(fixup{ u32(m_cur - m_base), l.id });
		emit((u32(c) << 28) | (link ? 0x0b000000 : 0x0a000000));
	}

	void branch_abs(u8 *target, bool link, condition c)
	{
		u32 const word = (u32(c) << 28) | (link ? 0x0b000000 : 0x0a000000);
		u8 *const here = m_cur;
		emit(word);
		patch_branch(here, target);
	}

	// The A32 branch offset is relative to the instruction address plus eight
	// -- the ARM pipeline's PC, not the instruction's own address. Getting
	// this wrong by one instruction is the classic A32 encoding bug.
	static bool patch_branch(u8 *at, u8 *target)
	{
		ptrdiff_t const delta = target - (at + 8);
		if ((delta & 3) || delta < -33554432 || delta > 33554428)
			return false;
		u32 word;
		std::memcpy(&word, at, 4);
		word = (word & 0xff000000) | (u32(s32(delta) >> 2) & 0x00ffffff);
		std::memcpy(at, &word, 4);
		return true;
	}

	u8 *m_base;
	u8 *m_cur;
	u8 *m_limit;
	std::vector<u32> m_labels;
	std::vector<fixup> m_fixups;
};

} // namespace arm32

#endif // MAME_CPU_ARM32EMIT_H

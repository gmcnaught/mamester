// license:BSD-3-Clause
/***************************************************************************

    drc_diff.cpp

    Differential test of a native UML back-end against drcbe_c.

    Run the same UML block through the native back-end and through the
    interpreter, execute both, and diff the resulting machine state. That is
    what makes each opcode group in drcbearm32.cpp's REMAINING WORK an
    independently verifiable unit rather than an aspiration.

    ---------------
    Why it lives inside MAME
    ---------------

    A standalone harness does not work. drcuml_state's constructor reads
    device.machine().options() to choose a back-end, and device_t::machine()
    and running_machine::options() are header-inline, so they never appear as
    undefined symbols -- nm on drcuml.o shows no device_t reference at all,
    which makes linking against stubs look viable and it is not. It builds and
    then dereferences a fake device at runtime.

    The way in is that the libretro core loads with NULL content and MAME
    starts its ___empty driver, so a real running_machine and a real device_t
    exist with no romset, no hardware, and no MiSTer. tests/drc-diff/nogame.c
    is the proof of that; this file is what it was proved for.

    ---------------
    Getting two back-ends into one process
    ---------------

    Not by flipping drc_use_c(). That option is read once, in drcuml_state's
    constructor, and it selects the single back-end that state will own -- one
    drcuml_state can never hold both. The factories are exported, though, so
    the harness constructs both itself.

    Each back-end gets its OWN drc_cache. That is not tidiness: drc_hash_table,
    drc_map_variables, drc_label_list and the drcuml_machine_state the
    generated code operates on are all per-back-end members allocated out of
    the cache it was handed, so sharing one would have the two code streams
    writing over each other's register file.

    One drcuml_block is fed to both, which the tree permits: a back-end's
    generate() reads nothing from the block but invariant(), and uses it only
    as the channel for abort(). Neither the back-ends nor the drcbeut
    bookkeeping asserts inuse(), so the block does not have to be re-begun --
    and deliberately no block.end() is called, since end() would route
    generation through the state's own back-end, which is neither of the two
    under test.

    ---------------
    How a case reports
    ---------------

    UML has SAVE and RESTORE, which move the whole drcuml_machine_state to and
    from memory in one opcode. So a case is

        HANDLE h / RESTORE seed / <body> / SAVE out / EXIT imm

    and the diff is over every I register, every F register, exp, fmod and
    flags without a line of per-opcode readout plumbing. The cost is that SAVE
    and RESTORE are themselves lowered code: until a back-end has them, every
    case reports unimplemented. They are therefore the two opcodes worth
    lowering first, because they unlock the rest of the corpus.

    An opcode a back-end has not lowered yet raises emu_fatalerror by design.
    That is caught here per case and reported as UNIMPL rather than allowed to
    take down the run, so the harness is useful as a coverage report from the
    first day of lowering rather than only after the last.

***************************************************************************/

#include "emu.h"
#include "drc_diff.h"

#include "drcbec.h"
#include "drccache.h"
#include "drcuml.h"
#include "uml.h"

#include "emuopts.h"    // drc_rwx() -- emu.h forward-declares emu_options only

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>


// Mirrors drcuml.cpp's NATIVE_DRC chain. Kept as a copy rather than shared
// because that chain is a private macro of drcuml.cpp, and because the harness
// wants the native back-end BY NAME -- make_drcbe_native resolves to
// make_drcbe_c on a host with no native back-end, which would silently turn
// this into drcbe_c against itself and pass.
#if !defined(MAME_NOASM) && (defined(__x86_64__) || defined(_M_X64))
#include "drcbex64.h"
#define DIFF_NATIVE_NAME    "drcbe_x64"
#define DIFF_MAKE_NATIVE    drc::make_drcbe_x64
#elif !defined(MAME_NOASM) && (defined(__aarch64__) || defined(_M_ARM64))
#include "drcbearm64.h"
#define DIFF_NATIVE_NAME    "drcbe_arm64"
#define DIFF_MAKE_NATIVE    drc::make_drcbe_arm64
#elif !defined(MAME_NOASM) && defined(__arm__)
#include "drcbearm32.h"
#define DIFF_NATIVE_NAME    "drcbe_arm32"
#define DIFF_MAKE_NATIVE    drc::make_drcbe_arm32
#else
#define DIFF_NO_NATIVE      1
#define DIFF_NATIVE_NAME    "(none)"
#endif


namespace drc {

namespace {

using namespace uml;


//**************************************************************************
//  CONFIGURATION
//**************************************************************************

// Matches the SH cores' drcuml_state parameters, since the SH-2/SH-3 boards
// are the drivers this back-end exists to recover.
constexpr int MODES       = 1;
constexpr int ADDRBITS    = 32;
constexpr int IGNOREBITS  = 1;
constexpr u32 MAX_SEQ_LEN = 128;

// The same 32 MB the SH cores give their caches (sh.h, CACHE_SIZE), and it is
// the hash table that sets the floor, not the generated code.
//
// drc_hash_table splits ADDRBITS-IGNOREBITS in half, so 32/1 gives 15 L1 bits
// and 16 L2 bits, and the empty tables alone are (8 << 15) + (8 << 16) = 768 KB
// out of the MAIN cache before a single instruction is generated. Undersizing
// this does not report an error: drc_cache::alloc() returns nullptr, the
// constructor's fills are null-guarded, and the crash happens later somewhere
// else entirely. A 1 MB cache segfaulted here, which is what the --host
// calibration run was for.
constexpr size_t CACHE_BYTES = 32 * 1024 * 1024;

constexpr u32 MAXINST = 64;


//**************************************************************************
//  CRASHING IS A RESULT, TOO
//**************************************************************************

// A back-end being written will not only raise fatalerror on an opcode it does
// not have -- it will also emit code that is wrong and jump into it. That
// arrives as SIGSEGV/SIGBUS/SIGILL somewhere inside the code cache, with no
// stack worth reading (the generated frame is not walkable) and, without this,
// no indication of which of forty cases was running.
//
// So the harness tracks where it is in three strings and prints them from the
// handler. Everything below the handler is async-signal-safe: write(2) only,
// no printf, no allocation.

char const *volatile g_case    = "(none)";
char const *volatile g_backend = "(none)";
char const *volatile g_phase   = "(none)";

void write_str(char const *s) noexcept
{
	if (!s)
		s = "(null)";
	size_t n = 0;
	while (s[n])
		n++;
	ssize_t const ignored = ::write(STDERR_FILENO, s, n);
	(void)ignored;
}

extern "C" void diff_crash_handler(int sig) noexcept
{
	write_str("\nDRC-DIFF: CRASH ");
	switch (sig)
	{
	case SIGSEGV: write_str("SIGSEGV"); break;
	case SIGBUS:  write_str("SIGBUS");  break;
	case SIGILL:  write_str("SIGILL");  break;
	case SIGFPE:  write_str("SIGFPE");  break;
	default:      write_str("signal");  break;
	}
	write_str(" in case='");
	write_str(const_cast<char const *>(g_case));
	write_str("' backend=");
	write_str(const_cast<char const *>(g_backend));
	write_str(" phase=");
	write_str(const_cast<char const *>(g_phase));
	write_str("\n");

	// Not a return and not abort(): returning would re-fault at the same
	// instruction forever, and a core dump of generated code is not the
	// artefact anyone wants. 3 is distinct from 1 (a real diff) and 2 (no
	// native back-end).
	std::_Exit(3);
}

void install_crash_handler()
{
	std::signal(SIGSEGV, diff_crash_handler);
	std::signal(SIGBUS,  diff_crash_handler);
	std::signal(SIGILL,  diff_crash_handler);
	std::signal(SIGFPE,  diff_crash_handler);
}


//**************************************************************************
//  THE SEED STATE
//**************************************************************************

// Every case starts from this, so an opcode that fails to write its
// destination is caught rather than reading back whatever happened to be
// there. The values are deliberately awkward -- both halves of each 64-bit
// register distinct and non-zero, so a back-end that synthesises a 64-bit
// operation out of two 32-bit ones cannot pass by getting one half right and
// leaving the other untouched.
drcuml_machine_state make_seed()
{
	drcuml_machine_state s;
	std::memset(&s, 0, sizeof(s));

	for (int i = 0; i < REG_I_COUNT; i++)
		s.r[i].d = 0x0123456789abcdefULL ^ (u64(i + 1) * 0x1111111111111111ULL);
	for (int i = 0; i < REG_F_COUNT; i++)
		s.f[i].d = -1.5 * double(i + 1);

	s.exp = 0xdeadbeef;

	// Not ROUND_DEFAULT. drcbec masks fmod to two bits on the way in
	// (m_state.fmod = PARAM0 & 0x03), so ROUND_DEFAULT (4) does not survive a
	// round trip through it, and a native back-end that stores the value
	// as-written would differ over what is a UML grey area rather than a
	// lowering bug. The corpus stays inside the four modes both agree on.
	s.fmod  = ROUND_TRUNC;
	s.flags = 0;
	return s;
}


//**************************************************************************
//  WHAT IS ACTUALLY DEFINED AFTER A BLOCK RUNS
//**************************************************************************

// UML leaves state undefined in places, and a differential test that compares
// undefined state reports differences that are not bugs. The --host
// calibration run found all three of these, and every one of them would
// otherwise have been read as an ARM32 lowering bug:
//
//   * A 4-byte operation on a 64-bit register defines the LOW 32 BITS ONLY.
//     drcbe_c happens to zero the upper half and drcbe_x64 happens to preserve
//     it. Neither is wrong.
//   * FLAG_U is defined for floating point only. drcbe_x64 reconstructs flags
//     with lahf and maps x86's PARITY flag onto FLAG_U, so after any integer
//     op it holds parity while drcbe_c holds zero. Neither is wrong.
//   * Flags in general are undefined until an opcode defines them -- after
//     SETFMOD/GETFMOD alone, drcbe_x64's saved flags are whatever the host
//     happened to be carrying.
//
// So the mask is computed from the block itself: a register is compared to the
// width of the last thing that wrote it, and the flags are compared to the set
// the last flag-producing opcode defines. `is_param_out()` and
// `output_flags()` are public, so this is read off the IR rather than
// hand-maintained per case.
struct compare_masks
{
	u64 ireg[REG_I_COUNT];
	u64 freg[REG_F_COUNT];
	u8  flags;
};

compare_masks masks_for(std::vector<instruction> const &b)
{
	compare_masks m;

	// RESTORE writes every register in full from the seed, and it is the first
	// thing every case does, so everything starts fully defined.
	for (u64 &v : m.ireg)
		v = ~u64(0);
	for (u64 &v : m.freg)
		v = ~u64(0);

	// Flags are the exception: RESTORE sets them from the seed, but nothing
	// downstream is obliged to preserve them, so nothing is comparable until an
	// opcode defines it.
	m.flags = 0;

	for (instruction const &i : b)
	{
		// instruction::size() is not always the destination's width. FTOINT is
		// the fd*/fs* prefix -- the width of the FLOAT SOURCE -- while the
		// integer destination's width is the SIZE_ parameter, so fdtoint with
		// SIZE_DWORD is an 8-size instruction that defines 32 bits of an I
		// register. Reading size() there compares an undefined upper half.
		u64 intwidth = (i.size() == 8) ? ~u64(0) : 0xffffffffULL;
		if (i.opcode() == OP_FTOINT)
		{
			for (int p = 0; p < i.numparams(); p++)
			{
				if (i.param(p).is_size())
					intwidth = (i.param(p).size() == SIZE_QWORD) ? ~u64(0) : 0xffffffffULL;
			}
		}

		for (int p = 0; p < i.numparams(); p++)
		{
			if (!i.is_param_out(p))
				continue;

			parameter const &param = i.param(p);
			if (param.is_int_register())
				m.ireg[param.ireg() - REG_I0] = intwidth;
			else if (param.is_float_register())
				m.freg[param.freg() - REG_F0] = (i.size() == 8) ? ~u64(0) : 0xffffffffULL;
		}

		// RESTORE is excluded deliberately. It LOADS flags, which is not the
		// same as COMPUTING them: a back-end is free to keep UML flags in the
		// host's own flag register and rematerialise them only when an opcode
		// needs them, so restored flags need not survive intervening opcodes
		// that define none. drcbe_x64 does exactly that -- after
		// SETFMOD/GETFMOD alone its saved flags are host state, and calling
		// that a defect on this evidence would be wrong.
		if (i.output_flags() && (i.opcode() != OP_RESTORE))
			m.flags = i.output_flags();
	}

	return m;
}


//**************************************************************************
//  THE CORPUS
//**************************************************************************

// The flags an INTEGER opcode can define. FLAGS_ALL includes FLAG_U, which is
// floating-point only, so a GETFLGS over FLAGS_ALL in an integer context reads
// back whatever the host left in it.
constexpr u8 IFLAGS = FLAG_C | FLAG_V | FLAG_Z | FLAG_S;


// CALLC's target, and the one thing in the corpus that observes the world
// outside the machine state. The ARM lowering brackets a C call with an FPSCR
// save and restore -- C code is entitled to the host's rounding mode, not
// whatever SETFMOD last asked for -- so the case sets a rounding mode across
// the call and reads it back afterwards.
u32 g_callc_seen;

void diff_cfunc(void *param)
{
	g_callc_seen = u32(uintptr_t(param)) ^ 0xa5a5a5a5;
}

using builder = void (*)(std::vector<instruction> &);

// The control-flow cases need code handles, and a handle belongs to the
// drcuml_state the case is running under -- so the state is published for the
// duration of the build rather than threaded through every case's signature.
drcuml_state *g_uml = nullptr;

code_handle &hnd(char const *name)
{
	return *g_uml->handle_alloc(name);
}

struct testcase
{
	char const *group;  // the REMAINING WORK group this belongs to
	char const *name;
	builder     build;
};

// Shorthand: b.emplace_back() returns the new instruction, so a case body
// reads as one UML mnemonic per line.
instruction &ins(std::vector<instruction> &b)
{
	return b.emplace_back();
}

// Every instruction is asked for every flag its opcode defines, applied to the
// whole block after it is built.
//
// Normally drcuml_block::optimize() decides this, and it is skipped here on
// purpose: the point is to test the lowering of exactly the opcode written,
// not of whatever the optimiser rewrote it into. Something still has to set
// the field, though, and asking each opcode for its own output_flags() is both
// the most demanding choice -- flags are extra observable state on every
// case -- and the only one that cannot ask an opcode for a flag it does not
// define, which back-ends assert on.
void request_all_flags(std::vector<instruction> &b)
{
	for (instruction &i : b)
		i.set_flags(i.output_flags());
}

// TWO PLACES WHERE THERE IS NO ORACLE, both found by the calibration run and
// both worth stating rather than quietly avoiding:
//
//   * A 64-bit ROLC/RORC by more than 32. drcbec computes the carry's
//     contribution as `(flags & FLAG_C) << (shift - 1)` with `flags` a 32-bit
//     int, so a shift of 33 or more is undefined and comes out zero on x86;
//     drcbe_x64 gives the mathematically right answer. drcbearm32 agrees with
//     drcbe_x64 (its helper does the shift in 64 bits), so the corpus stops at
//     32 rather than asserting one of the two references is authoritative.
//   * BFXU/BFXS with a width equal to the register width. drcbec returns the
//     whole value; drcbe_x64 returns zero. UML front-ends emit MOV for that,
//     so neither behaviour is exercised in anger.
//
// Where the two references disagree, a differential test has nothing to say.

testcase const CORPUS[] =
{
// ---- the empty case: proves the entry/exit/nocode stub contract alone -----
{ "control", "empty", [] (std::vector<instruction> &b) { } },

{ "control", "nop", [] (std::vector<instruction> &b) {
	ins(b).nop();
} },

// ---- MOV -----------------------------------------------------------------
{ "mov", "mov.reg.imm", [] (std::vector<instruction> &b) {
	ins(b).mov(I0, 0x12345678);
	ins(b).mov(I1, 0);
	ins(b).mov(I2, 0xffffffff);
} },

{ "mov", "mov.reg.reg", [] (std::vector<instruction> &b) {
	ins(b).mov(I0, I5);
	ins(b).mov(I1, I0);
} },

{ "mov", "dmov.reg.imm", [] (std::vector<instruction> &b) {
	ins(b).dmov(I0, 0x0123456789abcdefULL);
	ins(b).dmov(I1, 0);
	ins(b).dmov(I2, 0xffffffffffffffffULL);
} },

{ "mov", "dmov.reg.reg", [] (std::vector<instruction> &b) {
	ins(b).dmov(I0, I7);
} },

// A 32-bit MOV into a 64-bit register. The upper half is a DON'T CARE, not a
// zero: drcbe_c zeroes it, drcbe_x64 preserves it, and both are conforming.
// masks_for() is what keeps this case honest.
{ "mov", "mov.32into64", [] (std::vector<instruction> &b) {
	ins(b).mov(I0, 0xffffffff);
	ins(b).mov(I1, I2);
} },

{ "mov", "mov.cond", [] (std::vector<instruction> &b) {
	ins(b).cmp(I0, I0);
	ins(b).mov(COND_Z,  I1, 0x11111111);
	ins(b).mov(COND_NZ, I2, 0x22222222);
} },

// ---- SEXT ----------------------------------------------------------------
{ "sext", "sext", [] (std::vector<instruction> &b) {
	ins(b).mov(I9, 0x000000ff);
	ins(b).sext(I0, I9, SIZE_BYTE);
	ins(b).sext(I1, I9, SIZE_WORD);
	ins(b).mov(I9, 0x0000ffff);
	ins(b).sext(I2, I9, SIZE_WORD);
} },

{ "sext", "dsext", [] (std::vector<instruction> &b) {
	ins(b).mov(I9, 0x80000000);
	ins(b).dsext(I0, I9, SIZE_DWORD);
	ins(b).dsext(I1, I9, SIZE_BYTE);
	ins(b).dsext(I2, I9, SIZE_WORD);
} },

// ---- ROLAND / ROLINS -----------------------------------------------------
{ "rol", "roland", [] (std::vector<instruction> &b) {
	ins(b).mov(I9, 0x89abcdef);
	ins(b).roland(I0, I9, 0,  0xffffffff);
	ins(b).roland(I1, I9, 8,  0x0000ffff);
	ins(b).roland(I2, I9, 31, 0xf0f0f0f0);
} },

{ "rol", "rolins", [] (std::vector<instruction> &b) {
	ins(b).mov(I9, 0x89abcdef);
	ins(b).rolins(I0, I9, 4, 0x0000ff00);
	ins(b).rolins(I1, I9, 0, 0xffffffff);
} },

// ---- the integer ALU -----------------------------------------------------
{ "alu", "add", [] (std::vector<instruction> &b) {
	ins(b).mov(I8, 0x7fffffff);
	ins(b).mov(I9, 1);
	ins(b).add(I0, I8, I9);          // signed overflow
	ins(b).add(I1, I8, 0);           // no flags set
	ins(b).mov(I8, 0xffffffff);
	ins(b).add(I2, I8, I9);          // carry out, zero result
} },

{ "alu", "dadd", [] (std::vector<instruction> &b) {
	ins(b).dmov(I8, 0xffffffffffffffffULL);
	ins(b).dmov(I9, 1);
	ins(b).dadd(I0, I8, I9);
	ins(b).dmov(I8, 0x00000000ffffffffULL);
	ins(b).dadd(I1, I8, I9);         // carry across the 32-bit seam
} },

{ "alu", "addc", [] (std::vector<instruction> &b) {
	ins(b).mov(I8, 0xffffffff);
	ins(b).mov(I9, 1);
	ins(b).add(I0, I8, I9);          // sets C
	ins(b).addc(I1, I9, I9);         // consumes it
} },

{ "alu", "sub", [] (std::vector<instruction> &b) {
	ins(b).mov(I8, 0);
	ins(b).mov(I9, 1);
	ins(b).sub(I0, I8, I9);          // borrow
	ins(b).sub(I1, I9, I9);          // zero
	ins(b).mov(I8, 0x80000000);
	ins(b).sub(I2, I8, I9);          // signed overflow
} },

{ "alu", "subb", [] (std::vector<instruction> &b) {
	ins(b).mov(I8, 0);
	ins(b).mov(I9, 1);
	ins(b).sub(I0, I8, I9);
	ins(b).subb(I1, I9, I9);
} },

{ "alu", "cmp", [] (std::vector<instruction> &b) {
	ins(b).mov(I8, 5);
	ins(b).mov(I9, 7);
	ins(b).cmp(I8, I9);
	ins(b).set(COND_B,  I0);
	ins(b).set(COND_L,  I1);
	ins(b).set(COND_E,  I2);
	ins(b).cmp(I8, I8);
	ins(b).set(COND_E,  I3);
} },

{ "alu", "dcmp", [] (std::vector<instruction> &b) {
	ins(b).dmov(I8, 0x0000000100000000ULL);
	ins(b).dmov(I9, 0x00000000ffffffffULL);
	ins(b).dcmp(I8, I9);
	ins(b).set(COND_A, I0);
	ins(b).set(COND_B, I1);
} },

{ "alu", "mulu", [] (std::vector<instruction> &b) {
	ins(b).mov(I8, 0x00010001);
	ins(b).mov(I9, 0x00010001);
	ins(b).mulu(I0, I1, I8, I9);
	ins(b).mov(I8, 0xffffffff);
	ins(b).mulu(I2, I3, I8, I8);
} },

{ "alu", "muls", [] (std::vector<instruction> &b) {
	ins(b).mov(I8, 0xffffffff);                  // -1
	ins(b).mov(I9, 2);
	ins(b).muls(I0, I1, I8, I9);
} },

// The A9 has no integer divide, so DIVU/DIVS must lower to a call. That is
// exactly the kind of thing that is easy to get subtly wrong.
{ "alu", "divu", [] (std::vector<instruction> &b) {
	ins(b).mov(I8, 100);
	ins(b).mov(I9, 7);
	ins(b).divu(I0, I1, I8, I9);
} },

{ "alu", "divs", [] (std::vector<instruction> &b) {
	ins(b).mov(I8, 0xffffff9c);                  // -100
	ins(b).mov(I9, 7);
	ins(b).divs(I0, I1, I8, I9);
} },

{ "alu", "logic", [] (std::vector<instruction> &b) {
	ins(b).mov(I8, 0xf0f0f0f0);
	ins(b).mov(I9, 0x0ff00ff0);
	ins(b)._and(I0, I8, I9);
	ins(b)._or (I1, I8, I9);
	ins(b)._xor(I2, I8, I9);
	ins(b)._and(I3, I8, 0);    // zero flag
} },

{ "alu", "dlogic", [] (std::vector<instruction> &b) {
	ins(b).dmov(I8, 0xf0f0f0f0f0f0f0f0ULL);
	ins(b).dmov(I9, 0x0ff00ff00ff00ff0ULL);
	ins(b).dand(I0, I8, I9);
	ins(b).dor (I1, I8, I9);
	ins(b).dxor(I2, I8, I9);
} },

{ "alu", "test", [] (std::vector<instruction> &b) {
	ins(b).mov(I8, 0xf0f0f0f0);
	ins(b).test(I8, 0x0f0f0f0f);
	ins(b).set(COND_Z, I0);
	ins(b).test(I8, 0x80000000);
	ins(b).set(COND_S, I1);
} },

{ "alu", "lzcnt.tzcnt.bswap", [] (std::vector<instruction> &b) {
	ins(b).mov(I8, 0x00010000);
	ins(b).lzcnt(I0, I8);
	ins(b).tzcnt(I1, I8);
	ins(b).bswap(I2, I8);
	ins(b).mov(I8, 0);
	ins(b).lzcnt(I3, I8);      // the all-zero edge
	ins(b).tzcnt(I4, I8);
} },

{ "alu", "dlzcnt.dtzcnt.dbswap", [] (std::vector<instruction> &b) {
	ins(b).dmov(I8, 0x0000000100000000ULL);
	ins(b).dlzcnt(I0, I8);
	ins(b).dtzcnt(I1, I8);
	ins(b).dbswap(I2, I8);
} },

// ---- shifts --------------------------------------------------------------
//
// Shift by 0 and shift by the full width are the two amounts that break
// hosts, and on ARM they are the same encoding problem that
// tests/a32-asmjit/ already caught once in the encoder.
{ "shift", "shl.shr.sar", [] (std::vector<instruction> &b) {
	ins(b).mov(I8, 0x89abcdef);
	ins(b).shl(I0, I8, 1);
	ins(b).shr(I1, I8, 1);
	ins(b).sar(I2, I8, 1);
	ins(b).shl(I3, I8, 0);
	ins(b).shr(I4, I8, 31);
	ins(b).sar(I5, I8, 31);
} },

{ "shift", "shift.by.reg", [] (std::vector<instruction> &b) {
	ins(b).mov(I8, 0x89abcdef);
	ins(b).mov(I9, 12);
	ins(b).shl(I0, I8, I9);
	ins(b).shr(I1, I8, I9);
	ins(b).sar(I2, I8, I9);
} },

{ "shift", "rol.ror", [] (std::vector<instruction> &b) {
	ins(b).mov(I8, 0x89abcdef);
	ins(b).rol(I0, I8, 4);
	ins(b).ror(I1, I8, 4);
} },

{ "shift", "rolc.rorc", [] (std::vector<instruction> &b) {
	ins(b).mov(I8, 0xffffffff);
	ins(b).mov(I9, 1);
	ins(b).add(I7, I8, I9);          // sets C
	ins(b).rolc(I0, I8, 1);
	ins(b).rorc(I1, I8, 1);
} },

// 64-bit shifts on a 32-bit host are synthesised, and the amounts either side
// of the seam are where that synthesis fails.
{ "shift", "dshift", [] (std::vector<instruction> &b) {
	ins(b).dmov(I8, 0x0123456789abcdefULL);
	ins(b).dshl(I0, I8, 1);
	ins(b).dshl(I1, I8, 31);
	ins(b).dshl(I2, I8, 32);
	ins(b).dshl(I3, I8, 33);
	ins(b).dshr(I4, I8, 32);
	ins(b).dsar(I5, I8, 32);
	ins(b).dshl(I6, I8, 0);
} },

// ---- LOAD / STORE --------------------------------------------------------
//
// Host memory, not an address space: the harness runs on a machine started
// with no content, whose root device has no memory interface, so m_space is
// empty and READ/WRITE have nothing to talk to. LOAD/STORE is the part of the
// memory lowering that can be tested here, and READ/WRITE needs a driver.
{ "loadstore", "load", [] (std::vector<instruction> &b) {
	static u32 const src[4] = { 0x11223344, 0x55667788, 0x99aabbcc, 0xddeeff00 };
	ins(b).mov(I9, 1);
	ins(b).load(I0, src, 0,  SIZE_DWORD, SCALE_x4);
	ins(b).load(I1, src, I9, SIZE_DWORD, SCALE_x4);
	ins(b).load(I2, src, 0,  SIZE_BYTE,  SCALE_x1);
	ins(b).load(I3, src, 0,  SIZE_WORD,  SCALE_x2);
	ins(b).loads(I4, src, 0, SIZE_BYTE,  SCALE_x1);
} },

{ "loadstore", "store", [] (std::vector<instruction> &b) {
	static u32 dst[4];
	ins(b).mov(I8, 0xa5a5a5a5);
	ins(b).mov(I9, 1);
	ins(b).store(dst, 0,  I8, SIZE_DWORD, SCALE_x4);
	ins(b).store(dst, I9, I8, SIZE_DWORD, SCALE_x4);
	ins(b).load(I0, dst, 0,  SIZE_DWORD, SCALE_x4);
	ins(b).load(I1, dst, I9, SIZE_DWORD, SCALE_x4);
} },

// ---- flag ops ------------------------------------------------------------
{ "flags", "getflgs", [] (std::vector<instruction> &b) {
	ins(b).mov(I8, 0xffffffff);
	ins(b).mov(I9, 1);
	ins(b).add(I7, I8, I9);
	// Not FLAGS_ALL: that includes FLAG_U, which is defined for floating point
	// only, so asking for it here reads back drcbe_x64's x86 parity bit.
	ins(b).getflgs(I0, IFLAGS);
	ins(b).getflgs(I1, FLAG_C);
	ins(b).getflgs(I2, FLAG_Z);
} },

{ "flags", "setflgs", [] (std::vector<instruction> &b) {
	ins(b).setflgs(FLAG_C | FLAG_Z);
	ins(b).getflgs(I0, IFLAGS);
	ins(b).setflgs(0);
	ins(b).getflgs(I1, IFLAGS);
} },

{ "flags", "carry", [] (std::vector<instruction> &b) {
	ins(b).mov(I8, 0x00000100);
	ins(b).carry(I8, 8);                         // bit 8 is set -> C
	ins(b).getflgs(I0, FLAG_C);
	ins(b).carry(I8, 7);                         // bit 7 is clear -> no C
	ins(b).getflgs(I1, FLAG_C);
} },

{ "flags", "set", [] (std::vector<instruction> &b) {
	ins(b).mov(I8, 1);
	ins(b).mov(I9, 2);
	ins(b).cmp(I8, I9);
	ins(b).set(COND_Z,  I0);
	ins(b).set(COND_NZ, I1);
	ins(b).set(COND_S,  I2);
	ins(b).set(COND_B,  I3);
	ins(b).set(COND_A,  I4);
	ins(b).set(COND_L,  I5);
	ins(b).set(COND_G,  I6);
} },

// ---- control flow --------------------------------------------------------
{ "control", "jmp.label", [] (std::vector<instruction> &b) {
	ins(b).mov(I0, 0x11111111);
	ins(b).jmp(1);
	ins(b).mov(I0, 0x22222222);                  // skipped
	ins(b).label(1);
	ins(b).mov(I1, 0x33333333);
} },

{ "control", "jmp.cond", [] (std::vector<instruction> &b) {
	ins(b).mov(I8, 1);
	ins(b).mov(I9, 1);
	ins(b).cmp(I8, I9);
	ins(b).jmp(COND_NZ, 1);
	ins(b).mov(I0, 0x44444444);                  // taken
	ins(b).label(1);
	ins(b).mov(I1, 0x55555555);
} },

// ---- floating point ------------------------------------------------------
{ "float", "fmov", [] (std::vector<instruction> &b) {
	ins(b).fdmov(F0, F3);
	ins(b).fsmov(F1, F4);
} },

{ "float", "fdarith", [] (std::vector<instruction> &b) {
	ins(b).fdadd(F0, F1, F2);
	ins(b).fdsub(F1, F1, F2);
	ins(b).fdmul(F2, F3, F4);
	ins(b).fddiv(F3, F4, F5);
	ins(b).fdneg(F4, F5);
	ins(b).fdabs(F5, F6);
} },

{ "float", "fsarith", [] (std::vector<instruction> &b) {
	ins(b).fssub(F1, F1, F2);
	ins(b).fsmul(F2, F3, F4);
	ins(b).fsneg(F4, F5);
} },

{ "float", "fdcmp", [] (std::vector<instruction> &b) {
	ins(b).fdcmp(F1, F2);
	ins(b).set(COND_B, I0);
	ins(b).set(COND_E, I1);
	ins(b).fdcmp(F1, F1);
	ins(b).set(COND_E, I2);
} },

{ "float", "fdtoint", [] (std::vector<instruction> &b) {
	ins(b).fdtoint(I0, F1, SIZE_DWORD, ROUND_TRUNC);
	ins(b).fdtoint(I1, F1, SIZE_DWORD, ROUND_ROUND);
	ins(b).fdtoint(I2, F1, SIZE_QWORD, ROUND_TRUNC);
} },

{ "float", "ffrint.ffrflt", [] (std::vector<instruction> &b) {
	ins(b).mov(I9, 0xfffffff0);
	ins(b).fdfrint(F0, I9, SIZE_DWORD);
	// FFRFLT converts BETWEEN float widths, so a 64-bit destination takes a
	// 32-bit source. Size-matched is not a no-op, it is an invalid opcode, and
	// drcbe_c refusing it is what flagged this.
	ins(b).fdfrflt(F1, F2, SIZE_DWORD);
	ins(b).fsfrint(F2, I9, SIZE_DWORD);
} },

{ "float", "setfmod.getfmod", [] (std::vector<instruction> &b) {
	ins(b).setfmod(ROUND_TRUNC);
	ins(b).getfmod(I0);
	ins(b).setfmod(ROUND_CEIL);
	ins(b).getfmod(I1);
	ins(b).setfmod(ROUND_FLOOR);
	ins(b).getfmod(I2);
} },

// ---- the second wave -------------------------------------------------------
//
// Everything above was written before any of the ARM32 lowering existed, and
// it passed on the first run once the lowering did. That is a suspicious
// result, not a happy one: a corpus that passes immediately is more likely to
// be missing the hard cases than to have found a flawless back-end. These are
// the opcodes and widths it was NOT covering -- the ones a back-end is most
// likely to get wrong precisely because nothing was asking.

{ "bfx", "bfxu.bfxs", [] (std::vector<instruction> &b) {
	ins(b).mov(I9, 0x89abcdef);
	ins(b).bfxu(I0, I9, 4, 8);
	ins(b).bfxs(I1, I9, 4, 8);
	// NOT width 32: drcbe_c and drcbe_x64 disagree there -- see the note above
	// the corpus -- so there is no oracle for a full-width extract.
	ins(b).bfxu(I2, I9, 16, 16);
	ins(b).bfxs(I3, I9, 28, 4);
	ins(b).bfxu(I4, I9, 24, 8);
} },

{ "bfx", "dbfx", [] (std::vector<instruction> &b) {
	ins(b).dmov(I9, 0x0123456789abcdefULL);
	ins(b).dbfxu(I0, I9, 8, 16);
	ins(b).dbfxs(I1, I9, 60, 4);
	ins(b).dbfxu(I2, I9, 16, 32);   // not width 64: no oracle, as above
} },

// The narrow multiplies keep the low half but take overflow from the wide
// product, which is the part a back-end forgets.
{ "alu", "mululw.mulslw", [] (std::vector<instruction> &b) {
	ins(b).mov(I8, 0x00010001);
	ins(b).mov(I9, 0x00010001);
	ins(b).mululw(I0, I8, I9);       // fits
	ins(b).mov(I8, 0xffffffff);
	ins(b).mululw(I1, I8, I8);       // overflows
	ins(b).mov(I8, 0xffffffff);      // -1
	ins(b).mov(I9, 2);
	ins(b).mulslw(I2, I8, I9);
} },

{ "alu", "dmul", [] (std::vector<instruction> &b) {
	ins(b).dmov(I8, 0x0000000100000001ULL);
	ins(b).dmov(I9, 0x0000000100000001ULL);
	ins(b).dmulu(I0, I1, I8, I9);
	ins(b).dmov(I8, 0xffffffffffffffffULL);
	ins(b).dmulu(I2, I3, I8, I8);    // the full 128-bit product
	ins(b).dmuls(I4, I5, I8, I8);    // -1 * -1
	ins(b).dmululw(I6, I8, I8);
} },

{ "alu", "ddiv", [] (std::vector<instruction> &b) {
	ins(b).dmov(I8, 1000000);
	ins(b).dmov(I9, 7);
	ins(b).ddivu(I0, I1, I8, I9);
	ins(b).dmov(I8, 0xfffffffffffffff0ULL);
	ins(b).ddivs(I2, I3, I8, I9);
} },

// Divide by zero writes neither destination and reports overflow. A back-end
// that computes into the destination first gets the flags right and the
// registers wrong.
{ "alu", "div.by.zero", [] (std::vector<instruction> &b) {
	ins(b).mov(I0, 0x11111111);
	ins(b).mov(I1, 0x22222222);
	ins(b).mov(I8, 100);
	ins(b).mov(I9, 0);
	ins(b).divu(I0, I1, I8, I9);
	ins(b).divs(I0, I1, I8, I9);
} },

{ "alu", "daddc.dsubb", [] (std::vector<instruction> &b) {
	ins(b).dmov(I8, 0xffffffffffffffffULL);
	ins(b).dmov(I9, 1);
	ins(b).dadd(I0, I8, I9);         // carry out of the top
	ins(b).daddc(I1, I9, I9);        // consumes it
	ins(b).dmov(I8, 0);
	ins(b).dsub(I2, I8, I9);         // borrow
	ins(b).dsubb(I3, I9, I9);        // consumes it
} },

{ "alu", "dtest.dcarry", [] (std::vector<instruction> &b) {
	ins(b).dmov(I8, 0xf0f0f0f000000000ULL);
	ins(b).dtest(I8, 0x0f0f0f0f00000000ULL);
	ins(b).dset(COND_Z, I0);
	ins(b).dcarry(I8, 60);           // a bit in the high half
	ins(b).getflgs(I1, FLAG_C);
	ins(b).dcarry(I8, 3);            // and one in the low half
	ins(b).getflgs(I2, FLAG_C);
} },

{ "shift", "dshift.by.reg", [] (std::vector<instruction> &b) {
	ins(b).dmov(I8, 0x0123456789abcdefULL);
	ins(b).mov(I9, 40);
	ins(b).dshl(I0, I8, I9);
	ins(b).dshr(I1, I8, I9);
	ins(b).dsar(I2, I8, I9);
	ins(b).drol(I3, I8, I9);
	ins(b).dror(I4, I8, I9);
} },

{ "shift", "drolc.drorc", [] (std::vector<instruction> &b) {
	ins(b).dmov(I8, 0xffffffffffffffffULL);
	ins(b).dmov(I9, 1);
	ins(b).dadd(I7, I8, I9);         // sets C
	ins(b).drolc(I0, I8, 1);
	ins(b).drorc(I1, I8, 1);
	ins(b).drolc(I2, I8, 32);        // across the 32-bit seam, but see the
	                                 // note above the corpus about shift > 32
} },

// A rotate through carry by a register amount is the one shift form the ARM
// lowering hands to a helper in both widths, so it needs its own case.
{ "shift", "rolc.by.reg", [] (std::vector<instruction> &b) {
	ins(b).mov(I8, 0x89abcdef);
	ins(b).mov(I9, 5);
	ins(b).mov(I7, 0xffffffff);
	ins(b).add(I6, I7, 1);           // sets C
	ins(b).rolc(I0, I8, I9);
	ins(b).rorc(I1, I8, I9);
	ins(b).rolc(I2, I8, 0);          // zero preserves the carry
} },

{ "loadstore", "dloadstore", [] (std::vector<instruction> &b) {
	static u64 dst[2];
	static u32 const src[4] = { 0x11223344, 0x55667788, 0x99aabbcc, 0xddeeff00 };
	ins(b).dmov(I8, 0x0123456789abcdefULL);
	ins(b).dstore(dst, 0, I8, SIZE_QWORD, SCALE_x8);
	ins(b).dload(I0, dst, 0, SIZE_QWORD, SCALE_x8);
	ins(b).dload(I1, src, 0, SIZE_DWORD, SCALE_x4);   // zero extended
	ins(b).dloads(I2, src, 0, SIZE_DWORD, SCALE_x4);  // sign extended
	ins(b).dloads(I3, src, 2, SIZE_BYTE, SCALE_x1);
} },

{ "flags", "getflgs.dsize", [] (std::vector<instruction> &b) {
	ins(b).mov(I8, 0);
	ins(b).mov(I9, 1);
	ins(b).sub(I7, I8, I9);          // borrow and sign
	ins(b).getflgs(I0, IFLAGS);
	ins(b).getflgs(I1, FLAG_S);
	ins(b).getflgs(I2, FLAG_V);
} },

// SET over every condition the integer flags can express, from one compare, so
// a mis-mapped condition code cannot hide behind a lucky value.
{ "flags", "set.all.conds", [] (std::vector<instruction> &b) {
	ins(b).mov(I8, 0x80000000);
	ins(b).mov(I9, 1);
	ins(b).cmp(I8, I9);
	ins(b).set(COND_Z,  I0);
	ins(b).set(COND_NZ, I1);
	ins(b).set(COND_S,  I2);
	ins(b).set(COND_NS, I3);
	ins(b).set(COND_C,  I4);
	ins(b).set(COND_NC, I5);
	ins(b).set(COND_V,  I6);
	ins(b).set(COND_NV, I7);
} },

{ "flags", "set.all.conds2", [] (std::vector<instruction> &b) {
	ins(b).mov(I8, 0x80000000);
	ins(b).mov(I9, 1);
	ins(b).cmp(I8, I9);
	ins(b).set(COND_A,  I0);
	ins(b).set(COND_BE, I1);
	ins(b).set(COND_G,  I2);
	ins(b).set(COND_LE, I3);
	ins(b).set(COND_L,  I4);
	ins(b).set(COND_GE, I5);
} },

{ "mov", "mov.cond.all", [] (std::vector<instruction> &b) {
	ins(b).mov(I8, 5);
	ins(b).mov(I9, 5);
	ins(b).cmp(I8, I9);
	ins(b).mov(COND_E,  I0, 0x11111111);
	ins(b).mov(COND_NE, I1, 0x22222222);
	ins(b).mov(COND_A,  I2, 0x33333333);
	ins(b).mov(COND_BE, I3, 0x44444444);
	ins(b).dmov(COND_GE, I4, 0x5555555555555555ULL);
} },

{ "float", "fscmp", [] (std::vector<instruction> &b) {
	ins(b).fscmp(F1, F2);
	ins(b).set(COND_B, I0);
	ins(b).set(COND_E, I1);
	ins(b).fscmp(F1, F1);
	ins(b).set(COND_E, I2);
} },

{ "float", "fsqrt.frecip", [] (std::vector<instruction> &b) {
	ins(b).fdsqrt(F0, F1);
	ins(b).fdrecip(F1, F2);
	ins(b).fdrsqrt(F2, F3);
	ins(b).fssqrt(F3, F4);
	ins(b).fsrecip(F4, F5);
} },

{ "float", "frnds.copy", [] (std::vector<instruction> &b) {
	ins(b).fdrnds(F0, F1);
	ins(b).mov(I9, 0x40490fdb);
	ins(b).fscopyi(F1, I9);
	ins(b).icopyfs(I0, F1);
	ins(b).dmov(I9, 0x400921fb54442d18ULL);
	ins(b).fdcopyi(F2, I9);
	ins(b).icopyfd(I1, F2);
} },

{ "float", "fstoint", [] (std::vector<instruction> &b) {
	ins(b).fstoint(I0, F1, SIZE_DWORD, ROUND_TRUNC);
	ins(b).fstoint(I1, F1, SIZE_QWORD, ROUND_TRUNC);
	ins(b).fdtoint(I2, F1, SIZE_QWORD, ROUND_ROUND);
	ins(b).fdtoint(I3, F1, SIZE_DWORD, ROUND_CEIL);
	ins(b).fdtoint(I4, F1, SIZE_DWORD, ROUND_FLOOR);
} },

{ "float", "ffri8", [] (std::vector<instruction> &b) {
	ins(b).dmov(I9, 0xfffffffffffffff0ULL);
	ins(b).fdfrint(F0, I9, SIZE_QWORD);
	ins(b).fsfrint(F1, I9, SIZE_QWORD);
	ins(b).fsfrflt(F2, F3, SIZE_QWORD);
} },

// ---- the call contract -----------------------------------------------------
//
// Nothing above this point calls anything. On a host with a link register
// rather than a pushed return address, that is the whole of the remaining
// risk: a handle's code pointer has to be a call target, a RET has to undo
// exactly what entering through that pointer did, and a HASHJMP has to unwind
// a call chain it is abandoning. None of it is visible in a corpus of straight
// line arithmetic, and all of it is on the path of every real driver.

{ "control", "callh.ret", [] (std::vector<instruction> &b) {
	code_handle &sub = hnd("diff_sub");
	ins(b).mov(I0, 0x11111111);
	ins(b).callh(sub);
	ins(b).mov(I2, 0x33333333);
	ins(b).jmp(9);
	ins(b).handle(sub);
	ins(b).mov(I1, 0x22222222);
	ins(b).ret();
	ins(b).label(9);
} },

// Two levels, because a link register makes one level look like it works
// whether or not the return address is being saved at all.
{ "control", "callh.nested", [] (std::vector<instruction> &b) {
	code_handle &outer = hnd("diff_outer");
	code_handle &inner = hnd("diff_inner");
	ins(b).mov(I0, 1);
	ins(b).callh(outer);
	ins(b).mov(I3, 4);
	ins(b).jmp(9);
	ins(b).handle(outer);
	ins(b).mov(I1, 2);
	ins(b).callh(inner);
	ins(b).ret();
	ins(b).handle(inner);
	ins(b).mov(I2, 3);
	ins(b).ret();
	ins(b).label(9);
} },

{ "control", "callh.cond", [] (std::vector<instruction> &b) {
	code_handle &taken = hnd("diff_taken");
	code_handle &nottaken = hnd("diff_nottaken");
	ins(b).mov(I8, 5);
	ins(b).mov(I9, 5);
	ins(b).cmp(I8, I9);
	ins(b).callh(COND_E, taken);
	ins(b).callh(COND_NE, nottaken);
	ins(b).jmp(9);
	ins(b).handle(taken);
	ins(b).mov(I0, 0x11111111);
	ins(b).ret();
	ins(b).handle(nottaken);
	ins(b).mov(I1, 0x22222222);
	ins(b).ret();
	ins(b).label(9);
} },

// A conditional RET returns down one path and falls through on the other, so
// it exercises the un-taken side of the same stack adjustment.
{ "control", "ret.cond", [] (std::vector<instruction> &b) {
	code_handle &sub = hnd("diff_sub");
	ins(b).mov(I0, 0);
	ins(b).callh(sub);
	ins(b).jmp(9);
	ins(b).handle(sub);
	ins(b).mov(I8, 1);
	ins(b).mov(I9, 2);
	ins(b).cmp(I8, I9);
	ins(b).ret(COND_E);          // not taken
	ins(b).mov(I0, 0x12345678);
	ins(b).ret(COND_NE);         // taken
	ins(b).mov(I0, 0xdeadbeef);  // must not run
	ins(b).ret();
	ins(b).label(9);
} },

// An exception handler does not come back, and that is not a simplification
// of the case -- it is the contract. drcbec's EXH pushes the EXH instruction
// itself where CALLH pushes the one after it (drcbec.cpp:846 against :819), so
// a RET out of an EXH handler re-executes the EXH forever. Real CPU cores end
// an exception handler in HASHJMP or EXIT, never RET, which is why nothing has
// ever tripped over it.
{ "control", "exh", [] (std::vector<instruction> &b) {
	code_handle &handler = hnd("diff_handler");
	code_handle &skipped = hnd("diff_skipped");
	ins(b).mov(I8, 5);
	ins(b).mov(I9, 6);
	ins(b).cmp(I8, I9);
	ins(b).exh(COND_E, skipped, 0xdead);   // condition fails, falls through
	ins(b).mov(I0, 0x11111111);
	ins(b).exh(handler, 0x5150);
	ins(b).mov(I2, 0xbadbad);              // must not run
	ins(b).handle(skipped);
	ins(b).mov(I3, 0xbadbad);              // must not run
	ins(b).handle(handler);
	ins(b).getexp(I1);
} },

// A hash jump that HITS. The target is reached through the hash table rather
// than by a branch, and the jump abandons whatever call depth it was at.
{ "control", "hashjmp.hit", [] (std::vector<instruction> &b) {
	code_handle &handler = hnd("diff_handler");
	code_handle &sub = hnd("diff_sub");
	ins(b).jmp(1);
	ins(b).hash(0, 0x2000);
	ins(b).mov(I1, 0x77777777);
	ins(b).jmp(2);
	ins(b).handle(sub);
	ins(b).mov(I0, 0x55555555);
	ins(b).hashjmp(0, 0x2000, handler);   // jumps out of a call, never returns
	ins(b).label(1);
	ins(b).callh(sub);
	ins(b).mov(I3, 0xbadbad);             // must not run
	ins(b).label(2);
} },

// A hash jump that MISSES, which is the path through the nocode stub and into
// the exception handle -- and the only way RECOVER's map lookup is reachable,
// because the address it recovers from is the one the miss path returns to.
{ "control", "hashjmp.recover", [] (std::vector<instruction> &b) {
	code_handle &handler = hnd("diff_handler");
	ins(b).mapvar(MAPVAR_M0, 0x1234abcd);
	ins(b).mov(I0, 0x11111111);
	ins(b).hashjmp(0, 0x3000, handler);   // nothing hashed at 0x3000
	ins(b).handle(handler);
	ins(b).getexp(I1);
	ins(b).recover(I2, MAPVAR_M0);
} },

{ "control", "callc", [] (std::vector<instruction> &b) {
	static u32 marker = 0x11111111;
	ins(b).setfmod(ROUND_CEIL);
	ins(b).callc(diff_cfunc, &marker);
	ins(b).getfmod(I2);
	ins(b).load(I0, &g_callc_seen, 0, SIZE_DWORD, SCALE_x4);
	ins(b).mov(I8, 1);
	ins(b).mov(I9, 1);
	ins(b).cmp(I8, I9);
	ins(b).callc(COND_NE, diff_cfunc, &marker);   // must not fire
	ins(b).load(I1, &g_callc_seen, 0, SIZE_DWORD, SCALE_x4);
} },
};



//**************************************************************************
//  RUNNING ONE CASE ON ONE BACK-END
//**************************************************************************

enum backend_kind { BE_C, BE_NATIVE };

struct outcome
{
	bool                    ran = false;        // false => the back-end refused
	std::string             err;                // why, if it refused
	int                     exitcode = 0;       // what EXIT returned
	drcuml_machine_state    state;              // what SAVE wrote
	compare_masks           masks;              // which of it is even defined
};

// A full, isolated run: its own caches, its own drcuml_state, its own
// back-end. Isolation per case rather than per corpus is what lets an
// unimplemented opcode be caught and reported instead of poisoning the
// bookkeeping for everything after it -- generate() raises from the middle of
// a block, so block_end() never runs, and the cheapest way to be certain that
// leaves nothing behind is to throw the whole back-end away.
outcome run_case(device_t &device, backend_kind kind, testcase const &tc)
{
	outcome result;
	std::memset(&result.state, 0, sizeof(result.state));

	g_case    = tc.name;
	g_backend = (kind == BE_C) ? "drcbe_c" : DIFF_NATIVE_NAME;
	g_phase   = "cache";

	try
	{
		// drc_cache is TWO-PHASE in 0.289: the constructor allocates nothing
		// and leaves every pointer null, and allocate_cache() is what maps the
		// memory. Skipping it does not fail loudly -- alloc_near() just returns
		// null, and the crash lands in whichever back-end constructor first
		// writes through it, which reads exactly like a broken back-end. Every
		// CPU core calls this from device_start (sh.cpp:41 and friends); a
		// harness that builds back-ends outside a device has to do it itself.
		drc_cache umlcache(CACHE_BYTES);
		drc_cache becache(CACHE_BYTES);
		bool const rwx = device.mconfig().options().drc_rwx();
		umlcache.allocate_cache(rwx);
		becache.allocate_cache(rwx);

		// This state constructs a back-end of its own, per drc_use_c(), which
		// is neither of the two under test and is never asked to do anything.
		// It is here for the block, handle and symbol bookkeeping.
		g_phase = "drcuml_state";
		drcuml_state uml(device, umlcache, 0, MODES, ADDRBITS, IGNOREBITS, MAX_SEQ_LEN);

		g_phase = "construct";
		std::unique_ptr<drcbe_interface> be;
		if (kind == BE_C)
			be = drc::make_drcbe_c(uml, device, becache, 0, MODES, ADDRBITS, IGNOREBITS);
#if !defined(DIFF_NO_NATIVE)
		else
			be = DIFF_MAKE_NATIVE(uml, device, becache, 0, MODES, ADDRBITS, IGNOREBITS);
#else
		else
			{ result.err = "no native back-end on this host"; return result; }
#endif

		g_phase = "reset";
		be->reset();

		g_phase = "build";
		g_uml = &uml;
		drcuml_machine_state seed = make_seed();

		code_handle *const entry = uml.handle_alloc("diff_entry");

		std::vector<instruction> block;
		block.reserve(MAXINST);
		ins(block).handle(*entry);
		ins(block).restore(&seed);
		tc.build(block);
		ins(block).save(&result.state);
		ins(block).exit(0x600d600d);
		request_all_flags(block);
		result.masks = masks_for(block);

		if (block.size() > MAXINST)
			throw emu_fatalerror("drc-diff: case '%s' is %u instructions, over MAXINST", tc.name, unsigned(block.size()));

		// No block.end(): end() would route generation through the state's own
		// back-end. begin_block() is called only to get a block object, which
		// generate() reads nothing from but invariant().
		g_phase = "generate";
		drcuml_block &blk = uml.begin_block(MAXINST);
		be->generate(blk, block.data(), u32(block.size()));

		g_phase = "execute";
		result.exitcode = be->execute(*entry);
		result.ran = true;
	}
	catch (emu_fatalerror const &e)
	{
		// The expected path for an opcode that is not lowered yet.
		result.err = e.what();
	}
	catch (drcuml_block::abort_compilation const &)
	{
		result.err = "out of cache space";
	}
	catch (std::exception const &e)
	{
		result.err = e.what();
	}

	g_uml = nullptr;
	g_phase = "(between cases)";
	return result;
}


//**************************************************************************
//  DIFFING
//**************************************************************************

// Field by field rather than memcmp, for two reasons: drcuml_machine_state has
// tail padding that no back-end writes, so a memcmp would compare uninitialised
// bytes; and a failing case has to name what disagreed to be worth anything.
unsigned diff_state(char const *name, outcome const &c, outcome const &n)
{
	unsigned bad = 0;
	auto report = [&] (char const *field, u64 cv, u64 nv)
	{
		std::fprintf(stderr, "  %-20s c=%016llx  " DIFF_NATIVE_NAME "=%016llx\n",
				field, (unsigned long long)cv, (unsigned long long)nv);
		bad++;
	};

	for (int i = 0; i < REG_I_COUNT; i++)
	{
		u64 const mask = c.masks.ireg[i];
		if ((c.state.r[i].d & mask) != (n.state.r[i].d & mask))
		{
			char buf[16];
			std::snprintf(buf, sizeof(buf), "i%d", i);
			report(buf, c.state.r[i].d & mask, n.state.r[i].d & mask);
		}
	}

	// Floats compared as their bit patterns: this is a bit-exactness test, and
	// comparing as doubles would call two NaNs unequal and two differently
	// encoded zeroes equal, both of which are the wrong answer here.
	for (int i = 0; i < REG_F_COUNT; i++)
	{
		u64 cv, nv;
		std::memcpy(&cv, &c.state.f[i].d, sizeof(cv));
		std::memcpy(&nv, &n.state.f[i].d, sizeof(nv));
		u64 const mask = c.masks.freg[i];
		if ((cv & mask) != (nv & mask))
		{
			char buf[16];
			std::snprintf(buf, sizeof(buf), "f%d", i);
			report(buf, cv & mask, nv & mask);
		}
	}

	if (c.state.exp != n.state.exp)
		report("exp", c.state.exp, n.state.exp);
	if (c.state.fmod != n.state.fmod)
		report("fmod", c.state.fmod, n.state.fmod);
	u8 const flagmask = c.masks.flags;
	if ((c.state.flags & flagmask) != (n.state.flags & flagmask))
		report("flags", c.state.flags & flagmask, n.state.flags & flagmask);
	if (c.exitcode != n.exitcode)
		report("exitcode", u32(c.exitcode), u32(n.exitcode));

	return bad;
}

} // anonymous namespace


//**************************************************************************
//  ENTRY POINT
//**************************************************************************

void diff_run_once(device_t &device)
{
	static bool done = false;
	if (done || !std::getenv("MAMESTER_DRC_DIFF"))
		return;
	done = true;

	// Optional filter: MAMESTER_DRC_DIFF=alu runs only the alu group, so a
	// lowering session can iterate on one group without reading past it.
	char const *const filter = std::getenv("MAMESTER_DRC_DIFF");
	bool const all = !std::strcmp(filter, "1") || !std::strcmp(filter, "all");

#if defined(DIFF_NO_NATIVE)
	std::fprintf(stderr, "DRC-DIFF: no native back-end is compiled in on this host -- nothing to diff\n");
	std::fflush(nullptr);
	std::_Exit(2);
#else
	std::fprintf(stderr, "DRC-DIFF: drcbe_c vs " DIFF_NATIVE_NAME "\n");
	install_crash_handler();

	unsigned pass = 0, fail = 0, unimpl = 0, skipped = 0;

	for (testcase const &tc : CORPUS)
	{
		if (!all && std::strcmp(filter, tc.group) && std::strcmp(filter, tc.name))
		{
			skipped++;
			continue;
		}

		outcome const c = run_case(device, BE_C,      tc);
		outcome const n = run_case(device, BE_NATIVE, tc);

		// drcbe_c is the oracle. If IT cannot run a case, the case is wrong,
		// not the back-end, and saying so is the difference between a corpus
		// bug and a lowering bug.
		if (!c.ran)
		{
			std::fprintf(stderr, "BAD-CASE %-24s the C back-end refused it: %s\n", tc.name, c.err.c_str());
			fail++;
			continue;
		}

		if (!n.ran)
		{
			std::fprintf(stderr, "UNIMPL   %-24s %s", tc.name, n.err.c_str());
			unimpl++;
			continue;
		}

		unsigned const bad = diff_state(tc.name, c, n);
		if (bad)
		{
			std::fprintf(stderr, "FAIL     %-24s %u field(s) disagree\n", tc.name, bad);
			fail++;
		}
		else
		{
			std::fprintf(stderr, "ok       %-24s\n", tc.name);
			pass++;
		}
	}

	std::fprintf(stderr, "DRC-DIFF: pass=%u fail=%u unimpl=%u skipped=%u\n",
			pass, fail, unimpl, skipped);

	// _Exit rather than a return: the harness has just left a drcuml_state and
	// two back-ends in whatever condition the last case put them, and MAME's
	// orderly shutdown has no reason to survive that. The exit status is the
	// test result, and unimplemented opcodes are not failures -- they are the
	// work queue.
	std::fflush(nullptr);
	std::_Exit(fail ? 1 : 0);
#endif
}

} // namespace drc

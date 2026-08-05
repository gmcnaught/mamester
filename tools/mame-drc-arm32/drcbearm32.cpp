// license:BSD-3-Clause
// copyright-holders:MAMESTer port
// Derived from drcbex86.cpp (Aaron Giles) and drcbearm64.cpp (windyfairy, Vas Crabb)
/***************************************************************************

    drcbearm32.cpp

    32-bit ARM (ARMv7-A, A32) back-end for the universal machine language.

****************************************************************************

    STATUS: scaffolding. It compiles against 0.289's drcbe_interface and the
    structural opcodes (HANDLE/HASH/LABEL/COMMENT/MAPVAR/NOP) are real, but
    NO instruction that computes anything is lowered yet, and the entry/exit/
    nocode stub shapes below are provisional -- they follow drcbex86's model
    (call into generated code, nocode returns to the caller) but have not been
    checked against its hashjmp/exit contract. Every unlowered opcode is a
    fatalerror rather than a silent wrong answer. See "REMAINING WORK".

    ---------------
    Where this came from
    ---------------

    This is drcbex86 retargeted, not drcbearm64 backported. MAME carried a
    32-bit back-end -- drcbex86 -- through mame0287 and deleted it in
    0.288/0.289, so the hard parts of running a 64-bit-register IR on a
    32-bit host (register pairs, synthesised 64-bit shifts and multiplies,
    flag reconstruction) have an in-tree solution with two decades of driver
    testing behind it. drcbearm64 contributes only the ARM-specific carry
    handling. uml.h is byte-identical between 0.287 and 0.289, so the IR this
    lowers is exactly the IR drcbex86 lowered.

    ---------------
    The encoder
    ---------------

    asmjit's AArch32 back-end, from its unmerged a32_port branch, injected into
    MAME's vendored asmjit by tools/mame-drc-arm32/inject.sh. Chosen over the
    hand-written encoder in arm32emit.h for one reason: drcbex86 is itself
    written against asmjit, so retargeting its ~7,700 lines happens inside the
    same CodeHolder/Label/Mem/relocation machinery rather than across an API
    boundary.

    a32_port is upstream WIP and is treated as such. tests/a32-asmjit/ diffs it
    against arm-linux-gnueabihf-as over the subset this lowering needs (88
    encodings, all matching), and asmjit-a32-fixes.py carries the two defects
    that test found. arm32emit.h is kept as the fallback, and as the oracle the
    corpus was built from.

    Two API traps, both of which cost time here:
      * The shift operation lives in the predicate of the LAST operand, so it
        is add(rd, rn, rm, lsr(16)), never add(rd, rn, lsr(rm), imm(16)). The
        wrong form silently encodes as LSL, because LSL is predicate 0.
      * A rejected instruction segfaulted rather than returning its error, as
        the a32 emitter installs no formatter and the reporting path calls
        through it. Fixed locally; do not drop that fix.

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
        d16-d31    - volatile

    ---------------
    Execution model
    ---------------

    Registers:
        r0-r10     - scratch
        r11        - pinned: &m_state, the UML register file base
        r12        - scratch (also the call trampoline register)

        d0-d7      - scratch

    UML registers are NOT mapped to host registers in this version. Every
    I-register access is a [r11, #offset] load or store and every 64-bit value
    occupies a register pair for the duration of one operation.
    get_info() therefore reports direct_iregs = 0.

    That is slower than drcbex86, which pins I0-I3's low halves, and much
    slower than this can eventually be -- ARMv7 has fourteen usable GPRs
    against x86-32's seven, so there is real room. It is deliberately the
    starting point: register mapping is an optimisation that can be layered
    onto a lowering that already passes a frame-hash A/B against drcbec,
    whereas a lowering built around register mapping cannot be tested until it
    is finished.

    The one pinned register pays for itself immediately. Without it every UML
    register access would need a movw/movt pair to materialise an absolute
    address before the load.

    Entry point:
        One parameter: the codeptr to jump to once the environment is set up.

    Entry stack:
        The callee-saved set plus lr is pushed, and sp is stashed in
        m_near.stacksave so EXIT can unwind from arbitrary depth.

    ---------------
    Flags
    ---------------

    UML carries C V Z S U; ARM's NZCV maps N->S, Z->Z, C->C, V->V, with U
    reachable only from float compares. The one place the correspondence
    breaks is subtraction:

        ARM sets C to NOT-borrow. UML, following x86, defines it as borrow.

    So after any subtract the hardware carry is the logical inverse of the UML
    carry. drcbearm64 solves this by tracking at generate time which of three
    states the stored carry is in and inverting lazily, only when a consumer
    needs it; carry_state below is that design, and the reason it is copied
    rather than re-derived is that it is the single most error-prone part of
    the lowering and it already works.

    Two further ARM shift/flag details the lowering must respect, neither of
    which drcbex86 can warn about:
      * a shift by zero leaves C untouched rather than defined, and C after a
        shift is the last bit shifted out
      * there is no ROL (use ROR #(32-n)) and no RCL/RCR, so ROLC/RORC are
        synthesised -- RRX is the only path for carry INTO a shift

    ---------------
    REMAINING WORK
    ---------------

    Implemented:  HANDLE HASH LABEL COMMENT MAPVAR NOP BREAK
    Not yet:      the control flow that gives the stubs their contract
                  (DEBUG EXIT HASHJMP JMP EXH CALLH RET CALLC RECOVER),
                  the integer ALU (LOAD/STORE/READ/WRITE/MOV/SEXT/ROLAND/
                  ROLINS/ADD/ADDC/SUB/SUBB/CMP/MULU/MULS/DIVU/DIVS/AND/TEST/
                  OR/XOR/LZCNT/TZCNT/BSWAP/SHL/SHR/SAR/ROL/ROLC/ROR/RORC),
                  the flag ops (GETFLGS/SETFLGS/CARRY/SET), and the entire
                  float set (F*).

    Each of those is a distinct, independently testable unit of work, which is
    why the dispatch below lists them explicitly instead of falling through to
    a default. Adding one means deleting its line from the unimplemented set,
    not editing a catch-all.

***************************************************************************/

#include "emu.h"
#include "drcbearm32.h"

#include "drcbeut.h"

#include "debug/debugcpu.h"
#include "emuopts.h"

#include "mfpresolve.h"

#include "asmjit/asmjit/a32.h"

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

// r11 holds &m_state for the life of a generated sequence
const Gp REG_STATE = r11;

// the call trampoline register: any address in two instructions, no literal pool
const Gp REG_CALL = r12;

// bytes of scratch reserved below sp inside generated code. AAPCS wants sp
// 8-byte aligned at every public interface, and a call may pass arguments on
// the stack once it runs out of r0-r3.
constexpr uint32_t STACK_SCRATCH = 32;


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
	// See the file comment: the hardware carry after a subtract is the inverse
	// of the UML carry, so what is currently stored has to be tracked rather
	// than assumed.
	enum class carry_state
	{
		POISON,     // does not correspond to the UML carry flag at all
		CANONICAL,  // corresponds directly
		LOGICAL     // is the inverse
	};

	// near-cache block: everything generated code reaches by absolute address
	struct near_state
	{
		uint32_t    stacksave;      // sp at entry, so EXIT can unwind
		uint32_t    emulated_flags; // UML flags when they are not live in NZCV
	};

	size_t emit(CodeHolder &ch, bool invariant);
	void emit_load_state_base(Assembler &a);
	void emit_call(Assembler &a, void const *target);

	void generate_one(Assembler &a, const uml::instruction &inst);
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

	drc_hash_table      m_hash;
	drc_map_variables   m_map;
	drc_label_list      m_labels;
	near_state &        m_near;

	carry_state         m_carry_state;

	arm32_entry_point_func m_entry;
	drccodeptr          m_exit;
	drccodeptr          m_nocode;
};


//**************************************************************************
//  CONSTRUCTION
//**************************************************************************

drcbe_arm32::drcbe_arm32(drcuml_state &drcuml, device_t &device, drc_cache &cache, uint32_t flags, int modes, int addrbits, int ignorebits)
	: drcbe_interface(drcuml, cache, device)
	, m_hash(cache, modes, addrbits, ignorebits, drcuml.max_sequence_length())
	, m_map(cache, 0)
	, m_labels()
	, m_near(*cache.alloc_near<near_state>())
	, m_carry_state(carry_state::POISON)
	, m_entry(nullptr)
	, m_exit(nullptr)
	, m_nocode(nullptr)
{
	std::fill_n((uint8_t *)&m_near, sizeof(m_near), 0);

	// The UML register file must be reachable from a single pinned base
	// register with a 12-bit displacement. drcuml_machine_state is well inside
	// that, but assert rather than discover it as corruption later.
	static_assert(sizeof(drcuml_machine_state) <= 4096,
			"UML machine state no longer fits a single ldr/str displacement from the pinned base");
}

drcbe_arm32::~drcbe_arm32()
{
}


//**************************************************************************
//  REQUIRED OVERRIDES
//**************************************************************************

void drcbe_arm32::reset()
{
	uint8_t *const dst = (uint8_t *)m_cache.top();

	CodeHolder ch;
	ch.init(Environment::host(), uint64_t(dst));

	Assembler a(&ch);

	GpList const saved({ r4, r5, r6, r7, r8, r9, r10, r11, r14 });

	// ---- entry point: uint32_t entry(void *codeptr) ----
	uint64_t const entry_offs = a.offset();
	a.push(saved);
	a.sub(sp, sp, imm(STACK_SCRATCH));

	// stash sp so EXIT can unwind from any depth
	a.mov(r1, imm(uint32_t(uintptr_t(&m_near.stacksave))));
	a.str(sp, ptr(r1));

	emit_load_state_base(a);

	// call rather than jump, so the nocode handler can simply return -- this is
	// drcbex86's arrangement and the three stubs only make sense together
	a.blx(r0);

	// falls straight through into the exit point below, which is the whole
	// reason exit is generated here rather than somewhere more convenient
	// ---- exit point: return value already in r0 ----
	uint64_t const exit_offs = a.offset();
	a.mov(r1, imm(uint32_t(uintptr_t(&m_near.stacksave))));
	a.ldr(sp, ptr(r1));
	a.add(sp, sp, imm(STACK_SCRATCH));
	a.pop(saved);
	a.bx(r14);

	// ---- nocode handler: the hash table's default target ----
	// Just a return: a hashjmp that misses lands here and unwinds to whoever
	// called into the block, which is what decides what to do about the miss.
	uint64_t const nocode_offs = a.offset();
	a.bx(r14);

	a.finalize();
	if (!emit(ch, true))
		fatalerror("drcbearm32: out of cache space generating the entry stubs\n");

	m_entry = (arm32_entry_point_func)(uintptr_t(dst) + entry_offs);
	m_exit = drccodeptr(uintptr_t(dst) + exit_offs);
	m_nocode = drccodeptr(uintptr_t(dst) + nocode_offs);

	m_hash.reset();
	m_hash.set_default_codeptr(m_nocode);
	m_carry_state = carry_state::POISON;
}


int drcbe_arm32::execute(uml::code_handle &entry)
{
	return (*m_entry)(entry.codeptr());
}


void drcbe_arm32::generate(drcuml_block &block, const uml::instruction *instlist, uint32_t numinst)
{
	// tell the utilities a block is starting
	m_hash.block_begin(block, instlist, numinst);
	m_labels.block_begin(block);
	m_map.block_begin(block);

	CodeHolder ch;
	ch.init(Environment::host(), uint64_t(m_cache.top()));

	Assembler a(&ch);
	m_carry_state = carry_state::POISON;

	for (uint32_t inum = 0; inum < numinst; inum++)
	{
		assert(instlist[inum].size() == 4 || instlist[inum].size() == 8);
		generate_one(a, instlist[inum]);
	}

	a.finalize();
	if (!emit(ch, false))
		block.abort();

	m_map.block_end(block);
	m_labels.block_end(block);
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


//**************************************************************************
//  HELPERS
//**************************************************************************

// Copy a finished CodeHolder into the DRC cache at the address it was
// assembled for, and make the instruction cache agree. Same shape as
// drcbe_arm64::emit(): asmjit assembles against a fixed base address, so the
// allocation has to land at or below that address rather than wherever the
// allocator would otherwise put it.
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


void drcbe_arm32::emit_load_state_base(Assembler &a)
{
	a.mov(REG_STATE, imm(uint32_t(uintptr_t(&m_state))));
}


// A call to an arbitrary C function. Deliberately not BL: the code cache and
// libc are not guaranteed to be within BL's +/-32 MB of each other, and a call
// that is nearly always in range is worse than one that never is.
void drcbe_arm32::emit_call(Assembler &a, void const *target)
{
	a.mov(REG_CALL, imm(uint32_t(uintptr_t(target))));
	a.blx(REG_CALL);
}


[[noreturn]] void drcbe_arm32::unimplemented(const uml::instruction &inst) const
{
	// Deliberately fatal. A back-end that silently skipped an opcode would
	// produce a game that runs and is wrong, which is far more expensive to
	// diagnose than one that refuses to start.
	fatalerror("drcbearm32: UML opcode %d is not lowered yet (see REMAINING WORK in drcbearm32.cpp)\n",
			int(inst.opcode()));
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

	inst.param(0).handle().set_codeptr(drccodeptr(a.code()->base_address() + a.offset()));
	m_carry_state = carry_state::POISON;
}


void drcbe_arm32::op_hash(Assembler &a, const uml::instruction &inst)
{
	assert_no_condition(inst);
	assert_no_flags(inst);
	assert(inst.numparams() == 2);
	assert(inst.param(0).is_immediate());
	assert(inst.param(1).is_immediate());

	m_hash.set_codeptr(inst.param(0).immediate(), inst.param(1).immediate(), drccodeptr(a.code()->base_address() + a.offset()));
	m_carry_state = carry_state::POISON;
}


void drcbe_arm32::op_label(Assembler &a, const uml::instruction &inst)
{
	assert_no_condition(inst);
	assert_no_flags(inst);
	assert(inst.numparams() == 1);
	assert(inst.param(0).is_code_label());

	m_labels.set_codeptr(inst.param(0).label(), drccodeptr(a.code()->base_address() + a.offset()));
	m_carry_state = carry_state::POISON;
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
	a.mov(r0, imm(uint32_t(uintptr_t(message))));
	emit_call(a, (void *)&osd_break_into_debugger);
	m_carry_state = carry_state::POISON;
}


void drcbe_arm32::op_debug(Assembler &a, const uml::instruction &inst)
{
	unimplemented(inst);
}


void drcbe_arm32::op_exit(Assembler &a, const uml::instruction &inst)
{
	unimplemented(inst);
}


void drcbe_arm32::op_hashjmp(Assembler &a, const uml::instruction &inst)
{
	unimplemented(inst);
}


void drcbe_arm32::op_jmp(Assembler &a, const uml::instruction &inst)
{
	unimplemented(inst);
}


void drcbe_arm32::op_exh(Assembler &a, const uml::instruction &inst)
{
	unimplemented(inst);
}


void drcbe_arm32::op_callh(Assembler &a, const uml::instruction &inst)
{
	unimplemented(inst);
}


void drcbe_arm32::op_ret(Assembler &a, const uml::instruction &inst)
{
	unimplemented(inst);
}


void drcbe_arm32::op_callc(Assembler &a, const uml::instruction &inst)
{
	unimplemented(inst);
}


void drcbe_arm32::op_recover(Assembler &a, const uml::instruction &inst)
{
	unimplemented(inst);
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

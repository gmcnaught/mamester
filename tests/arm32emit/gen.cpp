// license:BSD-3-Clause
// Differential test generator for tools/mame-drc-arm32/arm32emit.h.
//
// For every emitter entry point this walks a slice of the operand space,
// records the word our encoder produces, and records the assembly text that
// should produce the same word. run.sh assembles the text with
// arm-linux-gnueabihf-as and compares word for word.
//
// The assembler is the oracle. If a case disagrees, the encoder is wrong until
// proven otherwise -- do not "fix" the expected text to match the encoder.
//
// Adding an entry point to arm32emit.h without adding cases here is how a
// wrong encoding reaches a game, so treat the two files as one change.

#include "../../tools/mame-drc-arm32/arm32emit.h"

#include <cstdio>
#include <cstdarg>
#include <string>
#include <vector>

using namespace arm32;

namespace {

struct testcase
{
	std::vector<u32> words;
	std::vector<std::string> text;
};

std::vector<testcase> g_cases;

std::string fmt(char const *f, ...)
{
	char buf[256];
	va_list ap;
	va_start(ap, f);
	vsnprintf(buf, sizeof(buf), f, ap);
	va_end(ap);
	return buf;
}

// run `f` on a fresh assembler and pair the words it produced with `text`
template <typename Fn>
void add(std::vector<std::string> const &text, Fn &&f)
{
	u8 buf[64];
	assembler a(buf, buf + sizeof(buf));
	f(a);
	testcase tc;
	tc.text = text;
	size_t const n = a.size() / 4;
	for (size_t i = 0; i < n; i++)
	{
		u32 w;
		std::memcpy(&w, buf + i * 4, 4);
		tc.words.push_back(w);
	}
	if (tc.words.size() != text.size())
	{
		fprintf(stderr, "case '%s' emitted %zu words for %zu text lines\n",
				text.empty() ? "?" : text[0].c_str(), tc.words.size(), text.size());
		exit(1);
	}
	g_cases.push_back(std::move(tc));
}

template <typename Fn>
void add1(std::string const &text, Fn &&f) { add(std::vector<std::string>{ text }, std::forward<Fn>(f)); }


//**************************************************************************
//  CASES
//**************************************************************************

char const *const COND_NAME[15] = {
	"eq","ne","cs","cc","mi","pl","vs","vc","hi","ls","ge","lt","gt","le",""
};

char const *const SHIFT_NAME[4] = { "lsl", "lsr", "asr", "ror" };

void data_processing()
{
	struct { char const *name; assembler::dp_op op; } const THREE[] = {
		{ "and", assembler::DP_AND }, { "eor", assembler::DP_EOR },
		{ "sub", assembler::DP_SUB }, { "rsb", assembler::DP_RSB },
		{ "add", assembler::DP_ADD }, { "adc", assembler::DP_ADC },
		{ "sbc", assembler::DP_SBC }, { "rsc", assembler::DP_RSC },
		{ "orr", assembler::DP_ORR }, { "bic", assembler::DP_BIC },
	};

	// three-operand forms, immediate and register second source
	for (auto const &e : THREE)
	{
		for (int s = 0; s < 2; s++)
		{
			for (u32 ci : { u32(COND_AL), u32(COND_EQ), u32(COND_LT) })
			{
				condition const c = condition(ci);
				char const *sfx = s ? "s" : "";
				add1(fmt("%s%s%s r4, r5, #0x1f", e.name, sfx, COND_NAME[ci]),
						[&](assembler &a) { a.dp(e.op, s, r4, r5, imm(0x1f), c); });
				add1(fmt("%s%s%s r0, r1, r2", e.name, sfx, COND_NAME[ci]),
						[&](assembler &a) { a.dp(e.op, s, r0, r1, reg(r2), c); });
			}
		}
	}

	// every shift kind and the boundary amounts for each
	for (u32 t = 0; t < 4; t++)
	{
		shift_type const st = shift_type(t);
		u32 const amounts_lsl_ror[] = { 1, 7, 31 };
		u32 const amounts_lsr_asr[] = { 1, 16, 31, 32 };
		bool const wide = (st == SHIFT_LSR || st == SHIFT_ASR);
		u32 const count = wide ? 4u : 3u;
		for (u32 i = 0; i < count; i++)
		{
			u32 const amt = wide ? amounts_lsr_asr[i] : amounts_lsl_ror[i];
			add1(fmt("add r3, r4, r5, %s #%u", SHIFT_NAME[t], amt),
					[&](assembler &a) { a.add(r3, r4, reg(r5, st, amt)); });
		}
		add1(fmt("add r3, r4, r5, %s r6", SHIFT_NAME[t]),
				[&](assembler &a) { a.add(r3, r4, reg(r5, st, r6)); });
	}

	add1("mov r0, r1, rrx", [](assembler &a) { a.mov(r0, rrx(r1)); });
	add1("adcs r0, r1, r2, rrx", [](assembler &a) { a.adcs(r0, r1, rrx(r2)); });

	// single-source and comparison forms, where one of Rd/Rn is unused
	for (u32 ci : { u32(COND_AL), u32(COND_NE) })
	{
		condition const c = condition(ci);
		add1(fmt("mov%s r7, #0x40", COND_NAME[ci]), [&](assembler &a) { a.mov(r7, imm(0x40), c); });
		add1(fmt("movs%s r7, r8", COND_NAME[ci]), [&](assembler &a) { a.movs(r7, reg(r8), c); });
		add1(fmt("mvn%s r7, #0", COND_NAME[ci]), [&](assembler &a) { a.mvn(r7, imm(0), c); });
		add1(fmt("mvns%s r7, r8", COND_NAME[ci]), [&](assembler &a) { a.mvns(r7, reg(r8), c); });
		add1(fmt("tst%s r9, #0x80", COND_NAME[ci]), [&](assembler &a) { a.tst(r9, imm(0x80), c); });
		add1(fmt("teq%s r9, r10", COND_NAME[ci]), [&](assembler &a) { a.teq(r9, reg(r10), c); });
		add1(fmt("cmp%s r9, #0xff", COND_NAME[ci]), [&](assembler &a) { a.cmp(r9, imm(0xff), c); });
		add1(fmt("cmn%s r9, r10, lsl #2", COND_NAME[ci]), [&](assembler &a) { a.cmn(r9, reg(r10, SHIFT_LSL, 2), c); });
	}

	// rotated immediates: the encodable set is not contiguous, and these are
	// the shapes that exercise every rotate bucket
	u32 const rotated[] = { 0, 1, 0xff, 0x100, 0xff00, 0xff000000, 0x3fc, 0x0000ff00, 0xc0000034 };
	for (u32 v : rotated)
	{
		if (!is_imm_encodable(v))
			continue;
		add1(fmt("add r0, r1, #0x%x", v), [&](assembler &a) { a.add(r0, r1, imm(v)); });
	}
}

void constants()
{
	// one instruction where the value fits a rotated immediate...
	add1("mov r0, #0x1f", [](assembler &a) { a.mov32(r0, 0x1f); });
	add1("mvn r0, #0x1f", [](assembler &a) { a.mov32(r0, ~u32(0x1f)); });
	// ...movw alone when it fits 16 bits...
	add1("movw r1, #0x1234", [](assembler &a) { a.mov32(r1, 0x1234); });
	// ...and the movw/movt pair otherwise
	add({ "movw r2, #0x5678", "movt r2, #0x1234" }, [](assembler &a) { a.mov32(r2, 0x12345678); });
	// all-ones is one instruction, not two: it is the complement of an
	// encodable immediate, which mov32 checks before reaching for movw/movt
	add1("mvn r3, #0", [](assembler &a) { a.mov32(r3, 0xffffffff); });
	add({ "movw r3, #0x1234", "movt r3, #0xffff" }, [](assembler &a) { a.mov32(r3, 0xffff1234); });

	add1("movw r4, #0xabcd", [](assembler &a) { a.movw(r4, 0xabcd); });
	add1("movt r4, #0xabcd", [](assembler &a) { a.movt(r4, 0xabcd); });
	add1("movweq r5, #0x0", [](assembler &a) { a.movw(r5, 0, COND_EQ); });
}

void loads_stores()
{
	struct { char const *name; void (assembler::*fn)(gpr, mem const &, condition); } const WB[] = {
		{ "ldr",  &assembler::ldr },  { "str",  &assembler::str },
		{ "ldrb", &assembler::ldrb }, { "strb", &assembler::strb },
	};

	for (auto const &e : WB)
	{
		add1(fmt("%s r0, [r1]", e.name), [&](assembler &a) { (a.*e.fn)(r0, ptr(r1), COND_AL); });
		add1(fmt("%s r0, [r1, #4]", e.name), [&](assembler &a) { (a.*e.fn)(r0, ptr(r1, 4), COND_AL); });
		add1(fmt("%s r0, [r1, #-4]", e.name), [&](assembler &a) { (a.*e.fn)(r0, ptr(r1, -4), COND_AL); });
		add1(fmt("%s r0, [r1, #4095]", e.name), [&](assembler &a) { (a.*e.fn)(r0, ptr(r1, 4095), COND_AL); });
		add1(fmt("%s r0, [r1, #-4095]", e.name), [&](assembler &a) { (a.*e.fn)(r0, ptr(r1, -4095), COND_AL); });
		add1(fmt("%s r0, [r1, r2]", e.name), [&](assembler &a) { (a.*e.fn)(r0, ptr(r1, r2), COND_AL); });
		add1(fmt("%s r0, [r1, r2, lsl #3]", e.name), [&](assembler &a) { (a.*e.fn)(r0, ptr(r1, r2, SHIFT_LSL, 3), COND_AL); });
		add1(fmt("%s r0, [r1, r2, asr #5]", e.name), [&](assembler &a) { (a.*e.fn)(r0, ptr(r1, r2, SHIFT_ASR, 5), COND_AL); });
		add1(fmt("%s r0, [r1], #8", e.name), [&](assembler &a) { (a.*e.fn)(r0, ptr_post(r1, 8), COND_AL); });
		add1(fmt("%s r0, [r1, #8]!", e.name), [&](assembler &a) { (a.*e.fn)(r0, ptr_pre(r1, 8), COND_AL); });
		add1(fmt("%seq r0, [r1, #8]", e.name), [&](assembler &a) { (a.*e.fn)(r0, ptr(r1, 8), COND_EQ); });
	}

	// the extra load/store family: a different encoding with a split 8-bit
	// immediate, so ±255 rather than ±4095
	struct { char const *name; void (assembler::*fn)(gpr, mem const &, condition); } const EX[] = {
		{ "ldrh",  &assembler::ldrh },  { "strh", &assembler::strh },
		{ "ldrsb", &assembler::ldrsb }, { "ldrsh", &assembler::ldrsh },
	};

	for (auto const &e : EX)
	{
		add1(fmt("%s r0, [r1]", e.name), [&](assembler &a) { (a.*e.fn)(r0, ptr(r1), COND_AL); });
		add1(fmt("%s r0, [r1, #2]", e.name), [&](assembler &a) { (a.*e.fn)(r0, ptr(r1, 2), COND_AL); });
		add1(fmt("%s r0, [r1, #-2]", e.name), [&](assembler &a) { (a.*e.fn)(r0, ptr(r1, -2), COND_AL); });
		add1(fmt("%s r0, [r1, #255]", e.name), [&](assembler &a) { (a.*e.fn)(r0, ptr(r1, 255), COND_AL); });
		add1(fmt("%s r0, [r1, #-255]", e.name), [&](assembler &a) { (a.*e.fn)(r0, ptr(r1, -255), COND_AL); });
		add1(fmt("%s r0, [r1, r2]", e.name), [&](assembler &a) { (a.*e.fn)(r0, ptr(r1, r2), COND_AL); });
		add1(fmt("%s r0, [r1], #16", e.name), [&](assembler &a) { (a.*e.fn)(r0, ptr_post(r1, 16), COND_AL); });
		add1(fmt("%s r0, [r1, #16]!", e.name), [&](assembler &a) { (a.*e.fn)(r0, ptr_pre(r1, 16), COND_AL); });
	}

	// doubleword: Rt must be even, and the pair is Rt:Rt+1
	add1("ldrd r4, r5, [r1]", [](assembler &a) { a.ldrd(r4, ptr(r1)); });
	add1("ldrd r4, r5, [r1, #8]", [](assembler &a) { a.ldrd(r4, ptr(r1, 8)); });
	add1("ldrd r4, r5, [r1, #-8]", [](assembler &a) { a.ldrd(r4, ptr(r1, -8)); });
	add1("strd r6, r7, [r2, #24]", [](assembler &a) { a.strd(r6, ptr(r2, 24)); });
	add1("strd r0, r1, [r2, r3]", [](assembler &a) { a.strd(r0, ptr(r2, r3)); });

	add1("push {r0, r4, lr}", [](assembler &a) { a.push(assembler::rmask(r0, r4, lr)); });
	add1("pop {r0, r4, pc}", [](assembler &a) { a.pop(assembler::rmask(r0, r4, pc)); });
	add1("push {r4, r5, r6, r7, r8, r9, r10, r11}",
			[](assembler &a) { a.push(assembler::rmask(r4, r5, r6, r7, r8, r9, r10, r11)); });
}

void multiplies()
{
	add1("mul r0, r1, r2", [](assembler &a) { a.mul(r0, r1, r2); });
	add1("muls r0, r1, r2", [](assembler &a) { a.mul(r0, r1, r2, true); });
	add1("muleq r3, r4, r5", [](assembler &a) { a.mul(r3, r4, r5, false, COND_EQ); });
	add1("mla r0, r1, r2, r3", [](assembler &a) { a.mla(r0, r1, r2, r3); });
	add1("mlas r0, r1, r2, r3", [](assembler &a) { a.mla(r0, r1, r2, r3, true); });

	add1("umull r0, r1, r2, r3", [](assembler &a) { a.umull(r0, r1, r2, r3); });
	add1("umulls r0, r1, r2, r3", [](assembler &a) { a.umull(r0, r1, r2, r3, true); });
	add1("umlal r4, r5, r6, r7", [](assembler &a) { a.umlal(r4, r5, r6, r7); });
	add1("smull r0, r1, r2, r3", [](assembler &a) { a.smull(r0, r1, r2, r3); });
	add1("smulls r0, r1, r2, r3", [](assembler &a) { a.smull(r0, r1, r2, r3, true); });
	add1("smlal r4, r5, r6, r7", [](assembler &a) { a.smlal(r4, r5, r6, r7); });
}

void bit_manipulation()
{
	add1("clz r0, r1", [](assembler &a) { a.clz(r0, r1); });
	add1("clzne r2, r3", [](assembler &a) { a.clz(r2, r3, COND_NE); });
	add1("rbit r0, r1", [](assembler &a) { a.rbit(r0, r1); });
	add1("rev r0, r1", [](assembler &a) { a.rev(r0, r1); });
	add1("rev16 r0, r1", [](assembler &a) { a.rev16(r0, r1); });

	u32 const fields[][2] = { {0,1}, {0,32}, {4,8}, {8,16}, {31,1}, {16,16} };
	for (auto const &f : fields)
	{
		add1(fmt("ubfx r0, r1, #%u, #%u", f[0], f[1]), [&](assembler &a) { a.ubfx(r0, r1, f[0], f[1]); });
		add1(fmt("sbfx r0, r1, #%u, #%u", f[0], f[1]), [&](assembler &a) { a.sbfx(r0, r1, f[0], f[1]); });
		add1(fmt("bfi r0, r1, #%u, #%u", f[0], f[1]), [&](assembler &a) { a.bfi(r0, r1, f[0], f[1]); });
		add1(fmt("bfc r0, #%u, #%u", f[0], f[1]), [&](assembler &a) { a.bfc(r0, f[0], f[1]); });
	}

	for (u32 rot : { 0u, 8u, 16u, 24u })
	{
		char const *suffix = rot ? ", ror #%u" : "";
		std::string const tail = rot ? fmt(", ror #%u", rot) : std::string();
		(void)suffix;
		add1("uxtb r0, r1" + tail, [&](assembler &a) { a.uxtb(r0, r1, rot); });
		add1("uxth r0, r1" + tail, [&](assembler &a) { a.uxth(r0, r1, rot); });
		add1("sxtb r0, r1" + tail, [&](assembler &a) { a.sxtb(r0, r1, rot); });
		add1("sxth r0, r1" + tail, [&](assembler &a) { a.sxth(r0, r1, rot); });
	}

	add1("mrs r0, apsr", [](assembler &a) { a.mrs(r0); });
	add1("msr APSR_nzcvq, r0", [](assembler &a) { a.msr(r0); });
}

void branches()
{
	add1("bx r0", [](assembler &a) { a.bx(r0); });
	add1("bxeq r1", [](assembler &a) { a.bx(r1, COND_EQ); });
	add1("blx r2", [](assembler &a) { a.blx(r2); });
	add1("blxne r3", [](assembler &a) { a.blx(r3, COND_NE); });

	// The A32 branch offset is relative to the instruction address plus eight.
	// `b .+N` in the assembler is relative to the instruction's own address, so
	// these two spellings agreeing is exactly the +8 check.
	int const deltas[] = { 8, 12, 4, 0, -4, 1024, -1024 };
	for (int d : deltas)
	{
		add1(fmt("b . + %d", d), [&](assembler &a) {
			u8 *const here = a.pc();
			a.b(here + d);
		});
		add1(fmt("bl . + %d", d), [&](assembler &a) {
			u8 *const here = a.pc();
			a.bl(here + d);
		});
		add1(fmt("bgt . + %d", d), [&](assembler &a) {
			u8 *const here = a.pc();
			a.b(here + d, COND_GT);
		});
	}

	// forward and backward label branches must land on the bound position
	add({ "b . + 8", "mov r0, r0", "mov r0, r0" }, [](assembler &a) {
		label l = a.new_label();
		a.b(l);
		a.mov(r0, reg(r0));
		a.bind(l);
		a.mov(r0, reg(r0));
		a.finalize();
	});
	// the label is bound at the branch's own address, so this is `b .`
	add({ "mov r0, r0", "b . + 0", "mov r0, r0" }, [](assembler &a) {
		label l = a.new_label();
		a.mov(r0, reg(r0));
		a.bind(l);
		a.b(l);
		a.mov(r0, reg(r0));
		a.finalize();
	});
}

void vfp()
{
	add1("vadd.f64 d0, d1, d2", [](assembler &a) { a.vadd_f64(d0, d1, d2); });
	add1("vadd.f32 s0, s1, s2", [](assembler &a) { a.vadd_f32(s0, s1, s2); });
	add1("vsub.f64 d3, d4, d5", [](assembler &a) { a.vsub_f64(d3, d4, d5); });
	add1("vsub.f32 s3, s4, s5", [](assembler &a) { a.vsub_f32(s3, s4, s5); });
	add1("vmul.f64 d6, d7, d8", [](assembler &a) { a.vmul_f64(d6, d7, d8); });
	add1("vmul.f32 s6, s7, s8", [](assembler &a) { a.vmul_f32(s6, s7, s8); });
	add1("vdiv.f64 d9, d10, d11", [](assembler &a) { a.vdiv_f64(d9, d10, d11); });
	add1("vdiv.f32 s9, s10, s11", [](assembler &a) { a.vdiv_f32(s9, s10, s11); });

	add1("vmov.f64 d0, d15", [](assembler &a) { a.vmov_f64(d0, d15); });
	add1("vmov.f32 s0, s15", [](assembler &a) { a.vmov_f32(s0, s15); });
	add1("vabs.f64 d1, d2", [](assembler &a) { a.vabs_f64(d1, d2); });
	add1("vabs.f32 s1, s2", [](assembler &a) { a.vabs_f32(s1, s2); });
	add1("vneg.f64 d1, d2", [](assembler &a) { a.vneg_f64(d1, d2); });
	add1("vneg.f32 s1, s2", [](assembler &a) { a.vneg_f32(s1, s2); });
	add1("vsqrt.f64 d1, d2", [](assembler &a) { a.vsqrt_f64(d1, d2); });
	add1("vsqrt.f32 s1, s2", [](assembler &a) { a.vsqrt_f32(s1, s2); });
	add1("vcmp.f64 d3, d4", [](assembler &a) { a.vcmp_f64(d3, d4); });
	add1("vcmp.f32 s3, s4", [](assembler &a) { a.vcmp_f32(s3, s4); });
	add1("vcmpe.f64 d3, d4", [](assembler &a) { a.vcmpe_f64(d3, d4); });
	add1("vcmpe.f32 s3, s4", [](assembler &a) { a.vcmpe_f32(s3, s4); });
	add1("vmrs APSR_nzcv, fpscr", [](assembler &a) { a.vmrs_apsr(); });

	add1("vcvt.f64.f32 d0, s1", [](assembler &a) { a.vcvt_f64_f32(d0, s1); });
	add1("vcvt.f32.f64 s0, d1", [](assembler &a) { a.vcvt_f32_f64(s0, d1); });
	add1("vcvt.s32.f64 s0, d1", [](assembler &a) { a.vcvt_s32_f64(s0, d1); });
	add1("vcvt.u32.f64 s0, d1", [](assembler &a) { a.vcvt_u32_f64(s0, d1); });
	add1("vcvt.s32.f32 s0, s1", [](assembler &a) { a.vcvt_s32_f32(s0, s1); });
	add1("vcvt.u32.f32 s0, s1", [](assembler &a) { a.vcvt_u32_f32(s0, s1); });
	add1("vcvt.f64.s32 d0, s1", [](assembler &a) { a.vcvt_f64_s32(d0, s1); });
	add1("vcvt.f64.u32 d0, s1", [](assembler &a) { a.vcvt_f64_u32(d0, s1); });
	add1("vcvt.f32.s32 s0, s1", [](assembler &a) { a.vcvt_f32_s32(s0, s1); });
	add1("vcvt.f32.u32 s0, s1", [](assembler &a) { a.vcvt_f32_u32(s0, s1); });

	add1("vldr d0, [r1]", [](assembler &a) { a.vldr(d0, r1); });
	add1("vldr d0, [r1, #8]", [](assembler &a) { a.vldr(d0, r1, 8); });
	add1("vldr d0, [r1, #-8]", [](assembler &a) { a.vldr(d0, r1, -8); });
	add1("vldr d15, [r1, #1020]", [](assembler &a) { a.vldr(d15, r1, 1020); });
	add1("vstr d1, [r2, #16]", [](assembler &a) { a.vstr(d1, r2, 16); });
	add1("vldr s0, [r1, #4]", [](assembler &a) { a.vldr(s0, r1, 4); });
	add1("vstr s15, [r1, #-4]", [](assembler &a) { a.vstr(s15, r1, -4); });

	add1("vmov s3, r4", [](assembler &a) { a.vmov_to_s(s3, r4); });
	add1("vmov r4, s3", [](assembler &a) { a.vmov_from_s(r4, s3); });
	add1("vmov d5, r6, r7", [](assembler &a) { a.vmov_to_d(d5, r6, r7); });
	add1("vmov r6, r7, d5", [](assembler &a) { a.vmov_from_d(r6, r7, d5); });
}

} // anonymous namespace


int main()
{
	data_processing();
	constants();
	loads_stores();
	multiplies();
	bit_manipulation();
	branches();
	vfp();

	FILE *bin = fopen("ours.bin", "wb");
	FILE *asmf = fopen("ref.s", "w");
	FILE *txt = fopen("texts.txt", "w");
	if (!bin || !asmf || !txt)
	{
		fprintf(stderr, "cannot open output files\n");
		return 1;
	}

	fprintf(asmf, "\t.syntax unified\n\t.arch armv7-a\n\t.fpu neon\n\t.arm\n\t.text\n");

	size_t total = 0;
	for (testcase const &tc : g_cases)
	{
		for (size_t i = 0; i < tc.words.size(); i++)
		{
			fwrite(&tc.words[i], 4, 1, bin);
			fprintf(asmf, "\t%s\n", tc.text[i].c_str());
			fprintf(txt, "%s\n", tc.text[i].c_str());
			total++;
		}
	}

	fclose(bin);
	fclose(asmf);
	fclose(txt);
	printf("%zu instructions in %zu cases\n", total, g_cases.size());
	return 0;
}

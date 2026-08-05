// license:BSD-3-Clause
// Qualification harness for asmjit's a32_port branch.
//
//   sh tests/a32-asmjit/run.sh
//
// asmjit has an unmerged AArch32 branch (github.com/asmjit/asmjit, a32_port),
// which is a candidate to replace tools/mame-drc-arm32/arm32emit.h as the
// encoder under drcbearm32. It is described upstream as WIP, so this measures
// it rather than trusting or dismissing it: emit each case through asmjit,
// assemble the equivalent text with arm-linux-gnueabihf-as, compare.
//
// The corpus is deliberately the subset MAME's UML lowering needs, not a
// survey of the 1342 entries a32 exposes -- the question is not "is a32 good"
// but "is a32 correct where drcbearm32 would stand on it".
//
// Two outcomes are reported separately because they mean different things:
// an instruction a32 REFUSES is a coverage gap that the lowering can route
// around, while one it encodes WRONGLY is a silent miscompile.
//
// NOTE ON API USAGE: the shift operation lives in the predicate of the LAST
// operand, not on the shifted register -- a32assembler.cpp:972,986 reads
// o3.predicate(). So it is `add(rd, rn, rm, lsr(16))`, NOT
// `add(rd, rn, lsr(rm), imm(16))`. The wrong form silently encodes as LSL,
// because LSL is predicate 0, which makes it look like an asmjit bug when it
// is a call-site bug. It cost an hour here; hence this paragraph.

#include <asmjit/a32.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace asmjit;
using namespace asmjit::a32;

struct Case { std::string text; std::vector<unsigned> words; bool err; };
static std::vector<Case> g_cases;

static std::vector<std::string> g_refuse_ok, g_refuse_bad;

// records a case that MUST fail to encode; a success here is a silent
// miscompile waiting to happen, not a missing feature
template <typename Fn>
static void refuse(const char *text, Fn &&fn)
{
	Environment env;
	env.set_arch(Arch::kARM);
	CodeHolder code;
	if (code.init(env, 0) != Error::kOk) { printf("init failed\n"); exit(1); }
	Assembler a(&code);
	if (fn(a) == Error::kOk)
		g_refuse_bad.push_back(text);
	else
		g_refuse_ok.push_back(text);
}

template <typename Fn>
static void add(const char *text, Fn &&fn)
{
	Environment env;
	env.set_arch(Arch::kARM);
	CodeHolder code;
	if (code.init(env, 0) != Error::kOk) { printf("init failed\n"); exit(1); }
	Assembler a(&code);
	Error e = fn(a);
	Case c;
	c.text = text;
	c.err = (e != Error::kOk);
	if (!c.err)
	{
		code.flatten();
		CodeBuffer &buf = code.section_by_id(0)->buffer();
		for (size_t i = 0; i + 4 <= buf.size(); i += 4)
		{
			unsigned w;
			memcpy(&w, buf.data() + i, 4);
			c.words.push_back(w);
		}
	}
	g_cases.push_back(c);
}

int main()
{
	// ---- data processing, register and immediate ----
	add("add r0, r1, r2",      [](Assembler &a){ return a.add(r0, r1, r2); });
	add("sub r0, r1, r2",      [](Assembler &a){ return a.sub(r0, r1, r2); });
	add("rsb r0, r1, r2",      [](Assembler &a){ return a.rsb(r0, r1, r2); });
	add("adc r0, r1, r2",      [](Assembler &a){ return a.adc(r0, r1, r2); });
	add("sbc r0, r1, r2",      [](Assembler &a){ return a.sbc(r0, r1, r2); });
	add("and r0, r1, r2",      [](Assembler &a){ return a.and_(r0, r1, r2); });
	add("orr r0, r1, r2",      [](Assembler &a){ return a.orr(r0, r1, r2); });
	add("eor r0, r1, r2",      [](Assembler &a){ return a.eor(r0, r1, r2); });
	add("bic r0, r1, r2",      [](Assembler &a){ return a.bic(r0, r1, r2); });
	add("add r4, r5, #0x1f",   [](Assembler &a){ return a.add(r4, r5, imm(0x1f)); });
	add("sub r4, r5, #0xff00", [](Assembler &a){ return a.sub(r4, r5, imm(0xff00)); });
	add("adds r0, r1, r2",     [](Assembler &a){ return a.adds(r0, r1, r2); });
	add("subs r0, r1, r2",     [](Assembler &a){ return a.subs(r0, r1, r2); });
	add("adcs r0, r1, r2",     [](Assembler &a){ return a.adcs(r0, r1, r2); });
	add("sbcs r0, r1, r2",     [](Assembler &a){ return a.sbcs(r0, r1, r2); });
	add("ands r0, r1, r2",     [](Assembler &a){ return a.ands(r0, r1, r2); });
	add("addeq r0, r1, r2",    [](Assembler &a){ return a.add(CondCode::kEQ, r0, r1, r2); });
	add("sublt r0, r1, r2",    [](Assembler &a){ return a.sub(CondCode::kLT, r0, r1, r2); });

	// ---- shifted operands: the form the lowering leans on hardest ----
	add("add r3, r4, r5, lsl #3",  [](Assembler &a){ return a.add(r3, r4, r5, lsl(3)); });
	add("add r3, r4, r5, lsr #16", [](Assembler &a){ return a.add(r3, r4, r5, lsr(16)); });
	add("add r3, r4, r5, asr #31", [](Assembler &a){ return a.add(r3, r4, r5, asr(31)); });
	add("add r3, r4, r5, ror #7",  [](Assembler &a){ return a.add(r3, r4, r5, ror(7)); });
	// REMOVED: "add r3, r4, r5, lsr #32" -- a32 rejects it (kInvalidInstruction)
	// and the rejection path segfaults; see notes.
	add("add r3, r4, r5, lsr #32", [](Assembler &a){ return a.add(r3, r4, r5, lsr(32)); });
	add("add r3, r4, r5, asr #32", [](Assembler &a){ return a.add(r3, r4, r5, asr(32)); });
	add("mov r0, r1, lsr #32",     [](Assembler &a){ return a.mov(r0, r1, lsr(32)); });
	add("add r3, r4, r5, lsl r6",  [](Assembler &a){ return a.add(r3, r4, r5, lsl(r6)); });
	add("rrx r0, r1",              [](Assembler &a){ return a.rrx(r0, r1); });
	add("add r3, r4, r5, lsr r6",  [](Assembler &a){ return a.add(r3, r4, r5, lsr(r6)); });
	add("add r3, r4, r5, asr r6",  [](Assembler &a){ return a.add(r3, r4, r5, asr(r6)); });
	add("mov r0, r1, lsr #4",      [](Assembler &a){ return a.mov(r0, r1, lsr(4)); });
	add("ldr r0, [r1, r2, asr #5]",[](Assembler &a){ return a.ldr(r0, ptr(r1, r2, asr(5))); });
	add("ldr r0, [r1, r2, lsr #5]",[](Assembler &a){ return a.ldr(r0, ptr(r1, r2, lsr(5))); });

	// ---- single-source and comparison ----
	add("mov r7, #0x40",  [](Assembler &a){ return a.mov(r7, imm(0x40)); });
	add("movs r7, r8",    [](Assembler &a){ return a.movs(r7, r8); });
	add("mvn r7, #0",     [](Assembler &a){ return a.mvn(r7, imm(0)); });
	add("cmp r9, #0xff",  [](Assembler &a){ return a.cmp(r9, imm(0xff)); });
	add("cmn r9, r10",    [](Assembler &a){ return a.cmn(r9, r10); });
	add("tst r9, #0x80",  [](Assembler &a){ return a.tst(r9, imm(0x80)); });
	add("teq r9, r10",    [](Assembler &a){ return a.teq(r9, r10); });

	// ---- constants ----
	add("movw r1, #0x1234", [](Assembler &a){ return a.movw(r1, imm(0x1234)); });
	add("movt r2, #0xabcd", [](Assembler &a){ return a.movt(r2, imm(0xabcd)); });

	// ---- loads and stores ----
	add("ldr r0, [r1]",              [](Assembler &a){ return a.ldr(r0, ptr(r1)); });
	add("ldr r0, [r1, #4]",          [](Assembler &a){ return a.ldr(r0, ptr(r1, 4)); });
	add("ldr r0, [r1, #-4]",         [](Assembler &a){ return a.ldr(r0, ptr(r1, -4)); });
	add("ldr r0, [r1, #4095]",       [](Assembler &a){ return a.ldr(r0, ptr(r1, 4095)); });
	add("str r0, [r1, #8]",          [](Assembler &a){ return a.str(r0, ptr(r1, 8)); });
	add("ldrb r0, [r1, #1]",         [](Assembler &a){ return a.ldrb(r0, ptr(r1, 1)); });
	add("strb r0, [r1, #1]",         [](Assembler &a){ return a.strb(r0, ptr(r1, 1)); });
	add("ldrh r0, [r1, #2]",         [](Assembler &a){ return a.ldrh(r0, ptr(r1, 2)); });
	add("strh r0, [r1, #2]",         [](Assembler &a){ return a.strh(r0, ptr(r1, 2)); });
	add("ldrsb r0, [r1, #2]",        [](Assembler &a){ return a.ldrsb(r0, ptr(r1, 2)); });
	add("ldrsh r0, [r1, #-2]",       [](Assembler &a){ return a.ldrsh(r0, ptr(r1, -2)); });
	add("ldr r0, [r1, r2]",          [](Assembler &a){ return a.ldr(r0, ptr(r1, r2)); });
	add("ldr r0, [r1, r2, lsl #3]",  [](Assembler &a){ return a.ldr(r0, ptr(r1, r2, lsl(3))); });
	add("ldrd r4, r5, [r1, #8]",     [](Assembler &a){ return a.ldrd(r4, r5, ptr(r1, 8)); });
	add("strd r6, r7, [r2, #24]",    [](Assembler &a){ return a.strd(r6, r7, ptr(r2, 24)); });

	// ---- multiplies ----
	add("mul r0, r1, r2",        [](Assembler &a){ return a.mul(r0, r1, r2); });
	add("mla r0, r1, r2, r3",    [](Assembler &a){ return a.mla(r0, r1, r2, r3); });
	add("umull r0, r1, r2, r3",  [](Assembler &a){ return a.umull(r0, r1, r2, r3); });
	add("smull r0, r1, r2, r3",  [](Assembler &a){ return a.smull(r0, r1, r2, r3); });
	add("umlal r4, r5, r6, r7",  [](Assembler &a){ return a.umlal(r4, r5, r6, r7); });
	add("smlal r4, r5, r6, r7",  [](Assembler &a){ return a.smlal(r4, r5, r6, r7); });

	// ---- bit manipulation ----
	add("clz r0, r1",            [](Assembler &a){ return a.clz(r0, r1); });
	add("rbit r0, r1",           [](Assembler &a){ return a.rbit(r0, r1); });
	add("rev r0, r1",            [](Assembler &a){ return a.rev(r0, r1); });
	add("rev16 r0, r1",          [](Assembler &a){ return a.rev16(r0, r1); });
	add("ubfx r0, r1, #4, #8",   [](Assembler &a){ return a.ubfx(r0, r1, imm(4), imm(8)); });
	add("sbfx r0, r1, #8, #16",  [](Assembler &a){ return a.sbfx(r0, r1, imm(8), imm(16)); });
	add("bfi r0, r1, #4, #8",    [](Assembler &a){ return a.bfi(r0, r1, imm(4), imm(8)); });
	add("uxtb r0, r1",           [](Assembler &a){ return a.uxtb(r0, r1); });
	add("uxth r0, r1",           [](Assembler &a){ return a.uxth(r0, r1); });
	add("sxtb r0, r1",           [](Assembler &a){ return a.sxtb(r0, r1); });
	add("sxth r0, r1",           [](Assembler &a){ return a.sxth(r0, r1); });

	// ---- VFP ----
	add("vadd.f64 d0, d1, d2",   [](Assembler &a){ return a.vadd_f64(d0, d1, d2); });
	add("vadd.f32 s0, s1, s2",   [](Assembler &a){ return a.vadd_f32(s0, s1, s2); });
	add("vsub.f64 d3, d4, d5",   [](Assembler &a){ return a.vsub_f64(d3, d4, d5); });
	add("vmul.f64 d6, d7, d8",   [](Assembler &a){ return a.vmul_f64(d6, d7, d8); });
	add("vdiv.f64 d9, d10, d11", [](Assembler &a){ return a.vdiv_f64(d9, d10, d11); });
	add("vabs.f64 d1, d2",       [](Assembler &a){ return a.vabs_f64(d1, d2); });
	add("vneg.f64 d1, d2",       [](Assembler &a){ return a.vneg_f64(d1, d2); });
	add("vsqrt.f64 d1, d2",      [](Assembler &a){ return a.vsqrt_f64(d1, d2); });
	add("vcmp.f64 d3, d4",       [](Assembler &a){ return a.vcmp_f64(d3, d4); });
	add("vldr d0, [r1, #8]",     [](Assembler &a){ return a.vldr_64(d0, ptr(r1, 8)); });
	add("vstr d1, [r2, #16]",    [](Assembler &a){ return a.vstr_64(d1, ptr(r2, 16)); });
	add("vmov s3, r4",           [](Assembler &a){ return a.vmov(s3, r4); });
	add("vmov r4, s3",           [](Assembler &a){ return a.vmov(r4, s3); });
	add("vmov d5, r6, r7",       [](Assembler &a){ return a.vmov(d5, r6, r7); });
	add("vmov r6, r7, d5",       [](Assembler &a){ return a.vmov(r6, r7, d5); });

	// ---- must be REFUSED ----
	// There is no LSR #0 or ASR #0 in A32: an encoded amount of 0 means 32.
	// Upstream a32_port accepts these and silently emits shift-by-32 -- the
	// same word arm-linux-gnueabihf-as gives for `lsr #32`. Accepting a
	// spelling that means something else is worse than rejecting a legal one,
	// which is why this is checked as its own class.
	refuse("add r3, r4, r5, lsr #0", [](Assembler &a){ return a.add(r3, r4, r5, lsr(0)); });
	refuse("add r3, r4, r5, asr #0", [](Assembler &a){ return a.add(r3, r4, r5, asr(0)); });
	refuse("add r3, r4, r5, ror #0", [](Assembler &a){ return a.add(r3, r4, r5, ror(0)); });

	// ---- emit ----
	FILE *bin = fopen("ours.bin", "wb");
	FILE *asmf = fopen("ref.s", "w");
	FILE *txt = fopen("texts.txt", "w");
	fprintf(asmf, "\t.syntax unified\n\t.arch armv7-a\n\t.fpu neon\n\t.arm\n\t.text\n");

	int failed = 0, emitted = 0;
	for (const Case &c : g_cases)
	{
		if (c.err || c.words.size() != 1)
		{
			printf("NOT ENCODED: %s%s\n", c.text.c_str(),
					c.err ? "" : "  (multi-word or empty output)");
			failed++;
			continue;
		}
		fwrite(&c.words[0], 4, 1, bin);
		fprintf(asmf, "\t%s\n", c.text.c_str());
		fprintf(txt, "%s\n", c.text.c_str());
		emitted++;
	}
	fclose(bin); fclose(asmf); fclose(txt);
	printf("%d encoded, %d unexpectedly refused, of %zu cases\n", emitted, failed, g_cases.size());

	for (const std::string &t : g_refuse_bad)
		printf("ACCEPTED BUT MUST NOT BE: %s\n", t.c_str());
	printf("%zu of %zu invalid forms correctly refused\n",
			g_refuse_ok.size(), g_refuse_ok.size() + g_refuse_bad.size());

	return (failed || !g_refuse_bad.empty()) ? 1 : 0;
}

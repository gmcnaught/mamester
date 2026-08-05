# `drcbearm32` — a 32-bit ARM back-end for MAME's UML

Status: **spec, drafted 2026-08-05.** Reverses the descope recorded in
[`progress.md`](../progress.md) and in `tools/lrmame-drc-scan.sh`'s header, on
the operator's instruction, and on the strength of one fact that the descope did
not have.

## The fact that changes the decision

The earlier assessment sized this project as *"backport `drcbearm64.cpp`,
~5,700 lines, made harder than the arm64 one by ARMv7 having ~14 usable GPRs
against 31 and no 64-bit registers for a 64-bit-register IR."*

Both halves of that sentence are true and neither is the relevant one, because
**MAME had a 32-bit back-end until three releases ago**:

| tag | `src/devices/cpu/drcbex86.cpp` |
| --- | --- |
| `mame0250` … `mame0287` | present |
| `mame0289` (this port's engine) | **absent** |

`drcbex86` is the x86-32 back-end. It ran the entire UML on a host with **seven**
usable GPRs and no 64-bit registers, for roughly twenty years, on every DRC CPU
MAME has. Every problem the descope cited as making ARM32 hard — 64-bit UML
values on 32-bit registers, register scarcity, flag reconstruction, 64-bit
shifts and multiplies and divides synthesised from 32-bit parts — is *already
solved there*, in tree, under two decades of driver testing.

So the base for this work is **`drcbex86.cpp` @ `mame0287`, not
`drcbearm64.cpp`.** `drcbearm64` contributes the ARM-specific knowledge
(condition codes, carry polarity, shift/flag semantics) and nothing else;
`drcbex86` contributes the algorithms.

Better still, the interface it must satisfy barely moved between 0.287 and
0.289. `uml.h` — the entire IR, every opcode, every parameter type — is
**byte-identical** across the two. The whole delta a resurrected back-end must
absorb is:

1. `drcbe_interface` gains `virtual void hash_invalidate_range(u32, u32)`.
2. `drcuml_state` and `drc_hash_table` gain a `max_sequence_length` constructor
   argument; `drc_hash_table` gains `invalidate_range()`.
3. The `make_drcbe_*` factory signature.

That is the honest size of "port to current MAME". The work is not the port; it
is the instruction emitter, discussed next.

## The encoder: asmjit's vendored copy has no AArch32, but upstream has a branch

**Corrected 2026-08-05, after the operator pointed at
[`asmjit/asmjit@a32_port`](https://github.com/asmjit/asmjit/tree/a32_port).**
This section previously said flatly that asmjit has no AArch32 back-end. That
is true of the copy MAME vendors and false of asmjit upstream, and the
difference matters enough to restate carefully.

**What MAME vendors** (`3rdparty/asmjit/asmjit/arm/`) is `a64*` and nothing
else. `core/archtraits.h` *enumerates* `kARM = 5` and `kThumb = 7`, which is
what makes this easy to get wrong — the enumerator exists, the back-end behind
it does not.

**What upstream has** is a real AArch32 port on an unmerged branch: one WIP
commit (`594cb9e`, 2025-11-29, by asmjit's author) adding **21,406 lines** —
`a32assembler.cpp` alone is 11,016 — with Assembler, Builder, Compiler, A32
*and* Thumb, 1,342 emitter entries, an instruction DB and a tablegen. It is
not a stub.

Three things make it a serious candidate rather than a curiosity:

1. **It is the same asmjit version line MAME vendors** — both report
   `ASMJIT_LIBRARY_VERSION 1.21.0`, and only 16 shared files differ. The
   branch's own changes to `core/` are ~70 lines across five files, so the
   `a32*` sources plausibly drop into the vendored tree.
2. **It is correct where we need it.** `tests/a32-asmjit/` diffs its output
   against `arm-linux-gnueabihf-as` over the subset the lowering needs — data
   processing with every shift form, both load/store families, `ldrd`/`strd`,
   multiplies, the bitfield ops, VFP: **85 of 85 encodings match exactly.**
3. **`drcbex86` is written against asmjit.** Retargeting it inside the same
   framework — same `CodeHolder`, `Label`, `Mem`, relocation and
   buffer-growth machinery — is far more mechanical than retargeting it onto a
   bespoke encoder, and that lowering is ~7,700 lines, i.e. the dominant
   remaining cost of this whole project.

Two defects found, both small and both localised:

- **`lsr #32` / `asr #32` are rejected** (`kInvalidInstruction`). These are
  legal A32 — for LSR and ASR an encoded amount of 0 *means* 32 — and they are
  not exotic here: synthesising 64-bit shifts on a 32-bit host reaches for
  shift-by-32 constantly.
- **The rejection path segfaults.** `EmitterUtils::log_instruction_failed()`
  calls `self->_funcs.format_instruction(...)`, which the a32 emitter never
  installs, so a refused instruction dereferences null instead of returning its
  error. For a DRC that turns an unsupported operand combination into a crash
  with no diagnostic.

**A trap worth recording, because it cost real time and produced a false bug
report before it was caught:** the shift operation lives in the predicate of
the **last** operand, not on the shifted register —
`a32assembler.cpp:972,986` reads `o3.predicate()`. So it is
`add(rd, rn, rm, lsr(16))`, never `add(rd, rn, lsr(rm), imm(16))`. The wrong
form encodes silently as **LSL**, because LSL is predicate 0. That reads
exactly like "asmjit ignores the shift type" — six cases disagreed with the
assembler in precisely that pattern — and it is a call-site bug. The lesson is
the harness's, not asmjit's: a differential test tells you *that* something
disagrees, never *whose fault it is*, and the encoder is not guilty until the
call site is cleared.

### Which encoder, then

`tools/mame-drc-arm32/arm32emit.h` (391 instructions, all matching the
assembler) is kept, but its role changes: it is the **fallback and the oracle**,
not necessarily the production path. The recommendation is
**qualify-then-adopt** — widen `tests/a32-asmjit/` toward the full set the
lowering ends up using, fix the two defects locally, offer them upstream, and
adopt a32 as the encoder if it keeps passing. Deciding this before the lowering
is written is the point: the two choices produce very different lowering code,
and switching later is a rewrite.

The residual risk with a32 is not correctness-so-far but **staleness and
churn**: the branch is unmerged, four months behind master, and its API may
move under us. Vendoring 21k lines of WIP into a submodule we do not control is
a maintenance position, not a free win.

## Target and its consequences

**Cortex-A9, ARMv7-A, A32 (not Thumb-2), NEON/VFPv3-D32, hard-float EABI** —
the same `-marm -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard` the other three
engines build with. Four properties of that target are load-bearing:

- **`movw`/`movt` exist** (ARMv7). Any 32-bit constant or absolute address is
  two instructions and no literal pool, no PC-relative window, no pool-drain
  bookkeeping in the middle of a generated sequence. This is the single biggest
  simplification available and it is why the back-end targets ARMv7 rather than
  ARMv5/v6 — dropping the pool removes an entire class of code-cache problem
  that `drcbex86` never had to have and we should not invent.
- **There is no integer divide.** `SDIV`/`UDIV` are ARMv7-R/M, and on
  ARMv7-A they arrive only with the idiv extension (Cortex-A15 and later). The
  A9 has neither. `DIVU`/`DIVS` therefore lower to a call, exactly as
  `drcbex86` lowers the 64-bit cases.
- **Condition codes are on every instruction**, so the `SETcc`/`CMOVcc`
  sequences `drcbex86` spends instructions on collapse to a predicated `mov`.
  This is where ARM32 is genuinely *better* than the base.
- **Fourteen GPRs against x86-32's seven.** The register scarcity the descope
  worried about is scarcity relative to *arm64*. Relative to the back-end we are
  actually copying, it is a doubling.

### Execution model (v1)

`drcbex86` pins I0–I3's low halves to `EBX/ESI/EDI/EBP` and leaves everything
else in memory. v1 here deliberately does **not** map UML registers to host
registers at all:

| register | role |
| --- | --- |
| `r0`–`r3` | scratch / AAPCS argument registers |
| `r4`–`r10` | scratch |
| `r11` | **pinned**: `&m_state` (UML register file base) |
| `r12` (`ip`) | scratch |
| `r13`–`r15` | `sp`, `lr`, `pc` |
| `d0`–`d7` | scratch / FP arguments |
| `d8`–`d15` | callee-saved, unused by generated code in v1 |

Every UML integer register is a `[r11, #offset]` load/store, every 64-bit value
is a register pair, and `drcbe_info::direct_iregs` is reported as **0** so no CPU
front-end assumes otherwise. That is slower than `drcbex86`'s four pinned
registers and much slower than what this can eventually be — and it is the right
v1, because register mapping is an optimisation that can be added under a
working, *tested* lowering, while a lowering built around register mapping
cannot easily be tested before it is finished. The pinned base register is the
one exception, and it pays for itself immediately: it is what lets the register
file be reached in one instruction instead of a `movw`/`movt` pair per access.

### Flags

UML carries `C V Z S U`. ARM's `NZCV` maps `N→S`, `Z→Z`, `C→C`, `V→V`, with `U`
reachable only from float compares — a direct correspondence x86 also has. The
one place it breaks is the one `drcbearm64` already had to solve:

> **On ARM, `SUB`/`CMP` set `C` to *not-borrow*; UML (following x86) defines it
> as *borrow*.** After any subtract the hardware carry is the logical inverse of
> the UML carry.

`drcbearm64` handles this by tracking, at generate time, which of three states
the stored carry is in — `CANONICAL` (matches UML), `LOGICAL` (inverted),
`POISON` (neither) — and inverting lazily, only when a consumer actually needs
it (`drcbearm64.cpp:447-450, 1457-1489`). v1 copies that state machine
wholesale. It is the single most error-prone part of the lowering and it already
has a known-good design in tree; there is no reason to re-derive it.

Two further ARM shift/flag details the lowering must respect, both of which
differ from x86 and neither of which `drcbex86` can warn us about:

- A shift **by zero leaves `C` untouched** rather than defined, and the flag
  `C` after `LSL`/`LSR`/`ASR`/`ROR` is the last bit shifted out.
- There is **no `ROL`** (`ROR #(32-n)`), and **no `RCL`/`RCR`**, so UML's
  `ROLC`/`RORC` are synthesised.

## Enablement: on wherever the AArch64 back-end would be

**Operator decision 2026-08-05.** `PLATFORM=arm` is folded **into**
`CPU_INCLUDE_DRC_NATIVE` rather than kept in a parallel `CPU_INCLUDE_DRC_ARM32`
flag beside it:

```lua
CPU_INCLUDE_DRC_NATIVE = CPU_INCLUDE_DRC and (not FORCE_DRC_C_BACKEND)
    and ((PLATFORM == "x86") or (PLATFORM == "arm64") or (PLATFORM == "arm"))
CPU_INCLUDE_DRC_ARM32  = CPU_INCLUDE_DRC_NATIVE and (PLATFORM == "arm")
```

The point of folding rather than paralleling: everything upstream gates on "the
native DRC is available" then turns on for ARM32 in exactly the places it turns
on for arm64 — including the i386 disassembler at the bottom of `cpu.lua`, which
arm64 already pulls in. A parallel flag would have to be added to each of those
conditions by hand, and any condition upstream adds later would silently miss
ARM32.

The **files** stay architecture-split (`CPU_INCLUDE_DRC_ARM32` selects
`drcbearm32.cpp`, the others get `drcbearm64.cpp`/`drcbex64.cpp`), because
"enabled in the same places" must not become "same source list" — the 64-bit
back-ends do not compile for a 32-bit target.

`tools/build-lrmame.sh` therefore defaults to **DRC on**, and two flags come off
rather than one going on: `FORCE_DRC_C_BACKEND` (the obvious one) and `NOASM`,
which defines `MAME_NOASM` and would make `drcuml.cpp`'s chain pick `drcbe_c`
even with the back-end compiled in.

**What this costs until the lowering lands.** With DRC on, a driver on a
DRC-backed CPU **aborts** on the first unlowered opcode instead of running slowly
through `drcbec`. That is the intended trade — a hard stop is how the remaining
work gets found, and a silent wrong answer is what is being avoided — but it
means `DRC=0` is the configuration for shipping or benching those ~3.2% of
drivers. Everything else, including the whole target library and the pacman
gate, never reaches `drcuml` and is unaffected either way.

## Verification

The encoder is the part that can be wrong silently and catastrophically — a
mis-encoded instruction is not a compile error, it is a wrong answer inside a
game five minutes in. It is also the part that can be checked completely,
without the device and without MAME, because a reference encoder is sitting in
the cross container:

> **Differential test against `arm-linux-gnueabihf-as`.** For every emitter entry
> point, enumerate the operand space (registers, shift kinds and amounts,
> immediate boundaries, condition codes), emit both our bytes and the equivalent
> `.s` text, assemble the text, and compare word for word.

That converts "is the encoder right" from a judgement into a build step, and it
is the reason the encoder is being written as a standalone header rather than
inline in the back-end.

The lowering above it gets a coarser check, in this order:

1. It compiles and links into the `pacman` subset build (which uses no DRC CPU
   at all — this only proves the build wiring).
2. A DRC subset build — `psikyosh` is the named gap target, SH-2 — links and
   boots.
3. `MISTER_FRAME_HASH` over N frames matches between a `FORCE_DRC_C_BACKEND=1`
   build and a native build of the same driver. Same input, same frames, same
   hash: that is the real correctness gate, and it is an A/B the harness already
   supports.
4. `MISTER-BENCH fps=` between the two says whether any of it was worth doing.

Nothing in steps 2–4 can be run in a container; they need the device.

## What this buys, restated honestly

`tools/lrmame-drc-scan.sh` measured the reachable set: **147 of 4652 driver
files, 3.2%**, and most of that is out of scope for other reasons (SGI, Mac,
Jaguar, skeletons). The genuine casualties this recovers are the SH-2/SH-3
arcade boards — **`psikyosh`** (a `CLAUDE.md` gap target), `stv`, `feversoc`,
`cv1000` — plus MIPS3 and PowerPC families that were never on the table on an
A9 and probably still are not.

It does **not** move the 0.289 gate. Pac-Man is a Z80 and never touches
`drcuml`; the question of whether 0.289's device model, `emumem` dispatch and
scheduler fit in an 800 MHz A9 is untouched by anything in this document. If
that gate fails, this back-end has no engine to live in.

That ordering is a real risk to state plainly: **this work is downstream of a
gate that has not been run.** It is being done first because the operator asked
for it, and it is being written to be gate-independent — the encoder and the
lowering are tied to MAME's UML, not to the libretro host, so they survive a
change of engine as long as the engine is a MAME new enough to have `drcuml`.

## Layout

Following the repo's existing convention for code that must end up inside a
vendored tree (`tools/mame-frontend/mister-backend/` → mame4all), the sources
live here and are injected at build time; the submodule stays pristine.

```
tools/mame-drc-arm32/
    arm32emit.h        A32 encoder
    drcbearm32.h       factory declaration
    drcbearm32.cpp     UML lowering
    inject.sh          copy into vendor/lrmame + patch drcuml.cpp, cpu.lua
tests/arm32emit/       differential test against arm-linux-gnueabihf-as
```

## Licensing

MAME is **GPL-2.0-or-later / BSD-3-Clause** from 0.172 on, so a derived back-end
is unproblematic where the pre-2016 non-commercial licence governing mame4all
and 2003-plus would have been awkward. `drcbex86.cpp` and `drcbearm64.cpp` are
both `BSD-3-Clause`; `drcbearm32` derives from both and carries the same, with
`copyright-holders` naming the upstream authors it derives from alongside this
port. `CLAUDE.md`'s licensing note still describes only the old licence and is
wrong for this engine — noted in `progress.md`, still outstanding.

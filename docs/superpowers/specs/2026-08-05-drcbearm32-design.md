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

## Why this is still real work: asmjit has no AArch32

`drcbex86` @ 0.287 no longer carries its own encoder — the old `x86emit.h` is
gone and it emits through `asmjit::x86::Assembler`. `drcbearm64` likewise uses
`asmjit::a64::Assembler`. The natural assumption is that asmjit covers ARM32 too.
It does not:

```
3rdparty/asmjit/asmjit/arm/   →  a64assembler, a64builder, a64compiler,
                                 a64emitter, a64instdb, a64operand, …
                                 (nothing a32, nothing thumb)
```

`core/archtraits.h` *enumerates* `kARM = 5` and `kThumb = 7` in `Arch`, which is
what makes this easy to get wrong — the enumerator exists, the back-end behind
it does not. There is no `a32::Assembler` to target.

So this project is two pieces, and the second is the one with genuine risk:

- **`arm32emit.h`** — an A32 instruction encoder written for this back-end,
  covering the subset the UML lowering needs. New code, but bounded and, more
  importantly, *mechanically verifiable* (see "Verification").
- **`drcbearm32.cpp`** — the UML lowering, structurally `drcbex86` with its
  x86 emission retargeted and its flag handling replaced with ARM's.

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

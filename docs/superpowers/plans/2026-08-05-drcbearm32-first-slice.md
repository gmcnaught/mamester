# drcbearm32 — first lowering slice: RESTORE, SAVE, EXIT

> **DONE, and overtaken in the same cycle.** The slice below was written as
> three opcodes and turned out to be the gate it predicted: with SAVE, RESTORE
> and EXIT in place the corpus became an instrument, and the rest of the
> lowering followed immediately rather than over several cycles. The whole of
> `uml.h` is lowered; `tests/drc-diff/` runs 77 cases over twelve input states.
> Two of the plan's specifics were revised by building rather than by
> reasoning, and both are worth keeping:
>
> * **SAVE/RESTORE are subroutines, not inline sequences**, exactly as
>   `drcbex86` has them — but the flag byte cannot be part of the copy, because
>   the live flags are in APSR and the software flags register rather than in
>   the machine state. SAVE takes the byte as an argument and RESTORE hands it
>   back.
> * **The flag permutation is not a lookup table.** The plan proposed two
>   tables in the near cache, mirroring `flags_map`/`flags_unmap`. UML's Z and S
>   sit at bits 2 and 3 and APSR's at 30 and 31, so one mask and one shift place
>   both, and V moves with a second — cheaper than a load, and no cache
>   footprint.
>
> `FLAG_U` was descoped here and is no longer: it is carried in bit 4 of the
> software flags register, FCMP writes it, and the conditional opcodes test it
> by borrowing NZCV and giving it back on both paths.
>
> See `docs/superpowers/progress.md`, Stage 11.


Plan for the cycle after the differential harness went live. Design:
[`../specs/2026-08-05-drcbearm32-design.md`](../specs/2026-08-05-drcbearm32-design.md).
Harness: [`../../../tests/drc-diff/README.md`](../../../tests/drc-diff/README.md).

## Why these three, and not "control flow"

The ARM baseline is `pass=0 fail=0 unimpl=48 skipped=0`, and **all 48 name the
same opcode** — 23, `OP_RESTORE` — including the float and jump cases. Every
harness case is `RESTORE` inputs → the opcode under test → `SAVE` outputs →
`EXIT`, so `RESTORE` is the first instruction in every block and nothing after
it is ever reached.

So the corpus is measuring one missing opcode 48 times. These three are what
turn it into a real instrument, and the payoff is disproportionate: three
opcodes take the report from one bit to forty-eight.

This is the third time the ordering has been revised by building rather than
reasoning (`REMAINING WORK` said control flow; that turned out to stand on the
parameter layer; the parameter layer turned out to need the harness first).
`SAVE`/`RESTORE` are not in the control-flow bucket at all — they are state ops
— and nothing about reading the design would have surfaced them as the gate.

## What the work actually is

`drcbex86`'s `op_save`/`op_restore` are **eleven lines each**: put the state
pointer in a register, `call` a stub. The substance is in the two stubs
generated once in `reset()` (`drcbex86.cpp:1182-1256`), beside the entry/exit
stubs `drcbearm32::reset()` already generates. So the shape is settled; only the
ARM specifics are open.

### 1. `m_save` / `m_restore` stubs in `reset()`

Copy `drcuml_machine_state` — `r[]` (lo/hi per register), `f[]`, `exp` (u32),
`fmod` (u8), `flags` (u8) — between the caller's buffer and the live state.

Two things make this *easier* than the x86 original:

- The pointer argument is **r0** (AAPCS), not `ecx`.
- The live state is at **`[r11, #offset]`** because r11 is pinned to
  `&m_state`, where x86 needed an absolute `MABS` per access. And v1 maps no UML
  register to a host register, so `int_register_map[]` and its whole special
  case disappear — every register is a plain load/store pair.

### 2. Flags, which is the only hard part

ARM has four condition flags in APSR (N=31, Z=30, C=29, V=28); UML has five —
`FLAG_C`=bit0, `FLAG_V`=1, `FLAG_Z`=2, `FLAG_S`=3, `FLAG_U`=4. Mapping is
N↔S, Z↔Z, C↔C, V↔V, with **U unrepresentable**.

- SAVE: `mrs` APSR → permute NZCV into the UML byte → store.
- RESTORE: load byte → permute into NZCV → `msr APSR_nzcvq`.
- Two lookup tables in the near cache (16-entry nzcv→uml, 32-entry uml→nzcv),
  mirroring `flags_map`/`flags_unmap`, rather than a shift/mask chain.

**`FLAG_U` is deliberately out of scope for this cycle.** It is float-only, the
float cases stay `UNIMPL` after this slice anyway, and the harness's `masks_for()`
already excludes undefined flags from the compare. Read how `drcbearm64` carries
U before lowering any float op — do not invent a scheme here.

**The carry inversion is a call-site problem, not a stub problem.** ARM sets C to
NOT-borrow after a subtract, UML defines it as borrow, and `m_carry_state`
tracks which of the three states the stored carry is in. The stub is generated
once and cannot know that, so it assumes canonical: `op_save` emits the
normalisation before the call when `m_carry_state` says inverted. `op_restore`
sets `m_carry_state = CANONICAL` — it has just loaded a known-good NZCV.

### 3. `op_exit`

Return value into r0, then branch to `m_exit`. Two ARM-specific points:

- `m_exit` is a far absolute address, so a conditional exit cannot be a
  predicated branch. Emit **invert-and-skip**: `b<!cond> skip` over a
  `movw/movt r12` + `bx r12`.
- The parameter load must not disturb the flags the condition is about to test.
  `movw`/`movt`/`ldr` do not set flags, so loading r0 *before* the test is
  safe — the same ordering `drcbex86` relies on.

### 4. The minimum parameter slice

Only what these three need — **not** the whole `be_parameter` layer:

- `emit_mov_r32_p32(a, Gp dst, be_parameter const &)`, flag-preserving, for
  immediate / int-register / memory sources.
- A UML→ARM condition mapping for `COND_Z/NZ/S/NS/C/NC/V/NV/A/BE/G/LE/L/GE`.
  `COND_U`/`COND_NU` are float-only: leave them a `fatalerror`.
- `be_parameter` itself, cut down — v1 pins nothing to host registers, so the
  whole `select_register` family collapses to "load it into the scratch you
  were going to use anyway".

## Exit criteria

The cycle is **not** "48 passes" — most cases test opcodes still unlowered.

1. **The reported opcode changes per case.** `alu` cases report `OP_ADD`,
   `shift` reports `OP_SHL`, and so on, instead of all 48 reporting `OP_RESTORE`.
   That alone is the deliverable: the corpus becomes an instrument.
2. **`control/empty` and `control/nop` PASS.** They need only
   HANDLE/NOP/SAVE/RESTORE/EXIT, so they are the first genuine end-to-end
   proof that generated ARM code runs and returns correct state.
3. **`--host` stays 48/48.** Any movement there is a harness regression, not
   progress.
4. No crashes. A `SIGSEGV` inside the code cache means the emitted code is
   wrong; the harness's handler names case, back-end and phase.

## Loop

The ARM core with the hook is built, so this is no longer a cold build:

```sh
# edit tools/mame-drc-arm32/drcbearm32.cpp
SUBTARGET=drcsh SOURCES=src/mame/psikyo/psikyosh.cpp tools/build-lrmame.sh
bash tests/drc-diff/run.sh            # arm
bash tests/drc-diff/run.sh --host     # regression check
```

`build-lrmame.sh` re-runs the injector, so editing the copy in
`tools/mame-drc-arm32/` is what takes effect — never edit
`vendor/lrmame/src/devices/cpu/drcbearm32.cpp`, which is overwritten. Incremental
rebuild is one TU plus a 40 MB link.

## Risks

- **asmjit a32's `mrs`/`msr` may not be in the corpus** `tests/a32-asmjit/`
  qualified (88 encodings, chosen for the lowering as then imagined). If they
  are absent or wrong, add them there **first** — that harness exists because
  the a32 port is upstream WIP and has already been caught silently
  mis-encoding. Do not debug a flag bug through the differential harness when
  the encoder is the suspect.
- **The stubs are generated in the invariant cache** alongside entry/exit, so a
  mistake there breaks every case at once rather than one — expect the first
  failure to be global and do not read that as "the whole approach is wrong".
- `masks_for()` derives the compare mask from the block, so a `SAVE` that
  writes a register the case did not define will not be caught here. That is
  intended, and is why the `--host` control run has to stay clean.

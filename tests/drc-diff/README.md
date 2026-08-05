# drc-diff — differential testing for the ARM32 back-end

Run a UML block through the native back-end and through `drcbec`, execute both,
and diff the resulting machine state. That is what makes each opcode group in
`drcbearm32.cpp`'s REMAINING WORK an independently verifiable unit rather than
an aspiration.

```sh
tests/drc-diff/run.sh --host       # calibrate: drcbe_c vs drcbe_x64
tests/drc-diff/run.sh              # the real thing: drcbe_c vs drcbe_arm32
tests/drc-diff/run.sh alu          # one group, or one case, by name
tests/drc-diff/run.sh --probe      # just the no-content probe, no diff
```

**Status: `--host` is clean — 48 of 48 cases agree between `drcbe_c` and
`drcbe_x64`.** The ARM32 run has not been done yet; with only the structural
opcodes lowered it will report `UNIMPL` for everything, `SAVE`/`RESTORE` first.

## Baseline, 2026-08-05

```
--host (drcbe_c vs drcbe_x64)      pass=48 fail=0 unimpl=0  skipped=0
--arm  (drcbe_c vs drcbe_arm32)    pass=0  fail=0 unimpl=48 skipped=0
```

The ARM run is the loop working, not the back-end working: 48 clean `UNIMPL`
reports, no crash, no hang, exit 0. From here every opcode lowered turns one or
more of those into `PASS` or `FAIL`, which is the whole point of building this
before writing the lowering.

**All 48 name the same opcode, and it is not any of the opcodes the cases are
about.** Every one reports UML opcode 23 — `OP_RESTORE` — including `fmov`,
`getflgs` and `jmp.label`. That is the harness's own prologue: a case begins by
`RESTORE`-ing its input machine state and ends with `SAVE` + `EXIT`, so
`RESTORE` is the first instruction in every block and nothing downstream of it
is ever reached.

So the corpus is currently measuring exactly one missing opcode 48 times, and
the first slice of lowering is **not a matter of preference — it is
`RESTORE`, `SAVE` and `EXIT`.** Until all three exist no case can report
anything else, and the moment they do, all 48 start reporting on the opcode they
are actually named for. That is a much better first target than "control flow"
in the abstract: three opcodes, and the corpus goes from one bit of information
to forty-eight.

## Run `--host` first, and believe nothing until it is clean

`--host` diffs `drcbe_c` against **`drcbe_x64`** — a back-end with two decades
of drivers behind it. Every case that fails there is the harness's fault or the
corpus's, never the back-end's.

This is not caution for its own sake. **A differential test proves that two
things disagree and never whose fault it is**, and this project has already
paid for that lesson once: `tests/a32-asmjit/`'s first run showed six
disagreements in a pattern that read exactly like "asmjit drops the shift
type", and the bug was in the calling code. The wrong answer was *plausible*,
which is what made it dangerous. `--host` is the control that makes an ARM32
failure mean something.

**It earned its keep immediately.** The first calibration run crashed, and the
next reported 32 of 48 cases failing. Every one was the harness's or the
corpus's fault; `drcbe_x64` was correct throughout. What it caught:

- **`drc_cache` is two-phase in 0.289** — the constructor allocates nothing and
  leaves every pointer null; `allocate_cache()` maps the memory, and every CPU
  core calls it from `device_start` (`sh.cpp:41`). Omitting it does not fail
  loudly: `alloc_near()` just returns null and the crash lands in whichever
  back-end constructor first writes through it. That reads exactly like a
  broken back-end.
- **The cache floor is set by the hash table, not by the generated code.** At
  `addrbits=32, ignorebits=1` the empty L1/L2 tables alone are
  `(8 << 15) + (8 << 16)` = 768 KB out of the *main* cache. 1 MB segfaulted;
  the SH cores use 32 MB, which is what the harness now uses.
- **Three kinds of state UML leaves undefined**, all of which the corpus was
  comparing — see below.
- **Two real corpus bugs**: `FFRFLT` converts *between* float widths, so a
  size-matched `fdfrflt(F1, F2, SIZE_QWORD)` is not a no-op but an invalid
  opcode — `drcbe_c` refusing it is what flagged it, which is the `BAD-CASE`
  path working as designed. And a `GETFLGS` over `FLAGS_ALL` in an integer
  context reads back `FLAG_U`, which is meaningless there.

The crash handler and `masks_for()` both exist because of that run. Neither was
in the original design.

## What is undefined, and why the diff has to know

A differential test that compares undefined state reports differences that are
not bugs. Three showed up, and each would have been read as an ARM32 lowering
bug:

- **A 4-byte operation on a 64-bit register defines the low 32 bits only.**
  `drcbe_c` zeroes the upper half, `drcbe_x64` preserves it, and both conform.
- **`FLAG_U` is floating-point only.** `drcbe_x64` reconstructs flags with
  `lahf` and maps x86's *parity* flag onto `FLAG_U`, so after any integer op it
  holds parity while `drcbe_c` holds zero.
- **Flags are undefined until an opcode computes them.** `RESTORE` *loads*
  flags, which is not the same thing: a back-end may keep UML flags in the
  host's own flag register and rematerialise them only when an opcode needs
  them, so restored flags need not survive intervening opcodes that define
  none. Calling that a `drcbe_x64` defect on this evidence would have been
  wrong.

So the compare mask is computed from the block itself, in `masks_for()`: a
register is compared to the width of the last thing that wrote it, and flags to
the set the last flag-*producing* opcode defines. `is_param_out()` and
`output_flags()` are public, so this is read off the IR rather than
hand-maintained per case.

One trap in that computation, which the `fdtoint` case found:
**`instruction::size()` is not always the destination's width.** For `FTOINT`
it is the width of the float *source*, while the integer destination's width is
the `SIZE_` parameter — so `fdtoint(I0, F1, SIZE_DWORD, ...)` is a size-8
instruction that defines 32 bits of an I register.

## Crashing is a result too

A back-end being written does not only `fatalerror` on a missing opcode — it
also emits wrong code and jumps into it. That arrives as SIGSEGV somewhere
inside the code cache, with no stack worth reading and no indication of which
of forty-eight cases was running. The harness tracks case, back-end and phase
in three strings and prints them from a signal handler (`write(2)` only, so it
is async-signal-safe), then exits 3.

That is what turned the first bare segfault into
`case='empty' backend=drcbe_c phase=drcuml_state`, which named the bug in a
single run.

Exit status: **0** everything agrees, **1** a real diff, **2** no native
back-end compiled in, **3** crashed.

## How the harness works

### It has to run inside MAME, but it needs no romset and no hardware

A standalone harness was tried first and does not work. `generate()` needs a
`drcuml_block`, which needs a `drcuml_state`, whose constructor is:

```cpp
m_beintf(device.machine().options().drc_use_c() ? make_drcbe_c(...) : make_drcbe_native(...))
```

`device_t::machine()` and `running_machine::options()` are header-inline, so
they never appear as undefined symbols — `nm` on `drcuml.o` shows no `device_t`
reference at all, which makes a fake device look viable and it is not. It
dereferences at runtime. Linking the DRC objects against stubs therefore builds
and then crashes.

`nogame.c` settles the alternative. It loads the core with **NULL content** —
the core advertises `RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME` — and MAME starts
its `___empty` driver:

```
retro_load_game(NULL) = OK -- machine is running
retro_run() survived
```

So a real `running_machine`, a real `device_t` and therefore a real
`drcuml_state` are all reachable with no ROMs, no MiSTer and no device.

### Reaching the machine

No call-site plumbing. `mame_machine_manager::instance()->machine()` is a global
the libretro OSD already uses (`libretro.cpp:260,352`), so the hook is a free
function called from the top of `retro_run()`, and `root_device()` is the
`device_t` every DRC object needs. It is compiled in unconditionally and
returns immediately unless `MAMESTER_DRC_DIFF` is set, so a core with the
harness in it is the same core in normal use.

### Getting both back-ends into one process

*Not* by flipping `drc_use_c()`. That option is read once, inside
`drcuml_state`'s constructor (`drcuml.cpp:158`), and it selects the single
back-end that state will own — one `drcuml_state` can never hold both. The
factories are exported, so the harness calls them directly:

```cpp
drc::make_drcbe_c  (drcuml_state &, device_t &, drc_cache &, flags, modes, addrbits, ignorebits)
drc::make_drcbe_x64(...)   // or _arm64, or _arm32 — same signature
```

**Each back-end gets its own `drc_cache`.** Not tidiness: `drc_hash_table`,
`drc_map_variables`, `drc_label_list` and the `drcuml_machine_state` the
generated code actually operates on are all per-back-end members allocated out
of the cache the back-end was handed (`drcbec.cpp:379-381,444`), so sharing one
would have the two code streams writing over each other's register file.

### One block, fed to both — the open question, now answered

The previous version of this file left this to be checked before relying on it.
It holds, and the reason is narrow enough to be worth writing down:

**A back-end's `generate()` reads nothing from the block but `invariant()`, and
uses it only as the channel for `abort()`.** In the whole of `drcbec.cpp` and
`drcbearm64.cpp` the block appears three times each — `block.invariant()` and
`block.abort()`, nothing else. The `drcbeut` bookkeeping is the same:
`drc_hash_table::block_begin` reads `invariant()` and calls `abort()` on a
failed allocation, `drc_label_list::block_begin` is a `reset(false)`,
`drc_hash_table::block_end` is empty. **Nothing anywhere asserts `inuse()`**, so
the block does not have to be re-begun between the two back-ends, and no second
`drcuml_state` is needed.

`begin_block()` is therefore called only to obtain a block object, and
`block.end()` is deliberately **never** called — `end()` routes generation
through the state's own back-end, which is neither of the two under test.

### How a case reports: SAVE and RESTORE

UML has `SAVE` and `RESTORE`, which move an entire `drcuml_machine_state` to and
from memory in one opcode. So a case is

```
HANDLE h / RESTORE seed / <case body> / SAVE out / EXIT imm
```

and the diff covers all ten I registers, all ten F registers, `exp`, `fmod` and
`flags` with no per-opcode readout plumbing at all.

The cost is that `SAVE` and `RESTORE` are themselves lowered code: **until a
back-end has those two, every case reports `UNIMPL`.** They are therefore the
first two opcodes worth lowering, because they unlock the entire corpus.

Three details that are load-bearing:

- **The seed is deliberately awkward** — both halves of every 64-bit register
  distinct and non-zero — so a back-end that synthesises a 64-bit operation out
  of two 32-bit ones cannot pass by getting one half right and leaving the other
  untouched.
- **Comparison is field by field, not `memcmp`.** `drcuml_machine_state` has
  tail padding no back-end writes, so a `memcmp` would compare uninitialised
  bytes. Floats are compared as bit patterns, because comparing as `double`
  calls two NaNs unequal and two differently-encoded zeroes equal.
- **`fmod` never takes `ROUND_DEFAULT`.** `drcbec` masks it to two bits on the
  way in (`m_state.fmod = PARAM0 & 0x03`), so `ROUND_DEFAULT` (4) does not
  survive a round trip through it, and the corpus would be reporting a UML grey
  area as a lowering bug.

### Flags

Every instruction is asked for every flag its opcode defines
(`i.set_flags(i.output_flags())`), applied to the whole block after it is built.
`drcuml_block::optimize()` normally decides this and is skipped here on purpose
— the point is to test the lowering of exactly the opcode written, not of
whatever the optimiser rewrote it into. Asking each opcode for its own
`output_flags()` is both the most demanding choice (flags become extra
observable state on every case) and the only one that cannot ask an opcode for
a flag it does not define, which back-ends assert on.

### Unimplemented opcodes are a report, not a crash

An opcode a back-end has not lowered raises `emu_fatalerror` by design. The
harness catches that per case and reports `UNIMPL`, so it is useful as a
coverage report from the first day of lowering rather than only after the last.
Each case runs in its own `drcuml_state` over its own pair of caches, because
`generate()` raises from the middle of a block — `block_end()` never runs — and
throwing the whole back-end away is the cheapest way to be certain that leaves
nothing behind for the next case.

`drcbec` is the oracle: if *it* cannot run a case, the harness says `BAD-CASE`
rather than `UNIMPL`, which is the difference between a corpus bug and a
lowering bug.

## Layout

| file | what it is |
| --- | --- |
| `drc_diff.{h,cpp}` | the hook, the corpus and the state diff |
| `inject.sh` | copy into `vendor/lrmame` + patch `cpu.lua` and `libretro.cpp` |
| `run.sh` | build the loader, run the corpus, exit with the result |
| `nogame.c` | the no-content probe the whole arrangement stands on |

`inject.sh` is separate from `tools/mame-drc-arm32/inject.sh`, and separately
revertible, because the harness is useful on a host with no ARM32 back-end at
all — it gates on `CPU_INCLUDE_DRC`, not on the ARM32 flag. Both are idempotent
and both have `--check`/`--revert`.

Either mode needs a core built with a DRC-backed CPU in it: `CPU_INCLUDE_DRC` is
false when none is present, and then every `drcbe*` file — including the one
under test — drops out of the build entirely. `psikyosh` is the SH-2 driver used
for that, and it is also one of the gap targets the back-end exists to recover.

```sh
HOST=1 SUBTARGET=drcdiff SOURCES=src/mame/psikyo/psikyosh.cpp tools/build-lrmame.sh
      SUBTARGET=drcsh    SOURCES=src/mame/psikyo/psikyosh.cpp tools/build-lrmame.sh
```

`HOST=1` builds x86_64 natively into its own `BUILDDIR` (`build-host`), so the
two configurations do not clean each other out; the price is a second run of
the layout codegen.

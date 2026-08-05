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

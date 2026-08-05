# drc-diff — differential testing for the ARM32 back-end

Goal: run a UML block through `drcbe_arm32` and through `drcbec`, execute both,
and diff the resulting machine state. That is what makes each opcode group in
`drcbearm32.cpp`'s REMAINING WORK an independently verifiable unit rather than
an aspiration.

## What is settled

**The harness must run inside MAME, and it does not need a romset or hardware.**

A standalone harness was tried first and does not work. The chain is
`generate()` needs a `drcuml_block`, which needs a `drcuml_state`, whose
constructor is:

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

under `qemu-arm`, on an x86_64 build host. So a real `running_machine`, a real
`device_t` and therefore a real `drcuml_state` are all reachable with no ROMs,
no MiSTer and no device. `drc_use_c()` is the switch that picks the back-end,
so both are constructible in one process.

## Running the probe

```sh
tests/drc-diff/run.sh          # builds the SH-2 + DRC core if needed, then probes
```

It needs a core built with a DRC-backed CPU in it: `CPU_INCLUDE_DRC` is false
when none is present, and then every `drcbe*` file — including the one under
test — drops out of the build entirely.

## The hook: design settled, not yet built

Two unknowns remained after the probe. Both are now answered, by reading the
tree rather than guessing, and they are what the implementation needs:

**Reaching the machine.** No call-site plumbing is required.
`mame_machine_manager::instance()->machine()` is a global accessor the libretro
OSD already uses (`libretro.cpp:260,352`), so the hook can be a free function
called from `retro_run()` behind an env-var guard, and still get the live
`running_machine` — and from it `root_device()`, which is the `device_t` every
DRC object needs.

**Getting both back-ends into one process.** *Not* by flipping `drc_use_c()`.
That option is read once, inside `drcuml_state`'s constructor
(`drcuml.cpp:165`), and it selects the single back-end that state will own —
one `drcuml_state` can never hold both. Instead call the factories directly:

```cpp
drc::make_drcbe_c    (drcuml_state &, device_t &, drc_cache &, flags, modes, addrbits, ignorebits)
drc::make_drcbe_arm32(drcuml_state &, device_t &, drc_cache &, flags, modes, addrbits, ignorebits)
```

Both are exported — `drcuml.o` references them, which is how they were found —
so the hook builds one `drcuml_state` for the block/handle/label bookkeeping and
constructs the two back-ends itself, **each over its own `drc_cache`**, so the
two code streams cannot collide.

The remaining design question, and the first thing to resolve when implementing:
`drcuml_block::end()` drives generation through the state's own back-end, so
feeding the *same* block to a second back-end means calling
`drcbe_interface::generate(block, instlist, numinst)` directly, the same call
`drcuml_state` makes. Whether the `drc_hash_table` / `drc_label_list` /
`drc_map_variables` bookkeeping tolerates a second `block_begin`/`block_end`
pass over one block has to be checked before relying on it; if it does not, each
back-end needs its own `drcuml_state` and the instruction list is replayed
rather than the block reused.

Then:

1. The hook itself, per the above.
2. A UML instruction corpus, per opcode group, mirroring the shape of
   `tests/a32-asmjit/gen.cpp`'s encoding corpus.
3. The state diff, so a failing opcode names itself.

Until (1) exists nothing in the lowering is verified, which is why it is the
next unit of work rather than more lowering.

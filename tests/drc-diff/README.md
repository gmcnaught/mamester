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

## Remaining to build

1. A hook inside the core that, on an env var, builds two `drcuml_state`s over
   the empty driver's root device — one forced to `drcbec` via `drc_use_c()`,
   one native — generates the same instruction list through both, executes
   both, and diffs `drcuml_machine_state`.
2. A UML instruction corpus, per opcode group, mirroring the shape of
   `tests/a32-asmjit/gen.cpp`'s encoding corpus.
3. The diff report, so a failing opcode names itself.

Until (1) exists nothing in the lowering is verified, which is why it is the
next unit of work rather than more lowering.

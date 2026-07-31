# tools/ — build, deploy, and bring-up tooling (planned)

**Status: stub.** Modeled on `sonic-mania-mister/tools/` and
`maldita.castilla-mister/tools/`.

Planned contents:

| Path | Purpose | Adapt from |
|---|---|---|
| `mame-frontend/` | the MAME→DDR present shim + input/audio glue (the actual port layer) | see its own README |
| `build-mame.sh` | Docker armhf cross-compile of the MAME ELF (static) | `sonic-mania-mister/tools/mister/build-game.sh` |
| `setup-build-container.sh` | armhf toolchain container (Debian + clang/gcc) | `sonic-mania-mister/tools/mister/setup-build-container.sh` |
| `test-frame-writer.c` | standalone DDR present-path proof (drives the reader with **zero** emulator) — bring-up step 2 | copy `sonic-mania-mister/tools/mister-wrapper/test-frame-writer.c`, retarget addresses |
| `cpu-bench/` | on-device fps measurement harness for step 1 (per-driver CPU validation) | new |

## First tool to write: the CPU bench (step 1)

Per `../docs/feasibility.md` §6, the one open risk is whether one A9 core runs a
given driver at full speed. Before any RTL or present-path work, build/run stock
mame4all-pi (or MAME 2003-Plus) on the DE10-Nano HPS and measure sustained fps on
target drivers (a Midway T-unit title, a Psikyo/NMK shmup, a Sega System 24 game).
Watch for the armhf 32-bit `long` overflow in ns perf counters (a sibling-port
gotcha). This directory is where that harness lives.

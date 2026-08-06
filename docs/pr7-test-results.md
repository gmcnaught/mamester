# PR #7 (lrmame / MAME 0.289 + ARM32 DRC) — test results

Tested at `d3ece56` (PR head) plus four fixes, on branch `pr7-lrmame`.
Device: MiSTer @ 192.168.20.81, `armv7l`, Linux 5.15.1, glibc **2.31**.
Build host: macOS/arm64, Docker 29.2.1.
ROMs: `pacman.zip` and `s1945ii.zip` from the MAME 0.260 non-merged set, in
`/media/fat/games/mame/roms289/` so the existing 0.37b5-era sets are untouched.
(The `mame-sl` archive linked in the request is **software-list** ROMs — it has
no arcade parents.)

## The gate

The design doc's kill decision was: build `SOURCES=pacman`, run it on the
device with sound, and stop if it cannot hold 60 fps.

```
MISTER-HOST: 288x224 @ 60.6061 Hz, 48000 Hz audio, present=DDR
MISTER-HOST: ALSA 48000 Hz stereo, 60.61 fps, non-blocking (emulator is the clock)
MISTER-BENCH fps=88.3          1800 frames in 20.39s, 4 underruns
```

**88.3 fps, sound on. The gate passes** — 0.289's device model and scheduler are
not the wall the doc feared, at least for a Z80 driver.

`s1945ii` (SH-2, `psikyosh.cpp`, ROT270) also runs: 320x224 @ 60 Hz,
**30.1 fps**. That is half speed, and it is the number that decides whether this
engine is useful beyond the trivial drivers.

## The DRC A/B — the test the corpus cannot do

`tests/drc-diff/README.md` states plainly that the harness has no address space
and so has never executed `READ`/`WRITE`/`READM`/`WRITEM`. A real driver does.

| | `pacman` (Z80, no UML) | `s1945ii` (SH-2, UML) |
| --- | --- | --- |
| DRC=1 (drcbe_arm32) | 88.6 fps | 27.9–30.1 fps |
| DRC=0 (drcbec) | 88.1 fps | **3.8 fps** |
| frame hash @400/@600 | `601bc720788077ab` | `912aeffbaed7ea59` |
| frame hash @800/@1200 | `d7946c8cc9c3a464` | `466e938dbc9304cb` |

**Hashes are identical between the two back-ends on both drivers.** The ARM32
lowering produces bit-identical output to the UML interpreter on a real driver,
memory path included, and it is worth **~7.4x** on SH-2.

## Tiers

| Tier | What | Result |
| --- | --- | --- |
| T0 | `tests/game_manager_test.sh`; `deploy.py --dry-run` | 46/46; missing-engine reporting correct |
| T1 | `tests/arm32emit/run.sh` | 391/391 match `arm-linux-gnueabihf-as` |
| T1 | `tests/a32-asmjit/run.sh`, against the **vendored** submodule | 130/130, +3 invalid forms refused |
| T3 | CI `drc-arm32.yml`, x86_64 runner | 77/77 cases x 8 seeds, `drcbe_c` vs `drcbe_arm32`; `drcbearm32.o` present, no 64-bit back-end |
| T2 | artefact checks | ELF 32-bit ARM; `NEEDED` = bare `lrmame_libretro.so`; `RUNPATH=$ORIGIN`; 34 exported symbols, all `retro_*` |
| T4 | device, above | gate passes |
| T4 | launch harness, real OSD pick | see below |

### Launch harness on the device

A pick written to `/media/fat/config/MAMESTer.s0` with `engine lrmame` in
`opts/pacman.opt`:

```
  opts    (none)
  engine  lrmame -> /media/fat/games/mame/lrmame
```

Game ran; two screenshots four seconds apart have different MD5s (Pac-Man
attract mode). `engine bogus` logged
`WARNING unknown engine 'bogus' — falling back to /media/fat/games/mame/mame`
and launched anyway; `engine mame2003` resolved to the 2003-plus binary. The
"named engine is not installed" branch was not exercised on hardware — every
engine name this build knows happens to be present on the device — and is
covered only by the host tests.

## Defects found, and what was done

**1. CI: `drc-diff` fetches only `vendor/lrmame`.**
`tools/mame-drc-arm32/inject.sh` needs `vendor/asmjit-a32` and exits 1 — after
it has already patched `drcuml.cpp` and `cpu.lua`. Run 31073871965 died in 30
seconds and the `report` step blamed a missing `drcbearm32.o`, which was true
and not the cause. Fixed; the job now runs green end to end.

**2. The documented gate build does not link.**
`SOURCES=pacman` contains no DRC-backed CPU, so `CPU_INCLUDE_DRC` is false,
`cpu.lua` leaves `drc_diff.cpp` out, and the unconditional call `inject.sh` adds
to `retro_run()` is left dangling:

```
ld: vendor/lrmame/lrmame_libretro.so: undefined reference to
    `drc::diff_run_once(device_t&)'
```

A shared object may carry undefined symbols, so the core links clean and this
surfaces at the *host* link, one 40-minute build later, naming a test harness
that has nothing to do with the driver subset. Fixed with a weak declaration
plus a null test at the call site, so `libretro.cpp` agrees with the decision
`cpu.lua` already made.

**3. Nothing the trixie container builds can run on the MiSTer.** The important
one.

```
./lrmame: /lib/libc.so.6: version `GLIBC_2.38' not found
./lrmame: /lib/libc.so.6: version `GLIBC_2.34' not found      (+2.36, +2.32)
```

Device glibc is 2.31; trixie's armhf libc is 2.41. Nothing before the device
catches it: the build is clean, and the qemu-arm run recorded in `progress.md`
is clean too, because qemu was pointed at the container's own libraries with
`-L`.

`--sysroot` does not fix it — Debian's cross gcc resolves `crt1.o` and `libc.so`
through its own prefix, which `--sysroot` leaves alone. Replacing the
toolchain's target tree in place does, and the multiarch path with it, since
glibc's `libc.so` is a linker script with absolute paths.

That is still not enough, and the reason is an ABI rather than symbol versions:
**trixie IS Debian's armhf time64 transition**, so its `libstdc++.a` is prebuilt
for 64-bit `time_t` and refers to `__clock_gettime64`, `__ioctl_time64`,
`__fstat64_time64`. A prebuilt runtime library's ABI cannot be changed by a
flag. The container is now **bookworm/gcc-12** (armhf there is still 32-bit
`time_t`) with the bullseye 2.31 target tree, plus:

- `tools/mister/glibc231-compat.c` — `__libc_single_threaded` (glibc 2.32) and
  `arc4random` (2.36), which bookworm's `libstdc++.a` references. Defining
  `__libc_single_threaded` as 0 means "threads may exist" and selects the atomic
  path, which is what a 2.31-era libstdc++ did unconditionally.
- `-lpthread` — 2.31 still has a separate libpthread.
- `LDOPTS` by absolute path — genie links from
  `build/projects/retro/mame<SUBTARGET>/gmake-linux/`.

Both artefacts now require nothing later than `GLIBC_2.30`, and a C++20 probe
using threads, chrono, locale, filesystem, random and iostreams runs on the
device.

**4. Not a defect, but it will cost someone an hour.** `deploy.py` pushes
`game_manager.sh` while it is running, and a running shell script does not
re-read itself. The first pick after a deploy launched the *old* manager: no
`engine` line in the log, and `engine lrmame` passed to MAME as a command-line
flag. Killing the manager and reloading the core fixed it. A deploy that
"changed nothing" looks exactly like this.

Two incidental build-host notes: MAME's `genie` binary is rebuilt by whichever
container ran last, so the gcc-14 one refused to run under bookworm
(`GLIBC_2.38 not found`) until deleted; and Docker's `invalid signature` apt
failures were the VM disk being full.

## What was not tested

- Input, and any driver that is not `pacman` or `s1945ii`.
- `M16B=1` (RGB565, no per-frame convert) — still an unmeasured arm; the runs
  above are all XRGB8888.
- Threaded present (`MISTER_THREADED_PRESENT`) — off throughout.
- `tests/drc-diff/run.sh --host` locally: `HOST=1` runs outside the container by
  design and cannot run on macOS, and on aarch64 the calibration oracle would be
  `drcbe_arm64` rather than the documented `drcbe_x64`. CI does the x86_64 run —
  though note CI runs only the `--arm` mode, never the `--host` calibration the
  README says to run first.

# MAMESTer — software MAME on MiSTer FPGA

> Repo/build slug: `mamester` (lowercase). `MAMESTer` is the core's user-facing
> name — the RBF filename, the OSD title, and `/tmp/CORENAME`.

A port of a **software MAME** arcade emulator to the [MiSTer FPGA](https://mister-devel.github.io/MkDocs_MiSTer/)
platform (DE10-Nano), running as an ARM/Linux app on the Cortex-A9 HPS and
presenting video through the standard MiSTer scaler/shader pipeline.

**This is not an FPGA recreation.** The arcade hardware is emulated in software on
the HPS's dual Cortex-A9 (the same lineage as `sonic-mania-mister`); the FPGA side
is only the video output layer — it scans a framebuffer the ARM writes into DDR3
and feeds it into MiSTer's stock `arcade_video`/`video_freak`/`ascal` pipeline, so
scaling, aspect, scanlines, shadow-mask and gamma come from the framework, not from
this project. Think of it as a Linux MAME build that uses the MiSTer FPGA as its
display + audio hardware.

The point of the project is to fill the **1990–1995 arcade gap** where MiSTer's
native FPGA cores are thinnest, with mid-90s raster games that no one has built (or
will build) a native core for — Midway T/Y-unit (Mortal Kombat, NBA Jam), Atari
System 1/2, Namco System 2, Sega System 24/32, Konami GX, Taito F3, and a large
uncored shmup / Kaneko / Seta / Leland / Incredible-Technologies / Exidy catalog. It
deliberately does **not** target games that already have better native cores (Neo
Geo, CPS1/2, System 16/18, Cave) or 3D/PS1-class hardware that won't run in software
on this ARM. Which of those families the DE10-Nano can actually hold at 60 Hz is a
measured question, not an assumed one — see the sweep table under Status; several of
the heaviest (System 32, Konami GX, Taito F3) do not currently clear real time.

> The emulator itself is upstream MAME — [MAME 2003-Plus](https://github.com/libretro/mame2003-plus-libretro)
> (MAME 0.78) as the primary engine and [mame4all-pi](https://github.com/RetroPie/mame4all-pi)
> (MAME 0.37b5) as the fallback. Only the *porting layer* (FPGA RTL, the bitmap→DDR
> video writer, input/audio glue, the libretro host, build + deploy tooling) is this
> project. Both MAME builds are under the **old non-commercial MAME license** (not
> GPL): no commercial packaging, source must be available, and **no ROM images are
> distributed** with this project.

## Status

**Playable on hardware, with two working engines.** Stages 1–7 are complete and
hardware-verified on a real DE10-Nano: a game launches from MiSTer's main menu or
from the core's own picker, presents at the driver's native geometry through the
framework scaler, takes pad input and produces sound. Stage 8 — establishing the
shippable driver list by measurement — is the current work.

What runs today:

| | MAME 2003-Plus (MAME 0.78) | mame4all-pi (MAME 0.37b5) |
|---|---|---|
| role | **primary engine** — anything that reaches acceptable fps here ships here | fallback, for drivers 2003-Plus cannot run acceptably |
| binary | `mame2003` — libretro core built as a static lib (`tools/build-m2003p.sh`) linked against this project's own host (`tools/mame-frontend/libretro-host/`) | `mame` (`tools/build-mame.sh`) |
| drivers in build | 5005 game entries / 2840 parents | 2270 setnames |
| present path | shared `nv_present.c` → DDR double-buffer → FPGA reader | same |
| video / input / audio / launch | verified on device | verified on device |
| ASM CPU cores | Cyclone + DrZ80 **on**, at upstream's curated per-driver default | both disabled (they crash in this vintage) |
| rotation | `SET_ROTATION` accepted, so vertical games currently present **sideways** — rotation is deferred to `ascal` | per-game launch flags (`-norotate`/`-ror`/`-rol`) |

Both engines are cross-compiled with the same armhf toolchain and write through the
same MiSTer backend, which is what makes a comparison between them mean anything.

**Coverage, measured rather than estimated** (family-level, against every setname in
the installed `_Arcade` MRAs, NeoGeo excluded by hand):

- mame4all-pi holds **196 hardware families / ~622 drivers** that no MiSTer core
  covers. Its per-driver sweep numbers are **not currently trustworthy** — the run
  was made with an orphaned busy-loop pinning one of the two A9 cores, and the
  present-path staging fix then superseded it with only 22 games re-benched. It is
  deliberately not being re-run alone; it waits so that one pass measures both
  engines under identical conditions. No per-driver table exists in the repo: commit
  `bbad545` "full 196-family sweep results" is a 30-line bucketed summary in the
  ledger ([docs/bench-results.md](docs/bench-results.md) records the invalidation).
- MAME 2003-Plus adds **297 further families / 816 parent romsets** on top of that —
  **703** net of mahjong titles. Largest: `taito_f3` (31), `metro` (23), `konamigx`
  (17), `system32` (17), `itech32`+`itech8` (29), `kaneko16` (13), `system24` (13).
  Full report: [docs/coverage-2003plus.md](docs/coverage-2003plus.md).

**How much of that coverage is actually reachable** — a one-representative-per-family
sweep of all 288 non-mahjong-only new families, run in the shipping configuration
(core loaded, DDR present live, sound emulated and written to ALSA, ASM CPU cores on
their curated per-driver default, unthrottled, 600 frames), weighted by the parent
romsets each family represents:

| verdict | families | parents | share |
|---|---:|---:|---:|
| OK (>75 fps) | 150 | 318 | 40.3% |
| SLOW (<60 fps) | 46 | 219 | 27.7% |
| MARGINAL (60–75) | 39 | 130 | 16.5% |
| renders nothing (under re-check) | 41 | 98 | 12.4% |
| ROM load failed / absent | 12 | 25 | 3.2% |

The coverage and the *reachable* coverage are not the same library: five of the seven
largest net-new families miss 60 fps, including `taito_f3` (31 parents, 43.6 fps),
`metro` (23, 49.9), `ssv` (23, 56.1), `konamigx` (17, 47.0) and `system32`
(17, 25.2). `system32` would need 140% more, which is not a tuning problem.

Two limits on how far that table can be pushed:

- The "renders nothing" row is a **static detector**, and it over-fires. Its re-check
  is paused at 2 of 41; both of the first two came back MARGINAL, i.e. they were
  running. Treat the row as an upper bound on breakage, not a count of it.
- **Per-driver boundary calls from one 600-frame sample are unreliable.** `crospang`
  measured 62.8 fps in the sweep and 52.9–58.7 across four later runs — MARGINAL or
  SLOW depending on which run you read. The aggregate bands hold; an individual
  driver's band does not, and anything near 60 needs re-measurement before it decides
  which engine ships it.

Numbers and method: [docs/bench-results-2003plus.md](docs/bench-results-2003plus.md).

The trade is speed for coverage: 0.78 costs more CPU than 0.37b5 on the drivers both
engines run, which is exactly why mame4all-pi stays in the tree. The rule is
per-driver, not blanket — 2003-Plus wherever it holds up, mame4all-pi underneath it.
Quantifying the mame4all side of that boundary waits for the joint sweep, since its
existing figures are invalid.

Known gaps: no software rotation on the 2003-plus path yet, so vertical games present
sideways; four-player and analog (spinner/dial/trackball) inputs are unwired; 46 of
the 288 new families are below real time and 39 more are marginal; `deploy.py` still
pushes only the mame4all binary and has no `mame2003` path.

## Design summary

- **Pattern:** pure framebuffer (the `sonic-mania-mister` model). MAME renders a
  finished bitmap per frame → RGB565 into a DDR double-buffer at `0x3A000000` →
  passthrough reader. 8bpp drivers convert into a cached staging frame and cross to
  the uncached `/dev/mem` window once with `memcpy`; writing halfword-by-halfword
  directly to DDR was the single largest cost in the port (e.g. `720` 44 → 79 fps).
- **No FPGA compositing offload.** The `solarus-mister` blitter accelerates SDL
  surface compositing, which MAME does not do; it buys nothing here. If a driver is
  slow, the deficit is emulated-CPU time, which the fabric cannot touch.
- **Framework scaler/shaders reused.** The reader feeds the stock MiSTer video
  pipeline (the `solarus`-PR#138 `ddr3_scan_adapter` model), presenting each driver
  at its native resolution via a register-programmable timing generator (per-game
  H/V geometry, fractional `CE_PIXEL` divider and aspect, published by the HPS at
  launch). 99.5% of drivers present natively; `ascal` scales and shades. No per-game
  RBF, no output-PLL reconfig.
- **Engine-agnostic backend.** `nv_present.c/h` owns the DDR double-buffer, the
  timing publish and the joystick words. mame4all's OSD glue and the libretro host
  are both thin clients of it.
- **ASM CPU cores are engine-specific, and the verdicts do not transfer.** mame4all
  ships with Cyclone (68000) and DrZ80 disabled — that vintage segfaults on entry for
  every 68000 driver and crashes in `DrZ80Run` for sound Z80s. 2003-Plus's copies of
  the same two cores were validated here and ship **on**: DrZ80 produces a
  bit-identical frame hash across four games and five replaced Z80s, and Cyclone is
  worth +22% (`batman` 70.7 → 86.6 fps). `cyclone_mode=default` is not a blanket
  switch but a curated 2,286-entry per-driver whitelist; drivers absent from it get
  no ASM core at all.
- **Launch** via the Master_Daemon + `_handler.sh` path (app as a child of stock
  `Main_MiSTer`), **not** the `main=` wrapper (measured frame-1 wedges in the
  sibling ports). A pick lands in `/media/fat/config/MAMESTer.s0`; `game_manager.sh`
  resolves it to a setname and launches the emulator (`MAME_BIN` selects which).
- **Audio** is ALSA through MiSTer's existing `/dev/MrAudio` → SPI → `sys/alsa.sv`
  route — no RTL needed. Blocking writes when playing (the sound card is the better
  clock), non-blocking when benchmarking.

See [docs/feasibility.md](docs/feasibility.md) for the original study — pattern
comparison, CPU-budget calibration, gap analysis, build selection and open risks.

## Build, deploy, run

```sh
tools/build-m2003p.sh        # mame2003-plus static lib; then make -C tools/mame-frontend/libretro-host
tools/build-mame.sh          # armhf mame4all-pi + MiSTer backend  -> vendor/mame4all-pi/mame
./deploy.py                  # sha1-verified scp of RBF + emulator + launch harness (mame4all)
./deploy.py --harness-only   # scripts-only iteration
tools/make-shortcuts.py      # one .mgl per romset into /media/fat/_MAMESTer
make -C fpga/sim             # Icarus testbenches for the reader/timing RTL
sh tests/game_manager_test.sh  # host-side launch-harness tests
```

The FPGA bitstream is built by GitHub Actions (`raetro/quartus:17.0`); Quartus 17.0
Lite is the only supported version. ROMs are never committed — they live on the
device under `/media/fat/games/mame/roms/`.

## Documentation

- [docs/superpowers/progress.md](docs/superpowers/progress.md) — the durable
  execution ledger: what each stage proved, and how it was verified. Read first.
- [docs/feasibility.md](docs/feasibility.md) — the architecture study.
- [docs/bench-results-2003plus.md](docs/bench-results-2003plus.md),
  [docs/bench-results.md](docs/bench-results.md) — measurements and the
  (non-negotiable) benchmark protocol, including which figures are invalid and why.
- [docs/coverage-2003plus.md](docs/coverage-2003plus.md),
  [docs/geometry-survey.md](docs/geometry-survey.md) — what each engine adds, and how
  each driver's raster maps onto the timing generator.
- [CLAUDE.md](CLAUDE.md) — working notes for agents and contributors.

## Related projects

Sibling native-app-on-MiSTer ports under `~/MisterFPGA-Projects/`:
- [`sonic-mania-mister`](../sonic-mania-mister) — pure-framebuffer template (the model here)
- [`solarus-mister`](../solarus-mister) — FPGA 2D-compositing offload (not applicable to MAME)
- [`maldita.castilla-mister`](../maldita.castilla-mister) — FPGA triangle-rasterizer offload; source of the deploy/handler + audio-ring patterns

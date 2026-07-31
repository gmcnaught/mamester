# mame-mister — software MAME on MiSTer FPGA

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
native FPGA cores are thinnest, with the ~1,500–1,800 mid-90s raster games that no
one has built (or will build) a native core for — Midway T-unit (Mortal Kombat, NBA
Jam, Cruis'n), Atari System 1/2, Namco System 2, Sega System 24/32, Konami GX, and
a large uncored shmup / Kaneko / Seta / Leland / Incredible-Technologies / Exidy
catalog. It deliberately does **not** target games that already have better native
cores (Neo Geo, CPS1/2, System 16/18, Cave) or 3D/PS1-class hardware that won't run
in software on this ARM.

> The emulator itself is upstream MAME — either [mame4all-pi](https://github.com/RetroPie/mame4all-pi)
> (MAME 0.37b5) for v1 or [MAME 2003-Plus](https://github.com/libretro/mame2003-plus-libretro)
> (MAME 0.78) for wider coverage. Only the *porting layer* (FPGA RTL, the
> bitmap→DDR video writer, input/audio glue, build + deploy tooling) is this
> project. Both MAME builds are under the **old non-commercial MAME license** (not
> GPL): no commercial packaging, source must be available, and **no ROM images are
> distributed** with this project.

## Status

**Design / kickoff.** The architecture is settled and documented in
[docs/feasibility.md](docs/feasibility.md); no port code exists yet. The immediate
next step is **empirical, not RTL**: run stock mame4all-pi (or MAME 2003-Plus) on
the real DE10-Nano HPS Linux and measure sustained fps on a spread of target
drivers (a Midway T-unit title, a Psikyo/NMK shmup, a Sega System 24 game). The
present path is not in doubt; the only open risk is whether one A9 core emulates a
given driver at full speed. See the feasibility study's §6 recommendation.

## Design summary

- **Pattern:** pure framebuffer (the `sonic-mania-mister` model). MAME renders a
  finished bitmap per frame → RGB565 into a DDR double-buffer → passthrough reader.
- **No FPGA compositing offload.** The `solarus-mister` blitter accelerates SDL
  surface compositing, which MAME does not do; it buys nothing here. If a driver is
  slow, the deficit is emulated-CPU time, which the fabric cannot touch.
- **Framework scaler/shaders reused.** The reader feeds the stock MiSTer video
  pipeline (the `solarus`-PR#138 `ddr3_scan_adapter` model), presenting each driver
  at its native resolution with a register-programmable timing generator; `ascal`
  scales + shades. No per-game RBF or output-PLL reconfig.
- **Launch** via the Master_Daemon + `_handler.sh` path (app as a child of stock
  `Main_MiSTer`), **not** the `main=` wrapper (measured frame-1 wedges in the
  sibling ports).

See [docs/feasibility.md](docs/feasibility.md) for the full study — the pattern
comparison, CPU-budget calibration, the arcade-core gap analysis, build selection,
and the open risks.

## Related projects

Sibling native-app-on-MiSTer ports under `~/MisterFPGA-Projects/`:
- [`sonic-mania-mister`](../sonic-mania-mister) — pure-framebuffer template (the model here)
- [`solarus-mister`](../solarus-mister) — FPGA 2D-compositing offload (not applicable to MAME)
- [`maldita.castilla-mister`](../maldita.castilla-mister) — FPGA triangle-rasterizer offload; source of the deploy/handler + audio-ring patterns

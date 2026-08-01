# mame2003-plus as a second engine — design

Status: **spec, approved for planning 2026-08-01.** Supersedes the "keep 2003-Plus
as the coverage upgrade" note in [`docs/feasibility.md`](../../feasibility.md) §4 by
turning it into concrete work.

## Goal

Evaluate MAME 2003-Plus (MAME 0.78 + ~350 backports, libretro) as an engine
alongside — or in place of — mame4all-pi (0.37b5), on the same MiSTer present path,
launch harness and DDR contract that Stages 1–8 already proved.

**Two operator-stated wins** (2026-08-01), and only these two:

1. **Drivers mame4all does not have.** 2003-plus carries 4,989 sets / 2,824 parents
   / **726 hardware families** against mame4all's 2,270 drivers. `taito_f3.c` alone
   is 79 games, and Sega System 24/32, Konami GX and later Data East are named gap
   families in `CLAUDE.md` that 0.37b5 largely lacks.
2. **Romset compatibility with the wider world.** 0.78 sets are curated and
   distributed as a single reference collection; the Stage-8 scavenging for
   individual 0.37b5 sets goes away. Source:
   `https://archive.org/details/mame-2003-plus-reference-set` — 5,035 individual
   `roms/<setname>.zip`, 72 `samples/`, and a 20 MB `-listinfo` XML.

Explicitly **not** goals: better accuracy on titles that already run, and fixing the
0.37b5 failure list. Those may fall out; they do not justify the work.

## Approach: spike first, but build the spike out of the port's real parts

Approved 2026-08-01. The spike must produce comparable numbers, and "comparable" on
this device means *with the FPGA core loaded and the DDR present active* — the
Stage-8 lesson was that every earlier figure measured the present path rather than
the driver. So the spike cannot stub out the present; it has to do the real one.
That makes the spike's host the port's skeleton rather than a throwaway.

Binding: **static-link the core into a purpose-built host ELF.**
`make platform=mister STATIC_LINKING=1` emits `mame2003_plus_libretro.a`
(`ar rcs`, Makefile:907), linked against our own host. No `dlopen`, no RetroArch.
Deploys as `games/mame/mame2003` next to `mame` and launches through the existing
`_handler.sh` / `game_manager.sh` with no harness change.

Rejected: a `dlopen` host (engine-swappability is speculative until 2003-plus itself
proves out; the binding is ~30 lines, so B stays a cheap later refactor), and
RetroArch with a MiSTer video driver (drags a second frontend's config/UI/input
stack in to duplicate `game_manager.sh` and puts it between us and the DDR contract).

## Architecture

```
                      ┌─────────────────────────────────────┐
  mame4all-pi ───────▶│ mister_video.cpp  (gp2x_* OSD glue)  │──┐
  (0.37b5, C++)       └─────────────────────────────────────┘  │
                                                                ├──▶ nv_present.c
  mame2003_plus  ────▶┌─────────────────────────────────────┐  │    (DDR contract,
  _libretro.a         │ libretro-host/ (retro_* callbacks)   │──┘     timing publish,
  (0.78, C)           └─────────────────────────────────────┘        joystick words)
                                                                            │
                                                            0x3A000000 DDR double-buffer
                                                                            │
                                                        openbor_video_reader → ascal → HDMI
```

### Component 1 — extract the engine-agnostic backend (approved)

`tools/mame-frontend/mister-backend/mister_video.cpp` currently mixes two things.
Split the MiSTer half into `nv_present.c` / `nv_present.h`: pure C, no MAME headers,
no SDL.

```c
void     nv_open(void);                                    /* map /dev/mem, init state */
void     nv_set_mode(int w, int h, double hz, int rot);    /* modeline + aspect publish */
void     nv_frame(const void *src, int pitch, int fmt);    /* stage, convert, publish */
uint32_t nv_pads(int player);                              /* joystick word */
void     nv_close(void);
```

`fmt` is one of `NV_FMT_RGB565` (staged then one memcpy, or written direct — see the
16bpp carve-out in `docs/bench-results.md`), `NV_FMT_PAL8` (mame4all's path, palette
supplied separately), `NV_FMT_XRGB8888`, `NV_FMT_0RGB1555`.

`mister_video.cpp` keeps only mame4all's `gp2x_*` API and calls into this.
`mister_profile.cpp` and `nv_modeline.h` are already engine-agnostic and move
unchanged.

Rationale for doing this before the spike rather than copying: the DDR contract cost
real hardware debugging (double-emitted first pixel, four-pixel line skew, the
uncached-store throughput cliff, the stale-frame watchdog). Two copies of it will
drift, and the second copy's bugs will look like 2003-plus bugs.

**Risk:** this touches a hot path that was optimised and hardware-verified hours ago.
**Mitigation:** the refactor is mechanical (no logic change), and it is re-validated
by an A/B bench under the interleaved alternating-order protocol in
`docs/bench-results.md` before anything else is built on it. Regression budget: the
protocol's own noise floor, 1.5–4%.

### Component 2 — `tools/mame-frontend/libretro-host/`

| File | Responsibility |
|---|---|
| `host_main.c` | argv (setname, `-rompath`, `-nothrottle`, `MISTER_BENCH_FRAMES`), core lifecycle, `retro_run` loop, frame pacing |
| `host_env.c` | `retro_environment`: `SET_PIXEL_FORMAT`, `GET_SYSTEM_DIRECTORY`, `GET_SAVE_DIRECTORY`, `GET_VARIABLE`/`SET_VARIABLES`, `SET_SYSTEM_AV_INFO`, `SET_GEOMETRY`, `GET_LOG_INTERFACE`, `SET_CONTROLLER_INFO` |
| `host_video.c` | `retro_video_refresh` → format dispatch → `nv_frame` |
| `host_audio.c` | `retro_audio_sample_batch` → ALSA (spike: sink) |
| `host_input.c` | `retro_input_state` → `nv_pads` mapped to `RETRO_DEVICE_JOYPAD` |

`host_video.c` is the only one with real content, and it is smaller than mame4all's
equivalent because **the core already converts palettised output to RGB565
internally** (`src/mame2003/video.c`, `VCT_PALTO565`) — the exact conversion that
dominated `nv_present` profiling in mame4all happens inside the core. Three inbound
formats, per `mame2003_video_init_conversion`:

- `depth==16` without `VIDEO_NEEDS_6BITS_PER_GUN` → **RGB565**, passthrough. Common case.
- `depth==15` → `0RGB1555`, shift-convert to 565.
- `depth==32`, or `depth==16` with `VIDEO_NEEDS_6BITS_PER_GUN` → `XRGB8888`,
  convert to 565. DDR traffic is unchanged (we convert into the staging frame); CPU
  is not, and this is the path to watch when a driver benches unexpectedly slow.

Geometry comes from `retro_get_system_av_info` (`base_width/height`, `fps`) and goes
straight to `nv_set_mode`; `SET_SYSTEM_AV_INFO`/`SET_GEOMETRY` mid-run re-publishes.
The 2003-plus XML carries `<video width height refresh>` per game, so modeline fit
across all 4,989 sets can be predicted offline before any of them is run.

### Component 3 — build

- `tools/mister/Makefile.m2003p` — a `platform=mister` block derived from the
  existing `s812` block (Makefile:292–312: `-marm -mtune=cortex-a9 -mfpu=neon-vfpv3
  -mfloat-abi=hard`, `HAVE_NEON=1`, `ARM=1`), reconciled with the flags settled in
  `83f30d3`, plus `STATIC_LINKING=1`.
- `tools/build-m2003p.sh` — mirrors `tools/build-mame.sh` (idempotent patch apply,
  `STRIP ?=` for the unstripped profiling build).
- `tools/mister/Dockerfile.cross-armhf` — **x86_64 Debian + `crossbuild-essential-armhf`**,
  replacing qemu-emulated native for this build. 2,097 translation units against
  mame4all's 1,131, and 0.78 files are larger; the emulated build is an afternoon
  per iteration and the spike iterates.

### Component 4 — coverage diff (no build required)

`tools/coverage-diff.py`. Inputs: the 2003-plus `-listinfo` XML; mame4all's driver
list (`mame "*" -sourcefile`, already the Stage-8 method); the MiSTer MRA setname
list (every `<setname>` and `zip=` under `/media/fat/_Arcade`, 6,622 MRAs / 2,954
setnames, NeoGeo excluded by hand as in Stage 8). Outputs:

- families present in 2003-plus, absent from mame4all, uncovered by any MiSTer core
  — **the coverage win, quantified**;
- how the existing 196 uncovered families map across (same family, better driver?);
- the spike's new-family bench set;
- predicted native-geometry fit from `<video>`, against the reader's 512-pixel line
  limit.

This runs today, host-side, and picks the bench set before a line of host code
exists. Family-level judgement, per the Stage-8 scope rule — per-driver matching is
useless because clone setnames are rarely named in MRAs.

### Component 5 — romset fetch

`tools/gap-triage-2003.sh` — `gap-triage.sh` with
`BASE=https://archive.org/download/mame-2003-plus-reference-set/roms`, same per-file
`wget` (curl's TLS fails on this device), same verdict classification. ROMs stay on
the device and are never committed.

## What the spike measures

Configuration, matching Stage 8 so the numbers are comparable: **FPGA core loaded,
DDR present active**, sound chips emulated but samples dropped at the callback
(excludes only the ALSA write, ~1–3%), unthrottled, 600 frames, `-nothrottle`
equivalent. **Interleaved, order alternated, repeated** per the protocol in
`docs/bench-results.md` — per-cell spread on this device is 1.5–4% and cell order
alone moved one arm by 2%.

Four arms per driver, because the ASM CPU cores are the dominant variable:

| arm | engine | 68000 core | Z80 core |
|---|---|---|---|
| A | mame4all-pi | portable C | portable C (current shipped default) |
| B | mame2003-plus | portable C | portable C |
| C | mame2003-plus | Cyclone (`USE_CYCLONE`) | DrZ80 (`USE_DRZ80`) |
| D | mame4all-pi | Cyclone (`-cyclone`) | DrZ80 (`-drz80_snd`) — expected to crash; run once to confirm the delta is integration, not core |

Arm A vs B is the honest 0.37b5→0.78 driver/framework tax. B vs C is what the ASM
cores are worth on this silicon. A vs C is the number that actually decides
anything. D is one run, for the diagnosis in "Side finding" below.

**Bench set 1 — overlap** (drivers with existing mame4all figures, spanning the
CPU mix): `gng`, `klax`, `mk`, `nbajam`, `gunbird`, `720`, `contra`, `galaga`.
Covers Z80, 68000, TMS34010 and the marginal band.

**Bench set 2 — new families** (one parent per 2003-plus-only uncovered family, set
by Component 4): expected to include `taito_f3`, `segas32`, `segas24`, `konamigx`,
`itech8`/`itech32`, `deco32`.

**No preset pass threshold.** Superseded 2026-08-01: an earlier draft gated on a
1.3–1.5× tax reasoned from MAME version alone, which ignored that arm A already runs
without ASM cores while 2003-plus can use them. The output is a per-driver tax table
and a recomputed shippable list, judged against the existing 75 fps / 1.25×-real-time
band from Stage 8.

## Side finding to chase

mame4all's Cyclone segfaults on entry for every 68000 driver on this ARMv7 target
(`0002-default-asm-cores-off.patch`), and its DrZ80 crashes in `DrZ80Run` for sound
CPUs. mame2003-plus ships **the same two cores** and enables them on ARM platforms.
If arm C runs clean, the fault is in mame4all's *integration* of those cores rather
than in the cores themselves — and the delta is worth diagnosing, because it hands
speed back to the engine already shipping. Cheap to check as a by-product of arm C;
not a goal of this work.

## Decision gate

Results to `docs/bench-results-2003plus.md` and a ledger entry in
`docs/superpowers/progress.md`. Then one of:

- **Replace** — 2003-plus is at least as fast across the overlap set and the new
  families run. mame4all is retired; the harness keeps one engine.
- **Both engines** — 2003-plus costs measurably more on titles mame4all already runs
  well, but unlocks the new families. `game_manager.sh` picks the engine per setname
  from a table, since the host binary, launch path, present backend and opts
  mechanism are shared either way. This is the likeliest outcome and the one that
  serves both stated wins.
- **Stop** — the tax is large and the new families do not clear real time. The
  coverage diff and the extracted backend survive regardless.

## Risks

| risk | mitigation |
|---|---|
| Refactor regresses the hardware-verified present path | mechanical change, A/B re-bench under the interleaved protocol before anything builds on it |
| `XRGB8888` drivers add a convert mame4all avoided | measured, not assumed; the format is visible per driver in the XML, so exposure is countable offline |
| 2003-plus core options (frameskip, samplerate, overclock) silently change what is being measured | `host_env.c` pins every option explicitly; the pinned set is recorded in the results doc |
| Build time even cross-compiled | full driver set built once; iteration is on host code, which links against a cached `.a` |
| Device contention — a teammate session is running controlled A/B benches on `.81` | host-side work (Components 1, 3, 4) proceeds now; benching waits for the device to clear |
| Licence | mame2003-plus is the same pre-2016 non-commercial MAME licence as mame4all: source disclosure, no commercial packaging, no ROM bundling, derivatives carry a distinct name. No change to existing constraints. |

## Open questions

None blocking. Deferred to the plan: whether the host reuses mame4all's `mame.cfg`
and `opts/<setname>.opt` conventions verbatim (probably yes — `game_manager.sh`
already resolves them) and whether 2003-plus needs a `samples/` fetch path for the
72 sample sets in the reference collection.

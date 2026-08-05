# libretro MAME (current) as a third engine — design

Status: **spec, drafted 2026-08-05.** Follows
[`2026-08-01-mame2003-plus-engine-design.md`](2026-08-01-mame2003-plus-engine-design.md),
and deliberately reuses its conclusions rather than re-deriving them: the libretro
host built for 2003-plus is the frontend contract, and this engine plugs into it.

## Goal

Run **libretro's current MAME** — upstream MAME `0.289`, via
[`libretro/mame`](https://github.com/libretro/mame) — on the same MiSTer present
path, launch harness and DDR contract that Stages 1–9 proved for mame4all-pi
(0.37b5) and mame2003-plus (0.78).

The win is coverage and accuracy of the *whole* remaining library at once: every
family the two frozen forks lack, at current MAME's driver correctness, with
romsets that match the mainstream distributed set. That is the entire case, and it
is only worth anything if the thing runs at a playable rate — see the gate below.

**Not** a goal: replacing either existing engine. mame4all stays the fast path for
the drivers it already runs well, exactly as 2003-plus did not displace it.

## The gate, stated first

This engine is the first one in the project whose feasibility is genuinely in
doubt, so the plan is built around one early kill decision rather than around
delivery.

MAME 0.289's per-frame cost is not comparable to 0.78's. Its device model,
`emumem` address-space dispatch and scheduler are built for correctness on
64-bit desktop silicon; the DE10-Nano's HPS is a **dual Cortex-A9 at 800 MHz** with
a 32-bit ABI, and Stage 8 established that on this hardware even 0.78 needed the
present-path fix to bring `atarisy2` from 44 to 80 fps. On top of that, ARM32 has
**no DRC backend** in MAME — `FORCE_DRC_C_BACKEND=1` is mandatory — so every
recompiler-backed CPU falls back to the C interpreter.

**Gate: build the smallest possible subset, run `pacman` on the device, read
`MISTER-BENCH fps=`.** Pac-Man is a Z80 and a 2-bit-per-pixel tilemap: if 0.289
cannot clear 60 fps with sound on *that*, no arcade driver anyone wants will clear
it either, and this becomes a documented negative result rather than an engine.
The gate costs one subset build and one device run.

There is no point generalising the host, wiring the launch harness or diffing
coverage before that number exists.

## What transfers unchanged

This is why the engine is cheap to attempt at all. Nothing below needs work:

- **`nv_present.c` / the DDR contract.** The `0x3A000000` double buffer, the
  `(counter<<2)|buf` doorbell, the modeline block at `+0x300000`, the stale-frame
  watchdog. It already carries `NV_FMT_XRGB8888` and `NV_FMT_RGB565`, which are
  the only two formats this core can emit.
- **The FPGA side entirely** — reader, timing generator, CE divider, joystick
  writeback. This engine publishes frames through the same registers.
- **The libretro host shape** (`tools/mame-frontend/libretro-host/`): a static
  frontend that calls `retro_*` directly, with no RetroArch and no `dlopen`.
  `host_video.c`, `host_audio.c`, `host_input.c`, `host_throttle.c` and
  `host_present.c` are engine-agnostic as written.
- **The launch harness** — `_handler.sh`, `game_manager.sh`, `.mgl` shortcuts,
  `deploy.py`, per-game `opts/`.
- **The measurement conventions** — `MISTER-BENCH fps=`, `MISTER_FRAME_HASH`,
  `MISTER_PROFILE`, `MISTER_NO_NATIVE`, `MISTER_THREADED_PRESENT`.

`host_main.c`'s argv shape survives too: this core sets `need_fullpath = true` and
`valid_extensions = "cmd|zip|7z"` (`libretro.cpp:718`), so a setname still resolves
to a `<rompath>/<setname>.zip` handed over as a path — the same contract 2003-plus
uses.

## What is actually new

### 1. Toolchain — the existing cross container cannot build this

MAME 0.289 compiles at **`-std=c++20`** (`scripts/genie.lua:774,778,1174`).
`tools/mister/Dockerfile.cross-armhf` is Debian bullseye / **gcc 10.2.1**, which
has only partial C++20. So this engine needs its own container:
`tools/mister/Dockerfile.cross-armhf-cxx20`, Debian trixie / gcc 14.

The old container is **kept, not upgraded**. `docs/bench-results.md`'s engine
comparison rests on mame4all and 2003-plus being built by the same compiler;
bumping it under them would invalidate that quietly. New compiler, new file.

MAME also cross-builds differently from the other two engines: genie emits
host-native code generators (`complay.py`, `verinfo.py`, `png2bdc`) *and*
cross-compiles the emulator, so both toolchains must be present at once. That is
what `CROSS_BUILD=1` plus `OVERRIDE_CC`/`OVERRIDE_CXX` select
(`makefile:361,570-590`).

### 2. Build system — genie/lua, and `SOURCES=` is mandatory

The other two engines are flat makefiles. This one is genie-generated, and the
armhf configuration mirrors upstream's own `android-arm` block
(`Makefile.libretro:134-160`):

```
CONFIG=libretro OSD=retro TARGET=mame
PLATFORM=arm PTR64=0 NOASM=1 FORCE_DRC_C_BACKEND=1
CROSS_BUILD=1 OVERRIDE_CC=... OVERRIDE_CXX=... OVERRIDE_AR=...
ARCHOPTS="-marm -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard"
SOURCES=<driver .cpp list>
```

`SOURCES=` is **not an optimisation, it is a requirement.** A full-set MAME 0.289
build is on the order of a 2 GB link; the DE10-Nano has 1 GB of DDR total and the
upper 512 MB belongs to the FPGA. The shippable artefact has to be a driver
subset chosen from the coverage diff, and the subset is a permanent part of this
engine's design rather than a build-time convenience.

### 3. Pixel format — `M16B` is the free win, if it still works

`libretro_shared.h:10-24` defines `HAVE_RGB32` unconditionally, so the default
output is **XRGB8888** and every frame pays `nv_convert_8888_to_565`. Defining
**`M16B`** flips `PIXEL_TYPE` to `UINT16` and makes the core report
`RETRO_PIXEL_FORMAT_RGB565` (`libretro.cpp:771`) — already the DDR format, so
frames go straight out with no conversion, which is the cheap path Stage 8's
profiling showed to matter.

The header carries a `//FIXME: re-add way to handle 16/32 bit` beside it, so this
path is plausibly bit-rotted upstream. Treat `-DM16B` as a measured arm — build
both, compare with `MISTER_FRAME_HASH`, and fall back to XRGB8888 if 16-bit
output is wrong rather than merely slower.

### 4. No OpenGL means no `SET_HW_RENDER`

`SET_HW_RENDER` is inside `#if defined(HAVE_OPENGL) || defined(HAVE_OPENGLES)`
(`libretro.cpp:938-953`), and its failure path returns false from
`retro_load_game`. Building without either define means the core never issues it
and renders purely into a software framebuffer — which is exactly what the DDR
present wants. This must stay true; a build that acquires an OpenGL define fails
to load games rather than falling back.

### 5. `host_env.c` needs new commands, and one of them matters

The core issues 28 distinct `RETRO_ENVIRONMENT_*` calls against 2003-plus's 25.
Most of the newcomers are accept-and-ignore, but not all:

| Command | Handling |
|---|---|
| `SET_SYSTEM_AV_INFO` | **Load-bearing.** 2003-plus never sent it; this core does (`libretro.cpp:738`) and uses it when the geometry outgrows the reported maximum (`libretro.cpp:533`). It carries `retro_system_av_info`, so unlike `SET_GEOMETRY` it *can* change the refresh rate — which means it must republish the modeline, not just the raster. |
| `GET_CONTENT_DIRECTORY` | Answer it; the core falls back to the ROM directory otherwise and scatters state. |
| `GET_INPUT_BITMASKS` | Refuse — the host polls per button today. Accepting it is a later optimisation, not required. |
| `SET_KEYBOARD_CALLBACK` | Accept and ignore. |
| `GET_FASTFORWARDING` / `SET_FASTFORWARDING_OVERRIDE` | Refuse; the host owns pacing. |
| `SET_SUPPORT_ACHIEVEMENTS` / `SET_SUPPORT_NO_GAME` | Accept and ignore. |
| `SET_CORE_OPTIONS_V2(_INTL)` | Already accepted-and-ignored; `GET_CORE_OPTIONS_VERSION = 0` keeps the core on the legacy `SET_VARIABLES` path whose defaults `host_capture_defaults()` already snapshots. Verify this core honours version 0 — it uses the standard `libretro_core_options.h` helper, which does, but that is worth confirming rather than assuming. |

The `default:` arm already refuses-and-logs unknown commands, so anything missed
shows up under `MISTER_HOST_DEBUG=1` instead of silently misbehaving.

### 6. The host stops being single-engine

`host_env.c` pins a `mame2003-plus_*` core-option table; this core's namespace is
`mame_*`, with a different option set. The Makefile hardcodes
`mame2003_plus_libretro.a`. Both need an engine seam — a per-engine option table
and archive/library selected at build time — rather than a forked copy of the
host, because the whole value of the 2003-plus work is that the frontend is
shared.

### 7. Binding: link the `.so`, do not `dlopen` it

genie's libretro target links with `-shared -Wl,--version-script=link.T
-Wl,--no-undefined` (`scripts/src/osd/retro.lua:83`), and `link.T` exports
`retro_*` and hides everything else. That is precisely the frontend contract, so
the host can link **against the shared object directly** — the `retro_*` calls
resolve at link time and **no host source changes at all**. `dlopen` and its
symbol table stay unnecessary, consistent with the 2003-plus decision; the only
addition is an `$ORIGIN` rpath so the `.so` deploys beside the binary.

## Risks, honestly

1. **CPU — the gate above.** The most likely outcome of this whole spec is a
   documented "0.289 does not fit on an A9". That is a real result and worth the
   subset build to get, but it should be reached in one step, not five.
2. **Binary size and RAM.** Even a subset build is large by this device's
   standards, and the FPGA owns half the DDR. Measure the deployed `.so` and the
   RSS of a running game, not just the fps.
3. **ARM32 maturity.** 32-bit is a lightly-tested upstream configuration:
   `PTR64=0`, no DRC backend, `-Wno-cast-align` applied wholesale for
   `PLATFORM=arm` (`genie.lua:1143`). Expect to fix build breakage that upstream
   CI never sees.
4. **`M16B` bit-rot** — see §3.
5. **Licensing.** MAME has been **GPL-2.0-or-later / BSD-3-Clause since 0.172**,
   not the pre-2016 non-commercial licence that governs the other two engines.
   That is *less* restrictive for redistribution, but it is a different licence
   with different obligations, and `CLAUDE.md`'s licensing note currently
   describes only the old one. It needs updating rather than assuming carry-over.

## Staged plan

| Stage | Work | Exit condition |
|---|---|---|
| 0 | C++20 cross container; `vendor/lrmame` submodule | container builds, submodule pinned |
| 1 | `tools/build-lrmame.sh`; minimal `SOURCES=pacman` build | an armhf `.so` exists and is ARM ELF |
| 2 | Engine seam in the host; link and run `pacman` | binary runs off-device, `retro_load_game` succeeds |
| **3** | **Device bench: `pacman`, sound on, core loaded** | **fps ≥ 60 → continue; below → stop and write it up** |
| 4 | `M16B` vs XRGB8888 arm; `MISTER_FRAME_HASH` equality | format chosen on measurement |
| 5 | Coverage diff vs the other two engines + MRAs | the driver subset that justifies the build |
| 6 | Subset build, launch harness, per-game opts | games launch from the OSD as the other engines do |

Stages 4–6 are contingent on 3 and are deliberately left thin here; they get their
own plan if the gate passes.

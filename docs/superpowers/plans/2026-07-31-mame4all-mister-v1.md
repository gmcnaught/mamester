# mame4all-pi on MiSTer — v1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. This document is the **staged roadmap + detailed Stage 1**. Each later stage is expanded to full bite-sized tasks (its own plan doc under `docs/superpowers/plans/`) when it is reached — do not implement a stage from its roadmap summary alone.

**Goal:** Ship a MiSTer FPGA core that runs the mame4all-pi software emulator on the HPS Cortex-A9 and outputs native-resolution **analog CRT + HDMI** through the standard MiSTer scaler/shader pipeline — playable, with input and audio, launched from the MiSTer menu.

**Architecture:** mame4all renders a finished RGB565 bitmap per frame on the A9 (no VideoCore — the MiSTer backend `mister_video.cpp` owns the present seam). The finished frame is DMA'd into the **`0x3A000000` RGB565 DDR framebuffer** (control word + BUF0/BUF1). **The FPGA core is MiSTer_OpenBOR rebranded to MAME** — OpenBOR is the clean RGB565-framebuffer scanout core (`openbor_video_reader.sv` + `openbor_video_timing.sv`) and mame4all's RGB565 output matches exactly what it reads, so the fabric present path already exists. The one fabric change v1 needs is making the timing generator **register-programmable** (per-game native H/V/refresh) — OpenBOR's is fixed at 320×240 Genesis timing. Output goes through the stock `sys_top` scaler (ascal → HDMI, vga_out → analog CRT). No FPGA compositing offload — the fabric only scans out (see `docs/feasibility.md` §1). Launch is via MiSTer's Master_Daemon + `_handler.sh` (app as a child of stock `Main_MiSTer`), never a `main=` wrapper.

**NOTE (2026-07-31):** re-based on **MiSTer_OpenBOR_7533**, not solarus. Solarus was heavily modified from this same OpenBOR base for its custom blitter (finished-bitmap emulators don't need it), so OpenBOR is the correct, minimally-modified starting point. This collapses much of Stages 1–4: the RGB565 reader, scanout, and DDR format come from OpenBOR for free; OpenBOR even ships `src/native_video_writer.c`, the reference writer for that DDR format (basis for the Stage-2 test-writer and the Stage-4 present shim).

**Tech Stack:** mame4all-pi (MAME 0.37b5, C/C++, armhf); static minimal SDL 1.2 (`tools/mister/build-sdl12.sh`); MiSTer framework HDL (`sys/`, `arcade_video`, `video_freak`, `ascal`) + adapted sibling reader RTL; Quartus 17.0 Lite; Docker armhf/qemu build; Icarus Verilog for RTL sim; deploy over ssh/scp to `root@192.168.20.81`.

## Global Constraints

- **Quartus 17.0 Lite ONLY** for the RBF (newer Quartus is incompatible with MiSTer cores).
- **Feed the standard MiSTer scaler pipeline** (`arcade_video`/`video_freak`/`ascal`) — the `solarus`-PR#138 model. Do **NOT** use `sonic-mania`'s `vga_scaler=0` DAC-bypass.
- **Present format is RGB565** (mame4all-pi native; zero conversion).
- **Launch via Master_Daemon + `games/<setname>/_handler.sh`**, app as a child of stock `Main_MiSTer`. Never a `MiSTer.ini main=` wrapper (sibling ports measured frame-1 wedges: 3/5 vs 0/5).
- **SDL is the static minimal build** (`tools/mister/build-sdl12.sh`, dummy video + joystick, no X11/pulse). Device rootfs has glibc 2.31, libgcc_s, glib, ALSA; no libSDL.
- **Non-commercial MAME license**: ship source, **never commit ROMs**, derivative carries a distinct name.
- **Every fabric-side DDR transaction gets a timeout; settle ≥2 s after core load before first DDR access; scanout reader is arbiter default-owner; reader has a stale-frame watchdog** (maldita `HANDOFF_fabric_stall.md` discipline).
- **Device:** MiSTer @ `192.168.20.81`, passwordless SSH. Load core: `echo "load_core /media/fat/_Other/<rbf>" > /dev/MiSTer_cmd`. Screenshot: `echo screenshot > /dev/MiSTer_cmd` → newest PNG in `/media/fat/screenshots/<Core>/`. Register peek: `busybox devmem <addr> 32`.
- **Base is `MiSTer_OpenBOR_7533`** (rebranded to MAME): its `fpga/` provides the `emu` top, `sys/` tree, RGB565 reader (`rtl/openbor_video_reader.sv`), timing (`rtl/openbor_video_timing.sv`), scanout top (`rtl/openbor_video_top.sv`), and PLL. **The OpenBOR fabric is GPL-3.0** (MiSTer Organize) — preserve headers/attribution; it is separate from the non-commercial MAME HPS binary. Its `src/native_video_writer.c` is the reference writer for the `0x3A000000` RGB565 framebuffer.

---

## Stage roadmap

Each stage produces working, testable software and is gated by an on-hardware (or Icarus) verification. Stages are ordered to de-risk the fabric first (the v1 critical path), decoupled from the emulator via a standalone test-frame-writer, then attach mame4all, then input/audio/launch, then triage.

| # | Stage | Deliverable | Primary verification | Depends |
|---|---|---|---|---|
| 1 | **Rebrand OpenBOR → MAME; build + load** | The OpenBOR core, rebranded to MAME, builds via CI and loads on the device. Its RGB565 reader / scanout / standard-scaler wiring are OpenBOR's (already proven). | CI produces `MAME_*.rbf`; `load_core` on `.81` → the core comes up on HDMI **and** analog CRT (idle scanout), no crash; `screenshot` succeeds. | — |
| 2 | **Test-frame-writer proves the RGB565 path** | A standalone HPS writer (adapt OpenBOR `src/native_video_writer.c`) writes RGB565 patterns to `0x3A000000` + control-word doorbell → appear on screen tear-free. | On device, writer patterns display; changing the pattern changes the `screenshot` MD5; stopping the writer exercises the stale-frame watchdog. | 1 |
| 3 | **Register-programmable native timing** | Make `openbor_video_timing.sv` (fixed 320×240 Genesis) register-programmable (H/V total, active, `CE_PIXEL` divider, refresh) from an HPS-written register block. | Icarus sim per modeline; on device, 256×224@59.18 / 320×240@60 / 288×224@57.5 / tate 224×384 render correct geometry on CRT, ascal scales HDMI. | 2 |
| 4 | **mame4all present shim → DDR** | `mister_video.cpp` writes RGB565 to `0x3A000000` + doorbell (adapt OpenBOR `native_video_writer.c`) and publishes each driver's native geometry to the timing registers at `set_video_mode`. Format already matches OpenBOR's reader — near-trivial. | On device, `mk`/`nbajam` render and animate on the MAME core at native res on CRT + HDMI (screenshot MD5 changes; geometry matches driver). | 3 |
| 5 | **Input (MiSTer controllers → mame4all)** | MiSTer's mapped controllers drive mame4all input via the joy-SHM bridge (sonic-mania pattern); OSD pauses input. | On device, controller inputs move the game (e.g. MK); opening the OSD pauses. | 4 |
| 6 | **Audio** | mame4all ALSA output routed through MiSTer HDMI/analog; native DDR audio ring (maldita pattern) as fallback if ALSA contends. | On device, sustained gameplay audio with no underrun/stall. | 4 |
| 7 | **Launch / packaging / game selection** | `deploy.py` full path; `_handler.sh` launches the selected game as a child of stock main; MiSTer menu integration (CONF_STR setname + per-game selection). | On device, select and launch a gap game from the MiSTer menu, end-to-end. | 4,5,6 |
| 8 | **Driver/romset triage + shippable list** | Triage the ~40% run-failures (ROM-load: dkong/rtype; post-init hang: sf2/mk2/tmnt/…); establish the validated playable gap-game list + romset guidance. | Batch run across the target gap set; documented pass/fail list. | 4,6 |

**v1 = Stages 1–8.** Stages 1–3 are pure fabric (no emulator), verifiable with Icarus + the test-frame-writer. Stage 4 is the first mame-on-custom-core milestone. 5–7 make it a usable core; 8 defines what ships.

---

## Stage 1 — Rebrand OpenBOR → MAME core; build + load (detailed)

**Goal:** The OpenBOR fabric core, rebranded to MAME, builds via CI and loads on the device, coming up on HDMI **and** analog CRT. The RGB565 reader, scanout, and standard-scaler wiring are OpenBOR's (already proven on hardware), so the only Stage-1 risk is that the rebranded Quartus project builds and loads cleanly. No new RTL.

**Files (created on `stage1-core-skeleton`):**
- `fpga/MAME.{sv,qsf,qpf,sdc}`, `fpga/files.qip`, `fpga/build_mame.sh` — OpenBOR project rebranded.
- `fpga/rtl/` — `openbor_video_{top,timing,reader}.sv`, `ddram.sv`, `sdram.sv`, `cos.sv`, `lfsr.v`, `pll*` (verbatim from OpenBOR).
- `fpga/sys/` — stock MiSTer framework (verbatim from OpenBOR; 56 files).
- `.github/workflows/build-rbf.yml` — ubuntu-latest + `raetro/quartus:17.0`.
- Source: `/Users/gmcnaught/MisterFPGA-Projects/MiSTer_OpenBOR_7533/fpga/`.

**Interfaces:**
- Produces: the `MAME` core (`emu` top instantiated by `sys/sys_top.v`), CONF_STR setname `MAME`, presenting the `0x3A000000` RGB565 framebuffer via `openbor_video_top` → `sys_top` scaler. Stage 2 writes that framebuffer from the HPS; Stage 3 makes `openbor_video_timing.sv` programmable.

### Tasks

- [x] **Task 1.1 — Rebrand OpenBOR fpga/ → MAME.** Copied OpenBOR `fpga/` (sys/, rtl/, project files), renamed `OpenBOR.*`→`MAME.*`, set `PROJECT_REVISION=MAME` + CONF_STR setname `MAME`, dropped the unused `OPENBOR_CORE` macro, pointed the SDC at `MAME.sdc`, and added the `/opt/intelFPGA` Docker path to `build_mame.sh`'s Quartus locator. Verified `.qsf` sources `sys/sys.tcl`+`sys_analog.tcl`+`files.qip`; `files.qip` lists `MAME.sv`+`openbor_video_*`; PLL present. **Done** (commit on branch).

- [x] **Task 1.2 — Add the Linux CI build.** `.github/workflows/build-rbf.yml`: ubuntu-latest + `raetro/quartus:17.0` runs `build_mame.sh`, uploads `_Other/MAME_*.rbf`. **Done.**

- [x] **Task 1.3 — CI produces `MAME_*.rbf`.**
  - **Verify:** the `Build MAME RBF` run succeeds; `gh run download <id> -n mame-rbf -D _Other` yields `MAME_<date>.rbf`. If the build fails, diagnose from the log (fitter/STA) and iterate on the branch.

- [x] **Task 1.4 — Deploy + load on `.81`; verify the core comes up.**
  - **Do:** scp `MAME_*.rbf` to `/media/fat/_Other/`; `echo "load_core /media/fat/_Other/MAME_<date>.rbf" > /dev/MiSTer_cmd`.
  - **Verify:** the core loads with no crash; HDMI and analog CRT both show the reader's idle scanout (whatever `openbor_video_reader` outputs with no HPS writer); `echo screenshot > /dev/MiSTer_cmd` produces a PNG. Record the idle-scanout appearance as the Stage-2 baseline.
  - **Commit** the verified state; open a PR from `stage1-core-skeleton`.

**Stage 1 done when:** CI builds `MAME_*.rbf`, it loads on the device without crashing, and both HDMI and analog CRT show the core's scanout — proving the rebranded OpenBOR pipeline is intact and ready for the Stage-2 HPS test-frame-writer.

---

## Notes for expansion

- **Stages 2–3 (RTL)** will be detailed against the actual sibling reader/timing source at execution start (`openbor_video_reader.sv` control-word protocol, burst read, watchdog; `openbor_video_timing.sv`). Each RTL task pairs an **Icarus testbench** (the "failing test") with the module change, following the sibling `fpga/sim/` pattern — not xUnit.
- **Stage 4 (present shim)** is mostly known HPS C++: extend `tools/mame-frontend/mister-backend/mister_video.cpp`'s `DisplayScreen`/`gp2x_set_video_mode` from the current fb0/bench path to the DDR double-buffer + doorbell + timing-register publish. This is the one stage that can be written as concrete C now; it will be fully detailed when reached.
- **Testing model:** RTL → Icarus sim; HPS/integration → on-device screenshot MD5, `devmem` register peeks, and the fps bench (`MISTER-BENCH`). This follows the sibling ports' verification, which the writing-plans skill permits over xUnit for this domain.
- **Not in v1:** MAME 2003-Plus (a later port behind the same frontend), FPGA compositing offload (a category error for MAME), and any per-element rotation.

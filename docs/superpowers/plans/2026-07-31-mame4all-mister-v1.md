# mame4all-pi on MiSTer — v1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. This document is the **staged roadmap + detailed Stage 1**. Each later stage is expanded to full bite-sized tasks (its own plan doc under `docs/superpowers/plans/`) when it is reached — do not implement a stage from its roadmap summary alone.

**Goal:** Ship a MiSTer FPGA core that runs the mame4all-pi software emulator on the HPS Cortex-A9 and outputs native-resolution **analog CRT + HDMI** through the standard MiSTer scaler/shader pipeline — playable, with input and audio, launched from the MiSTer menu.

**Architecture:** mame4all renders a finished RGB565 bitmap per frame on the A9 (no VideoCore — the MiSTer backend `mister_video.cpp` owns the present seam). The finished frame is DMA'd into a DDR3 double-buffer; a custom FPGA passthrough reader (adapted from the sibling `openbor_video_reader.sv`) scans it out under a **register-programmable timing generator** (per-game native H/V/refresh set by the HPS) and feeds `arcade_video`/`video_mixer` → `video_freak` → `ascal`, so scaling, aspect, scanlines, shadow-mask and gamma come from the framework on both HDMI and analog. No FPGA compositing offload — the fabric only scans out (see `docs/feasibility.md` §1). Launch is via MiSTer's Master_Daemon + `_handler.sh` (app as a child of stock `Main_MiSTer`), never a `main=` wrapper.

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
- **Sibling RTL is the code source** for the reader/timing/top: adapt `../solarus-mister/fpga/rtl/{openbor_video_reader.sv,ddr3_scan_adapter.sv}`, `../maldita.castilla-mister/fpga/rtl/openbor_video_timing.sv`, and a sibling `emu` top (`../solarus-mister/fpga/Solarus.sv`). Keep `sys/` read-only from `Template_MiSTer`.

---

## Stage roadmap

Each stage produces working, testable software and is gated by an on-hardware (or Icarus) verification. Stages are ordered to de-risk the fabric first (the v1 critical path), decoupled from the emulator via a standalone test-frame-writer, then attach mame4all, then input/audio/launch, then triage.

| # | Stage | Deliverable | Primary verification | Depends |
|---|---|---|---|---|
| 1 | **Core skeleton + scaler pipeline** | A MiSTer core that loads and shows a fabric-generated **test pattern** on HDMI **and** analog CRT through `arcade_video`/`video_freak`/`ascal`. No DDR yet. | Load RBF; `screenshot` shows the pattern; CRT shows a stable image. | — |
| 2 | **DDR framebuffer reader + test-frame-writer** | Reader scans an RGB565 DDR double-buffer (control-word doorbell, stale-frame watchdog) into the Stage-1 pipeline; a standalone HPS `test-frame-writer` writes patterns to DDR. | Icarus sim of the reader handshake/burst; on device, writer patterns display tear-free; stop writer → watchdog freezes then blanks. | 1 |
| 3 | **Register-programmable native timing** | Timing generator (H/V total, active, `CE_PIXEL` divider, refresh) driven by an HPS-written register block; several modelines. | Icarus sim per modeline; on device, 256×224@59.18 / 320×240@60 / 288×224@57.5 / tate 224×384 render correct geometry on CRT, ascal scales HDMI. | 2 |
| 4 | **mame4all present shim → DDR** | `mister_video.cpp` writes RGB565 to the DDR double-buffer + doorbell, and publishes each driver's native geometry to the timing registers at `set_video_mode`. | On device, `mk`/`nbajam` render and animate on the custom core at native res on CRT + HDMI (screenshot MD5 changes; geometry matches driver). | 3 |
| 5 | **Input (MiSTer controllers → mame4all)** | MiSTer's mapped controllers drive mame4all input via the joy-SHM bridge (sonic-mania pattern); OSD pauses input. | On device, controller inputs move the game (e.g. MK); opening the OSD pauses. | 4 |
| 6 | **Audio** | mame4all ALSA output routed through MiSTer HDMI/analog; native DDR audio ring (maldita pattern) as fallback if ALSA contends. | On device, sustained gameplay audio with no underrun/stall. | 4 |
| 7 | **Launch / packaging / game selection** | `deploy.py` full path; `_handler.sh` launches the selected game as a child of stock main; MiSTer menu integration (CONF_STR setname + per-game selection). | On device, select and launch a gap game from the MiSTer menu, end-to-end. | 4,5,6 |
| 8 | **Driver/romset triage + shippable list** | Triage the ~40% run-failures (ROM-load: dkong/rtype; post-init hang: sf2/mk2/tmnt/…); establish the validated playable gap-game list + romset guidance. | Batch run across the target gap set; documented pass/fail list. | 4,6 |

**v1 = Stages 1–8.** Stages 1–3 are pure fabric (no emulator), verifiable with Icarus + the test-frame-writer. Stage 4 is the first mame-on-custom-core milestone. 5–7 make it a usable core; 8 defines what ships.

---

## Stage 1 — Core skeleton + scaler pipeline (detailed)

**Goal:** A loadable MiSTer core whose fabric generates a test pattern and drives it through the standard scaler to HDMI **and** analog CRT. This isolates and proves the `sys/` video wiring, clocks, CONF_STR, and both output paths before any DDR/reader/emulator complexity.

**Files:**
- Create: `fpga/MAME.sv` (the `emu` top), `fpga/MAME.qpf`, `fpga/MAME.qsf`, `fpga/MAME.sdc`, `fpga/files.qip`
- Create: `fpga/rtl/testpattern.sv` (fabric test-pattern generator)
- Create: `fpga/pll/` (video PLL, from sibling), `fpga/build_mame.sh`
- Copy (read-only): `fpga/sys/` from `Template_MiSTer` (pin to the same revision the siblings use)
- Reference/adapt: `../solarus-mister/fpga/Solarus.sv` (emu top + `arcade_video`/`video_freak`/`ascal` wiring), `../solarus-mister/fpga/build_solarus.sh`, `../solarus-mister/fpga/Solarus.{qsf,sdc,qpf}`

**Interfaces:**
- Produces: `emu` top module (name **must** be `emu`, instantiated by `sys/sys_top.v`); a `CONF_STR` with setname `MAME`; a video path `testpattern → arcade_video(RGB, CE_PIXEL, HS/VS/HBlank/VBlank) → video_freak → ascal → {HDMI, VGA}`. Stage 2 replaces `testpattern` with the DDR reader; the pipeline wiring is stable from here.

### Tasks

- [ ] **Task 1.1 — Scaffold the Quartus project + `emu` top that builds a black-screen core.**
  - **Files:** `fpga/MAME.{qpf,qsf,sdc}`, `fpga/MAME.sv`, `fpga/files.qip`, `fpga/sys/` (copied), `fpga/build_mame.sh`, `fpga/pll/`.
  - **Do:** Copy `sys/`, the PLL, and the Quartus project skeleton from `../solarus-mister/fpga/`; rename `Solarus*`→`MAME*`; strip solarus's blitter/compositor/DDR sources from `files.qip` and the `emu` body, leaving a minimal `emu` that ties video outputs off (black) and sets `CONF_STR` setname `MAME`. Wire `build_mame.sh` to `quartus_sh --flow compile MAME` (Quartus 17.0).
  - **Verify:** `fpga/build_mame.sh` produces `MAME_<date>.rbf`; on device `load_core` it → core loads, MiSTer shows a black screen with no crash; `screenshot` succeeds. (First bitstream build is slow; expect one STA/fitter iteration.)
  - **Commit.**

- [ ] **Task 1.2 — Add the fabric test-pattern generator and wire the standard scaler → HDMI.**
  - **Files:** Create `fpga/rtl/testpattern.sv` (counters → color bars / gradient at a fixed 320×240@60 with `CE_PIXEL`, `HS/VS/DE/HBlank/VBlank`); modify `fpga/MAME.sv` to instantiate it and feed `arcade_video`/`video_mixer` → `video_freak` → `ascal` (copy the exact instantiation from `Solarus.sv`).
  - **Verify:** Rebuild + load; `screenshot` shows the color-bar/gradient pattern on HDMI, correct colors, no tearing. Compare two screenshots to confirm any animated element changes.
  - **Commit.**

- [ ] **Task 1.3 — Bring up analog CRT output through the same pipeline.**
  - **Files:** Modify `fpga/MAME.sv`/`fpga/MAME.qsf` for the analog path (VGA_R/G/B via the sys DAC/`video_freak`), matching how `Solarus.sv` drives analog; confirm `MiSTer.ini` video mode and that the scaler (not a raw DAC bypass) owns analog.
  - **Verify:** On a real CRT/analog display the pattern is stable and correctly framed (validate on a monitor — perf counters lie about display health; STA `emu` clock may close slightly negative and still run, per maldita). HDMI unchanged.
  - **Commit.**

- [ ] **Task 1.4 — Lock the video-pipeline interface for Stage 2.**
  - **Files:** Document in `fpga/README.md` the exact signals `testpattern.sv` presents to `arcade_video` (bus widths, `CE_PIXEL` domain, blank/sync polarity) — this is the contract the Stage-2 DDR reader must satisfy.
  - **Verify:** The documented interface matches the built `MAME.sv`; a one-line Icarus smoke test elaborates `testpattern.sv` without errors.
  - **Commit.**

**Stage 1 done when:** the core loads, shows the pattern on HDMI and analog CRT through the framework scaler, and the video-pipeline interface is documented for Stage 2 to plug the DDR reader into.

---

## Notes for expansion

- **Stages 2–3 (RTL)** will be detailed against the actual sibling reader/timing source at execution start (`openbor_video_reader.sv` control-word protocol, burst read, watchdog; `openbor_video_timing.sv`). Each RTL task pairs an **Icarus testbench** (the "failing test") with the module change, following the sibling `fpga/sim/` pattern — not xUnit.
- **Stage 4 (present shim)** is mostly known HPS C++: extend `tools/mame-frontend/mister-backend/mister_video.cpp`'s `DisplayScreen`/`gp2x_set_video_mode` from the current fb0/bench path to the DDR double-buffer + doorbell + timing-register publish. This is the one stage that can be written as concrete C now; it will be fully detailed when reached.
- **Testing model:** RTL → Icarus sim; HPS/integration → on-device screenshot MD5, `devmem` register peeks, and the fps bench (`MISTER-BENCH`). This follows the sibling ports' verification, which the writing-plans skill permits over xUnit for this domain.
- **Not in v1:** MAME 2003-Plus (a later port behind the same frontend), FPGA compositing offload (a category error for MAME), and any per-element rotation.

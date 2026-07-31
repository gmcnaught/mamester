# CLAUDE.md

Guidance for Claude Code when working in this repository.

## What this project is

A MiSTer FPGA core that runs a **software MAME** emulator as an ARM/Linux app on
the DE10-Nano's Cortex-A9 HPS. It is **not a cycle-accurate FPGA core** — the arcade
hardware is emulated in software; the FPGA is only the video-output layer, scanning
a DDR framebuffer into MiSTer's stock scaler/shader pipeline. When reasoning about a
symptom, first decide which side it lives on: the **emulator** (CPU/driver
correctness, speed) or the **present path** (DDR scanout, timing, scaler).

**Current stage: bench build works; CPU validation pending on hardware.** The
anchor document is [`docs/feasibility.md`](docs/feasibility.md) — read it first; it
settles the architecture and the open risks. The mame4all-pi armhf bench binary
builds and runs (see "Bench build" below); the next step is measuring per-driver
fps on the real DE10-Nano. Do not invent implementation detail that contradicts the
feasibility study without saying so.

## Bench build (works today)

`tools/build-mame.sh` produces `vendor/mame4all-pi/mame` — an armhf ELF (MAME
0.37b5) with the MiSTer present backend (`tools/mame-frontend/mister-backend/mister_video.cpp`)
swapped in for the Raspberry Pi VideoCore path via `tools/mister/Makefile.mister`.
The backend v0 counts frames and reports achieved fps (`MISTER-BENCH fps=…` to
stderr); `MISTER_BENCH_FRAMES=N` exits after N frames, `MISTER_FB=1` blits RGB565
to `/dev/fb0`. Build container: `tools/mister/Dockerfile.mame-build` (armhf/qemu).
Verified: links (~10 MB, `ld-linux-armhf.so.3`), runs under qemu (prints the MAME
banner). Driver set includes the gap targets (Midway T/Y-unit, Atari System 1/2,
Psikyo, Kaneko, Seta, Namco System 2).

To bench on hardware: scp `mame` to `/media/fat/games/mame/mame`, put a
0.37b5-romset game zip in `.../roms/`, and run headless — e.g.
`SDL_VIDEODRIVER=dummy MISTER_BENCH_FRAMES=1800 ./mame <game> -nothrottle` — reading
the `MISTER-BENCH fps=` line. `-nothrottle` runs the emulation flat-out so the fps
number is the A9's actual ceiling for that driver.

## Settled architecture (from the feasibility study)

- **Pattern = pure framebuffer** (the `sonic-mania-mister` model): MAME renders a
  finished bitmap per frame → RGB565 → DDR double-buffer with a `(counter<<2)|buf`
  control-word doorbell → passthrough reader. Present is one ~150 KB `memcpy`
  (~2% of f2h bandwidth); DDR bandwidth is a non-issue (~50× headroom).
- **No FPGA compositing offload.** `solarus-mister`'s blitter accelerates SDL
  surface compositing; a MAME driver does all compositing in its own C rasterizer
  and emits a finished bitmap, so the blitter buys nothing. If a driver is slow it's
  emulated-CPU time — a native RTL core would be a *different project*.
- **Reuse the MiSTer framework scaler/shaders** (explicit project constraint). The
  reader feeds the stock `arcade_video`/`video_freak`/`ascal` pipeline (the
  `solarus`-PR#138 `ddr3_scan_adapter` model), **not** `sonic-mania`'s
  `vga_scaler=0` DAC-bypass. Present each driver at its **native** resolution via a
  **register-programmable timing generator** (per-game H/V total, active,
  `CE_PIXEL` divider, refresh, supplied by the HPS at launch); `ascal` scales +
  shades. No per-game RBF, no output-PLL reconfig. Off-60 Hz refresh rides the
  framework's `vsync_adjust`. Accepted trade: this gives up the lowest-latency
  analog "direct video" mode.
- **Video build target:** mame4all-pi for v1 (standalone ELF, ALSA already,
  RGB565-native = zero convert, built for sub-A9 silicon). MAME 2003-Plus is the
  coverage/accuracy upgrade behind the same framebuffer/ALSA/evdev frontend (needs a
  minimal libretro driver; `bitmap_rgb32`→RGB565 NEON convert).
- **Launch** via Master_Daemon + `games/<setname>/_handler.sh` (app as a child of
  stock `Main_MiSTer`), **never** the `main=` wrapper — the sibling ports measured
  the wrapper causing frame-1 wedges (maldita 3/5 vs 0/5).
- **Audio:** mame4all's ALSA path through MiSTer's HPS→FPGA audio routing. Optional:
  maldita's native 48 kHz DDR audio ring.
- **Input:** SDL2/evdev with `SDL_VIDEODRIVER=dummy`; optional joy-SHM bridge for
  OSD pause.

## Multi-repo topology

Sibling native-app-on-MiSTer ports under `~/MisterFPGA-Projects/` — the reusable
harness comes from these, so consult them before hand-rolling:
- **`sonic-mania-mister/`** — pure-framebuffer template. `tools/mister-wrapper/test-frame-writer.c`
  is the standalone DDR present-path proof (drives the reader with zero engine);
  `vendor/Menu_MiSTer/rtl/native_video_*.sv` is the reader/timing RTL.
- **`solarus-mister/`** — FPGA compositing offload (not the model here, but PR#138's
  `ddr3_scan_adapter` = the "feed the standard pipeline" scanout pattern to copy).
- **`maldita.castilla-mister/`** — richest example: `openbor_video_reader.sv` (DDR
  double-buffer reader + stale-frame watchdog + standard-pipeline feed), the native
  audio ring, and the `deploy.py` + `_handler.sh` + Master_Daemon launch harness.

## Next steps (in order)

1. **Empirical CPU validation FIRST (no RTL yet).** The bench binary is built (see
   "Bench build"). Deploy it to the real DE10-Nano HPS and measure sustained fps on
   target drivers (Midway T-unit, Psikyo/NMK shmup, Sega System 24). This decides
   which games actually ship. The present path is not the risk. (Needs user-supplied
   0.37b5 ROMs; none are committed.)
2. Bring up the present path from `sonic-mania`'s `test-frame-writer.c` against a
   reader that feeds the standard pipeline (adapt `maldita`/`solarus`).
3. Write the MAME-bitmap→DDR-writer shim (replace mame4all's `update_screen`/
   `gp2x_video` present call) + register-programmable timing driven by per-game
   geometry.
4. Input (SDL→evdev or keep SDL) + ALSA audio + `_handler.sh` launch + `deploy.py`.

## Device (MiSTer @ `192.168.20.81`, SSH-key/passwordless)

- Load a core: `echo "load_core /media/fat/_Other/<core>.rbf" > /dev/MiSTer_cmd`.
- `/dev/mem` peeks use `busybox devmem <addr> 32` (`dd` is blocked by
  CONFIG_STRICT_DEVMEM).
- Screenshot: `echo screenshot > /dev/MiSTer_cmd` → newest PNG in
  `/media/fat/screenshots/<Core>/`. Compare MD5s to tell "frozen" from "animating".
- Validate analog timing on a real monitor — perf counters lie about display health;
  watch for the armhf 32-bit `long` overflow in ns counters.

## Build toolchain (when it exists)

- **RBF:** Quartus 17.0 Lite ONLY (newer Quartus is incompatible with MiSTer cores).
- **ARM app:** Docker armhf cross-compile → static ELF (the sibling ports' pattern).
- **Deploy:** `deploy.py` with sha1-verified scp (a truncated ELF segfaults before
  `main`); FTP transfers of extensionless binaries must be **Binary** mode.

## Licensing

Both MAME builds are the pre-2016 **non-commercial MAME license** (not GPL): no
commercial packaging, source disclosure required, no ROM bundling, derivatives carry
a distinct name. Keep the port layer's own license compatible and never commit ROMs.

# CLAUDE.md

Guidance for Claude Code when working in this repository.

## What this project is

A MiSTer FPGA core that runs a **software MAME** emulator as an ARM/Linux app on
the DE10-Nano's Cortex-A9 HPS. It is **not a cycle-accurate FPGA core** — the arcade
hardware is emulated in software; the FPGA is only the video-output layer, scanning
a DDR framebuffer into MiSTer's stock scaler/shader pipeline. When reasoning about a
symptom, first decide which side it lives on: the **emulator** (CPU/driver
correctness, speed) or the **present path** (DDR scanout, timing, scaler).

**Current stage: Stages 1–7 done — games launch from the MiSTer menu with native
timing, pad input and sound. Stage 8 (driver/romset triage) is next.**
**Read [`docs/superpowers/progress.md`](docs/superpowers/progress.md)
first when resuming** — it is the durable execution ledger (what's done, how to
build/deploy/verify, and what each stage actually proved). The plan is
[`docs/superpowers/plans/2026-07-31-mame4all-mister-v1.md`](docs/superpowers/plans/2026-07-31-mame4all-mister-v1.md);
the feasibility study is [`docs/feasibility.md`](docs/feasibility.md). Do not invent
implementation detail that contradicts these without saying so.

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

To bench on hardware: `./deploy.py --no-rbf` puts `mame` at
`/media/fat/games/mame/mame` with romsets in `.../roms/`, then run headless — e.g.
`SDL_VIDEODRIVER=dummy MISTER_BENCH_FRAMES=1800 ./mame <game> -nothrottle` — reading
the `MISTER-BENCH fps=` line. `-nothrottle` runs the emulation flat-out so the fps
number is the A9's actual ceiling for that driver. Bench figures without
`-nosound` are what ships; sound costs ~5× the CPU.

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
- **Three engines behind one frontend**, selected per game by the `engine` line in
  `opts/<setname>.opt`; the framebuffer/ALSA/evdev host is shared.
  - **mame4all-pi (0.37b5)** — v1 default. Standalone ELF, ALSA already,
    RGB565-native = zero convert, built for sub-A9 silicon.
  - **mame2003-plus (0.78)** — the coverage/accuracy upgrade, behind a minimal
    libretro host (`bitmap_rgb32`→RGB565 NEON convert).
  - **lrmame (MAME 0.289)** — `make ENGINE=lrmame`. Gate passed: `pacman` 88.3 fps
    with sound. Needs the bookworm/gcc-12 C++20 cross container with a bullseye
    2.31 target tree, and carries `drcbearm32` (this repo's ARM32 DRC back-end,
    ~7.4× on SH-2) plus a render-pipeline bypass for `rgb32` drivers (+7.1%).
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

## Launch path (Stage 7)

Two device directories, and they are not interchangeable:
- `/media/fat/games/MAMESTer` — name must equal the CONF_STR setname, because
  that is how Master_Daemon finds `_handler.sh` and where the OSD file browser
  opens. Holds the launch harness (`_handler.sh`, `game_manager.sh`,
  `game_lib.sh`) plus a `roms` symlink.
- `/media/fat/games/mame` — the emulator and its data (`mame`, `mame.cfg`,
  `roms/`, `opts/<setname>.opt`, `cfg/nvram/hi/inp/snap`). MAME `chdir()`s to its
  own binary's directory at startup, so this is its cwd.

A game starts when Main_MiSTer writes the pick to
`/media/fat/config/MAMESTer.s0` (the `SC0,MGLZIP,Load Game` mount slot); the
manager resolves it to a setname and launches MAME. `tools/make-shortcuts.py`
writes one `.mgl` per romset into `/media/fat/_MAMESTer`, which serves both entry
points: MiSTer's main menu loads the core and mounts the romset, and the core's
own "Load Game" picker reaches the same files through the
`games/MAMESTer/Games` symlink. A romset zip can never be picked directly — the
browser fakes every `.zip` into a directory and descends into it. Host-side
tests: `sh tests/game_manager_test.sh`.

## Next steps

**Stage 8 — driver/romset triage.** ~40% of drivers fail (dkong/rtype ROM-load;
sf2/mk2/tmnt post-init hang; CPS1 exits silently after `set_video_mode`;
popeye/spyhunt stall). Establish the shippable list, and judge it with sound on
(~5× the CPU of the no-sound bench numbers in `docs/bench-results.md`).

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

**The three engines are not under the same license, and the difference is a
release boundary.**

- **mame4all-pi (0.37b5) and mame2003-plus (0.78)** are the pre-2016
  **non-commercial MAME license** (not GPL): no commercial packaging, source
  disclosure required, no ROM bundling, derivatives carry a distinct name.
- **lrmame (0.289)** is **GPL-2.0-or-later / BSD-3-Clause** — MAME relicensed at
  **0.172**. Less restrictive, but different obligations: copyleft on
  distribution of the combined work rather than a non-commercial clause.
- `tools/mister/mem_wc/` is vendored from minicast under **GPL-2.0**.

Keep the port layer's own license compatible with all three, and never commit
ROMs.

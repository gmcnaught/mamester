# mame-mister — execution ledger

Durable multi-session progress record. Read this first when resuming. The full plan
is `docs/superpowers/plans/2026-07-31-mame4all-mister-v1.md`; the feasibility study
is `docs/feasibility.md`. Stage order was reordered to **1 → 2 → 4 → 3 → 5 → 6 → 7 → 8**
(get a game on screen before per-game timing).

## Current state

- `main` @ `82f00ea`. Stages 1, 2, 4 complete and **hardware-verified on `.81`**.
- **A real game runs on the MAME core**: Ghosts'n Goblins, 60 fps, correct colors,
  256×224 centered in 320×240.
- Fabric = MiSTer_OpenBOR rebranded (RGB565 `0x3A000000` reader + `sys_top` scaler).
- Present path: mame4all → `mister_video.cpp` `nv_present()` → `0x3A000000` DDR
  double-buffer → `openbor_video_reader` → scaler → HDMI/analog.

| Stage | Status | Commit | Evidence |
|---|---|---|---|
| 1 Core builds + loads | ✅ | `f95b8aa` | CI `MAME_*.rbf`; loads `CORENAME:MAME`; 320×240 scanout |
| 2 RGB565 present path | ✅ | `9a9024e` | `test_frame_writer` → full-screen color bars, correct RGB order |
| 4 mame4all present shim | ✅ | `82f00ea` | gng title @ 60 fps, 256×224 centered, palette→RGB565 correct |
| 3 Programmable timing | ⏳ NEXT | — | (fixed 320×240 today; wider games clipped) |
| 5 Input | ⬜ | — | |
| 6 Audio | ⬜ | — | |
| 7 Launch/packaging | ⬜ | — | |
| 8 Driver/romset triage | ⬜ | — | |

## How to resume / environment

- **Device:** MiSTer @ `root@192.168.20.81` (passwordless SSH). Load core:
  `echo "load_core /media/fat/_Other/MAME_YYYYMMDD.rbf" > /dev/MiSTer_cmd`.
  Screenshot: `echo screenshot > /dev/MiSTer_cmd` → newest PNG in
  `/media/fat/screenshots/MAME/` (scp back + view). Register peek: `busybox devmem`.
- **Build the ARM binary:** `tools/build-mame.sh` (armhf/qemu Docker → `vendor/mame4all-pi/mame`).
- **Build the RBF:** push `fpga/**` → GitHub Actions `Build MAME RBF` (ubuntu +
  `raetro/quartus:17.0`) → `gh run download <id> -n mame-rbf -D _Other`. (Windows
  self-hosted runner to be added later; Linux is the path today.)
- **Deploy:** `mame` → `/media/fat/games/mame/mame`; `MAME_*.rbf` → `/media/fat/_Other/`.
  ROMs (0.37b5, archive.org Ghostware set) live in `/media/fat/games/mame/*.zip`;
  the modern MiSTer set there verifyroms-OK for many titles.
- **Fabric test without emulator:** `tools/mister/test_frame_writer.c` (writes a
  pattern to `0x3A000000`; run in `animate` mode so the watchdog doesn't blank).
- **Present contract (`0x3A000000`):** ctrl `= (frame_counter<<2)|active_buf`;
  BUF0 `+0x40`, BUF1 `+0x40040`; 320×240 RGB565. Bump the counter every frame
  (stale-frame watchdog blanks on stall). mame4all is RGB565-native (no BGR swap).

---

## Stage 3 — register-programmable native timing (execution ledger)

**Goal:** each MAME driver presents at its **native** H/V/refresh instead of the
fixed 320×240 — so sf2 (384×224), mk (≈400×254), tate games, etc. show at native
width (not clipped), and analog CRT gets a correct per-game 15 kHz modeline. `ascal`
scales for HDMI regardless.

**What's fixed today** (`fpga/rtl/openbor_video_timing.sv`): `localparam`s
`H_ACTIVE=320, H_FP=17, H_SYNC=38, H_BP=45, H_TOTAL=420; V_ACTIVE=240, V_FP=2,
V_SYNC=3, V_BP=17, V_TOTAL=262`. `CLK_VIDEO=53.693 MHz` with a variable `CE_PIXEL`
(Genesis H40). Inputs today are only `h_adj`/`v_adj` (CRT position). It emits
`hsync/vsync/hblank/vblank/de/hcount/vcount/new_frame/new_line`.

**Approach:**
1. **Make the timing params inputs.** Replace the `H_*`/`V_*` `localparam`s with
   module input ports (`h_active,h_fp,h_sync,h_bp,h_total,v_active,v_fp,v_sync,
   v_bp,v_total` — widths ~[11:0]), **latched at `new_frame`** (change geometry only
   at a frame boundary to avoid mid-frame tears). Keep `h_adj/v_adj`.
2. **CE_PIXEL divider per game.** The A9-side pixel clock rate must give a
   CRT-valid H rate (~15.7 kHz) at the new `h_total`. Options: (a) fixed
   `CLK_VIDEO` + programmable `CE_PIXEL` divider input so H rate ≈ constant across
   widths; (b) fixed divider and let `h_total` set the rate (simpler, but H rate
   drifts — fine for HDMI via ascal, risky for analog). Start with (a).
3. **Plumb params fabric-side.** Wire the new inputs through `openbor_video_top.sv`
   from a small **timing register block the HPS writes in DDR**. The `0x3A000000`
   region has spare space (joystick regs `+0x08..+0x28`, cart `+0x10..`); allocate
   an unused offset (e.g. `+0x30`) for packed timing words, and have
   `openbor_video_reader.sv` latch them (it already polls the control word each
   vblank — read the timing regs in the same pass). Keep every DDR read timed-out.
4. **HPS publishes geometry.** In `mister_video.cpp`: at `gp2x_set_video_mode`,
   compute the driver's modeline (from `surface_width/height` + the driver's
   refresh — mame exposes `Machine->drv->frames_per_second`) and write the packed
   timing words to the new DDR offset **before** the first `nv_present`. Add a
   small table mapping common arcade resolutions → (h_total, blanking split, CE
   divider) so the H rate stays ~15.7 kHz; fall back to a computed default.
5. **Present at native size.** Once timing is native, `nv_present` writes the full
   `surface_width×surface_height` frame (drop the 320×240 center-clip; the buffer
   and BUF1 offset must grow if width×height > 320×240×2 = 153600 B — check the
   region budget: BUF1 at `+0x40040` gives BUF0 ~256 KB, enough for ≈384×340).
   Widen `NV_W/NV_H` to the max supported, or make them per-game.

**Verification:**
- **Icarus** testbench for `openbor_video_timing.sv`: drive several param sets
  (256×224, 320×240, 384×224, tate 224×384), check `hcount/vcount` wrap, sync/blank
  windows, `new_frame` cadence. (First sim in the project — set up `fpga/sim/`.)
- **On device:** run `nemesis` (256×224), `sf2` (384×224 — note it hangs today,
  use another 384-wide title if so), a tate game; confirm each fills the screen at
  native width (no clip) with correct geometry on CRT, ascal scales HDMI. Use
  `test_frame_writer` first (extend it to set timing regs) to prove timing without
  the emulator.

**Key files:** `fpga/rtl/openbor_video_timing.sv` (params→inputs, latch at frame),
`fpga/rtl/openbor_video_top.sv` (plumb), `fpga/rtl/openbor_video_reader.sv` (latch
timing regs from DDR), `tools/mame-frontend/mister-backend/mister_video.cpp`
(`nv_init`/`set_video_mode` publish geometry; drop center-clip), `tools/mister/test_frame_writer.c`
(extend to set timing).

**Risks/open questions:** keeping the analog H rate valid across widths (the CE
divider math); DDR region budget for wider/taller buffers; whether to support fully
arbitrary geometry or a fixed table of arcade modelines first (recommend: table of
the common gap-game resolutions first, arbitrary later).

## Later stages (pointers)

- **5 Input:** MiSTer joystick → mame4all. The `0x3A000000` region already exposes
  joystick words (`+0x08` P1, `+0x18` P2, …) written by the FPGA (OpenBOR pattern);
  `mister_video.cpp` can read them (`NativeVideoWriter_ReadJoystick` equivalent) and
  feed mame's input, or use the sonic-mania joy-SHM bridge. OSD pause.
- **6 Audio:** mame4all ALSA already works on device (mk/nbajam ran with sound in
  the bench). Validate routing; native DDR audio ring (maldita) as fallback.
- **7 Launch/packaging:** `deploy.py` full path; `games/MAME/_handler.sh` (already a
  template) launched by Master_Daemon; per-game selection.
- **8 Driver/romset triage:** ~40% of drivers fail (dkong/rtype ROM-load;
  sf2/mk2/tmnt/… post-init hang) — establish the shippable list. See
  `docs/bench-results.md`.

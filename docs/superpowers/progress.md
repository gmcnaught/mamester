# mamester — execution ledger

Durable multi-session progress record. Read this first when resuming. The full plan
is `docs/superpowers/plans/2026-07-31-mame4all-mister-v1.md`; the feasibility study
is `docs/feasibility.md`. Stage order was reordered to **1 → 2 → 4 → 3 → 5 → 6 → 7 → 8**
(get a game on screen before per-game timing).

## Current state

- `main` @ `94dd4ce`; Stage 3 on branch `stage3-programmable-timing`.
  Stages 1, 2, 3, 4 complete and **hardware-verified on `.81`**.
- **Games present at their native geometry**: gng at 256×224, mk at 416×254
  @53.2 Hz — no center-clip, correct colors.
- Fabric = MiSTer_OpenBOR rebranded (RGB565 `0x3A000000` reader + `sys_top` scaler).
- **Naming (renamed 2026-07-31):** core display name = **`MAMESTer`** (RBF filename,
  CONF_STR/OSD title, `/tmp/CORENAME`, handler dir `games/MAMESTer/`,
  screenshots dir). Repo/build slug = lowercase **`mamester`** (renamed from
  `mame-mister` 2026-07-31; GitHub `gmcnaught/mamester`, local
  `~/MisterFPGA-Projects/mamester`). Quartus
  project files stay **`MAME.qpf/.qsf/.sv/.sdc`** and `PROJECT="MAME"` (internal
  build id, decoupled from RBF filename via `RBF_PREFIX`). The **emulator** and its
  data path stay lowercase **`mame`** (`games/mame/mame` ELF + ROMs). Don't blanket
  rename "MAME" — it means the emulator in most refs.
- Present path: mame4all → `mister_video.cpp` `nv_present()` → `0x3A000000` DDR
  double-buffer → `openbor_video_reader` → scaler → HDMI/analog.

| Stage | Status | Commit | Evidence |
|---|---|---|---|
| 1 Core builds + loads | ✅ | `f95b8aa` | CI `MAME_*.rbf`; loads `CORENAME:MAME`; 320×240 scanout |
| 2 RGB565 present path | ✅ | `9a9024e` | `test_frame_writer` → full-screen color bars, correct RGB order |
| 4 mame4all present shim | ✅ | `82f00ea` | gng title @ 60 fps, 256×224 centered, palette→RGB565 correct |
| 3 Programmable timing | ✅ | (branch) | sweep: 5 geometries raster-exact, borders intact; gng 256×224, mk 416×254@53.2 Hz, 1943 224×256 3:4, popeye 512×448; 99.5% of drivers native |
| 5 Input | ⏳ NEXT | — | |
| 6 Audio | ⬜ | — | |
| 7 Launch/packaging | ⬜ | — | |
| 8 Driver/romset triage | ⬜ | — | |

## How to resume / environment

- **Device:** MiSTer @ `root@192.168.20.81` (passwordless SSH). Load core:
  `echo "load_core /media/fat/_Other/MAMESTer_YYYYMMDD.rbf" > /dev/MiSTer_cmd`.
  Screenshot: `echo screenshot > /dev/MiSTer_cmd` → newest PNG in
  `/media/fat/screenshots/MAMESTer/` (scp back + view). Register peek: `busybox devmem`.
- **Build the ARM binary:** `tools/build-mame.sh` (armhf/qemu Docker → `vendor/mame4all-pi/mame`).
- **Build the RBF:** push `fpga/**` → GitHub Actions `Build MAME RBF` (ubuntu +
  `raetro/quartus:17.0`) → `gh run download <id> -n mame-rbf -D _Other`. (Windows
  self-hosted runner to be added later; Linux is the path today.)
- **Deploy:** `mame` → `/media/fat/games/mame/mame`; `MAMESTer_*.rbf` → `/media/fat/_Other/`.
  ROMs (0.37b5, archive.org Ghostware set) live in `/media/fat/games/mame/*.zip`;
  the modern MiSTer set there verifyroms-OK for many titles.
- **Fabric test without emulator:** `tools/mister/test_frame_writer.c` (writes a
  pattern to `0x3A000000`; run in `animate` mode so the watchdog doesn't blank).
- **Present contract (`0x3A000000`):** ctrl `= (frame_counter<<2)|active_buf`;
  BUF0 `+0x40`, BUF1 `+0x40040`; 320×240 RGB565. Bump the counter every frame
  (stale-frame watchdog blanks on stall). mame4all is RGB565-native (no BGR swap).

---

## Stage 3 — register-programmable native timing (done)

**What shipped.** The HPS publishes a per-game modeline to DDR; the reader
validates and forwards it; the timing generator latches it at a frame boundary;
a fractional CE divider produces the matching pixel clock. 97.2% of drivers now
present at native geometry (see [`docs/geometry-survey.md`](../geometry-survey.md)).

- **Timing register block at `0x3A0E0000`** (past the audio ring, collides with
  nothing): `+0x00` magic `TIM1` (written last, 0 = ignore), `+0x08`
  `h_active|h_fp<<16|h_sync<<32|h_total<<48`, `+0x10` same for V, `+0x18`
  `ce_inc[23:0]`. Back porches are implied. `h_active` is also the DDR line
  stride, so the HPS pads lines to a multiple of 4 pixels.
- **`mame_ce_pixel.sv`** — phase accumulator, `f_pix = 53.693182 MHz * inc/2^24`.
  Replaced the hardcoded Genesis /8,/9,/10 schedule, which could only make one
  line length. Measured within 50 ppm in sim; max real pixel clock is 10.55 MHz,
  well under the 26.8 MHz where it could fire on consecutive clocks.
- **`openbor_video_timing.sv`** — geometry is inputs, latched at the last pixel
  of a frame and only when sane; reset default is the old 320×240/420×262.
- **`openbor_video_reader.sv`** — 4-qword timing read per frame, applied only
  after two identical reads; line burst/stride/count derived from it; line
  address accumulated (no multiplier); line FIFO 256→512 entries.
- **`nv_modeline.h`** — shared modeline math: `h_total ≈ 1.3125 × width`,
  `v_total` from a 15.7 kHz target so the analog H rate stays CRT-legal as the
  width changes; blanking split 17/38/45 as in the Genesis reference mode.

**Two pre-existing bugs fixed** (found by the new reader testbench, both masked
by the old 320×240 canvas): the first pixel of each line was emitted twice, so
the last column fell outside `de`; and every line after the first started four
pixels late, because the word prefetched at the end of a line was discarded
during hblank. Sync/DE are now delayed one pixel to match the reader's
registered RGB.

**Simulation** (`fpga/sim`, Icarus — `make -C fpga/sim`): `tb_video_timing`
(geometry, sync/blank counts, frame period, insane-geometry rejection),
`tb_ce_pixel` (realised rates, no back-to-back enables), `tb_video_reader`
(DDR model, per-pixel compare of a whole frame at 256×224, 320×240, 384×224).

**Hardware evidence (.81, `MAMESTer_20260731.rbf` sha1 `20522fee`):**
`test_frame_writer sweep` — every mode's screenshot is exactly the published
raster with all four 1-px borders intact; `mame gng` → 256×224 @60 Hz;
`mame mk` → 416×254 @53.2 Hz (16bpp path, off-60 refresh).

**Follow-up batch (done, second RBF build):**
1. **1 MB buffer slots.** The upper 512 MB of DDR belongs to the FPGA, so the
   map no longer packs into 1 MB: BUF0 `+0x40`, BUF1 `+0x100040`, cart
   `+0x200000`, audio ring `+0x280000`, timing `+0x300000`; the HPS maps 4 MB.
   Native-present coverage 97.2% → **99.5%** (popeye 512×448, kroozr 512×480,
   spyhunt 480×496 and friends now fit). The 11 remaining fallbacks are wider
   than the reader's 512-pixel line.
2. **Per-game aspect.** `+0x18` carries `arx`/`ary` beside `ce_inc`; the reader
   publishes them (4:3 default) and `MAME.sv` drives `VIDEO_ARX/ARY` from them.
   `nv_make_modeline` picks 3:4 when the frame is taller than it is wide.
3. **Portrait orientation is a launch flag, no code needed** — mame4all parses
   `-norotate`/`-ror`/`-rol`. Measured: `1943` → 224×256 @16.32 kHz 3:4;
   `1943 -norotate` → 256×224 @15.72 kHz 4:3 (the real cabinet signal, CRT-legal).

**Batch verified on device** (RBF sha1 `04837c9d`): sweep unchanged after the
map move (all five geometries exact, borders intact); gng 256×224 @15.72 kHz
4:3 animating; 1943 224×256 @16.32 kHz 3:4 upright and animating; popeye
512×448 @27.84 kHz — a frame that could not be presented at all before —
scans out intact, though that driver sits in a frozen self-test (Stage 8);
spyhunt publishes 480×496 3:4 then exits (Stage 8).

**Still open:** CPS1 exits silently right after `set_video_mode` (ghouls,
strider, willow, ffight all die within ~20 s, no message) — driver-level,
Stage 8.

## Later stages (pointers)

- **5 Input:** MiSTer joystick → mame4all. The `0x3A000000` region already exposes
  joystick words (`+0x08` P1, `+0x18` P2, …) written by the FPGA (OpenBOR pattern);
  `mister_video.cpp` can read them (`NativeVideoWriter_ReadJoystick` equivalent) and
  feed mame's input, or use the sonic-mania joy-SHM bridge. OSD pause.
- **6 Audio:** mame4all ALSA already works on device (mk/nbajam ran with sound in
  the bench). Validate routing; native DDR audio ring (maldita) as fallback.
- **7 Launch/packaging:** `deploy.py` full path; `games/MAMESTer/_handler.sh` (already a
  template) launched by Master_Daemon; per-game selection.
- **8 Driver/romset triage:** ~40% of drivers fail (dkong/rtype ROM-load;
  sf2/mk2/tmnt/… post-init hang) — establish the shippable list. See
  `docs/bench-results.md`.

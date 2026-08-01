# mamester — execution ledger

Durable multi-session progress record. Read this first when resuming. The full plan
is `docs/superpowers/plans/2026-07-31-mame4all-mister-v1.md`; the feasibility study
is `docs/feasibility.md`. Stage order was reordered to **1 → 2 → 4 → 3 → 5 → 6 → 7 → 8**
(get a game on screen before per-game timing).

## Current state

- Stages 1–7 complete and **hardware-verified on `.81`**; Stage 7 on branch
  `stage7-launch`. Stage 8 (driver triage) is next.
- **A game launches from the MiSTer menu**: pick an `_MAMESTer/*.mgl` shortcut,
  or load the core and pick a romset from the OSD ("Load Game"). Video at the
  driver's native geometry, pad input, and sound all work.
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
| 5 Input | ✅ | (branch) | P1 full map verified bit-by-bit on device; gng played with a pad (642 events, combos included) |
| 6 Audio | ✅ | (branch) | gng/1943/mk run with sound, no flags; ALSA RUNNING, /dev/MrAudio held by mame; user-confirmed audible |
| 7 Launch/packaging | ✅ | (branch) | Master_Daemon → handler → manager; OSD mount slot; MGL shortcuts launch Contra/Galaga; per-game opts verified; 23 host tests |
| 8 Driver/romset triage | ⏳ NEXT | — | |

## How to resume / environment

- **Device:** MiSTer @ `root@192.168.20.81` (passwordless SSH). Load core:
  `echo "load_core /media/fat/_Other/MAMESTer_YYYYMMDD.rbf" > /dev/MiSTer_cmd`.
  Screenshot: `echo screenshot > /dev/MiSTer_cmd` → newest PNG in
  `/media/fat/screenshots/MAMESTer/` (scp back + view). Register peek: `busybox devmem`.
- **Build the ARM binary:** `tools/build-mame.sh` (armhf/qemu Docker → `vendor/mame4all-pi/mame`).
- **Build the RBF:** push `fpga/**` → GitHub Actions `Build MAME RBF` (ubuntu +
  `raetro/quartus:17.0`) → `gh run download <id> -n mame-rbf -D _Other`. (Windows
  self-hosted runner to be added later; Linux is the path today.)
- **Deploy:** `./deploy.py` (sha1-verified). `mame` + `mame.cfg` + `roms/` +
  `opts/` → `/media/fat/games/mame/`; the launch harness →
  `/media/fat/games/MAMESTer/`; `MAMESTer_*.rbf` → `/media/fat/_Other/`.
  ROMs (0.37b5, archive.org Ghostware set) live in
  `/media/fat/games/mame/roms/*.zip`; the modern MiSTer set there verifyroms-OK
  for many titles. `./deploy.py --harness-only` for a scripts-only iteration.
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

## Stage 5 — input (done)

**Convention.** Verified against Arcade-Pacman/DonkeyKong/Galaga/1942 and
NeoGeo_MiSTer rather than invented: directions are framework-fixed (`joy[0]`
right, `[1]` left, `[2]` down, `[3]` up, buttons from `[4]`) and Pause is the
last J1 entry. Single-game cores list "Start 1P, Start 2P, Coin" and take them
from either pad; MAMESTer runs any driver with up to four players, so it follows
the multi-game precedent (NeoGeo) — **Start and Coin on each player's own pad**,
so `joystick_0..3` all decode identically:

```
[0] right  [1] left  [2] down  [3] up
[4..9] Fire 1..Fire 6   [10] Start   [11] Coin   [12] Pause
CONF_STR: "J1,Fire 1,Fire 2,Fire 3,Fire 4,Fire 5,Fire 6,Start,Coin,Pause;"
          "jn,A,B,X,Y,L,R,Start,Select,R2;"
```

**Path.** The reader's joystick writeback now runs in lean scanout too (four
single-qword DDR writes per frame). On the HPS, `gp2x_joystick_read()` — which
MAME calls every frame via `osd_poll_joysticks()`, immediately before reading
the input ports — polls the four words and replays changed bits as MAME's own
default bindings (`src/inptport.cpp`): P1 arrows + LCtrl/LAlt/Space/LShift/Z/X,
P2 R/F/D/G + A/S/Q/W, P3 I/K/J/L + RCtrl/RShift/Enter, Start 1-4 = keys 1-4,
Coin 1-4 = keys 5-8, P1 Pause = P. No vendored input code is touched, and MAME's
TAB menu still remaps.

**Verified on device** (RBF sha1 `45c197a1`): writeback proven live by poking a
sentinel into each joystick word and watching the FPGA overwrite it within
200 ms; then every P1 input read back correctly — directions `0x1/2/4/8` with
diagonals, Fire 1-6 `0x10`…`0x200`, Start `0x400`, Coin `0x800`, Pause `0x1000`.
Ghosts'n Goblins was then played with the pad (642 events, combos such as
`0x11` right+Fire 1). `test_frame_writer joy` shows the four words live as
one row of cells per player, and prints them, so this is re-checkable over ssh.

**Gaps:** 0.37b5 has no keyboard defaults for P4 movement/buttons (Start 4 /
Coin 4 only) and just three buttons for P3 — a full four-player setup needs
bindings at the joystick layer (`osd_customize_inputport_defaults` plus a
`JOYCODE` source, which means patching or replacing `src/rpi/input.cpp`).
Analog controls (spinner/dial/trackball) and a keyboard path for MAME's service
and test inputs are also unwired.

## Stage 6 — audio (done)

**Path.** MiSTer already routes Linux audio to the core's output, so no RTL was
needed: `/etc/asound.conf` sends the default PCM through `plug -> rate ->
file(/dev/MrAudio) -> hw:0`, the `MiSTer-audio-spi` driver copies it into a
512 KB DMA ring and hands the FPGA the pointers over SPI, and `sys/alsa.sv`
(instantiated by `sys_top.v` unless `MISTER_DISABLE_ALSA` is set — we do not
set it) DMA-reads the ring and mixes into `audio_out`. The only ALSA card is
`Dummy`, a custom `model_MiSTer` build that is 48 kHz stereo only; the `plug`
layer converts mame4all's mono 44100. Reference for the full stack:
`kimchiman52/3s-mister-arm`, `docs/archive/research-audio-latency.md`.

**Two bugs had to be fixed, only one of them audio:**

1. `snd_pcm_set_params`' latency-derived buffer is rejected by that chain for
   many (rate, refresh) pairs, including the common 44100 at 60 Hz — the error
   is "Unable to get period size". Which values fail is scattered and
   rate-dependent (48000 also rejects 69639/73126/92879 us). The *same*
   configuration requested explicitly through `hw_params` is accepted, so the
   patch asks for it that way: one period per emulated frame
   (`sample_rate / refresh`, e.g. 735 at 44100/60) and four of them. The period
   must equal MAME's samples-per-frame, because `alsa_init` feeds
   `period_size_frames` back into `samples_per_frame`; an unrelated period
   (512) makes the mixer crash. When init failed, `alsa_init()` returned NULL
   and `alsa_write()` dereferenced it — that was the original segfault, and
   `TRY_ALSA` now reports which call failed instead of silently jumping.
2. With sound working, `gng` and `1943` still segfaulted — in `DrZ80Run`, per
   the on-device gdb backtrace (`DrZ80Run -> drz80_execute -> cpu_run`), the
   hand-written ARM Z80 core, which only executes when a sound Z80 runs. It is
   driver-specific (galaga and contra are fine) and those drivers are not on
   mame4all's `fe_drivers` ASM-core blacklist. Disabling the DrZ80 core for
   sound CPUs costs nothing measurable — contra 94.39 -> 94.57 fps, galaga
   128.20 -> 129.04 — so the MiSTer build defaults it off, with `-drz80_snd`
   to opt back in.

**Vendored patches.** `tools/mister/patches/*.patch`, applied idempotently by
`build-mame.sh` (it skips already-applied ones and aborts if the vendored
source has drifted). This is for edits to mame4all's own files; whole-file
replacements still go in `src/mister/`.

**Sound costs about 5x the CPU.** Unthrottled with sound: contra 94 fps
(was 472 without), galaga 128 (was 729). Still 1.6-2.1x real time, but the
figures in `docs/bench-results.md` are no-sound upper bounds and the shippable
list in Stage 8 has to be judged with sound on.

**Handy:** the MiSTer has `/usr/bin/gdb`. Build an unstripped binary with
`make -f Makefile.mister STRIP=true` (the Makefile now uses `STRIP ?=`) and run
`gdb -batch -ex run -ex bt --args ./mame <game>` on the device. Also: mame
ignores SIGTERM, so use `timeout -s KILL` and `killall -9`.

## Stage 7 — launch and packaging (done)

**How a game starts.** `SC0,ZIP,Load Game` in the CONF_STR gives the core a
mount slot, so MiSTer's OSD file browser can pick a romset. It is a *mount*, not
an `F` download slot: nothing streams over SPI, and the core ignores the mount
entirely (`hps_io` already had `img_mounted`/`img_size` wired and `sd_rd`/`sd_wr`
tied off). Main_MiSTer writes the picked path to `/media/fat/config/MAMESTer.s0`;
`game_manager.sh` turns that into a MAME setname. Same mechanism as
solarus-mister's `SC0,SOL,Load Quest`.

Chain: core load -> Master_Daemon sees `/tmp/CORENAME` change -> runs
`games/MAMESTer/_handler.sh` -> execs `game_manager.sh`, which idles until a pick
appears, then launches / switches / stops MAME. `exec` preserves the PID so the
daemon's `kill_child` reaches the manager and its TERM trap stops MAME.

**Two directories, not interchangeable:**
- `/media/fat/games/MAMESTer` (HOMEDIR) — name MUST equal the CONF_STR setname:
  that is how the daemon finds `_handler.sh` and where the OSD browser opens.
  Holds the harness plus a `roms` symlink to the romsets.
- `/media/fat/games/mame` (GAMEDIR) — the `mame` binary, `mame.cfg`, `roms/`,
  `opts/`, and the `cfg/nvram/hi/inp/snap` state. MAME `chdir()`s to its own
  binary's directory at startup (`src/rpi/rpi.cpp`, `realpath(argv[0])`), so this
  is the cwd every relative path in `mame.cfg` resolves against.

**Per-game launch flags** live in `games/mame/opts/<setname>.opt` (one flag per
line, `#` comments), falling back to `default.opt`. Verified on device: with
`-norotate` in `1943.opt`, 1943 publishes 256x224 @ 15.72 kHz 4:3 instead of
224x256 3:4.

**`deploy.py`** is the real thing now (it used to refuse): sha1-verified scp of
the harness, the emulator and the RBF, plus directory creation, the roms
symlink, and warnings when Master_Daemon is not running or no romsets are
visible. `--harness-only` for fast iteration, `--load` to load the core.

**`tools/make-mgl.py`** writes MiSTer `.mgl` shortcuts into `/media/fat/_MAMESTer`,
so games appear in MiSTer's own main menu instead of only in the core's picker.
Titles come from `mame -listfull`. Note the space cost: exFAT allocates 128 KB
per file here, so `--all` over 940 romsets burns ~120 MB — generate a subset.

**Two bugs found and fixed:**

1. *Absolute paths on MAME's command line were silently corrupted.*
   `fronthlp.cpp:342` carried the MS-DOS "`/option` means `-option`" convention
   and rewrote argv **in place**, so `-rompath /media/fat/games/mame` became
   `-rompath -media/fat/games/mame` and every ROM came back NOT FOUND (it breaks
   `-playback`/`-record`/`-romdir` the same way). Removed in
   `tools/mister/patches/0003-no-dos-slash-options.patch`. Relative paths worked,
   which is what made this look like a symlink or a path-length problem at first.
2. *An MGL pick could be filed as stale.* The manager decides "is this pick new?"
   by mtime, and an MGL mounts its file on a delay measured from core load
   (`delay="2"`) while the handler sleeps 2 s for the FPGA — so the pick can land
   before the manager starts. The baseline is now `/tmp/CORENAME`'s mtime, which
   Main_MiSTer stamps at core load: a `.s0` newer than that belongs to this
   session and is acted on; anything older is a leftover and ignored.

**Verified on device** (RBF `MAMESTer_20260801`): daemon spawns the handler on
core load; a `.s0` write launches gng at 60.0 fps; MGL shortcuts launch Contra
and Galaga (including the pick-before-manager-start path); switching picks kills
the running game and starts the new one; per-game opts reach MAME.
`tests/game_manager_test.sh` covers the selection and lifecycle logic on the host
(23 cases). **Not yet exercised: picking a game from the OSD browser by hand** —
every device test drove the same `.s0`/mount mechanism through MGL or a direct
write, which is what the browser writes, but the menu entry itself is unverified.

**Ledger note:** `_MAMESTer/*.mgl` and `games/mame/opts/*.opt` are device-side
user data. The repo ships `games/mame/opts/README.md` documenting the flags.

## Later stages (pointers)
- **8 Driver/romset triage:** ~40% of drivers fail (dkong/rtype ROM-load;
  sf2/mk2/tmnt/… post-init hang) — establish the shippable list. See
  `docs/bench-results.md`.

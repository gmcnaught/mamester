# mamester — execution ledger

Durable multi-session progress record. Read this first when resuming. The full plan
is `docs/superpowers/plans/2026-07-31-mame4all-mister-v1.md`; the feasibility study
is `docs/feasibility.md`. Stage order was reordered to **1 → 2 → 4 → 3 → 5 → 6 → 7 → 8**
(get a game on screen before per-game timing).

## Current state

- Stages 1–7 complete and **hardware-verified on `.81`**; Stage 7 on branch
  `stage7-launch`. Stage 8 (driver triage) is next.
- **A game launches from the MiSTer menu or from inside the core**: pick an
  `_MAMESTer/*.mgl` shortcut, or load the core and pick the same file from the
  OSD ("Load Game" → `Games/`). Video at the driver's native geometry, pad input
  and sound all work.
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
- **The present is write-combined** via `mem_wc.ko` (PRs #5, #8): 4.231 → 0.506
  ms/frame at 512×384, +18.8% to +72.6% flat-out fps across five drivers. It is
  the only host lever that survived re-measurement — see the present-path
  section. Check for `present=write-combined` on the bench line before trusting
  any present-path number.
- **Stage 10's gate passed: lrmame (MAME 0.289) runs `pacman` at 88.3 fps with
  sound**, and `s1945ii` at 39.0 fps against the 60 it needs. Host-side levers
  are exhausted; the `rgb32` render-pipeline bypass (+7.1%) landed, the `ind16`
  FPGA palette path (18%) has not.
- **Stage 11 (`drcbearm32`, the ARM32 DRC back-end) is lowered in full**, agrees
  with `drcbe_c` on 77 differential cases over twelve input states under qemu,
  and — on the device — produces **frame hashes bit-identical to `drcbec`** on
  both `pacman` and `s1945ii`, for **~7.4× on SH-2** (3.8 → 30.1 fps).

| Stage | Status | Commit | Evidence |
|---|---|---|---|
| 1 Core builds + loads | ✅ | `f95b8aa` | CI `MAME_*.rbf`; loads `CORENAME:MAME`; 320×240 scanout |
| 2 RGB565 present path | ✅ | `9a9024e` | `test_frame_writer` → full-screen color bars, correct RGB order |
| 4 mame4all present shim | ✅ | `82f00ea` | gng title @ 60 fps, 256×224 centered, palette→RGB565 correct |
| 3 Programmable timing | ✅ | (branch) | sweep: 5 geometries raster-exact, borders intact; gng 256×224, mk 416×254@53.2 Hz, 1943 224×256 3:4, popeye 512×448; 99.5% of drivers native |
| 5 Input | ✅ | (branch) | P1 full map verified bit-by-bit on device; gng played with a pad (642 events, combos included) |
| 6 Audio | ✅ | (branch) | gng/1943/mk run with sound, no flags; ALSA RUNNING, /dev/MrAudio held by mame; user-confirmed audible |
| 7 Launch/packaging | ✅ | (branch) | Master_Daemon → handler → manager; `.mgl` shortcuts launch Contra/Galaga from the menu and resolve as picker selections; per-game opts verified; 31 host tests |
| 8 Driver/romset triage | ⏳ NEXT | — | 196 families / ~622 drivers have no MiSTer core = the target library |
| — Present write-combining | ✅ | `5298fa5`, `0eec66b` | present 4.231 → 0.506 ms/frame @512×384; +18.8%…+72.6% fps over 5 drivers (PRs #5, #8) |
| 9 mame2003-plus engine | ⏳ | (branch) | 288 families swept, 40.3% >75 fps |
| 10 lrmame (MAME 0.289) | ⏳ gate passed | `8b2eb68`, `1d39a62` | `pacman` 88.3 fps sound on; `s1945ii` 39.0; rgb32 render bypass +7.1%, output MD5-identical (PRs #7, #9) |
| 11 `drcbearm32` | ⏳ | `afdb28c` | 77/77 differential cases × 12 states; on device, frame hashes identical to `drcbec` and **7.4× on SH-2** |

## How to resume / environment

- **Device:** MiSTer @ `root@192.168.20.81` (passwordless SSH). Load core:
  `echo "load_core /media/fat/_Other/MAMESTer_YYYYMMDD.rbf" > /dev/MiSTer_cmd`.
  Screenshot: `echo screenshot > /dev/MiSTer_cmd` → newest PNG in
  `/media/fat/screenshots/MAMESTer/` (scp back + view). Register peek: `busybox devmem`.
- **Build the ARM binary:** `tools/build-mame.sh` (armhf/qemu Docker → `vendor/mame4all-pi/mame`).
  For 0.289: `tools/build-lrmame.sh` (bookworm/gcc-12 C++20 cross container with a
  bullseye 2.31 target tree — **not** trixie, which is the armhf time64 ABI and
  builds artefacts the device cannot run). Host: `make ENGINE=lrmame`.
- **Bench:** `tools/lever-ab.sh` for any A/B — interleaved, alternating arm order,
  resumable from its own TSV. Load the core first; a present measurement with no
  core loaded measures nothing. Confirm `present=write-combined` on the bench
  line, and re-run every lever after changing the binary under test — `SCHED_RT`
  changed sign between two builds.
- **Build the RBF:** push `fpga/**` → GitHub Actions `Build MAME RBF` (ubuntu +
  `raetro/quartus:17.0`) → `gh run download <id> -n mame-rbf -D _Other`. (Windows
  self-hosted runner to be added later; Linux is the path today.)
- **Deploy:** `./deploy.py` (sha1-verified). `mame` + `mame.cfg` + `roms/` +
  `opts/` → `/media/fat/games/mame/`; the launch harness →
  `/media/fat/games/MAMESTer/`; `MAMESTer_*.rbf` → `/media/fat/_Other/`.
  ROMs (0.37b5, archive.org Ghostware set) live in
  `/media/fat/games/mame/roms/*.zip`; the modern MiSTer set there verifyroms-OK
  for many titles. `./deploy.py --harness-only` for a scripts-only iteration.
  **Two deploy traps, both of which have cost a measurement round:** a running
  `game_manager.sh` does not re-read itself, so the first pick after a deploy
  runs the *old* manager — kill it and reload the core; and `mem_wc.ko` shipping
  is not `mem_wc.ko` loading — a stale `_handler.sh` without the `insmod` leaves
  the present on its Strongly-Ordered fallback silently.
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

**How a game starts.** `SC0,MGLZIP,Load Game` in the CONF_STR gives the core a
mount slot, so MiSTer's OSD file browser can pick something. It is a *mount*, not
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

**Choosing a game — `.mgl` shortcuts serve both entry points.** The core's OSD
picker CANNOT select a romset: Main_MiSTer's browser fakes every `.zip` into a
directory and descends into it (`file_io.cpp` ScanDirectory, suppressed only by
`SCANO_NOZIP`, which a core cannot request). So `tools/make-shortcuts.py` writes
one `.mgl` per romset into `/media/fat/_MAMESTer` and symlinks
`games/MAMESTer/Games` to it:
- from MiSTer's **main menu**, selecting one loads the core and mounts the
  romset (`.s0` gets the zip path);
- from the core's **"Load Game"** picker, selecting the same file mounts the
  `.mgl` itself and `game_lib.sh` reads the romset out of its XML.

A "Games" symlink to `_MAMESTer` sits in BOTH the core home and the romset
directory, because MiSTer starts a mount browser at the directory of the last
mounted file — after any `.mgl` launch that is the romset directory, not the
core home. The romset listing is never selectable there: the browser fakes every
`.zip` into a directory whatever the CONF_STR extension filter says, so dropping
`ZIP` from `SC0` would not hide them either.

Titles come from `mame -listfull`. Space cost: exFAT allocates 128 KB per file
here, so `--all` over 940 romsets burns ~120 MB — generate a subset.

**Two bugs found and fixed:**

1. *Absolute paths on MAME's command line were silently corrupted.*
   `fronthlp.cpp:342` carried the MS-DOS "`/option` means `-option`" convention
   and rewrote argv **in place**, so `-rompath /media/fat/games/mame` became
   `-rompath -media/fat/games/mame` and every ROM came back NOT FOUND (it breaks
   `-playback`/`-record`/`-romdir` the same way). Removed in
   `tools/mister/patches/0003-no-dos-slash-options.patch`. Relative paths worked,
   which is what made this look like a symlink or a path-length problem at first.
2. *An `.mgl`'s XML path was resolved against the cwd.* `mgl_romset` tried the
   bare relative path first, which happened to hit because the manager's cwd is
   the home directory — so `-rompath` came out relative and was right only
   because MAME then `chdir()`s to a directory with the same `roms/` layout. It
   now resolves home-relative (what Main_MiSTer itself does) and takes a path
   as-written only when absolute.

3. *An MGL pick could be filed as stale.* The manager decides "is this pick new?"
   by mtime, and an MGL mounts its file on a delay measured from core load
   (`delay="2"`) while the handler sleeps 2 s for the FPGA — so the pick can land
   before the manager starts. The baseline is now `/tmp/CORENAME`'s mtime, which
   Main_MiSTer stamps at core load: a `.s0` newer than that belongs to this
   session and is acted on; anything older is a leftover and ignored.

**Verified on device**, both entry points user-confirmed: loading a game from
MiSTer's main menu (`_MAMESTer`) and from the core's own "Load Game" picker
(`Games/`). Also: the daemon spawns the handler on core load; a `.s0` write
launches gng at 60.0 fps; the pick-before-manager-start path works; switching
picks kills the running game and starts the new one; per-game opts reach MAME.
`tests/game_manager_test.sh` covers selection and lifecycle on the host (31 cases).

**Ledger note:** `_MAMESTer/*.mgl` and `games/mame/opts/*.opt` are device-side
user data. The repo ships `games/mame/opts/README.md` documenting the flags.

## Present path — write-combining (done, PRs #5 and #8)

Engine-independent: this is `nv_present.c`, so every engine gets it. Full
analysis in [`../dreamster-ddr-channel-review.md`](../dreamster-ddr-channel-review.md);
the origin is DreamSTer's DDR channel, reviewed for what transfers here.

**`/dev/mem` on ARM cannot give a write-combining mapping**, so every present
since Stage 2 paid Strongly-Ordered stores — one non-buffered write per word to
DDR. `mem_wc.ko` (a 120-line char driver, vendored from minicast under GPL-2.0
into `tools/mister/mem_wc/`) exposes an arbitrary physical window as WC. On
hardware: **present 4.231 → 0.506 ms/frame at 512×384 (8.4×)**, and flat-out
frame rate up **18.8% (`mk`, sound on) to 72.6% (`robotron`, `-nosound`)** over
five drivers. The module's own `ddr-write-bench` measures the raw memcpy at
**9.6× (858.5 vs 89.0 MB/s)**.

**The mapping is split page-exactly, and that is forced rather than tidy:**

```
0x000000..0x001000   control word + joystick words   strongly-ordered
0x001000..0x300000   BUF0 / BUF1                     write-combining
0x300000..0x400000   timing registers                strongly-ordered
```

The `MAP_FIXED` overlay *replaces* those pages rather than adding a second
mapping, so no physical page is ever live at two memory types at once. Two
things the analysis did not anticipate and the implementation had to handle:

- **`BUF0` begins `0x40` into page 0**, so its first 4032 bytes stay
  strongly-ordered — measured **~13%** of the present (0.506 ms against 0.440
  into an all-WC destination), not the ~1% first claimed. Recovering it means
  moving `BUF0` to `0x1000` in `openbor_video_reader.sv`; fold into the next
  reader revision, not worth one on its own against a 3.7 ms/frame win.
- **`MAP_FIXED` unmaps its target before the driver's `.mmap` runs**, so a
  rejected mapping leaves a *hole* mid-window and the next present takes
  SIGSEGV. `nv_map_wc()` probes at a scratch address first, then overlays.

**Write-combining removes the store ordering the doorbell relied on**, so
`NV_FENCE()` (`dsb sy`) precedes every doorbell store. The invariant to check if
`nv_present.c` is ever refactored: *`dsb sy` appears exactly three times — in
`nv_frame`, `nv_frame_repeat` and `nv_set_mode`.*

**PR #8 ships a prebuilt `.ko` keyed by kernel release**, because building it
needs a prepared MiSTer kernel tree that a checkout does not have.
`_handler.sh:84` `insmod`s it at core launch, failure-tolerant, and
`MISTER_NO_WC=1` forces the documented `/dev/mem` fallback.

**Operational trap, and it cost a whole benchmark round:** the module shipping
is not the module *loading*. A deployed `_handler.sh` predating the `insmod`
left `mem_wc.ko` on the device unloaded, `nv_present.c` silently took its
fallback, and every launch paid Strongly-Ordered stores — including PR #7's
88.3 fps gate figure, which is therefore an understatement. `nv_present` prints
which mapping it got and the exit line repeats it:

```
nv_present: pixel buffers write-combined via /dev/mem_wc
... present=write-combined
```

**Check that string before trusting any present-path number.**

## Stage 8 — driver/romset triage (in progress)

**Scope rule (operator, 2026-07-31): ignore any driver MiSTer already has a core
for.** Coverage is computed from the installed MRAs — every `<setname>` and every
`zip=` name under `/media/fat/_Arcade` (6622 MRAs, 2954 distinct setnames) — and
judged at FAMILY level (`mame "*" -sourcefile`): a hardware family counts as
covered if ANY of its drivers has an MRA. Per-driver matching is useless here
because clone setnames are rarely named in MRAs, which made cps1/pacman/system16
look like gaps. **NeoGeo must be excluded by hand** — it has a wholesale core but
ships `.neo` files, not MRAs. Result: 2270 drivers in the build, **196 families /
~622 drivers with no MiSTer core** — the actual target library for this port.

**Root cause of most "failures": the Cyclone ASM 68000 core.** It segfaults on
entry to emulation for EVERY 68000 driver tested (klax, rambo3, aerofgt,
xenophob, toki, rastan, tmnt, sf2); all of them run with it off. This is one bug
behind most of what the Stage 4 bench recorded as "~40% hang after
`set_video_mode`". It is NOT the per-driver question `fe_drivers` answers: those
drivers are listed `cores=1/3` ("Cyclone OK") and still crash, and `rpi.cpp`
never consults the table for Cyclone anyway — only DrZ80 is gated, the Cyclone
substitution above it is unconditional. Defaulted off in patch
`0002-default-asm-cores-off.patch`; `-cyclone` opts back in.

**The other failure class is romset version.** The romsets that ship with a
MiSTer install are a MODERN set and fail to load on many 0.37b5 drivers (files
renamed/split). Triage pulls a matching 0.37b5 set per game —
`tools/gap-triage.sh`, from the Ghostware collection on archive.org. ROMs stay on
the device; never commit them.

**39 gap representatives tested, 39 run** (unthrottled, with sound, core loaded —
the production configuration; divide by 60 for the real-time multiple):

| ≥100 fps | 60–100 fps | BELOW 60 |
|---|---|---|
| astrof 164, marineb 159, docastle 154, warpwarp 144, seicross 142, astrob 139, thepit 137, ninjakd2 131, locomotn 129, route16 128, monymony 125, snakepit 124, zodiack 122, jackrabt 121, tsamurai 115, wiz 115, atarifb 111, klax 104, kingofb 103, mainevt 103 | polepos 97, puzznic 99, battlnts 99, lkage 99, rpunch 90, rambo3 88, assault 82, aerofgt 81, nbajam 78, lastduel 77, ataxx 73, offroad 72, starcas 65, punchout 64 | **paperboy 42** (Atari System 2), **xenophob 33** (MCR68), **crossbow 25** (Exidy 440) |

`starcas` and `punchout` are marginal (~1.07x) and will likely not hold 60 Hz.

**Open question (deferred 2026-08-01, needs an idle device).** Is the in-core
OSD picker redundant? MiSTer's in-core core browser may already list the
`_MAMESTer/*.mgl` shortcuts, and selecting one switches games through exactly the
path already proven — a new mount writes `MAMESTer.s0`, the manager kills the
running game and starts the new one, with no handler restart. If it does, hide
the picker with `H0SC0,...` (`status_menumask` is already wired in `MAME.sv`).
**Do NOT simply delete `SC0`:** it is not just the OSD item, it is the mount slot
the `.mgl`'s `<file index="0">` targets and the reason `.s0` is written at all,
so removing it likely breaks MGL launching from the main menu too. To check: open
the OSD mid-game and see whether the core browser shows `_MAMESTer` and its
entries.

**Full sweep done (196 families, one parent representative each; unthrottled,
with sound, core loaded).** 188 of the 189 testable drivers run.

- **159 healthy** (>=75 fps, i.e. >=1.25x real time), range 77 (88games) to 195
  (cheekyms).
- **18 marginal (60-75 fps)** — will not reliably hold 60 Hz once throttle and
  OSD are in play: ultraman 61, dynduke 62, sichuan2 62, aafb 64, aztarac 66,
  punchout 67, jedi 68, lastduel 69, thunderj 69, **mk 71**, ataxx 71, wardner 71,
  tail2nos 72, batman 72, supbtime 73, hydra 73, blockout 75, eprom 70.
- **11 below real time**: cheyenne 27 (exidy440), archrivl 34 (mcr68), gunbird 41
  (psikyo), shanghai 41, cchasm 42, 720 44 (atarisy2), toobin 47, turbo 51,
  quantum 56, cischeat 57, wecleman 57.
- **1 crash**: armora (cinemat vector hardware) segfaults right after
  `set_video_mode` — the only driver-specific crash left after the Cyclone fix.
- **7 romsets not in the Ghostware collection** under their 0.37b5 setname, so
  untested: argus, karianx (deniam), firetrk, kncljoe, momoko, skyfox, hardhead
  (suna8).

The slow families reproduce across independent representatives, which is what
makes the family-level grouping trustworthy: exidy440 (crossbow 25, cheyenne 27),
mcr68 (xenophob 33, archrivl 34) and atarisy2 (paperboy 42, 720 44) each came out
slow twice from different games. **Psikyo at 41 fps is a notable miss** — it is
named as a gap target in CLAUDE.md.

**The sweep above measured the present path, not the drivers (2026-08-01).**
Profiling atarisy2 found 65% of process CPU in `nv_present`: the 8bpp path stored
converted pixels one halfword at a time into the uncached `/dev/mem` window,
where each store is its own bus transaction (24.8 MB/s = 15.1 ms per 512×384
frame, vs 89 MB/s for a memcpy — `tools/mister/ddr-write-bench.c`). It now
converts into a cached staging frame and crosses once with memcpy. `720`
44.2 → 78.9 fps, `paperboy` 44.2 → 80.2, `toobin` 45.2 → 82.6, `quantum`
55.1 → 113.2, `klax` 105.1 → 189.1; 22 games re-benched in
`docs/bench-results.md`. Six of the eleven below-real-time families clear 60 Hz
outright. 16bpp drivers keep writing DDR directly (staging them too cost `mk`
2.5%); with that carve-out `mk` is 78.1. Moving the residual 4.19 ms DDR write
to the second A9 core was built and backed out — 1.3× real time with sound is
enough, and it is not worth a threaded DDR channel. Also: an orphaned busy-loop
(PID 5922) was pinning one of the two A9 cores for the whole original sweep.
**Operator decision (2026-08-01): do NOT re-baseline the 196-family sweep.** Its
bands are a *lower* bound — every family is at least that fast, and the 8bpp ones
(most of the set) are substantially faster — so the shippable list only grows.
Treat the bands as conservative rather than current, and re-measure a single
family only when its exact number matters. Verified on device: `720` renders
correctly at 512×384 through the scaler.

New tools: `MISTER_PROFILE=<hz>` (SIGPROF PC sampler in the backend) plus
`tools/symbolize-prof.py`, since the device has no `perf` and gdb cannot unwind
these frames; `MISTER_NO_NATIVE=1` benches the emulator with the present removed.

**Next:** profile the below-real-time families (the operator deferred this);
`mk` at 71 fps deserves attention as a marquee title. Note the earlier
"1.6-2.1x real time" figures for mk/nbajam came from the no-core-loaded bench and
are not comparable — the present path costs ~3.5x (see `docs/bench-results.md`).

**Superseded:** extend the sweep across the remaining ~160 uncovered families, then
judge the three slow families (profile, or ship with a lower `samplerate` /
`-frameskip`).

## Later stages (pointers)
- **8 (original note):** ~40% of drivers fail (dkong/rtype ROM-load;
  sf2/mk2/tmnt/… post-init hang) — establish the shippable list. See
  `docs/bench-results.md`.

## Stage 9 — mame2003-plus as a second engine (in progress)

Evaluating MAME 2003-Plus (0.78, libretro) on the same present path and launch
harness as mame4all-pi. Spec:
[`specs/2026-08-01-mame2003-plus-engine-design.md`](specs/2026-08-01-mame2003-plus-engine-design.md);
plan: [`plans/2026-08-01-mame2003-plus-engine.md`](plans/2026-08-01-mame2003-plus-engine.md).
Operator's stated wins are **only** two: driver families mame4all lacks, and
romset compatibility with the widely distributed 2003-plus reference set.

Branch `mame2003-plus-eval`, based on `presentfix` (the present-path fix plus
`MISTER_FRAME_HASH`), not on `main`.

| Task | Status | Result |
|---|---|---|
| 0 Base on presentfix | ✅ | rebased; backend is the 26 KB hooked version |
| 1 Coverage diff | ✅ | **297 families / 816 parents / 703 non-mahjong** |
| 2 Cross-compile container | ✅ | **+0.27%**, bit-identical frames, ~3× faster |
| 3 Extract `nv_present.c` | ⏳ NEXT | |
| 4 Build 2003-plus static lib | ⏳ building | submodule `d6bf36f6` |
| 5–9 host, bench, decide | — | |

**Task 1 — what 2003-plus actually adds.** `tools/coverage-diff.py` against the
reference set's `-listinfo` XML, mame4all's `-sourcefile` list and the 2,954
`_Arcade` MRA setnames: **297 families / 816 parents** that mame4all lacks and no
MiSTer core covers, of which 113 are mahjong → **703 substantive net-new
parents**. Largest: `taito_f3` 31, `metro` 23, `konamigx` 17, `system32` 17,
`itech32`+`itech8` 29, `kaneko16` 13, `system24` 13. Excluded and reported
separately per `feasibility.md` §5: console-core false gaps (4 families, 152
parents — PlayChoice-10, Vs. System, NSS, Sega C-2) and PS1-class 3D (7 families,
76 parents). Report: [`../coverage-2003plus.md`](../coverage-2003plus.md).

Note this is a **different and smaller number than feasibility.md's ~1,868**,
deliberately: that compared 2003-plus against MiSTer cores, this compares it
against MiSTer cores *plus the mame4all port that already works* — the actual
marginal value of adding an engine.

**Two methodology bugs, both inflating the answer, both fixed with regression
tests.** (a) Driver *filenames* are not stable across MAME versions — mame4all's
`wmsyunit.cpp` is 2003-plus's `midyunit.c`, the hardware Stage 8 benched `mk` and
`nbajam` on — so matching is on **setnames**. (b) The tool sniffed its input
format, and a setname list and a filename list are both single-column; it read
2,301 setnames as families and silently fell back to filename matching. Format is
now an explicit argument.

**Task 2 — the toolchain is not a confound.** A host-native `arm-linux-gnueabihf`
container (`tools/mister/Dockerfile.cross-armhf`, `CROSS=1`) measured against the
qemu one: **+0.27% mean**, every cell inside the 1.5–4% noise floor, and
`MISTER_FRAME_HASH` reports **bit-identical frames** from both arms across four
frames and three drivers — matching values from a third separately built binary.
Both containers are gcc 10.2.1, so this measures *hosting*, not compiler
generation. And it is **~3× faster**: 9m10s against ~30 minutes, because on an
arm64 host there is no emulation at all. Details in
[`../bench-results.md`](../bench-results.md).

**`make` does not rebuild when flags or the compiler change** — no clean step,
one shared `obj_$(TARGET)_mister`, and pattern rules that depend only on their
source. Both arms under one `TARGET` would have relinked identical objects and
reported a ~0% delta: the right-looking answer with nothing behind it.
`TARGET_NAME` now gives each configuration its own object directory. It overrides
`OBJ`/`EMULATOR` rather than `TARGET`, because `Makefile.mister` also uses
`TARGET` for `include src/$(TARGET).mak` — `TARGET=mame-cross` dies on a missing
`src/mame-cross.mak`.

**`MISTER_FRAME_HASH=N`** (on `presentfix`) hashes the published DDR frame, so it
covers both the 8bpp staged and 16bpp direct paths. Two usage traps, documented in
the source: run the binary from its own directory (MAME `chdir()`s to
`realpath(argv[0])`, so `/tmp/mame-x -rompath roms` looks in `/tmp/roms` and
reports every ROM missing), and pick a frame where the driver animates — attract
modes hold still (gng 899–902 are byte-identical), and it has already caught
three would-be false passes.

## Stage 10 — libretro's current MAME (0.289) as a third engine (in progress)

Design: [`specs/2026-08-05-lrmame-engine-design.md`](specs/2026-08-05-lrmame-engine-design.md).
Branch `claude/libretro-mame-engine-target-u14ehm`. Submodule `vendor/lrmame` =
[`libretro/mame`](https://github.com/libretro/mame) `85eaed9c` (upstream MAME
**0.289**, 2026-08-04).

**This is the first engine whose feasibility is genuinely in doubt, so the plan
is one kill decision rather than a schedule.** MAME 0.289's device model,
`emumem` dispatch and scheduler are built for 64-bit desktop silicon; this is an
800 MHz Cortex-A9 on a 32-bit ABI. **Gate: build `SOURCES=pacman`, bench it on the
device with sound and the core loaded. Below 60 fps → stop and write it up.**
Everything past the gate is deliberately unbuilt. (The missing ARM32 DRC backend
is a separate and much smaller issue — measured at 3.2% of drivers and descoped,
below. Pac-Man is a Z80 and never touches `drcuml`, so the gate measures core
overhead, which is the thing genuinely in doubt.)

**The 2003-plus work paid off — the engine is mostly a build problem, not a port.**
Verified by reading the core, not assumed:
- `need_fullpath = true`, `valid_extensions = "cmd|zip|7z"` (`libretro.cpp:718`) —
  so `host_main.c`'s setname → `<rompath>/<setname>.zip` contract is unchanged.
- `SET_HW_RENDER` is inside `#if defined(HAVE_OPENGL) || defined(HAVE_OPENGLES)`
  (`libretro.cpp:938`), so a no-GL build never issues it and renders purely into a
  software framebuffer. **Its failure path returns false from `retro_load_game`**,
  so an accidental GL build does not degrade — it fails to load every game.
- genie links the target with `--version-script=link.T` (exports `retro_*`, hides
  everything else) and `--no-undefined`. That is exactly the frontend contract, so
  the host **links against the `.so` directly** — no `dlopen`, no host source
  change, no libretro-common obligation.
- `nv_present.c` already carries both formats this core can emit.

**What was actually new, and done:**
1. **Toolchain.** 0.289 is `-std=c++20` (`genie.lua:774`); the bullseye/gcc-10.2.1
   cross container cannot build it. New container
   `tools/mister/Dockerfile.cross-armhf-cxx20` (trixie/gcc-14). The old one is
   **kept, not upgraded** — `docs/bench-results.md`'s engine comparison rests on
   mame4all and 2003-plus sharing a compiler.
2. **`tools/build-lrmame.sh`.** Six load-bearing flags, documented in the script.
   The one that is not guessable: **`ARCHITECTURE=` (empty) is mandatory.**
   `PTR64=0` sets `ARCHITECTURE:=_x86` regardless of `PLATFORM` (`makefile:364`),
   which routes the build to the `linux_x86` target and puts `-m32` on an ARM
   compiler. Upstream's own `android-arm` block clears it the same way.
   Also required: `OSD=retro` — `CONFIG=libretro` does **not** imply it (the OSD
   default is `sdl`, `makefile:455`), and without it there are no `retro_*` entry
   points at all.
3. **Engine seam in the host.** `make ENGINE=lrmame`; default unchanged.
   Per-engine library, include path, container and link flags.
4. **`host_env.c` gains the commands 0.289 issues and 0.78 did not.**
   `SET_SYSTEM_AV_INFO` is the load-bearing one — it carries a timing block, so
   unlike `SET_GEOMETRY` it can change the refresh rate, and the modeline is
   derived from that rate. Every new case is `#ifdef`'d on the constant so one
   shared source compiles against either engine's `libretro.h`.

**Verified this far:** genie generates all 27 projects for the armhf
configuration; the emu core cross-compiles; all six host sources compile clean
against 0.289's `libretro.h` with `-DMAMESTER_ENGINE_LRMAME`; the Makefile
resolves both engines' link lines.

**The build works end to end.** `SOURCES=src/mame/pacman/pacman.cpp` cross-built
clean (~40 min on 4 cores, dominated by MAME's own emu core and 3rdparty, not by
the driver): **`mamester_libretro.so`, 40 MB, ELF 32-bit ARM EABI5**, 151 drivers,
exporting **34 `retro_*` symbols and nothing else** (link.T works as advertised)
and needing only `libm`/`libc`/`ld-linux-armhf.so.3` — no SDL, no GL, not even
pthread. The host links against it and **runs**: under `qemu-arm` it reports
`MAME 0.289 (85eaed9c)`, captures 28 core-option defaults through
`host_environment()`, selects XRGB8888, and MAME's own init resolves the driver
("System found: pacman", rotation 3 = 270° CCW accepted) before failing at ROM
load with `Required files are missing` — correct, since no romset exists here.
So the frontend contract, the environment callback, the engine seam and the
toolchain recipe are all proven; what is untested is pixels, sound and speed.

**Two findings that only appeared by running the artefact:**

1. **Link the `.so` with `-L<dir> -l:<file>`, never by path.** genie builds it
   with **no `-soname`**, so `ld` records whatever it is handed; given a path,
   `DT_NEEDED` becomes the absolute *build-host* path, which does not exist on
   the MiSTer — the binary then dies in the loader before `main()`. Both forms
   link without a warning, so this is visible only under `objdump -p` or on the
   device. Fixed; `RUNPATH=$ORIGIN` resolves the bare name beside the binary.
2. **`SOURCES=` filtering breaks clone→parent links across files.** The pacman
   build emitted **30** `Driver is a clone of nonexistent driver` validity errors
   (e.g. `8bpm` → `8ballact`) because the parents live in files the filter
   excluded. Non-fatal — it proceeded to ROM load — but the shippable subset has
   to be closed over parents, or accept that those clones are unrunnable. Worth
   settling when the subset is chosen (Stage 5 of the design doc).

**Launch integration done (2026-08-05).** `deploy.py` had claimed since the
2003-plus work that "the per-driver choice is made at launch time
(`game_manager.sh`)" — **it was not**: `MAME_BIN` was a single fixed path, so
every game launched mame4all and the deployed `mame2003` binary was never used
by anything. An `engine <name>` line in `games/mame/opts/<setname>.opt` now
selects the binary (`game_engine()` reads it; `game_opts()` deletes it, since
`engine lrmame` on MAME's command line is an unknown-option error). Unknown names
and un-deployed engines fall back to the default and say why in the game log —
loud but not fatal, because a device with no console is a bad place to fail
silently. **The default stays mame4all**: `deploy.py` calls 2003-plus primary,
but flipping it here would silently re-point every already-deployed game, so
`engine mame2003` in `default.opt` is left as an operator decision and the
mismatch is documented at `engine_bin()`.

`deploy.py` also pushes **`lrmame_libretro.so`**, paired with its binary rather
than listed separately — lrmame is the only engine that is not self-contained
(the host resolves MAME through an `$ORIGIN` RUNPATH), and a missing `.so` fails
in the dynamic loader before `main()`, which looks nothing like a missing engine.
Tests 31 → 46.

**THE GATE PASSES — 88.3 fps on `pacman`, sound on** (device, 1800 frames in
20.39 s, 4 underruns; full run in [`../pr7-test-results.md`](../pr7-test-results.md)).
0.289's device model, `emumem` dispatch and scheduler are **not** the wall the
design doc feared, at least for a Z80 driver. `s1945ii` (SH-2, ROT270, 320×224
@ 60 Hz) also runs, at **30.1 fps** — half speed, and that is the number that
decides whether the engine is useful beyond trivial drivers.

That 88.3 is an **understatement**: it was measured with `mem_wc` unloaded, on
the Strongly-Ordered fallback (see the present-path section above).

**Four defects came out of that test round**, all fixed, and the third is the
one worth remembering: **nothing the trixie container builds can run on the
MiSTer.** Device glibc is 2.31, trixie's armhf is 2.41, and `--sysroot` does not
fix it — Debian's cross gcc resolves `crt1.o` and `libc.so` through its own
prefix. Worse, **trixie *is* Debian's armhf time64 transition**, so its prebuilt
`libstdc++.a` references `__clock_gettime64` and cannot be flagged out of it.
The container is now **bookworm/gcc-12** with the bullseye 2.31 target tree,
plus `tools/mister/glibc231-compat.c` (`__libc_single_threaded`, `arc4random`)
and `-lpthread`. Nothing caught this before the device: the build was clean and
so was the qemu run, because qemu was pointed at the container's own libraries
with `-L`. The other three: `drc-diff` CI fetched only `vendor/lrmame`; the
documented gate build did not link (`SOURCES=pacman` has no DRC CPU, so
`drc_diff.cpp` is dropped and `inject.sh`'s call dangles — now a weak
declaration plus a null test); and `deploy.py` pushes `game_manager.sh` while
it is running, so the first pick after a deploy runs the *old* manager.

**DRC on ARM32: measured, then descoped (operator, 2026-08-05).** 0.289 has no
32-bit native DRC backend for ANY architecture (x86-32's went too), and
`drcbearm64` cannot apply — the A9 is ARMv7-A with no 64-bit mode — so everything
DRC-backed runs `drcbec`, the portable UML interpreter. **This is narrower than it
sounds: 147 of 4652 driver files, 3.2%** (`tools/lrmame-drc-scan.sh --summary`).
Z80/6502/6809/68000/68020/V60 never touch `drcuml`, and none of the Stage-8 gap
families checked (`taito_f3`, `konamigx`, `segas24`, `kaneko16`) include a DRC
CPU. Most of the 147 is out of scope regardless (SGI, Mac, skeletons, Jaguar);
the real loss is the borderline SH-2/SH-3 boards — **`psikyosh`** (a named gap
target), `stv`, `feversoc`, `cv1000`. Writing `drcbearm32` was rejected: ~5,700
lines by `drcbearm64.cpp`'s measure, and harder on ARMv7 (~14 GPRs vs 31, and a
64-bit-register IR needing register pairs). Stage 5 picks the subset from the
non-DRC majority, with the exclusion list generated rather than remembered.

**This does not move the gate.** Pac-Man is a Z80 and never sees the DRC, so the
fps number measures 0.289's core overhead — device model, `emumem` dispatch,
scheduler — which is the thing actually in question.

**Open, deliberately deferred to after the gate:** `M16B`. Defining it makes the
core report RGB565 (`libretro.cpp:771`) — already the DDR format, skipping a
per-frame convert — but `libretro_shared.h:10` defines `HAVE_RGB32`
unconditionally beside a `FIXME: re-add way to handle 16/32 bit`, so the 16-bit
path is plausibly bit-rotted. Treat it as a measured arm (`MISTER_FRAME_HASH`
both ways), not a free win.

**Licensing changes with this engine.** MAME has been **GPL-2.0-or-later /
BSD-3-Clause since 0.172**, not the pre-2016 non-commercial licence that governs
mame4all and 2003-plus. Less restrictive, but different obligations —
`CLAUDE.md`'s licensing note describes only the old one and needs updating.

### Perf levers (PR #9) — full results in [`../bench-results-lrmame.md`](../bench-results-lrmame.md)

Protocol, non-negotiable on this device: **interleaved, arm order alternating
per (game, rep), repeated.** Per-cell spread is 1.5–4% and cell order alone
moved one arm by 2%, so a non-interleaved result under ~5% means nothing.
`tools/lever-ab.sh` is the harness and resumes from its own TSV. Test drivers:
`pacman` (Z80, never touches `drcuml`) and `s1945ii` (SH-2, DRC-backed). The
core must be **loaded** — a present measurement with no core loaded measures
nothing.

**What ships: write-combining on, everything else off.** That is already the
default set, and it is the whole of what the host side delivers.

| lever | `pacman` | `s1945ii` | verdict |
|---|---|---|---|
| write-combining | +15.0% | +5.0% | **real, ships on** |
| salvaged host (`nv_present.o` at `-O3`, `build_reverse()` as a constructor) | +6.0% | +2.5% | **real, landed** |
| `rgb32` render-pipeline bypass | −0.3% | **+7.1%** | **real, landed** |
| `MISTER_SCHED_RT=5` | +2.8% median | −1.8% | **sign flipped — off** |
| `MISTER_THREADED_PRESENT=1` | −1.4% | +0.5% | null |
| `MISTER_EMU_CPU=0` | +0.8% | +0.6% | null |
| `M16B` (RGB565), stride bug fixed | −2.6% | −0.2% | not a lever |
| MAME's UI | — | — | not a lever, sub-1% |

**MAME's own render pipeline is the largest single host cost, and it is a fixed
tax.** Profiling `pacman` to answer an FPGA-offload question found
`software_renderer<>::setup_and_draw_textured_quad` at **27.3%** — more than
twice the emulated Z80 (12.5%). Normalised on the 800 MHz A9 the two texture
paths cost **31.3 cyc/px (`PALETTE16`)** and **21.7 cyc/px (`RGB32`)**, so at
60 Hz on a 320×240 that is **18.0% of the A9 for `ind16` and 12.5% for
`rgb32`**, independent of emulation weight. A driver at 53 fps reaches 60 on
this alone. The gap families split by `screen_update` bitmap type — `rgb32`:
`psikyosh` `taito_f3` `konamigx` `segas32` `suprnova` `cv1k` `metro` `ms32`;
`ind16`: `segas24` `ssv` `seta2` `namconb1` `pacman`; mixed: `kaneko16`.

**The `rgb32` half is fixed in software, no RTL: +7.1%.**
`mamester_try_direct_blit()` recognises the trivial case — one QUAD, RGB32/ARGB32
with `BLENDMODE_NONE`, no colour modulation, 1:1 bounds, identity texcoords —
and `memcpy`s per row instead of calling `draw_primitives`. Screenshot is
**MD5-identical**. The predicate engages **297/300 frames on `s1945ii` and 0/300
on `pacman`**, which is correct: `pacman` is `PALETTE16`, where the blit is a
palette lookup, not a copy. Predicted 12.5%, delivered 7.1% — the remainder is
the full-frame `memcpy` the fast path still does; removing that means handing
the driver's bitmap pointer to the host, which changes what
`retro_video_refresh` passes. **The `ind16` half wants the FPGA**: palette RAM
in fabric, 16-bit indices in DDR, lookup at the scanout tap. Worth 18%, needs
RTL, still open. Bypass caveats, both acceptable: MAME's UI overlay and
artwork/bezel are lost (the host ignores both), and vector/SVG/multi-screen
drivers must keep the normal path.

**Patches to `vendor/lrmame` use `git apply`, not sentinel fences**
(`tools/lrmame-patches/`), because this one *replaces* upstream lines. Conflicts
are **fatal** in `build-lrmame.sh` — a patch that stops applying after a
submodule bump must not silently become a build without it, which is how the
M16B bug survived. One patch per region: per-patch state checks cannot see a
stacked series.

**Three methodology findings, each of which produced a wrong answer first:**

- **A lever measured against one binary does not carry to another.** `SCHED_RT`
  measured **+11.6%** on `s1945ii` pre-bypass and **−1.8%** on `lrmame-fast`. It
  bought back preemption during a frame that was ~29 ms of emulation plus a full
  `draw_primitives` pass; the bypass removed part of that frame and what is left
  does not pay for a `SCHED_FIFO` thread on a two-core box also running
  Main_MiSTer and three shell poll loops. **Re-run every lever after changing
  the thing being measured.** Stacking all three shipping candidates is −3.8%.
- **An A/B between two binaries is only about the change you made if every other
  difference is zero.** Comparing the salvaged host against the *deployed*
  binary gave +20.8%/+7.1% — but that binary predated write-combining support,
  so the A arm was Strongly-Ordered and the B arm was not. It re-measured the WC
  lever and attributed it to a convert loop. "The old one is still on the
  device" is not a controlled A arm.
- **Instrument for the miss as loudly as the hit, and never sample frame 1.**
  The fast path first reported `declined` on both drivers because the report
  fired on frame 1 — MAME's own startup UI, 28 primitives of rects and font
  glyphs with **no screen quad in the list at all**. Both now sample frame 300
  and report an aggregate. A predicate that silently never matches is
  indistinguishable from a lever that did nothing.
- **The frame hash is the wrong gate for a pixel-format lever.** The 32-bit path
  renders 8/8/8 and truncates; the 16-bit path rasterises 5/6/5 natively. The
  hashes differ even when both arms are correct. `M16B`'s garbage output was
  caught by a **screenshot**, not a hash.

**Also ruled out:** killing `Main_MiSTer` (DreamSTer does this; not available
here — `fpga/MAME.sv:332` sources `joystick_0..3` from `hps_io`, which only
Main_MiSTer drives, so `nv_pads()` would read frozen input and the launch path
would go with it). Renice is the usable form of the same idea.

**Where `s1945ii` stands: 39.0 fps against the 60 it needs — a 35% gap**, on
`lrmame-fast` with WC on. The host-side lever set is exhausted. The profile says
why the rest is hard: about a third of the steady state is the JIT'd SH-2 and
the remainder is spread thin. Closing it needs the `ind16` FPGA palette path
(does not apply to this driver), removing the bypass's remaining `memcpy`, or a
different engine for this driver.

**Still open on this engine:** FPGA palette lookup at scanout for `ind16` (18%,
needs RTL); FPGA format-convert offload (~6.6%/~2%, needs RTL); **ROM-load
latency** — ~19% of a 21-second run is SHA-1 verification of the romset, which
costs no frames but is seconds of launch delay on every start;
`mame_thread_mode`/`OPENMP=1`, blocked on a discrete-sound romset in 0.289
format; compiler arms (LTO, `-ffast-math`, PGO — not `-O2`-vs-`-O3`, which is
already `-O3`).

## Stage 11 — `drcbearm32`, an ARM32 DRC back-end (in progress)

Reverses this file's own 2026-08-05 descope, on the operator's instruction.
Design: [`specs/2026-08-05-drcbearm32-design.md`](specs/2026-08-05-drcbearm32-design.md).

**The descope had the wrong base, and that is the whole finding.** It sized the
work as backporting `drcbearm64.cpp` and rejected it on ARMv7 having ~14 GPRs
against 31 and no 64-bit registers for a 64-bit-register IR. Both facts are
true; neither is the relevant one. **MAME shipped a 32-bit back-end,
`drcbex86.cpp`, through `mame0287` and deleted it only in 0.288/0.289** —
verified by fetching the file at each tag (present at 0.250 … 0.287, 404 at
0.289). It ran the whole UML on *seven* usable GPRs for two decades. Register
pairs, synthesised 64-bit shifts/multiplies/divides, flag reconstruction: all
already solved there. Relative to the back-end actually worth copying, ARMv7 is
a *doubling* of the register file, not a scarcity.

And the port distance is short. **`uml.h` is byte-identical between 0.287 and
0.289** — every opcode, every parameter type. The entire interface delta is
`drcbe_interface::hash_invalidate_range()`, a `max_sequence_length` argument on
`drcuml_state`/`drc_hash_table`, and the factory signature.

**What is genuinely new is the encoder, because asmjit has no AArch32.** It
enumerates `Arch::kARM` and `kThumb` — which is the trap — but
`3rdparty/asmjit/asmjit/arm/` ships `a64*` and nothing else. There is no
`a32::Assembler` to target, so `drcbex86`'s algorithms cannot simply be
retargeted by swapping an emitter namespace.

### Done

- **`tools/mame-drc-arm32/arm32emit.h`** — an ARMv7-A A32 encoder: data
  processing with all four shift kinds, `movw`/`movt`, both load/store
  families, block transfer, multiplies, ARMv6T2 bitfield ops, NZCV access,
  VFP single/double. Targeting ARMv7 rather than v5/v6 is load-bearing:
  `movw`/`movt` mean **no literal pool**, so no pool placement, no
  mid-sequence drain, no PC-relative reach limit inside the code cache.
  `SDIV`/`UDIV` are deliberately absent — they are ARMv7-R/M or ARMv7-A with
  the idiv extension and **the A9 has neither**, so `DIVU`/`DIVS` must lower
  to a call.
- **`tests/arm32emit/`** — the encoder differentially tested against
  `arm-linux-gnueabihf-as`, which is installed in this container, so it runs
  with no device and no MAME: **391 instructions, all matching.** It paid for
  itself on the first run — the single-precision `Vm` field splits high-4 into
  bits[3:0] and low-1 into bit 5, the first draft wrote it as a plain shift,
  and 15 VFP instructions silently addressed the wrong register. That is
  exactly the failure mode this back-end cannot afford, since a mis-encode is
  not a compile error but a wrong answer inside a game minutes in.
- **`tools/mame-drc-arm32/inject.sh`** — idempotent copy-and-patch into the
  submodule (`--check`, `--revert`), following `build-mame.sh`'s arrangement
  for the mame4all present back-end. Patches `drcuml.cpp`'s `NATIVE_DRC` chain
  and `scripts/src/cpu.lua`. Note it adds a **separate** `files{}` block for
  `PLATFORM=arm` rather than widening the existing one, which would have handed
  the armhf compiler `drcbex64.cpp` and `drcbearm64.cpp` for no reason.
- **`DRC=1` in `tools/build-lrmame.sh`.** Two flags come off, not one:
  `FORCE_DRC_C_BACKEND=1` is obvious, but **`NOASM=1` also has to go** — it
  defines `MAME_NOASM`, which the `NATIVE_DRC` chain tests, so leaving it set
  selects `drcbec` even with the back-end compiled in. Dropping it also lets
  `eminline.h` reach `eigccarm.h`, an ARM path unused in this build until now.
- **`drcbearm32.{h,cpp}` compiles against 0.289's `drcbe_interface`**, verified
  with a native `g++ -fsyntax-only` over MAME's headers — no cross toolchain
  and no full build needed, which makes the iteration loop seconds rather than
  the hour a genie build costs.

### Not done — and this is most of the work

**No instruction that computes anything is lowered yet.** Structural opcodes
(`HANDLE`/`HASH`/`LABEL`/`COMMENT`/`MAPVAR`/`NOP`) are real; everything else —
control flow, the integer ALU, the flag ops, the entire float set — is a
`fatalerror`, deliberately, so a missing opcode cannot become a game that runs
and is wrong. The entry/exit/nocode stub shapes follow `drcbex86`'s model (call
into generated code, `nocode` returns to the caller) but have **not** been
checked against its `hashjmp`/`exit` contract; that check is the next step and
it gates everything after.

Correctness plan, in order: link into the `pacman` subset (proves wiring only —
Pac-Man is a Z80 and never touches `drcuml`); link and boot a DRC subset
(`psikyosh`, SH-2, a `CLAUDE.md` gap target); then the real gate, **`MISTER_FRAME_HASH`
matching between a `FORCE_DRC_C_BACKEND=1` build and a `DRC=1` build of the same
driver over N frames**; then `MISTER-BENCH fps=` to say whether it was worth
doing. Steps 2–4 need the device.

**This is downstream of a gate that has not been run.** `tools/lrmame-drc-scan.sh`
still measures the reachable set at **147 of 4652 driver files, 3.2%**, most of
it out of scope for other reasons (SGI, Mac, Jaguar, skeletons); the real
recoveries are the SH-2/SH-3 boards — `psikyosh`, `stv`, `feversoc`, `cv1000`.
And Stage 10's Pac-Man gate — can 0.289's device model, `emumem` dispatch and
scheduler hold 60 fps on an 800 MHz A9 — is untouched by any of this and still
unmeasured. If that gate fails, this back-end has no engine to live in. It is
being built gate-independent (it is tied to MAME's UML, not to the libretro
host) but the ordering risk is real and deliberate.

### Correction (same day): asmjit *does* have an AArch32 port

The entry above says asmjit ships `a64*` and nothing else. That is true of the
copy **MAME vendors** and false of asmjit **upstream**, which has an unmerged
[`a32_port`](https://github.com/asmjit/asmjit/tree/a32_port) branch — flagged by
the operator, and it materially changes the encoder decision.

It is not a stub: one WIP commit (`594cb9e`, 2025-11-29, by asmjit's author),
**21,406 lines**, `a32assembler.cpp` alone 11,016, Assembler/Builder/Compiler,
A32 *and* Thumb, 1,342 emitter entries. It is on the **same version line MAME
vendors** (both `ASMJIT_LIBRARY_VERSION 1.21.0`, 16 shared files differing), and
its own `core/` changes are ~70 lines across five files — so the `a32*` sources
plausibly drop into the vendored tree.

`tests/a32-asmjit/` qualifies it the same way `tests/arm32emit/` qualifies our
own encoder — diff against `arm-linux-gnueabihf-as`, over the subset the
lowering needs rather than a survey of all 1,342 entries. Result: **85 of 85
encodings match exactly.** Two defects, both small and localised:

- **`lsr #32` / `asr #32` rejected** (`kInvalidInstruction`). Legal A32 — an
  encoded amount of 0 *means* 32 for those two — and not exotic here, since
  synthesising 64-bit shifts on a 32-bit host reaches for shift-by-32 constantly.
- **The rejection path segfaults**: `EmitterUtils::log_instruction_failed()`
  calls `_funcs.format_instruction`, which the a32 emitter never installs. A
  refused instruction is a null-pointer crash with no diagnostic instead of an
  error return. `run.sh` patches this to run at all.

**A false bug report, caught before it went anywhere, and worth recording as a
method failure rather than a code one.** The first run showed six disagreements
in a damning pattern — every non-LSL shift encoded as LSL — which reads exactly
like "a32 drops the shift type". It was **our call-site bug**: the shift op
lives in the predicate of the *last* operand (`a32assembler.cpp:972,986` reads
`o3.predicate()`), so it is `add(rd, rn, rm, lsr(16))`, never
`add(rd, rn, lsr(rm), imm(16))` — and the wrong form encodes silently as LSL
because LSL is predicate 0. Corrected usage: 85/85. **A differential test proves
that something disagrees, never whose fault it is**, and the fact that the wrong
answer was *plausible* is what made it dangerous.

**Recommendation: qualify-then-adopt, and decide before the lowering is
written.** `arm32emit.h` stays as fallback and oracle. The case for adopting a32
is not its coverage but that **`drcbex86.cpp` is written against asmjit** —
retargeting it inside the same `CodeHolder`/`Label`/`Mem`/relocation machinery
is far more mechanical than retargeting onto a bespoke encoder, and that
lowering is ~7,700 lines, the dominant remaining cost. The residual risk is not
correctness-so-far but **staleness**: the branch is unmerged and four months
behind master, and vendoring 21k WIP lines into a submodule we do not control is
a maintenance position, not a free win. Switching encoders after the lowering
exists is a rewrite, which is why this is a decision and not a preference.

### Adopted: asmjit a32 is the encoder (operator decision)

Integration proved before commitment, in this order:

1. **The a32 sources compile against MAME's asmjit core**, not just upstream's.
   The branch's seven core/x86 hook files apply to MAME's copy despite the two
   trees sitting at different points on master — both are 1.21.0.
2. **They encode identically there.** Rebuilt on MAME's core, the corpus still
   matches `arm-linux-gnueabihf-as` on every case, so the 16-file drift changes
   nothing that matters.
3. **The `lsr #32` defect is fixed**, and the fix found a second, worse half.

**The shift bug was worse than "rejects a legal instruction".** a32 validates
every immediate shift amount as `amount <= 31`, which is wrong in both
directions at once: it rejects the legal `lsr #32`/`asr #32`, *and* it accepts
`lsr #0`/`asr #0` and silently encodes them as shift-by-32 — asmjit's `lsr(0)`
and the assembler's `lsr #32` are the same word, `e0843025`. A32 has no LSR #0
or ASR #0; an encoded amount of 0 *means* 32. The accepting half is the
dangerous one, and it would have been invisible: the lowering would have asked
for a no-op shift and got a 32-bit erasure.

`tools/mame-drc-arm32/asmjit-a32-fixes.py` fixes both that and the null-formatter
crash, at the three general data-processing sites. The `pkhbt`/`pkhtb`/`ssat`
sites have the same bug class and are left alone — different per-instruction
shift rules, and outside the UML lowering's path. Corpus after the fix:
**88 of 88 encodings match, 3 of 3 invalid forms correctly refused.**

**Vendoring.** `vendor/asmjit-a32` is a submodule pinned to `594cb9e`;
`inject.sh` copies the `a32*` sources into MAME's asmjit, takes the core hooks
straight from the commit (`git diff HEAD~1 HEAD`) rather than storing a patch so
provenance stays upstream, runs the fixes, and opens `3rdparty.lua`'s asmjit
project — which is gated on the same x86/arm64 pair `cpu.lua` is — to
`PLATFORM=arm`. All idempotent; `--revert` restores the submodule exactly.

**`drcbearm32.cpp` now emits through `a32::Assembler`**, with drcbearm64's
`CodeHolder`/`copy_flattened_data`/`invalidate_instruction_cache` mechanics
rather than a hand-rolled buffer. It compiles against 0.289 with the a32 encoder
installed. Scope is unchanged and still small: structural opcodes only,
everything else `fatalerror`. `arm32emit.h` and `tests/arm32emit/` stay as the
fallback and as the oracle the a32 corpus was built from — 391 instructions,
still passing, still the thing that would catch a32 regressing.

### The differential harness exists (`tests/drc-diff/`)

Nothing in the lowering was verified by anything, which is why this came before
more lowering. `tests/drc-diff/README.md` carries the detail; the findings worth
keeping in the ledger:

**One `drcuml_block` can be generated twice, and that was the open question.**
A back-end's `generate()` reads nothing from the block but `invariant()` and
uses it only as the channel for `abort()` — three appearances each in
`drcbec.cpp` and `drcbearm64.cpp`, and the `drcbeut` bookkeeping is the same
shape. **Nothing anywhere asserts `inuse()`**, so the block needs no second
`begin()` and the harness needs no second `drcuml_state`. `block.end()` is
never called, because `end()` routes generation through the state's own
back-end — which is neither of the two under test.

**Two back-ends in one process, but not via `drc_use_c()`.** That option is
read once in `drcuml_state`'s constructor and selects the single back-end that
state will own. The factories are exported, so the harness calls
`make_drcbe_c` and `make_drcbe_<native>` itself, **each over its own
`drc_cache`** — the hash table, label list, map variables and the
`drcuml_machine_state` the generated code operates on are all per-back-end
members allocated out of the cache the back-end was handed, so one shared cache
would have the two code streams overwriting each other's register file.

**`SAVE`/`RESTORE` are the readout.** They move a whole `drcuml_machine_state`
in one opcode, so a case is `HANDLE / RESTORE seed / body / SAVE out / EXIT`
and the diff covers all ten I registers, all ten F registers, `exp`, `fmod` and
`flags` with no per-opcode plumbing. The cost: a back-end without those two
reports *every* case unimplemented, which is precisely why they are the first
two to lower. Comparison is field-by-field, not `memcmp` — the struct has tail
padding no back-end writes — and floats compare as bit patterns, since
comparing as `double` calls two NaNs unequal and two encodings of zero equal.

**An unlowered opcode is a report, not a crash.** The deliberate `fatalerror`
is caught per case and reported as `UNIMPL`, so the harness is a coverage
report from the first day of lowering rather than only after the last. Each
case gets a fresh `drcuml_state` and cache pair, because `generate()` raises
from mid-block and `block_end()` never runs. `drcbec` is the oracle: if *it*
refuses a case the harness says `BAD-CASE`, which is the difference between a
corpus bug and a lowering bug.

**`HOST=1 tools/build-lrmame.sh`** builds x86_64 natively into its own
`BUILDDIR` (`build-host`, so the two configurations do not clean each other
out), with neither `NOASM` nor `FORCE_DRC_C_BACKEND`, so the host's native
back-end is compiled in. `run.sh --host` then diffs `drcbe_c` against
`drcbe_x64`. **That run is the calibration and it comes first**: a differential
test proves that two things disagree and never whose fault it is, and
`tests/a32-asmjit/` already paid for that lesson once — six disagreements in a
pattern that read exactly like an asmjit defect, and the bug was in the calling
code.

**The calibration run is clean: 48 of 48 cases agree between `drcbe_c` and
`drcbe_x64`.** It took four rounds to get there and `drcbe_x64` was correct in
every one — the failures were all the harness's or the corpus's, which is
exactly the outcome that makes the control worth running. What it caught,
because each would otherwise have been read as an ARM32 lowering bug:

- **`drc_cache` is two-phase in 0.289.** The constructor allocates nothing and
  leaves every pointer null; `allocate_cache()` maps the memory, and every CPU
  core calls it from `device_start` (`sh.cpp:41`). Omitting it does not fail
  loudly — `alloc_near()` returns null and the crash lands in whichever
  back-end constructor first writes through it.
- **The cache floor is the hash table, not the generated code.** At
  `addrbits=32, ignorebits=1` the empty L1/L2 tables alone are 768 KB out of
  the main cache; 1 MB segfaulted. The SH cores use 32 MB and so does this.
- **Three kinds of UML state are undefined and must not be compared**: a 4-byte
  op on a 64-bit register defines the low half only (`drcbe_c` zeroes the
  upper, `drcbe_x64` preserves it, both conform); `FLAG_U` is FP-only and
  `drcbe_x64` maps x86 *parity* onto it; and flags are undefined until an
  opcode *computes* them — `RESTORE` loading them is not the same thing. The
  compare mask is therefore derived from the block via `is_param_out()` and
  `output_flags()` rather than hand-maintained per case.
- **`instruction::size()` is not always the destination width.** For `FTOINT`
  it is the float *source* width; the integer destination's width is the
  `SIZE_` parameter.
- **Two corpus bugs**, one of them found by the oracle path: `FFRFLT` converts
  *between* float widths, so a size-matched `fdfrflt` is an invalid opcode, not
  a no-op, and `drcbe_c` refusing it is the `BAD-CASE` report working.

A crash handler was added off the back of this — a back-end being written emits
wrong code and jumps into it, and a bare SIGSEGV in the code cache has no
walkable stack. It names case, back-end and phase from a signal handler and
exits 3, which is what turned the first crash into
`case='empty' backend=drcbe_c phase=drcuml_state`.

### The lowering is complete, and the corpus is what says so

**Every UML opcode in `uml.h` is lowered.** `tests/drc-diff/` reports
**77 of 77 cases agreeing between `drcbe_c` and `drcbe_arm32`**, under
`qemu-arm`, over **twelve different input states**; the same corpus is clean
against `drcbe_x64` on the host, which is what makes the ARM number mean
anything at all.

**The flag model is `drcbearm64`'s, copied rather than re-derived.** N/Z/V live
in APSR as UML S/Z/V; C and U live in a software flags register (r10); and
`m_carry_state` tracks what the hardware carry currently is relative to the UML
one, so a consumer reloads it only when the polarity is wrong. ARM sets C to
NOT-borrow after a subtract and UML calls it borrow — that one disagreement is
the entire reason the mechanism exists, and it is the single most error-prone
part of an ARM UML back-end. `drcbex86` supplies the algorithms, because it ran
this IR on a 32-bit host for twenty years.

**Six families call a C helper in the same file rather than being synthesised
inline**, and the reason is the same for all of them — cold, long, and a UML
register is memory in this back-end so "pass the destination" is just passing a
pointer. Both divides (**the A9 has no divide instruction at all**, so a divide
is a call however it is written), 64×64 multiplies, 64-bit shifts and rotates,
and the 64-bit integer/float conversions.

**The encoder came first, as the design demands.** `tests/a32-asmjit/` grew from
88 encodings to **130, all matching `arm-linux-gnueabihf-as`** — `mrs`/`msr`,
the predicated forms the carry capture rides on, the register-amount shifts the
64-bit synthesis needs, and the three FPSCR words. That last one is a real gap
in a32: it has **no VMRS/VMSR emitter entry**, and its `MRC` path wants a
`kOpRegC` operand which is signature zero and which nothing in the library
constructs. FPSCR access is therefore `embed_uint32()` of the literal word,
qualified against the assembler like everything else.

**The corpus grew from 48 cases to 77, and that growth is the real work.** The
48 passed on the first run of the finished lowering, which is a reason for
suspicion rather than satisfaction: a corpus that passes immediately is more
likely to be missing the hard cases. The additions are deliberately what
nothing was asking about — the narrow multiplies whose overflow comes from the
wide product, divide by zero, 64-bit ADDC/SUBB, the rotates through carry,
BFXU/BFXS, every condition code through SET, and, worth more than all the rest,
**the call contract**: CALLH, nested CALLH, conditional CALLH and RET, EXH, a
hash jump that hits, a hash jump that misses through the nocode stub into
RECOVER, and CALLC. On a host with a link register rather than a pushed return
address that is the whole of the remaining risk, and none of it is visible in a
corpus of straight-line arithmetic.

**`MAMESTER_DRC_SEED` varies the state the corpus starts from**, and CI sweeps
eight seeds. One fixed seed tests one set of values, and flag reconstruction is
exactly the code that is right for the values it was written against. A forty
seed soak of the ARM back-end is clean.

**Four places where the two REFERENCES disagree**, all found by the calibration
and sweep runs, all recorded in the corpus rather than worked around silently.
Where `drcbe_c` and `drcbe_x64` disagree, a differential test has nothing to
say, so the corpus stays inside the defined domain:

- `drcbec` computes a 64-bit `ROLC`'s carry contribution with a **32-bit**
  shift, so a rotate by more than 32 is undefined and comes out zero;
  `drcbe_x64` is right and `drcbearm32` agrees with it.
- `BFXU`/`BFXS` at full register width: `drcbec` returns the value,
  `drcbe_x64` returns zero.
- `FTOINT` from single precision to a 64-bit integer, negative value: `drcbec`
  zero-extends the 32-bit answer, `drcbe_x64` sign-extends the 64-bit one.
- `drcbec`'s `EXH` pushes the EXH instruction where `CALLH` pushes the one
  after it, so a `RET` out of an exception handler re-executes the EXH forever.
  Real cores end a handler in `HASHJMP` or `EXIT`, which is why nothing has
  ever tripped over it.

A fifth finding was a **bug in this tree**: `tools/mame-drc-arm32/inject.sh`
wrapped upstream's own `cpu.lua` lines inside the sentinel fence, and
`--revert` deletes whatever a fence contains — so a revert left the tree
without `CPU_INCLUDE_DRC_NATIVE`, and the next host build failed to link
`make_drcbe_x64`. Both patches are additive now and a revert restores
`cpu.lua` byte for byte. That is the second time the revert path has been
wrong in a way only an alternating build would show.

### The device run closed the list — bit-identical, and worth 7.4×

The corpus left four things open: the emulated-memory opcodes had never
executed (the harness starts a machine with **no content**, so `m_space` is
empty and there is nothing to read — and they are the opcodes every real driver
leans on hardest), `DEBUG`/`BREAK`/the end-of-block handler likewise, no driver
had run at all, and **qemu is not a Cortex-A9** — it models NZCV faithfully
enough for the corpus but proves nothing about instruction-cache coherency (the
`osd::invalidate_instruction_cache` after every block), which is exactly where a
JIT that is right under emulation is wrong on hardware.

**Run on the device (PR #7 test round, `.81`, real A9):**

| | `pacman` (Z80, no UML) | `s1945ii` (SH-2, UML) |
|---|---|---|
| DRC=1 (`drcbe_arm32`) | 88.6 fps | 27.9–30.1 fps |
| DRC=0 (`drcbec`) | 88.1 fps | **3.8 fps** |
| frame hash @400/@600 | `601bc720788077ab` | `912aeffbaed7ea59` |
| frame hash @800/@1200 | `d7946c8cc9c3a464` | `466e938dbc9304cb` |

**Hashes are identical between the two back-ends on both drivers.** The ARM32
lowering produces bit-identical output to the UML interpreter on a real driver,
memory path included, on real silicon — and it is worth **~7.4× on SH-2**.
`pacman` is the control: it never touches `drcuml`, and the two arms agree
within noise, which is what says the A/B is measuring the back-end and not the
build.

**Still open, and it is now a short list:**

- **`DEBUG` and `BREAK`** remain untested. Neither fires in a normal run, so
  the driver test does not cover them and the corpus still cannot.
- **The corpus still has no address space.** The driver run covers `READ`/
  `WRITE` empirically for the paths `psikyosh` uses; it is not a substitute for
  a differential case, and a regression in an unused addressing form would not
  be caught.
- **One driver family.** SH-2 via `psikyosh` is the only DRC CPU exercised on
  hardware. `stv`, `feversoc` and `cv1000` are the rest of the borderline set
  the back-end was written for, and none has been run.

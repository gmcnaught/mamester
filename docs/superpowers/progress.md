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

| Stage | Status | Commit | Evidence |
|---|---|---|---|
| 1 Core builds + loads | ✅ | `f95b8aa` | CI `MAME_*.rbf`; loads `CORENAME:MAME`; 320×240 scanout |
| 2 RGB565 present path | ✅ | `9a9024e` | `test_frame_writer` → full-screen color bars, correct RGB order |
| 4 mame4all present shim | ✅ | `82f00ea` | gng title @ 60 fps, 256×224 centered, palette→RGB565 correct |
| 3 Programmable timing | ✅ | (branch) | sweep: 5 geometries raster-exact, borders intact; gng 256×224, mk 416×254@53.2 Hz, 1943 224×256 3:4, popeye 512×448; 99.5% of drivers native |
| 5 Input | ✅ | (branch) | P1 full map verified bit-by-bit on device; gng played with a pad (642 events, combos included) |
| 6 Audio | ✅ | (branch) | gng/1943/mk run with sound, no flags; ALSA RUNNING, /dev/MrAudio held by mame; user-confirmed audible |
| 7 Launch/packaging | ✅ | (branch) | Master_Daemon → handler → manager; `.mgl` shortcuts launch Contra/Galaga from the menu and resolve as picker selections; per-game opts verified; 31 host tests |
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
800 MHz Cortex-A9 on a 32-bit ABI, and **ARM32 has no DRC backend in MAME**
(`FORCE_DRC_C_BACKEND=1` is mandatory), so every recompiler-backed CPU runs its C
interpreter. **Gate: build `SOURCES=pacman`, bench it on the device with sound and
the core loaded. Below 60 fps → stop and write it up.** Everything past the gate
is deliberately unbuilt.

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

**Not verified — needs the operator's machine:** the gate itself. This session's
container has **no `ssh`**, so `.81` was unreachable and nothing was benched. The
full `SOURCES=pacman` link had not finished when the session ended either — it is
a multi-hour build on 4 cores, dominated by MAME's own emu core rather than by
the driver.

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

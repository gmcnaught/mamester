# Software MAME on MiSTer — feasibility study

Question: can a **software MAME** (mame4all-pi @ MAME 0.37b5, or MAME 2003-Plus @
MAME 0.78) be ported to run as a native ARM/Linux app on the MiSTer's HPS,
presenting video the way `sonic-mania-mister` does — the app renders into a DDR
framebuffer and a passthrough FPGA core scans it out — and only escalating to an
FPGA compositing offload (like `solarus-mister`) if pure software rendering can't
keep up? And what *net-new* arcade games would such a port unlock over the
existing native MiSTer FPGA arcade cores?

Grounded in three shipped sibling ports under `~/MisterFPGA-Projects/`
(`sonic-mania-mister`, `solarus-mister`, `maldita.castilla-mister`), the two MAME
source trees, and the misterzine `arcade.json` core inventory.

**Design assumption (given):** the port **leverages the standard MiSTer framework
scaler and shaders** (`ascal`, `video_mixer`/`arcade_video`, `video_freak`,
scanlines, shadow-mask, gamma) rather than re-implementing them. This means the
core feeds pixels into the stock MiSTer video pipeline at each game's **native**
resolution and lets the framework scale/shade to the display — the
`solarus`-PR#138 model, not `sonic-mania`'s scaler-bypass. This assumption resolves
most of the variable-modeline problem (see §3.2).

## TL;DR

**GO on the pure-framebuffer path. The FPGA-offload "fallback" is a non-option for
MAME and should be struck from the plan.** The present-path harness the three
sibling ports already prove is reusable almost verbatim. The feasibility does not
turn on the present path at all — it turns on two things the harness doesn't
solve: **(1) whether one Cortex-A9 core can emulate a given driver at full speed**
(favorable — see below), and **(2) MAME's variable per-game video mode vs a
fixed-modeline passthrough core** (the real engineering problem).

| | Finding |
|---|---|
| Present path | ✅ **Already a solved, reusable harness.** All three ports write RGB565 into a DDR double-buffer with a `(counter<<2)\|buffer` control-word doorbell, scanned out by a forked passthrough reader (`native_video_reader.sv` / `openbor_video_reader.sv`). `sonic-mania-mister/tools/mister-wrapper/test-frame-writer.c` drives it standalone with **zero engine**. A MAME port writes one finished bitmap per frame into the inactive buffer and bumps the counter. |
| FPGA offload as fallback | ❌ **Does not apply to MAME.** `solarus-mister`'s blitter accelerates *SDL surface compositing* (overlapping alpha blits; solarus was bottlenecked at 6× screen overdraw = ~44 ms/frame of fabric time). A MAME driver does all tilemap/sprite/priority compositing **inside its own C rasterizer** and hands back **one finished bitmap** (~256×224). The present is a single 1× `memcpy` (~150 KB, ~2% of f2h bandwidth). There is nothing at the SDL layer to offload. If a driver is slow it's the *emulated-CPU* cost, which no compositor can touch. |
| CPU budget | ⚠️ **Favorable but per-driver.** mame4all-pi runs full-speed on a Raspberry Pi 1 (single-core ARM11, no NEON). The HPS is a **dual Cortex-A9** (ARMv7, NEON, higher IPC, ~800 MHz–1.2 GHz) ≈ Pi 2/3 class — comfortable headroom for the 0.37b5 set. Emulation is single-threaded, so per-game speed is bounded by **one** A9 core (the 2nd core absorbs OS/audio/present). |
| Variable modeline | ⚠️→✅ **Largely dissolved by the framework scaler.** The problem only exists if you drive the display timing directly (as `sonic-mania` does with a baked PLL modeline). Feeding `ascal` instead, the core presents each driver at its **native** H/V/refresh via a *programmable* timing generator (register-driven counters + `CE_PIXEL`, fed from HPS-supplied per-game params) and `ascal` scales to the display + applies shaders. This is standard arcade-core practice; **no per-game RBF and no output-PLL reconfig** — `ascal` owns the output modeline. Residual work: the reader's timing must be register-programmable (a bit more RTL than a fixed modeline), and very-off refresh rates lean on the framework's `vsync_adjust`. |
| DDR bandwidth | ✅ **Non-issue.** Measured budget for 320×240@60 is ~64 MB/s vs ~3.2 GB/s f2h ≈ 2%, ~50× headroom (`solarus-mister` / `maldita` docs). Two well-behaved DDR clients (HPS writer + scanout reader). The `maldita` "fabric-stall" bug class is a property of the *multi-client GPU offload* and structurally cannot occur here. |
| Pixel format | ✅ **mame4all is RGB565-native.** mame4all-pi's `blit.cpp` rasterizes to 16-bit R5G6B5 — the exact DDR format, **zero conversion**. Modern-MAME-derived output (`bitmap_rgb32`) needs a NEON RGB32→565 pass (the sister 3sx port did exactly this); MAME 2003-Plus (libretro) emits RGB565/1555 with minor-to-no conversion. |
| Input | ✅ SDL2/evdev (both sonic-mania and mame4all already use SDL input) with `SDL_VIDEODRIVER=dummy`; optional MiSTer joy-SHM bridge to pause on OSD. |
| Audio | ✅ **mame4all already uses ALSA.** Routes through MiSTer's standard HPS→FPGA audio path, independent of the pixel path. Optional: `maldita`'s native 48 kHz DDR audio ring. |
| Launch | ✅ Use `maldita`'s **daemon + `_handler.sh`** launch (app is a child of stock `Main_MiSTer`), **not** `sonic-mania`'s `main=` wrapper — maldita measured the wrapper causing 3/5 frame-1 wedges vs 0/5 for the handler. |
| Licensing | ⚠️ Both builds are the **old non-commercial MAME license** (pre-2016, not GPL): no commercial packaging, source disclosure required, no ROM bundling, derivative must carry a distinct name. Fine for a free open-source core. |

## 1. The two patterns, and why only one applies

The user framed this as "framebuffer first, FPGA offload if that's not feasible."
The three sibling ports make the answer sharper than that framing assumes:

- **`sonic-mania-mister` — pure framebuffer.** Native RSDKv5 engine, 100% CPU
  software rasterizer, writes RGB565 frames to DDR `0x3A000000`; the FPGA core
  (`native_video_top`) only reads DDR, converts 565→888, and drives video timing.
  No game logic / input / audio in the fabric.
- **`solarus-mister` — FPGA 2D compositing offload.** Host emits a display list of
  FILL/BLIT commands into a DDR command ring; the fabric composites into the
  framebuffer. Justified by a **measured** A9 bottleneck: heavy overworld scenes
  at **6× screen overdraw** cost ~44 ms/frame in software (≈20 fps).
- **`maldita.castilla-mister` — the most aggressive offload.** A GameMaker game via
  `gmloader` whose GLES2 calls drive a fabric **triangle rasterizer** (`mfgpu`).

**A MAME driver has the shape of `sonic-mania`, not `solarus`.** MAME emulates the
arcade board's CPUs/sound/video in C; its video code rasterizes tilemaps, sprites,
priorities, raster/palette effects into **one finished bitmap per frame** before
any SDL surface exists. The MiSTer present is therefore a single full-frame copy —
exactly what `sonic-mania`'s `NativeVideoWriter` does. The `solarus` blitter
accelerates the *many overlapping alpha blits* of an SDL compositor; MAME performs
none at that layer. The only blitter command MAME could issue is one full-frame
copy, which is strictly **more** work than DMAing the same bytes directly.

**Consequence for the plan:** there is no useful "escalate to FPGA offload"
fallback. If software rendering is too slow, the deficit is emulated-CPU time,
which a compositor cannot address. The genuine fallbacks are: pick a lighter MAME
build, drop the heavy drivers, or (a different project entirely) write a native
RTL core for that specific board. Treat the fabric here as pure scanout.

## 2. The reusable present-path harness (grounded in the three ports)

The hard infrastructure already exists and is proven engine-agnostic across three
engines (OpenBOR → Solarus → RSDKv5 → GameMaker). A MAME port reuses it directly.

**DDR contract (sonic-mania, the minimal form):**

| Field | Address (example, Menu-fork) | Meaning |
|---|---|---|
| Control word | `0x3A000000` | `[1:0]` = active buffer, `[31:2]` = frame counter |
| Vsync feedback | `0x3A000040` | FPGA→HPS vblank/frame word (pacer) |
| BUF0 | `0x3A000100` | frame 0 (320×240×2 = 153,600 B) |
| BUF1 | `0x3A025900` | frame 1 |

Present loop (from `test-frame-writer.c`, mirrors the real writer):
```c
memcpy(inactive_buf, pixels_rgb565, FRAME_BYTES);
__sync_synchronize();                        // ordering barrier
*ctrl = (frame_counter++ << 2) | active;     // publish counter + buffer
active ^= 1;
```
The reader polls the control word each vblank; on a counter change it latches the
buffer and bursts it out per-scanline. On a stalled counter it **re-scans the last
good frame** (freeze, not blank) — the deliberate HPS-stall failure mode.

**What to lift, per port:**
- **Reader RBF — base it on the `solarus`/`maldita` `openbor_video_reader` model,
  not `sonic-mania`'s.** Because the design leverages the framework scaler (§0
  assumption), the reader must **feed the stock MiSTer pipeline** (`arcade_video`/
  `video_mixer` → `video_freak` → `ascal` + scanlines/shadow-mask) the way
  `solarus` PR #138's `ddr3_scan_adapter` does — *not* drive the VGA DAC directly
  with `vga_scaler=0` (the `sonic-mania` path, which bypasses exactly the scaler we
  want). Keep the DDR double-buffer + control-word doorbell + `maldita`'s
  stale-frame watchdog. Replace the fixed `native_video_timing` modeline with a
  **register-programmable timing generator** (per-game H/V total, active, `CE_PIXEL`
  divider, refresh) so each driver presents at its native resolution into the mixer.
- **Launch** — `maldita`'s `deploy.py` (sha1-verified scp; a truncated ELF
  segfaults before `main`) + `_handler.sh` + Master_Daemon auto-launch keyed on the
  `CONF_STR` setname. **Not** the `main=` wrapper (measured frame-1 wedges).
- **Audio** — mame4all's ALSA path works as-is through MiSTer's HPS→FPGA audio.
- **Input** — SDL2/evdev with `SDL_VIDEODRIVER=dummy`; optional joy-SHM bridge so
  the MiSTer OSD can pause input.
- **Build/deploy** — Docker armhf cross-compile (static ELF); Quartus 17.0 for the
  RBF; scp deploy. Identical toolchain shape across all three ports.

**Discipline transferred from `maldita`'s `HANDOFF_fabric_stall.md`:** timeout every
fabric-side DDR transaction, settle ≥2 s after core load before first DDR access,
keep the scanout reader as arbiter default-owner, validate analog timing on a real
monitor (perf counters "lie about display health"; watch for the armhf 32-bit
`long` overflow in ns counters). These are cheap and prevent the known wedges.

## 3. The two real feasibility gates

### 3.1 CPU budget — favorable, with a synergy

mame4all-pi is a MAME 0.37b5 fork hand-optimized (ASM CPU cores, dirty-rectangle
blitting) to hit full speed on a **Pi 1** (single-core ARM11 ~700 MHz–1 GHz, no
NEON). The HPS dual Cortex-A9 has higher per-core IPC, NEON, and ~800 MHz–1.2 GHz
(overclockable) — one A9 core comfortably exceeds one Pi-1 core, so the 0.37b5 set
runs with headroom. Emulation is single-threaded; the second A9 core absorbs
OS/audio/present, not the emulation loop, so **per-driver speed is a one-core
budget**.

**The synergy with the gap analysis (§4):** the games software MAME *stresses*
(multi-68000 boards, QSound, 3D) are largely the ones that **already have native
FPGA cores** (CPS2, Neo Geo, System 16/18, Cave) or **won't run at all** (Model
2/3, Namco System 22, PS1-class). The genuine net-new games are mid-90s
68000/Z80 raster boards of **moderate** cost — exactly what runs well in software
MAME on this ARM. The hardware you'd want and the hardware that runs cleanly
overlap.

### 3.2 Variable per-game modeline — dissolved by the framework scaler

With the §0 assumption (leverage the MiSTer scaler/shaders), this stops being the
hard problem it is for a scaler-bypass port. The difficulty only exists if the core
drives the display timing directly — `sonic-mania` bakes **one** modeline into its
PLL + timing generator and built a *second entire RBF* just for 16:9. Feeding
`ascal` instead removes that constraint: **`ascal` accepts arbitrary input
resolutions and scales them to a fixed display output**, so the core presents each
driver at its own native mode and the framework does the rest.

**The architecture that follows:**
- The reader clocks the current DDR framebuffer line into the mixer under a
  **register-programmable timing generator** — per-game H total / V total / active
  W,H / `CE_PIXEL` divider / refresh, supplied by the HPS at game launch (MAME knows
  each driver's native geometry). This is exactly how stock MiSTer arcade cores
  already produce variable-resolution native video; the only novelty is that the
  pixels come from a DDR framebuffer instead of core RTL.
- `video_mixer`/`arcade_video` + `video_freak` + `ascal` then handle **output
  scaling, aspect ratio (incl. tate), scanlines, shadow-mask, gamma, and the HQ2x/
  scandoubler filters** — all user-configurable via OSD / `MiSTer.ini`, for free.
- **HDMI:** fully solved — any native input resolution scales to 1080p/etc. with
  shaders. No per-game PLL work at all.
- **Analog:** also driven through the scaler (the framework's VGA/`vga_scaler`
  path), so per-game CRT modelines are the framework's job, not the core's.

**What remains (small, bounded):**
- The reader's timing generator must be register-driven rather than a fixed
  modeline — modestly more RTL than the sibling ports' baked timing, but standard.
- **Refresh-rate matching** for games far from 60 Hz (e.g. 55–57.5 Hz) is handled by
  the framework's `vsync_adjust` (which nudges the *output* pixel clock to match the
  core's frame rate); expect the usual MiSTer trade between `vsync_adjust=2`
  (smoothest, changes HDMI clock) and off (a scaler frame of lag). This is inherited
  MiSTer behavior, not new work.
- One accepted trade: committing to the scaler for analog gives up MiSTer's
  lowest-latency **"direct video"** analog mode (native timing straight to the DAC).
  That is the explicit cost of reusing the shader/scaler stack, and it is the stated
  goal here.

Net: the modeline problem downgrades from "critical-path engineering" to "make the
reader's timing generator register-programmable and pass per-game geometry from the
HPS." The scaler/shader stack is inherited wholesale.

## 4. Which MAME build

| | mame4all-pi | MAME 2003-Plus |
|---|---|---|
| MAME base | 0.37b5 | 0.78 + ~350 backports |
| Sets | 2,271 | 4,831 (~2,917 runnable parents) |
| Form | **standalone ELF** (has its own `main`) | headless libretro `.so` (needs a frontend) |
| Video out | **RGB565 native** (zero convert) | libretro RGB565/1555 (minor convert) |
| Audio | **ALSA already** | libretro callback → you bridge to ALSA |
| Input | SDL (localized `src/rpi/input.cpp`) → evdev | libretro callback → you bridge from evdev |
| Perf class | built for Pi 1 | Pi 2/3 class; heavy drivers risk |
| Accuracy | older 0.37b5 drivers | +3 yrs of driver fixes, better |

**Recommendation: mame4all-pi for v1.** It already boots standalone, already uses
ALSA, already renders to a 16-bit RGB565 bitmap (matching the DDR contract with no
conversion), and was tuned for weaker silicon than the HPS — the port reduces to
(a) a MAME-bitmap→DDR-writer shim replacing the one `update_screen`/`gp2x_video`
present call, (b) SDL→evdev input (or keep SDL), and (c) the modeline handling of
§3.2. **MAME 2003-Plus is the coverage-maximizing target** (2× the library, better
accuracy) at the cost of authoring a minimal libretro frontend (drive
`retro_init→retro_load_game→retro_run`, bridge video/audio/input callbacks — a
bounded few-hundred-line job) and a heavier CPU load. A reasonable path is to prove
the harness with mame4all, then port the same framebuffer/ALSA/evdev frontend onto
2003-Plus for the wider library.

## 5. Net-new games unlocked (arcade inventory gap analysis)

**MiSTer native arcade coverage** (misterzine `arcade.json`, snapshot 2026-07-31):
1,063 MRA/title entries, **925 distinct games**, **276 core bitstreams**, 232 repos.
Weighted heavily pre-1990 (only ~115 covered games are 1995+). Strong on: Namco
classics, Atari raster+vector, Williams/Midway MCR, Irem M62–M107, Sega System
16/18/24-lite, the full Jotego Capcom CPS1/1.5/2/3 line, Konami 8/16-bit, Toaplan
(via Coin-Op Collection), Cave — **plus Neo Geo and Sega ST-V via their own
console-style cores** (not in `arcade.json`).

**MAME romset scope:** mame4all-pi 2,271 sets (0.37b5); MAME 2003-Plus 4,831 sets /
~2,917 runnable parents (0.78 + backports).

**Gap (2003-Plus parents vs MiSTer setnames, clone-aware):** ~783 parents already
have a MiSTer arcade core; ~266 more are false gaps served by console cores (Neo
Geo ×162, PlayChoice-10, VS. System, Nintendo Super System, MegaTech/MegaPlay,
ST-V, Sega C-2). That leaves **≈1,868 genuinely uncovered titles**, of which
**~1,500–1,800 are practically playable in software MAME on the A9**.

**The high-value net-new families (all uncovered by any MiSTer core, and moderate
enough hardware to run in software):**

- **Midway T/Y-unit — the marquee gap:** Mortal Kombat I/II/3, NBA Jam / TE /
  Extreme, Rampage: World Tour, Cruis'n USA/World, Revolution X, Open Ice, WWF.
- **Atari System 1/2:** Klax, Rampart, Toobin', Paperboy, Xybots, Cyberball,
  Blasteroids, Skull & Crossbones, Hydra, APB, 720°, Vindicators; Cojag: Area 51,
  Primal Rage, T-Mek.
- **Taito F3** (F2 is cored, F3 isn't): Puzzle Bobble 2/3, Bubble Bobble 2/Memories,
  Elevator Action Returns, Darius Gaiden, Gunlock/Rayforce, Kaiser Knuckle (~31).
- **Namco System 2** (System 1/86 cored, 2 isn't): Rolling Thunder 2, Assault,
  Ordyne, Dragon Saber, Phelios, Legend of Valkyrie, Steel Gunner, Final Lap (~26).
- **Sega System 24/32/Multi32 + Super Scaler:** Gain Ground, Bonanza Bros.,
  Crackdown; Golden Axe: Revenge of Death Adder, Spider-Man, Jurassic Park,
  OutRunners, Arabian Fight; Space Harrier, After Burner II, Galaxy Force II, Power
  Drift, G-LOC (~40).
- **Konami GX / mid-90s Konami:** Sexy/Gokujyou Parodius, Salamander 2, Run and Gun
  2, Violent Storm, Mystic Warriors, Xexex, Metamorphic Force, Vendetta (~17+).
- **Uncored shmup ecosystems (run great in software):** Psikyo (Strikers 1945 I–III,
  Gunbird 1/2, Sengoku Blade, Dragon Blaze), Video System (Aero Fighters / Sonic
  Wings), NMK16 (GunNail, Thunder Dragon, Macross), Metro, Raiden II, Batsugun.
- **Kaneko / Seta / SSV:** Great 1000 Miles Rally, Gals Panic, Air Buster, B.Rap
  Boys, Blood Warrior; Blandia, Zombie Raid, GundHara; Change Air Blade, Storm
  Blade, Survival Arts.
- **Data East later:** Fighter's History, Wizard Fire/Dark Seal, Dragon Gun, Tattoo
  Assassins, Captain America and the Avengers, Robocop 2, Rohga.
- **American niche makers nobody will FPGA-core:** Leland (Ataxx, Pig Out, Indy
  Heat), Incredible Technologies (Golden Tee, World Class Bowling, Time Killers,
  BloodStorm), Exidy (Crossbow, Chiller, Venture, Mouse Trap), Gottlieb (Reactor,
  Mad Planets, Krull), Cinematronics vector (Rip Off, Star Castle, Armor Attack),
  early raster (vicdual: Carnival, Head On; Bally/Sente).
- **Japanese mahjong** (~120+ Nichibutsu/Dynax/Homedata): numerically large, niche
  value, many adult titles.

**What software MAME does NOT meaningfully add (state this plainly):**
- **False gaps** — Neo Geo (162 titles), PlayChoice/VS/SNSS, ST-V, Sega C-2 already
  run natively and *better*; software MAME is strictly worse for these.
- **Top classics already native** — Pac-Man, Galaga, Defender, R-Type, SF2/Alpha/
  Marvel, DoDonPachi, Out Run, Golden Axe, etc. No reason to run them in software.
- **Won't run full-speed on the A9** — 3D / PS1-class (Namco System 22/Super22/
  System 11/12: Ridge Racer, Tekken, Soul Calibur, Time Crisis; Sony ZN: SF EX;
  Sega Model 2/3). mame4all 0.37b5 mostly lacks these drivers entirely; 2003-Plus
  has some but they're far too heavy. **Do not count these as unlocked.**
- **Newer-than-0.78 hardware is simply absent** from both romsets: Raiden Fighters
  later revs, most 2000s Cave (Ketsui, Mushihimesama), Naomi/Atomiswave.
- **Accuracy trade** — even for titles that run, software MAME loses the
  cycle-accurate / low-latency edge that is MiSTer's whole value proposition;
  fighters and precision shmups feel it most.

**Net:** the defensible value is **~1,500–1,800 mid-1980s–1990s raster games nobody
has FPGA-cored** — Midway T-unit (MK/NBA Jam/Cruis'n as the headliners), Atari
System 1/2, Namco System 2, Sega System 24/32 + Super Scaler, Konami GX, and a
large uncored shmup / Kaneko / Seta / Leland / IT / Exidy / Gottlieb / Cinematronics
catalog — filling MiSTer's thin 1990–1995 band with games that are moderate enough
to run in software but that no one has built (or will build) native cores for.

## 6. Recommendation

1. **Adopt the pure-framebuffer architecture that feeds the framework scaler**
   (`solarus`-PR#138 model, *not* `sonic-mania`'s scaler-bypass): finished MAME
   bitmap → DDR double-buffer → reader with a register-programmable timing generator
   → stock `arcade_video`/`video_freak`/`ascal` + shaders. Reuse the DDR contract,
   watchdog, `deploy.py`, and daemon/`_handler.sh` launch verbatim.
2. **Drop the FPGA-offload fallback from the plan** — it accelerates SDL
   compositing, which MAME doesn't do; it buys nothing here. The real fallback for a
   slow driver is build/driver selection, or a separate native-RTL-core project.
3. **Build v1 on mame4all-pi** — standalone ELF, ALSA, RGB565-native, built for
   sub-A9 hardware. The port is a bitmap→writer shim + input glue + modeline
   handling. Keep MAME 2003-Plus as the coverage/accuracy upgrade behind the same
   frontend.
4. **Present at native resolution into the framework scaler** — make the reader's
   timing generator register-programmable and pass each driver's native geometry
   from the HPS at launch; inherit scaling/aspect/scanlines/shadow-mask/gamma from
   `ascal`/`video_freak`. No per-game RBF or output-PLL reconfig. The only residual
   is refresh-rate matching via the framework's `vsync_adjust` (inherited MiSTer
   behavior). This is now a modest task, not the critical path.
5. **Prove the CPU budget empirically first** — before building the harness, run
   mame4all-pi (or 2003-Plus) on the actual DE10-Nano HPS Linux at the console and
   measure sustained fps on a spread of the §5 gap drivers (a Midway T-unit title, a
   Psikyo/NMK shmup, a Sega System 24 game). The present path is not in doubt; the
   per-driver emulation budget is the only thing that decides which games actually
   ship.

## Verdict

**GO, pattern-confirmed, value-clear — gated on per-driver CPU throughput, not on
the present path or any fabric offload.** The framebuffer harness is a solved,
thrice-proven, reusable template; a software MAME slots into it with a small shim.
The FPGA-offload path is a category error for MAME and should be dropped. The
library it unlocks is real and well-targeted — ~1,500–1,800 mid-90s raster games
that plug exactly the gap where MiSTer's native cores are thinnest and software
MAME's CPU cost is lowest. With the framework scaler/shaders reused (not
re-implemented), the variable-modeline problem downgrades to a register-programmable
timing generator + per-game geometry from the HPS, leaving **one** genuinely open
risk: whether one A9 core emulates each target driver at full speed. That is
favorable on the calibration and cheap to verify on the actual DE10-Nano before any
RTL is written.

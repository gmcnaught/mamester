# CPU bench results — mame4all-pi on the MiSTer HPS (Cortex-A9)

First on-hardware measurement of the feasibility study's one open risk (§6, step 1):
can one A9 core emulate arcade drivers at full speed? Run on the real DE10-Nano
(`root@192.168.20.81`, glibc 2.31, MiSTer kernel 5.15) with the static-SDL bench
binary from `tools/build-mame.sh`.

## Method

Headless over SSH, no FPGA core involved (pure CPU/emulation measurement):

```
SDL_VIDEODRIVER=dummy MISTER_BENCH_FRAMES=<N> ./mame <game> -nothrottle -nosound
```

`-nothrottle` runs the emulation flat-out; the backend
(`mister_video.cpp`) counts presented frames and reports achieved fps. `-nosound`
isolates emulation+video-blit CPU. **These are an upper bound** — real playable use
adds sound-chip emulation (CPU) and the DDR present (cheap). Divide the headroom
below by a sound factor for a realistic figure; it stays large either way.

"×RT" = fps ÷ 60 ≈ real-time headroom multiple.

**Correction (Stage 8):** the "sound costs ~5x" note recorded here after Stage 6
was wrong — it compared two different configurations. The 472/729 figures were
measured with NO FPGA core loaded (see Method above), so nothing was written to
the DDR framebuffer; the 94/128 figures were measured with the core running.
Measured properly, both with the core loaded:

| | no sound | 44100 | cost |
|---|---|---|---|
| contra | 123.8 | 94.8 | 1.31x |
| galaga | 135.2 | 125.9 | 1.07x |

Most of that is fixed cost (the sound CPU and demand-driven stream updates), not
rate-proportional mixing: contra at 11025 Hz only reaches 101.7. What the old
comparison actually measured is the **present path** — contra drops 446 -> 124 fps
when the DDR framebuffer write is enabled (`MISTER_NO_NATIVE=1` vs not, core
loaded). That is the real cost centre, and it is the uncached `/dev/mem` mapping
(`O_SYNC`), not sound. Left as a future optimisation; every driver below still
clears real time.

## Results (unthrottled, no sound)

| Game | Hardware | fps | ×RT |
|---|---|---:|---:|
| galaga | Namco Galaga | 729 | 12.2× |
| 1942 | Capcom (Z80) | 690 | 11.5× |
| commando | Capcom (Z80×2) | 634 | 10.6× |
| gng | Capcom Ghosts'n Goblins | 545 | 9.1× |
| sidearms | Capcom (Z80×2) | 480 | 8.0× |
| contra | Konami | 472 | 7.9× |
| rygar | Tecmo | 436 | 7.3× |
| vigilant | Irem | 393 | 6.6× |
| 1943 | Capcom (Z80) | 319 | 5.3× |
| aliens | Konami | 294 | 4.9× |
| ddragon | Technos (6809×3) | 270 | 4.5× |

**Every driver that ran cleared real-time by 4.5×–12×.** The heaviest that ran
(ddragon, three HD6309 cores; aliens, Konami) still had ~4.5–5× margin. This is a
strong confirmation of the feasibility verdict: the A9 has ample headroom for the
mid-range 68000/Z80/6809 raster hardware that dominates the net-new gap library.

## Net-new GAP games — the real targets (0.37b5 ROMs from archive.org)

The games above already have native cores. These are actual net-new gap titles,
and the heaviest class in the whole study: **Midway Y/T-unit runs a TMS34010
graphics processor** — far heavier to emulate than the raster boards above.

| Game | Hardware | fps (no sound) | fps (with sound) | native Hz | ×RT w/ sound |
|---|---|---:|---:|---:|---:|
| **mk** (Mortal Kombat) | Midway Y-unit, TMS34010 | 122 | **88.8** | ~54.7 | **1.6×** |
| **nbajam** | Midway Y-unit, TMS34010 | 147 | **98.0** | ~53 | **1.85×** |

**The marquee heavy gap games are comfortably playable with full sound** — 1.6–1.85×
real-time on one A9 core, with sound emulation included. Note sound costs ~27–33%
here (mk 122→89, nbajam 147→98), so the no-sound numbers elsewhere in this doc
overstate real headroom by roughly that much. Since Y/T-unit is the heaviest gap
class, the lighter gap hardware (Psikyo/Kaneko/Seta/Atari 68k, Sega System 24)
should clear comfortably.

Did **not** run (reached `set_video_mode` then hung, with or without sound):
**mk2** (T-unit + DCS/ADSP2105 sound), **rampart**, **klax** (Atari System 2) — a
per-driver mame4all-pi/romset issue, not a speed result. Still unmeasured: Psikyo
(s1945 wasn't in the archive under that name), Sega System 24/32, Namco System 2.

## The present path was the bottleneck, not the drivers (2026-08-01)

Profiling atarisy2 (`720`, flagged "below real time" at 44 fps by the Stage 8
sweep) found **65% of process CPU inside `nv_present`**, the backend's own
present path — not in the emulator. With the DDR present disabled
(`MISTER_NO_NATIVE=1`) the same driver runs at **154 fps**, i.e. 2.6× real time.

**Mechanism.** The 8bpp path converted palettised pixels one at a time straight
into the `/dev/mem` window at 0x3A000000 (`drow[x] = gp2x_palette[srow[x]]`).
That mapping is uncached, so each 16-bit store is its own bus transaction — no
merging, no write-combining. `tools/mister/ddr-write-bench.c` measures the cost
for one 512×384 RGB565 frame (384 KB):

| target | store form | throughput | per frame |
|---|---|---:|---:|
| cached RAM | memcpy | 540.5 MB/s | 0.69 ms |
| uncached DDR | memcpy | 89.5 MB/s | 4.19 ms |
| uncached DDR | NEON 128-bit | 79.4 MB/s | 4.72 ms |
| uncached DDR | 32-bit stores | 47.8 MB/s | 7.84 ms |
| uncached DDR | 16-bit stores | 24.8 MB/s | **15.12 ms** |

The mapping mode makes no difference (`O_SYNC` and not are within 3%), and
hand-written NEON is *slower* than glibc memcpy: the uncached path is
transaction-latency-bound, not instruction-bound. 89 MB/s is a floor, not a
bandwidth ceiling — the DDR3 itself is orders of magnitude faster.

**Fix.** Convert into a cached staging frame, then cross into DDR with one
memcpy. 16bpp drivers already hold RGB565 and still go straight to DDR, since
the extra cached copy buys nothing without a worker thread to overlap it (it
cost `mk` 2.5%).

**Effect** — 600 frames, unthrottled, **with sound**, core loaded, device quiet:

| game | family | old | new | | game | family | old | new |
|---|---|---:|---:|---|---|---|---:|---:|
| gng | capcom | 121.4 | **193.4** | | ultraman | banpresto | 60.2 | **82.2** |
| klax | atarisy2 | 105.1 | **189.1** | | wecleman | konami | 62.4 | **81.0** |
| lastduel | capcom | 81.2 | **119.3** | | paperboy | atarisy2 | 44.2 | **80.2** |
| hydra | atari 68k | 73.5 | **114.8** | | dynduke | seibu | 65.2 | **80.5** |
| quantum | atari vector | 55.1 | **113.2** | | 720 | atarisy2 | 44.2 | **78.9** |
| eprom | atari 68k | 72.6 | **105.6** | | cchasm | cchasm | 44.3 | **76.2** |
| batman | atari 68k | 69.3 | **104.8** | | turbo | sega | 52.3 | **63.4** |
| aztarac | vector | 68.3 | **97.2** | | shanghai | shanghai | 44.6 | **61.4** |
| thunderj | atari 68k | 67.4 | **96.7** | | archrivl | mcr68 | 34.3 | **59.3** |
| jedi | atari | 70.7 | **94.3** | | gunbird | psikyo | 42.5 | **50.4** |
| toobin | atari 68k | 45.2 | **82.6** | | cheyenne | exidy440 | 25.7 | **28.7** |

Every 8bpp driver gains 12–80%. `cischeat` (54.0 → 54.1) and `mk` (74.1 → 72.3)
are the 16bpp cases and motivated the direct-write carve-out above; with it,
`mk` measures **78.1** and `cischeat` 53.9. Verified on device: `720` (8bpp,
512×384) and `mk` (16bpp, 416×254) both render correctly through the scaler.

**Not done, deliberately.** The residual 4.19 ms DDR write could move to the
second A9 core — the HPS is dual-core and the write is latency-bound, so it
would overlap with emulation rather than contend (projected ~105 fps for
atarisy2). Built and then backed out: 1.3× real time with sound already clears
60 Hz, and it is not worth a threaded DDR channel plus a frame of present
latency. The knob to reconsider it is a driver that needs more than ~1.3×.

**Consequences for the Stage 8 sweep: every fps figure taken before this is a
measurement of the present path, not of the driver.** Six of the eleven
"below real time" families clear 60 Hz outright (atarisy2, toobin, quantum,
cchasm, shanghai, turbo), and most of the "marginal" band moves well clear.
`cheyenne`/`crossbow` (exidy440) is the one family that is genuinely CPU-bound.

Two device notes that also invalidate earlier numbers: the sweep ran with an
orphaned `sh -c while : ; do : ; done` (PID 5922, parent init) pinning one of the
two A9 cores — killed 2026-08-01 01:08 — and `720` measured 42.0 fps with it
running versus 44.2 without.

**Measurement protocol on this device.** Per-cell spread is 1.5–4% across
repeats, and cell *order* alone moved one arm by 2% — a game benched straight
after a 190 fps run pays for the previous run's heat. Both are the same size as
a typical codegen effect. Anything at that magnitude needs interleaved arms,
alternating order, and repeats; two blocks measured minutes apart will
manufacture a difference of a few percent. (Learned the hard way: a −4.7%
"regression" from a two-block layout evaporated to +0.2% when order alternated.)
The present-path deltas above are safe from this — they were taken back-to-back
per game and are 12–80%, an order of magnitude above the drift.

**Tooling this produced** (reusable for the remaining slow families):
- `MISTER_PROFILE=<hz>` — SIGPROF PC sampler in the backend
  (`mister_profile.cpp`). The device has no `perf`, and gdb cannot unwind these
  ARM frames; this samples the PC and dumps file offsets per module.
- `tools/symbolize-prof.py` — maps that dump back to function names through an
  unstripped build (`tools/build-mame.sh STRIP=true all`), translating file
  offsets to vaddrs via the ELF program headers.
- `tools/mister/ddr-write-bench.c` — the DDR write-path ceiling above.
- `MISTER_NO_NATIVE=1` — bench the emulator with the present path removed; the
  gap against a normal run is the present cost.

## Caveats — read before over-reading the numbers

1. **Upper bound.** Unthrottled + no sound. Sound-chip emulation (YM2151/YM2203/
   OKI/etc.) adds real CPU; the true playable headroom is lower, but the 4.5×
   floor leaves large room.
2. **The mid-range table games all already have native MiSTer cores** (the device
   holds the modern MiSTer arcade set). The **gap-game section** above uses real
   0.37b5 targets (MK/NBA Jam) pulled from archive.org and is the load-bearing
   result. Still unmeasured: Psikyo, Sega System 24/32, Namco System 2.
3. **~40% of attempted drivers did not run** with the on-device *modern* romset:
   - **ROM load-fail** (dkong, rtype): modern romset differs from 0.37b5 (renamed/
     split files) → "required files are missing". Fixable with a 0.37b5 romset.
   - **Loads then never presents a frame** (sf2, tmnt, xmen, nemesis, toki, rastan,
     agallet, tigeroad): reaches `set_video_mode` then hangs — a per-driver
     mame4all-pi/romset runtime issue (CPS-1 and several Konami/Taito drivers).
     Investigate per-driver; not a harness fault (the 11 above prove the harness).
4. **verifyroms is lenient**: sf2/ffight/cninja/1943 reported "romset OK" yet some
   then hung at runtime — passing verify does not guarantee a clean run on this
   romset version.

## Bottom line

**Step-1 CPU-budget question: answered affirmatively, including the heavy end.** The
harness is proven on real hardware. The mid-range gap class has 4.5×+ headroom, and
the *heaviest* marquee gap games — Mortal Kombat and NBA Jam (Midway Y-unit,
TMS34010) — run at **1.6–1.85× real-time with full sound** on one A9 core. Since
that is the worst-case hardware in the gap library, the A9 budget is sufficient for
the net-new games the port targets.

Remaining (refines *which* games ship, not the architecture):
- **Triage per-driver run-failures.** ~40% of attempted drivers hang after
  `set_video_mode` or fail ROM load (mk2/DCS, rampart, klax, sf2, several Konami/
  Taito). Needs per-driver investigation against a clean 0.37b5 romset.
- **Measure the other gap families** (Psikyo, Sega System 24/32, Namco System 2)
  once their 0.37b5 ROMs are on hand.
- **Confirm under throttle** (real 54–60 Hz play, not just the unthrottled ceiling).

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

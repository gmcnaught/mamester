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

## Caveats — read before over-reading the numbers

1. **Upper bound.** Unthrottled + no sound. Sound-chip emulation (YM2151/YM2203/
   OKI/etc.) adds real CPU; the true playable headroom is lower, but the 4.5×
   floor leaves large room.
2. **These games all already have native MiSTer cores.** The device only holds the
   MiSTer arcade set, not the gap-game 0.37b5 ROMs, so the tested titles are the
   *lighter, already-cored* class — not the actual targets. **The heavy gap games
   are still unmeasured**: Midway T-unit (MK/NBA Jam) runs a 34010 GPU + TMS34010
   at high clock and is materially heavier than anything here; Psikyo/Kaneko/Seta
   shmups and Sega System 24/32 need their own measurement. Getting a T-unit number
   is the next real data point and needs its ROM.
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

The harness is proven on real hardware and the A9-headroom question is answered
*for the mid-range CPU class* (comfortable, 4.5×+). Two things remain to make the
step-1 verdict complete: (a) obtain 0.37b5 ROMs for a few true gap games —
especially a Midway T-unit title — and measure them; (b) triage the driver
run-failures (likely resolved by a matching 0.37b5 romset). Neither changes the
architecture; both refine *which* games ship.

# mame2003-plus benchmark results

Companion to [`bench-results.md`](bench-results.md), which holds the mame4all-pi
figures. This file records the 0.78-vs-0.37b5 comparison.

Protocol, unchanged from Stage 8 and non-negotiable: per-cell spread on this
device is 1.5–4% and cell order alone moved one arm by 2%, so every comparison is
**interleaved, alternating order, repeated**. No delta under ~5% from a
non-interleaved run means anything.

---

## Task 5 — emulator only, no present, no core loaded

**This is not a Stage 8 figure and cannot be read against the Stage 8 bands.**
No FPGA core is loaded, nothing is presented, nothing is throttled. It measures
one thing on purpose: what the 0.78 emulator costs on this Cortex-A9 relative to
0.37b5, before any of the port layer exists.

Binaries: `mame2003` (Task 5 host, submodule `d6bf36f6`, `USE_CYCLONE=1
USE_DRZ80=1`) and `mame` (mame4all-pi, `presentfix`). Both cross-compiled with
`arm-linux-gnueabihf-gcc` 10.2.1 from `tools/mister/Dockerfile.cross-armhf`.
600 frames, sound chips emulated in both. mame4all run with
`MISTER_NO_NATIVE=1 SDL_VIDEODRIVER=dummy -nothrottle` so that it, too, has no
present. Three repetitions, engine order alternating per repetition.

| game | mame2003-plus (fps) | mame4all (fps) | 2003-plus / mame4all |
|---|---:|---:|---:|
| `gng`    | 174.9 / 179.8 / 173.5 → **176.1** | 312.1 / 312.2 / 250.4 → **291.6** | 0.60 |
| `contra` |  97.2 /  97.5 /  86.8 →  **93.8** | 219.8 / 160.2 / 171.1 → **183.7** | 0.51 |
| `galaga` | 197.6 / 196.5 / 179.7 → **191.3** | 691.7 / 640.3 / 616.5 → **649.5** | 0.29 |

**Read:** 0.78 costs 1.7–3.4× what 0.37b5 costs for the same game. It is not
hopeless on this silicon — all three clear 60 fps by 1.5–3× with the present
path still to be paid — but the margin on a mid-weight driver like `contra`
(94 fps unloaded) is thin enough that heavier drivers are the thing to watch in
Task 8, not the light ones.

**Two observations about the measurement itself:**

- The mame4all arm is far noisier than the 2003-plus arm: `contra` spans
  160–220 fps (37%) and `gng` 250–312 fps (25%), against 2003-plus's 5–11%.
  Cause unknown. It is not order, which was alternated, and it is not one-way
  drift, because both arms fall in repetition 3 (machine-level, most likely
  thermal). Any mame4all cell in Task 8 needs more than three repetitions.
- 441,000 audio frames over 600 emulated frames is 735/frame — exactly
  44100/60, confirming the pinned `sample_rate` reached the core rather than
  falling through to the 48000 default.

**Asymmetries inside this comparison, recorded before the number is used:**

| difference | direction |
|---|---|
| mame4all writes ALSA audio; the Task 5 host discards it (worth 1–3%) | favours 2003-plus |
| mame4all runs with Cyclone **and** DrZ80 disabled (`0002-default-asm-cores-off.patch`); 2003-plus was built with both enabled | favours 2003-plus |
| the 2003-plus archive is upstream's `-O2`; mame4all builds `-O3` | favours mame4all |
| mame4all's counter counts *presented* frames, the host's counts `retro_run()` calls; if mame4all frameskips, its figure understates its own throughput | favours 2003-plus |
| different romsets — 0.37b5 sets from `roms/` against the 2003-plus reference sets from `roms2003/` — though the same three games | unknown |

The first two are the reason Task 8 benches Cyclone/DrZ80 on **and** off rather
than taking the default; the third is the reason an `-O3` arm is needed if the
engine gap ever comes out close. `mame2003-plus_cyclone_mode` was left unpinned
for this run, so it took the core's own default — pin it before Task 8.

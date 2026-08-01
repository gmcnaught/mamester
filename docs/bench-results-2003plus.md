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
than taking the default. The third is settled below. `mame2003-plus_cyclone_mode`
was left unpinned for this run, so it took the core's own default — pin it
before Task 8.

---

## Task 6 — the three format paths

The core picks its pixel format per driver and each takes a different route
through `nv_present.c`.

| format | driver | geometry | result |
|---|---|---|---|
| RGB565 (straight to DDR, no staging) | `gng`, `contra`, `pacman` | 256×224, 224×280, 224×288 | renders |
| XRGB8888 (6-bits-per-gun, converted) | `eprom`, `batman` | 336×240 | renders |
| 0RGB1555 (converted) | `crospang` | 320×240 | renders |

Rotation is done by the **core**: refusing `SET_ROTATION` sends
`mame2003_video_init_orientation()` down its `Mame will rotate internally`
branch, so `frame_convert()` transposes inside the pixel loop it already runs.
That matches mame4all and avoids a cache-hostile pass per frame on every
vertical game. `contra` and `galaga` come out portrait at 3:4, which is the
check that it works.

**The RGB565 row above was wrong when first written, and the way it was wrong is
the lesson.** gng was recorded as verified on the strength of a screenshot
showing `TOP SCORE` and a ticking clock; a second shot differed, which was read
as "it animates". It was in fact rendering *only its character layer* — no
background tilemap, no sprites — and the only thing changing between shots was
the clock. A frame that changes is not a frame that is correct. The cause is in
the Task 7 section below, and it was a defect in this host, not in the core.

**`klax` is stuck, and it is not the present path.** It publishes frames with an
advancing counter, but the DDR content is byte-identical at frame 300 and 1200
and is not all-zero. `eprom` and `batman` are the same XRGB8888 path at the same
336×240 and both render and change. Driver-level, for Task 8 triage — mame4all's
notes already list `klax` among the drivers that hung after `set_video_mode`.

---

## The missing-layers bug: a libretro contract violation in this host

Symptom: gng, contra and 1942 rendered their character layer only — text on
black — while `crospang` was perfect. mame4all rendered gng completely through
the *same* `nv_present`, on the same device, minutes apart.

Ruled out in order, each with evidence rather than reasoning:

| suspect | how it was eliminated |
|---|---|
| ROM version / integrity | all 19 CRC-32s in `roms2003/gng.zip` match the 0.78 driver's declared checksums, **including all six GFX2 tile and six GFX3 sprite ROMs** — the exact regions not drawing |
| `-O3` codegen | the `-O2` archive produces a byte-identical sparse screen |
| Cyclone / DrZ80 ASM cores | `cyclone_mode=disabled` and `=default` produce identical output |
| the present path | for RGB565 `nv_frame` is a per-row `memcpy` with no per-layer anything; it cannot drop a background for gng and keep one for pacman |

Then instrumented at the boundary — `MISTER_SRC_STATS=N` describes the frame the
**core** hands over, before DDR. gng at frame 800: **1.8% non-black, 4 distinct
colours**. The layers were never drawn.

**Root cause.** `update_variables()` (`core_options.c:977`) is:

```c
if (environ_cb(GET_VARIABLE, &var) && !string_is_empty(var.value))
    switch (index) {
      case OPT_BRIGHTNESS: options.brightness = atof(var.value);
                           palette_set_global_brightness(options.brightness); break;
      case OPT_GAMMA:      options.gamma = atof(var.value);
                           palette_set_global_gamma(options.gamma);      break;
```

There is **no else**. This host answered `false` for every option it had not
explicitly pinned, so those `options.*` fields were never assigned and their
`palette_set_global_*` calls never happened — the palette was built from
whatever the struct happened to hold. RetroArch never hits this because a
libretro frontend is expected to own every option value and always supply one.

That also explains the survivor: `crospang` is `VCT_PASS1555`, direct RGB from
the game bitmap with **no palette lookup at all**, so a broken palette cannot
touch it.

**Fix:** capture each option's default from the core's own `SET_VARIABLES`
payload (`"<description>; <default>|<alt>|<alt>"`, default first,
`core_options.c:1663`, buffer freed immediately so both strings are copied) and
serve it from `GET_VARIABLE` whenever the option is not pinned. Measured at the
same boundary:

| game | before | after |
|---|---|---|
| `gng` | 1.8% non-black, 4 colours | **12.2%, 12 colours** |
| `contra` | text only | **21.2%, 31 colours** |
| `pacman` | 2.6%, 3 colours | 4.5%, 7 colours |

gng now renders its full BEST RANKING table; contra its background.

**Every fps figure in this document that predates this fix was measured with
the unpinned core options in an undefined state, and none of them should be
treated as a baseline.** Post-fix spot checks, present on, `-nosound`, 600
frames: `gng` 146.7 against 146.2 before (noise), but `galaga` 159.5 against
149.0, about 7%. Those are single non-interleaved samples so no delta is being
claimed, but 7% is outside the 1.5–4% floor and cannot be waved away — the
options now take their real defaults, so the emulator's workload may genuinely
differ. Task 8 re-measures everything interleaved anyway; the point here is that
the Task 5 early-answer table is a *direction*, not a baseline.

**The general lesson for this host:** every remaining `return false` in
`host_env.c` deserves the same question — does the core have an `else` for it?
For `GET_VARIABLE` it did not, and the failure was silent and graphical rather
than loud.

---

## Task 7 — input, audio and throttle

**A blocking ALSA write silently turns the benchmark into a measurement of
ALSA.** `snd_pcm_writei` stalls once the buffer is full, which paces the
emulation loop to the audio clock. Caught by the numbers not making sense:

| `galaga`, 600 frames | fps | note |
|---|---:|---|
| unthrottled, **blocking** write | 60.7 | measuring ALSA, not the emulator |
| unthrottled, **non-blocking** write | **136.1** | 327 periods dropped, as intended |
| `-throttle` (a played run) | **60.6** | native 60.6061; 0 underruns, 0 dropped, 3 late |
| `-nosound` | 149.0 | ALSA output costs 8.7% |

So the mode is chosen by what is being measured: non-blocking when
benchmarking, where the figure must be the emulator's ceiling and a dropped
period is irrelevant; blocking when playing, where the audio clock is the
better master anyway because it absorbs the drift between `CLOCK_MONOTONIC` and
the sound card that a video-clock throttle eventually turns into an underrun.

For Task 8 this means **the audio-output cost is now inside the numbers** —
Task 5's figures had no ALSA write at all. `galaga` went 191.3 (no present, no
audio) → 149.0 (present, no audio output) → 136.1 (present + audio). Present is
22%, audio output a further 9%.

**Exactly one clock, too.** A first 3-minute run held 60.57 fps with 0
underruns and 0 dropped while the timer called **8117 of 10800 frames "late"** —
both cannot be faults. In a throttled run the blocking ALSA write already paces
the loop, so the timer found the deadline passed almost every frame and was
really reporting the sound card running 0.07% slower than the nominal
60.6061 Hz. The timer now only takes over when there is no audio to pace
against. After the fix, 1200 throttled frames: 0 late with audio, 5 late
(0.4%) with `-nosound`, 60.6 fps either way.

Verified on the device: the FPGA overwrites a `0xDEADBEEF` sentinel written into
the pad word at `0x3A000008` within a second, and all four words read 0 at rest;
ALSA reaches `state: RUNNING` with `/dev/MrAudio` held by `mame2003` and a
1706-frame prefill. **Still needing a human at the device: whether it is
actually audible, and whether every button does what it should.** No automated
check substitutes for either.

**Operator-confirmed 2026-08-01: audio is audible and the controls work.**

---

## `-O2` vs `-O3` — the NEON arm

Upstream builds 2003-plus at `-O2`; mame4all builds at `-O3` (`Makefile.mister:42`).
That was recorded as an asymmetry in the table above, and it is a bigger one than
an `-O` level usually is: **gcc 10.2.1 enables `-ftree-loop-vectorize` and
`-ftree-slp-vectorize` at `-O3` and not at `-O2`** (they only moved to `-O2` in
gcc 12, under the very-cheap cost model). At `-O2` the `-mfpu=neon` this build
passes therefore buys little beyond instruction selection — the NEON unit is
essentially idle.

Confirmed at the codegen level. Same source, same toolchain, only `-O` differs:

| | SIMD-typed instructions | `.text` |
|---|---:|---:|
| `src/palette.o` at `-O2` | 19 | 13,309 |
| `src/palette.o` at `-O3` | **307** | 30,813 |
| whole `mame2003` binary at `-O2` | 10,678 | 18,114,614 |
| whole `mame2003` binary at `-O3` | **39,883** | 19,995,790 |

3.7× the SIMD instructions, +10.4% text. And it changes **nothing** measurable:

| game | `-O3` (fps) | `-O2` (fps) | delta |
|---|---:|---:|---:|
| `gng`    | 179.9 / 181.8 / 178.5 → **180.1** | 180.6 / 181.5 / 181.8 → **181.3** | −0.7% |
| `contra` | 101.8 / 100.0 / 100.9 → **100.9** |  99.8 /  99.6 / 100.3 →  **99.9** | +1.0% |
| `galaga` | 200.9 / 206.6 / 203.6 → **203.7** | 202.0 / 198.3 / 193.6 → **198.0** | +2.9% |

Interleaved, order alternating per repetition, identical hosts and romsets, the
archive's `-O` level the only difference. Every delta is inside the device's
1.5–4% noise floor, so **no difference is being claimed in either direction** —
the protocol does not license reporting one.

**Read:** auto-vectorised NEON is not a lever for this workload. In hindsight
that follows from what MAME 0.78 spends its time on — CPU-emulation interpreters,
which are dependent chains of loads, table lookups and unpredictable branches.
The vectoriser needs countable, independent, contiguous loops, and an
instruction-set interpreter has almost none. The 39,883 SIMD instructions are
overwhelmingly in cold code: driver init, palette construction, blit setup, and
SLP-vectorised struct copies.

Two consequences:

- **The `-O` asymmetry is not the explanation for the engine gap.** The 0.29–0.60×
  in the table above was measured with the `-O2` archive; at `-O3` it is
  unchanged. mame4all's 1.7–3.4× lead over 0.78 is the emulator, not the flags.
- **`-O3` is kept as the default** (`tools/build-m2003p.sh`, `OPT=-O2` to revert).
  It costs nothing measurable, and it removes the asymmetry from the comparison
  rather than leaving it to be argued about.

Not tried, and on this evidence not worth a 21-minute rebuild: `-ffast-math`.
mame4all has it, and on ARMv7 it is what additionally unlocks vectorisation of
*floating-point* loops (NEON is not IEEE-754 there, so gcc refuses without
`-funsafe-math-optimizations`). But if integer vectorisation moved nothing,
floating-point vectorisation of a mostly-integer emulator will not either, and
`-ffast-math` changes results.

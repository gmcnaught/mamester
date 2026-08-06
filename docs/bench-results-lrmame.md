# lrmame (MAME 0.289) — perf-lever results

Companion to [`bench-results.md`](bench-results.md) (mame4all) and
[`bench-results-2003plus.md`](bench-results-2003plus.md) (0.78). This file holds
the 0.289 lever measurements.

Protocol, unchanged and non-negotiable: **interleaved, arm order alternating per
(game, rep), repeated.** Per-cell spread on this device is 1.5–4% and cell order
alone moved one arm by 2%, so nothing under ~5% from a non-interleaved run means
anything. `tools/lever-ab.sh` is the harness; it resumes from its own TSV, so a
run that dies partway does not restart from zero.

Device: MiSTer @ 192.168.20.81, Linux 5.15.1-MiSTer, armv7l, 2 cores,
governor `performance` @ 800 MHz. Core `MAMESTer_20260801.rbf` **loaded** — a
present measurement with no core loaded measures nothing.

Games: `pacman` (Z80, 288x224 @ 60.61 Hz, never touches `drcuml`) and `s1945ii`
(SH-2, `psikyosh.cpp`, 320x224 @ 60 Hz, DRC-backed). Romsets from the MAME 0.260
non-merged set in `roms289/`. 600 frames, 3 reps, sound on, DRC on.

---

## The environment levers

All four arms below run **the same binary against the same
`lrmame_libretro.so`**, so the emulator side is byte-identical and only the host
behaviour differs.

| lever | reps | `pacman` A → B | `s1945ii` A → B | verdict |
|---|---:|---|---|---|
| write-combining (A = `MISTER_NO_WC=1`) | 3 | 90.5 → 104.1 (**+15.0%**) | 32.6 → 34.3 (**+5.0%**) | real |
| `MISTER_SCHED_RT=5` | 6 | 105.4 → 113.3 (see below) | 34.3 → 38.3 (**+11.6%**) | real |
| `MISTER_THREADED_PRESENT=1` | 3 | 105.1 → 103.6 (−1.4%) | 34.4 → 34.4 (+0.1%) | null |
| `MISTER_EMU_CPU=0` | 3 | 104.5 → 105.4 (+0.8%) | 33.9 → 34.1 (+0.6%) | null |

Per-cell spread was under 1% in every cell of the write-combining run, which is
what lets a +5.0% be read as real rather than as the noise floor.

### `SCHED_RT`: report the median on `pacman`, not the mean

The `s1945ii` arm is unambiguous — all six B reps (39.4, 39.1, 37.9, 38.7, 36.9,
37.7) clear all six A reps (34.0–34.5), and mean and median agree at ~+11.5%.

`pacman` is **bimodal**: 107.4, **124.6**, **124.1**, 108.0, 107.3, 108.5. The
+7.5% mean is carried entirely by two runs; the median is 108.25 against a
baseline median of 105.35, i.e. **+2.8%**. A mean over a two-humped sample
describes a frame rate that never occurred, so the honest statement is "+2.8%
typical, occasionally much more" and the cause of the two fast runs is unknown.

The shape across the two games is the opposite of write-combining's, and it fits
the mechanism: RT priority buys back preemption by Main_MiSTer and the two shell
poll loops, and a 29 ms frame offers more chances to be preempted than an 9.5 ms
one. It also means the second core is the contended resource — which is the
direct argument against MAME's own work-queue threading on this box (see
`mame_thread_mode`, below).

### The salvaged host changes: +6.0% / +2.5%

`nv_present.o` at `-O3` plus `build_reverse()` as a constructor, measured as
`lrmame-base` (main's host) against `lrmame-lev` (main + the salvage), **both
linked to the same `lrmame_libretro.so` and both write-combined**: `pacman`
98.8 → 104.7 (+6.0%), `s1945ii` 33.5 → 34.3 (+2.5%), 4 reps, every rep favouring
the salvage.

**The first attempt at this measurement was wrong and is recorded because the
error is easy to repeat.** Comparing against the *deployed* `lrmame` binary gave
+20.8% / +7.1% — but that binary predates `nv_present.c`'s write-combining
support entirely (it prints no `/dev/mem_wc` line and no `present=` field), so
the A arm was Strongly-Ordered and the B arm was not. It re-measured the WC
lever and attributed it to a convert loop. **An A/B between two binaries is only
about the change you made if every other difference between them is zero**, and
"the old one is still on the device" is not that.

### Write-combining is the only large one, and it was switched off

`tools/mister/mem_wc/` has shipped a prebuilt `.ko` since the DDR-throughput
work, and `games/MAMESTer/_handler.sh:84` `insmod`s it at core launch. The
**deployed** handler on the device (dated 2026-08-06 01:49) contains no `mem_wc`
reference at all — it predates that commit. So the module sat at
`/media/fat/games/mame/mem_wc.ko` and was never loaded, `nv_present.c` took its
documented `/dev/mem` fallback, and every launch paid Strongly-Ordered stores.

**PR #7's 88.3 fps gate figure was therefore measured without write-combining.**
The gate passes by more than recorded. The fix is a `deploy.py` run, not a code
change.

`nv_present` says which mapping it got, and the exit line repeats it:

```
nv_present: pixel buffers write-combined via /dev/mem_wc
... present=write-combined
```

Check that string before trusting any present-path number.

### Why the other two are null, and why that is not evidence they are useless

Write-combining cuts the present `memcpy` by roughly the 9.6× the module's own
`ddr-write-bench` arm measures — from ~1.4 ms to ~0.15 ms on a 288x224 RGB565
frame. Both remaining levers exist to hide present cost:
`MISTER_THREADED_PRESENT` moves it to the second A9, `MISTER_EMU_CPU` stops the
emulator migrating away from a warm cache while it happens. With the cost
already gone there is nothing left for either to recover.

Both were measured **with WC on**, which is the shipping configuration, so
"null" is the correct answer for the build that ships. It is not a claim that
they were null before — they were never measured before, and on the
Strongly-Ordered path the arithmetic says threaded present had ~1.4 ms/frame to
work with.

The threaded arm was verified to have actually engaged rather than silently
declining:

```
MISTER-HOST: emulation pinned to cpu0, present worker to cpu1
MISTER-HOST: threaded present (worker owns nv_present)
... 0 present-dropped ...
```

`0 present-dropped` matters: a threaded arm that gets faster by not presenting
is not faster. The 146 underruns in that run are not a threading cost either —
the non-threaded baseline reports 142, and both are `s1945ii` running at half
speed with the audio clock derived from the emulator.

## MAME's own SMP: already enabled, and capped at one worker here

MAME's FAQ describes up to eight cores' worth of threads. Almost none of it can
fire in this build, and what remains was already on.

- **One switch, already on.** The libretro OSD gates the *entire* work-queue
  mechanism on the `mame_thread_mode` core option:
  `osd_get_num_processors()` returns 1 when `!thread_mode` (`osdsync.cpp:96`)
  and `osd_work_queue_alloc()` sets `threadnum = 0` (`osdsync.cpp:294`). The
  option defaults to `"enabled"` (`libretro_core_options.h:98`) and `host_env.c`
  pins nothing for lrmame, so every figure on this page already has it on.
- **Two cores means at most one worker.** `osd_work_queue_alloc` allocates
  `numprocs - 1` threads for `WORK_QUEUE_FLAG_MULTI` queues and 1 for the rest.
- **Four of the FAQ's five thread sources are absent**: bgfx texture upload (no
  GL in this build), output handlers and the HTTP server (not built), OpenMP
  (`genie.lua:966` gates on `OPENMP=1`, unset), and the netlist matrix solver
  (only `nld_log.cpp` references threads here; the parallel solver needs
  OpenMP).
- **The remaining consumers are driver-specific**: `devices/sound/discrete.cpp`,
  `devices/video/poly.h`, `devices/machine/laserdsc.cpp`, `lib/util/chd.cpp`,
  `mame/cave/cv1k_v.cpp`, `mame/sega/coolridr.cpp`. Neither `pacman` nor
  `psikyosh` touches any of them, which is the reason the second core measured
  idle — moving the present onto core 1 gained nothing because core 1 was free.

The FAQ's own caveat is the binding one here: it wants a spare core for the OS,
and this box has exactly two. `SCHED_RT`'s +11.6% is evidence that contention
for the second core already costs frames, so a MAME worker thread would be
competing with Main_MiSTer for the resource RT priority is winning back. Expect
`thread_mode` to be neutral-to-negative on this hardware and measure it on a
discrete-sound driver before believing either sign.

## Levers ruled out without a measurement

- **cpufreq.** `scaling_governor` is already `performance` and
  `scaling_cur_freq` is already 800000. There is nothing to raise.
- **`-O3`.** MAME's `makefile:613` sets `OPTIMIZE = 3` when nothing else does,
  and `tools/build-lrmame.sh` does not, so 0.289 has been building at `-O3`
  from the first gate build. The remaining compiler arms are LTO,
  `-ffast-math` and PGO, not `-O2`-vs-`-O3`.

## Where `s1945ii` stands

34.3 fps against the 60 it needs. No lever on this page closes a 74% gap, and
that is the point of measuring them: the remaining cost is emulation, not
present, so the next question is a profile rather than another environment
variable. `MISTER_PROFILE` needs an unstripped core — the shipped
`lrmame_libretro.so` has no `.symtab` at all — which is why the profiling build
carries `SYMBOLS=1`.

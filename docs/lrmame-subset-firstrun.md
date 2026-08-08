# First run of the lrmame subset build — 45 drivers on hardware

The 974-file build (`SUBTARGET=lrmame-gap`, 6,160 drivers, 134.5 MB) against a
45-romset sample on the device, 2026-08-07. Romsets from the **MAME 0.260
non-merged** set (archive.org `mame-0.260-roms-non-merged`), which is the newest
available; the build is 0.289, so romset acceptance is itself one of the results.

Sample construction: every DRC-backed family in the subset (those are what
`drcbearm32` correctness rides on), then the largest coverage families, taking
the smallest-romset parent of each. 45 sets, 258 MB.

All runs `MISTER_NO_NATIVE=1` (present removed), so **every fps here is an upper
bound** — it is emulator cost only, with no DDR write and no scaler. The core was
not loaded for these.

## Headline

| outcome | count |
|---|---:|
| ran | 30 of 45 |
| **segfaulted after full initialisation** | 11 |
| romset not accepted by 0.289 | 9 |

## The crashes are a CPU-family pattern, not a driver-quality pattern

Every one of these resolved its driver, reported its geometry and refresh, and
brought up ALSA — then died before producing a frame. `MISTER_PROFILE` on
`mrdig` returned exit 139. (An earlier read of `exit: 0` was wrong: it was the
exit status of a `tail` in the pipeline, not of the binary.)

| CPU front-end | ran | crashed |
|---|---:|---:|
| SH-2 | **6** (`coolridr`, `feversoc`, `gnbarich`, `hoops96`, `loderndf`, `vblokbrk`) | 0 |
| SH-3 (`cv1k`) | 0 | **1** (`mushitam`) |
| MIPS | 3 (`policetr`, `speglsht`, `gunwars`) | 1 (`bbust2`) |
| PowerPC | 0 | **1** (`fiveside`) |
| **Hyperstone (E1-32)** | 0 | **8 — every one tested** |

`crazywar`, `fmaniac2p`, `klondkp`, `linkypip`, `mosaicf2`, `mrdig`, `pasha2`,
`spotty` — eight Hyperstone drivers across seven different families, all
segfaulting, none running. A ninth (`x2222`) could not be tested because its
romset was rejected.

**Confirmed against `drcbec`: the crashes are the back-end.** A `DRC=0` build of
the same 974 files (`SUBTARGET=lrmame-gapc`, same tree, same sources, only the
UML back-end differs) was run over all thirteen crashers. **Not one segfaults
under the interpreter:**

| game | front-end | `drcbearm32` | `drcbec` |
|---|---|---|---|
| `mrdig` | E1-32 | SEGV | runs, 1.8 fps |
| `crazywar` | E1-32 | SEGV | runs, 1.6 |
| `linkypip` | E1-32 | SEGV | runs, 1.5 |
| `klondkp` | E1-32 | SEGV | runs, 1.0 |
| `spotty` | E1-32 | SEGV | runs, 8.0 |
| `x2222` | E1-32 | SEGV | runs, 2.5 |
| `mosaicf2`, `pasha2`, `fmaniac2p` | E1-32 | SEGV | no crash, under 0.5 fps |
| `mushitam` | SH-3 | SEGV | no crash, under 0.5 fps |
| `fiveside` | PowerPC | SEGV | runs, 5.2 |
| `dkmb` | PowerPC | SEGV | no crash, under 0.5 fps |
| `bbust2` | MIPS | SEGV | no crash, under 0.5 fps |

"No crash" means the 60-second budget expired (`timeout` SIGKILL, rc 137) rather
than a fault — the process was still emulating, just below half a frame per
second. Those runs shared the machine with the classification sweep, so the fps
figures are contended and low; the SEGV-vs-no-SEGV split is what the test is
for, and it is unanimous across four different UML front-ends.

**So `drcbearm32` miscompiles every front-end except SH-2**, and this is now a
defect with thirteen named reproducers rather than a correlation.

**What it does not establish is that fixing it makes these games playable.** The
interpreter figures are 1-8 fps. Even at the 7.4x the back-end is worth on SH-2,
and allowing for the sweep contention, most of this set lands well short of 60.
The reason to fix it is correctness and the SH-3 board (`cv1k`) more than the
Hyperstone library.

**SH-2 is the good news and it is substantial.** Before this, `drcbearm32` had
run exactly one driver family on hardware (`psikyosh`). It now runs six:
`psikyosh`, `psikyo4`, `deco_mlc`, `suprnova`, `coolridr`, `feversoc`. SH-3 is
NOT covered by that — `cv1k` is the only SH-3 board in the subset and it is the
one SH failure.

## Romset acceptance: 0.260 sets in a 0.289 build

9 of 45 rejected — and re-fetching all nine as **0.289 standalone sets from
mdk.cab** (`/download/standalone/<set>.7z`, extracted and rezipped) split them
cleanly into three causes, only one of which is a version problem:

- **Six need a CHD**, and no ROM zip satisfies those at any MAME version:
  `bm1stmix` (`753jaa11.chd`), `kinst`, `maxforce`, `polystar`, `ppd`,
  `turrett` — the error is literally `*.chd NOT FOUND`.
- **One is undumped**: `virtpool` wants `itvp-1.u53`, `NO GOOD DUMP KNOWN`. It
  cannot work in any version from any source.
- **Two were genuine 0.260→0.289 drift**: `dkmb` and `x2222`. Both then loaded
  from the 0.289 set — and both **segfaulted**, joining the crash bucket
  (PowerPC and Hyperstone respectively).

So version drift is **2 of 45 (4.4%)**, not the 20% the raw reject count
suggested, and it is fixable per-set from mdk.cab. An earlier reading of this
sample as "75-80% practical" attributed the whole reject rate to drift and was
wrong.

**CHD dependence measured across the whole subset**, since it is the only one of
the three that is both large and predictable: `lrmame-driver-index.py` now
parses `ROM_START`…`ROM_END` for `DISK_IMAGE`/`DISK_REGION`, and **80 of 2,017
parents (4.0%)** need one. It falls on whole families rather than scattering —
`firebeat` 19/19, `djmain` 17/17, `iteagle` 12/12, `cubo` 5/5, `konamim2` 3/3,
`kinst` 2/2 — so those families are unshippable from a zip library no matter
what else is fixed.

Net acquisition picture for the subset:

| | parents |
|---|---:|
| subset total | 2,017 |
| need a CHD — unobtainable as zips | 80 |
| **zip-satisfiable** | **1,937** |
| of those, present in the 0.260 set | 1,868 (96.4%) |
| remainder, needing 0.289 sets from mdk.cab | 69 |

## Speed, present removed

Fast enough to be interesting once the present path is paid (the present costs
roughly 3.5x on the 8bpp path — see `bench-results.md`):

`pacman` 89.6, `wits` 65.0, `feversoc` 54.3, `mangchi` 49.6, `gollygho` 46.9,
`s1945ii` 44.3, `gnbarich` 43.4, `pairsred` 40.6, `cerberus` 38.0,
`loderndf` 32.7, `telpacfl` 31.4, `htchctch` 30.9, `hoops96` 28.9,
`meosism` 28.5, `daitorid` 22.3, `8ball` 22.4.

Already below real time with no present at all, so out of reach:
`gunwars` 4.9 (Namco System 23), `speglsht` 6.5, `barrier` 8.8,
`sentetst` 10.7, `policetr` 11.0, `coolridr` 12.0 (Sega System H1),
`daiskiss` 14.3, `ar_airh` 14.7, `vblokbrk` 16.7 (Super Kaneko Nova),
`slipstrm` 18.1, `arkretrn` 18.3.

## What this build cost, against the two-driver one

Interleaved, arm order alternating per (game, rep), 3 reps, present removed on
both arms, so it isolates emulator cost.

| | 974-file (134.5 MB) | 2-file (57.9 MB) | delta |
|---|---:|---:|---:|
| `pacman` fps | 91.5 | 101.6 | **-10.0%** |
| `s1945ii` fps | 36.9 | 37.1 | -0.5% |
| `pacman` peak RSS | 105.2 MB | 76.5 MB | +28.7 MB |
| `s1945ii` peak RSS | **256.1 MB** | 223.3 MB | +32.8 MB |

**It fits, with less headroom than is comfortable.** The device has 492 MB of
RAM; `s1945ii` peaks at 256 MB, over half of it, alongside `Main_MiSTer`. Much
of that is file-backed pages of the mmap'd core and therefore reclaimable, but
it is what the kernel held.

**The -10% on `pacman` is not yet attributable to driver count.** The 2-file arm
is a 2026-08-06 build from `perf-levers-salvage`; the 974-file arm is today's
`main`. Every other difference between those two binaries is inside the
measurement, which is the trap `bench-results-lrmame.md` already documents. The
`rgb32` bypass is ruled out (`pacman` is `PALETTE16`, the predicate engages
0/300 frames), the rest is not. Isolating it needs a pacman-only build from
today's tree.

---

# Full-library sweep — 1,662 romsets

Every romset acquired (1,662 = the 1,645 zip-satisfiable parents under 8 MB from
the 0.260 set, plus the 0.289 sets fetched for the earlier rejects and the two
pre-existing ones). `lrmame-gap`, `MISTER_NO_NATIVE=1`, 60 frames, 25-second
budget. Per-setname results in [`lrmame-sweep.tsv`](lrmame-sweep.tsv).

| outcome | count | share |
|---|---:|---:|
| ran | **1,535** | 92.4% |
| romset not accepted | 71 | 4.3% |
| segfault | 35 | 2.1% |
| exceeded the 25 s budget | 21 | 1.3% |

**The crash surface is small and almost entirely one front-end.** Of 35
segfaults, **31 are Hyperstone**, 2 PowerPC, and just **2 are drivers with no DRC
at all** (`quiz18k`, `welltris`) — those two are the only genuine driver bugs in
1,662 romsets. So `drcbearm32`'s defect costs 33 of 1,662 sets (2.0%).

`TIMEOUT` and `SEGV` blur at the edges: a driver that faults after more than 25
seconds of emulation is killed before it can fault, which is why `mushitam` and
`bbust2` land in `TIMEOUT` here and in `SEGV` in the 45-driver sample, where the
budget was longer.

## Speed

Median **40.7 fps**, present removed. 205 sets at 60 fps or better, 20 at 100+,
481 below 30.

**134 of the 205 have no mame4all fallback** — that is the genuinely new library
at full speed, games this port cannot otherwise run at all: `cothello` 192,
`embargo` 172, `warpsped` 153, `gomoku` 137, `clayshoo` 113, `mmagic` 113,
`madball` 112 (`yunsung/paradise.cpp`), `coolpool` 99, `tgtpanic` 99,
`para2dx` 107, `torus` 96.

**115 of 626 families have at least one parent at 60 fps or better.**

**These are upper bounds and need re-measuring with the core loaded.** The
present path is cheap now that it is write-combined — 0.506 ms/frame, about 3%
of a 60 Hz budget — so the gap should be small, but "should be" is not a
measurement, and the ledger's own rule is that a present measurement with no
core loaded measures nothing. The 205 is the candidate list, not the shipped
list.

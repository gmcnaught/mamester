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

**What this is evidence for, and what it is not.** It is not yet proof that
`drcbearm32` is at fault: the proof is an A/B against `drcbec`, and `drc_use_c`
is not reachable from this host's command line, so it needs a `DRC=0` build of
this same subset. But 8 of 8 on one front-end, 0 of 6 on another, is not a
distribution that driver-specific bugs produce. It is the shape the Stage 11
ledger predicted: the back-end was validated against the SH front-end's UML
output and nothing else.

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

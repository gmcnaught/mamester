# Driver geometry survey — what Stage 3 has to present

Every raster driver in the build, run through the Stage-3 modeline generator
(`tools/mame-frontend/mister-backend/nv_modeline.h`). Reproduce with:

```
# geometry list straight out of the emulator
docker run --rm --platform linux/arm/v7 -v "$PWD:/src" -w /src \
    mamester-armhf-build ./mame -listinfo > listinfo.txt
# name,orientation,x,y,freq  (see tools/mister/modeline_report.c header)
cc -O2 -o modeline_report tools/mister/modeline_report.c && ./modeline_report geom.csv
```

`mame4all` rounds the surface width up to a multiple of 32
(`src/rpi/video.cpp`, `select_display_mode`) and hands vertical games their
**rotated (portrait)** dimensions (`src/mame.cpp` swaps on
`ORIENTATION_SWAP_XY`), so both are applied before building the modeline.

## Result (2,215 raster drivers)

| | count | share |
|---|---:|---:|
| present at native geometry | 2,204 | 99.5% |
| fall back to 320×240 center-clip | 11 | 0.5% |
| H rate 14.8–16.5 kHz (analog CRT ok) | 1,822 | 82.3% |
| H rate above 16.5 kHz (HDMI / multisync only) | 382 | 17.2% |
| bad porch / bad CE increment | 0 | — |

Max pixel clock 20.022 MHz (`solarfox` 480×512) — below the 26.8 MHz where the
fractional CE divider could fire on consecutive clocks. Max H rate 31.68 kHz
(same driver; it runs at 30 Hz, which the modeline builder presents as 60 Hz
timing with each frame shown twice).

With the original 256 KB buffers this was 97.2% native / 62 fallbacks; 1 MB
buffer slots recovered 51 of them. The remaining 11 are wider than the reader's
512-pixel line limit:

| surface | drivers | examples |
|---|---:|---|
| 672×240 | 6 | skullxbo, cyberbal |
| 640×240 | 3 | blstroid |
| 800×600 | 2 | dotron |

Going wider needs more than a bigger buffer: the line FIFO holds two 512-pixel
lines, and an 800×600 mode would want a ~39 MHz pixel clock, past the point
where the fractional CE divider would have to fire on consecutive clocks.

## Portrait games and the H rate

326 of the 331 high-H-rate drivers are vertical games. mame4all presents them
in **game** orientation — Pac-Man arrives as a 224×288 surface, so the raster is
288 lines and the line rate is 18.2 kHz at 60 Hz instead of 15.7 kHz. HDMI is
unaffected (ascal scales it); a 15 kHz analog CRT cannot lock to it.

The alternative — the hardware orientation, as a real cabinet emits it — is a
**launch flag**, not a build option: mame4all parses `-norotate` / `-ror` /
`-rol` from the command line (`src/rpi/config.cpp`). Measured on device:

| launch | surface | H rate | aspect |
|---|---|---|---|
| `mame 1943` | 224×256 portrait | 16.32 kHz | 3:4 |
| `mame 1943 -norotate` | 256×224 hardware orientation | 15.72 kHz | 4:3 |

So the default suits HDMI (upright portrait, pillarboxed, correct 3:4 aspect)
and `-norotate` suits a 15 kHz CRT on a rotated monitor. The handler can expose
it per game.

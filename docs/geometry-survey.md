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
| present at native geometry | 2,153 | 97.2% |
| fall back to 320×240 center-clip | 62 | 2.8% |
| H rate 14.8–16.5 kHz (analog CRT ok) | 1,822 | 82.3% |
| H rate above 16.5 kHz (HDMI / multisync only) | 331 | 15.0% |
| bad porch / bad CE increment | 0 | — |

Max pixel clock 10.550 MHz (`polyplay` 512×256) — far below the 26.8 MHz where
the fractional CE divider could fire on consecutive clocks. Max H rate
29.76 kHz (`punchout`, 256×480 dual screen).

## The 62 fallbacks

All are over the 256 KB buffer or wider than the reader's 512-pixel limit:

| surface | drivers | examples |
|---|---:|---|
| 512×480 | 24 | kroozr, wacko, twotiger |
| 480×512 | 9 | solarfox, kick, shollow |
| 672×240 | 6 | skullxbo, cyberbal |
| 512×384 | 5 | paperboy, ssprint, 720 |
| 384×512 | 5 | apb, toobin |
| 512×448 | 3 | popeye |
| 640×240 | 3 | blstroid |
| 800×600 | 2 | dotron |
| 480×480, 512×401, 480×496 | 5 | crater, narc, spyhunt |

51 of the 62 come back if the DDR buffers grow from 256 KB to 512 KB (move BUF1
from `+0x40040` to `+0x80040` and the timing block past it). Only the 11 wider
than 512 px (672/640/800) would still need a wider reader.

## Portrait games and the H rate

326 of the 331 high-H-rate drivers are vertical games. mame4all presents them
in **game** orientation — Pac-Man arrives as a 224×288 surface, so the raster is
288 lines and the line rate is 18.2 kHz at 60 Hz instead of 15.7 kHz. HDMI is
unaffected (ascal scales it); a 15 kHz analog CRT cannot lock to it.

The alternative is to present the **hardware** orientation (Pac-Man's real
288×224 @ 15.7 kHz signal) and rotate the display, as a real cabinet does.
That is a product decision, not a fabric limit — both are one flag away, and
the trade is: portrait-in-game-orientation is what an HDMI user expects, while
hardware orientation is what a CRT (or a physically rotated monitor) needs.

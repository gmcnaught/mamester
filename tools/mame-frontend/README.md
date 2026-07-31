# tools/mame-frontend/ — the port layer (planned)

**Status: stub — this is the core of the software port and does not exist yet.**

This is the seam where a software MAME becomes a MiSTer app. Three responsibilities,
all small given the right base build:

## 1. Video: MAME bitmap → DDR present shim

Replace mame4all-pi's single GPU-coupled present call
(`src/rpi/video.cpp` `update_screen` / the `gp2x_video` path) with a write into the
DDR double-buffer:

```
memcpy(inactive_buf, mame_bitmap_rgb565, frame_bytes);
__sync_synchronize();
*ctrl = (frame_counter++ << 2) | active;   // publish counter + buffer
active ^= 1;
```

- **mame4all-pi is RGB565-native** (`src/rpi/blit.cpp` R5G6B5) → zero conversion.
- MAME 2003-Plus / modern-MAME `bitmap_rgb32` → NEON RGB32→565 convert first.
- Also publish the driver's **native geometry** (H/V total, active, refresh) to the
  FPGA timing registers at game load, so the reader presents at native resolution
  (the framework scaler does the rest — see `../../fpga/README.md`).

Bring-up: prove the DDR contract first with `../test-frame-writer.c` (no emulator),
then wire this shim into MAME.

## 2. Input: SDL2/evdev

mame4all-pi already uses SDL for input (`src/rpi/input.cpp`, localized to the rpi
layer). Run with `SDL_VIDEODRIVER=dummy` (input+audio, no window), as the sibling
ports do. Optional: the MiSTer joy-SHM bridge to pause input while the OSD is open.

## 3. Audio: ALSA

mame4all-pi already outputs ALSA (`src/rpi/sound.cpp`) → routes through MiSTer's
standard HPS→FPGA audio path with no change. (Optional alternative: `maldita`'s
native 48 kHz DDR audio ring, if ALSA latency proves a problem.)

## Build selection

- **v1: mame4all-pi** — standalone ELF, ALSA, RGB565-native, built for sub-A9
  silicon. The port is this shim + input glue + native-geometry publish.
- **Upgrade: MAME 2003-Plus** — 2× library, better accuracy, but a headless libretro
  `.so`: you author a minimal frontend driving `retro_init→retro_load_game→retro_run`
  and bridge the video/audio/input callbacks to the same three targets above.

Both are the non-commercial MAME license — ship source, never bundle ROMs.

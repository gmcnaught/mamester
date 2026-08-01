/* nv_present.h -- the MiSTer present path, independent of any emulator.
 *
 * Both engines link this: mame4all-pi through mister_video.cpp's gp2x_* OSD
 * glue, and mame2003-plus through tools/mame-frontend/libretro-host/. Everything
 * that knows about the DDR contract at 0x3A000000 lives here and nowhere else.
 *
 * The contract (docs/superpowers/progress.md, Stage 3):
 *   +0x000    control = (frame_counter << 2) | active_buf, published AFTER pixels
 *   +0x040    BUF0        +0x100040  BUF1        +0x300000  timing registers
 *   the counter must advance every frame or the stale-frame watchdog blanks
 *
 * Keeping one implementation is the point. The contract cost real hardware
 * debugging -- a double-emitted first pixel, a four-pixel line skew, an
 * uncached-store throughput cliff -- and two copies of it would drift, with the
 * second copy's bugs looking like the second engine's bugs.
 *
 * Env knobs read by nv_open():
 *   MISTER_NO_NATIVE=1   disable the present entirely; the gap against a normal
 *                        run is the present cost (used for profiling)
 *   MISTER_FRAME_HASH=N  print an FNV-1a 64 of the published frame at frame N
 *   MISTER_JOY_DEBUG=1   log each joystick word as it changes
 */
#ifndef NV_PRESENT_H
#define NV_PRESENT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NV_FMT_RGB565   = 0,  /* already the DDR format; written straight out    */
    NV_FMT_PAL8     = 1,  /* 8bpp indices; needs nv_set_palette()            */
    NV_FMT_XRGB8888 = 2,  /* libretro 32-bit, converted down to 565          */
    NV_FMT_0RGB1555 = 3   /* libretro 15-bit, converted up to 565            */
} nv_format;

/* Map /dev/mem and initialise. Idempotent, and fails gracefully off-device (no
 * /dev/mem, or not root) so bench modes still work. Returns 1 if the present is
 * live, 0 if it is disabled or unavailable. */
int  nv_open(void);
void nv_close(void);

/* True when the present path is live. */
int  nv_is_enabled(void);

/* Choose the presented geometry, publish its modeline, and set the pixel format
 * the following nv_frame() calls will supply. Geometry too large for the reader
 * or for a buffer slot falls back to 320x240 centre-clip. Safe to call again
 * when a driver changes mode.
 *
 * `refresh_hz` is the driver's native rate (mk is 53.2, not 60). `rot` is
 * 0/90/180/270 clockwise and is accepted for the libretro SET_ROTATION path;
 * mame4all rotates its own bitmap and passes 0. */
void nv_set_mode(int width, int height, double refresh_hz, int rot, nv_format fmt);

/* Palette for NV_FMT_PAL8, as RGB565 entries. The pointer is retained, not
 * copied, and re-read each frame -- mame4all's palette is volatile and the OSD
 * layer rewrites it between frames. */
void nv_set_palette(const volatile uint16_t *pal565, int entries);

/* Convert if needed, stage if needed, publish exactly one frame, ring the
 * doorbell.
 *
 * `pitch_bytes` is the SOURCE stride; libretro cores do not guarantee it equals
 * width * bytes-per-pixel, which is why it is a parameter rather than derived.
 * `src_w`/`src_h` are the source frame's own dimensions -- larger than the
 * presented geometry means centre-clip. */
void nv_frame(const void *src, int pitch_bytes, int src_w, int src_h);

/* Joystick word for player 0..3, as written back by the FPGA reader:
 * [0]right [1]left [2]down [3]up [4..9]fire1..6 [10]start [11]coin [12]pause */
uint32_t nv_pads(int player);

/* Frames published since nv_open(). */
unsigned long nv_frame_count(void);

/* --- exposed for host-native unit testing; not part of normal use --------- */
void nv_convert_1555_to_565(uint16_t *dst, const uint16_t *src, size_t px);
void nv_convert_8888_to_565(uint16_t *dst, const uint32_t *src, size_t px);
void nv_convert_pal8_to_565(uint16_t *dst, const uint8_t *src, size_t px,
                            const uint16_t *pal565);
uint64_t nv_hash_pixels(const uint16_t *buf, size_t px);

#ifdef __cplusplus
}
#endif
#endif /* NV_PRESENT_H */

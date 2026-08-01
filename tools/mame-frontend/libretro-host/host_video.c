/* host_video.c -- libretro frames into the MiSTer DDR present path.
 *
 * Everything that knows about DDR lives in nv_present.c and is shared with
 * mame4all. This file is only the libretro-shaped adapter in front of it:
 * translate the pixel format, notice a geometry change, hand over the frame.
 *
 * NOTHING IS ROTATED IN SOFTWARE. host_env.c accepts SET_ROTATION, so the core
 * hands over an unrotated bitmap and a vertical game presents sideways --
 * galaga is 288x224, not 224x288. That is deliberate and temporary: rotation
 * belongs in ascal, with a [mamester_vertical] MiSTer.ini section in the shape
 * of arcade_vertical. Until then vertical games are meant to be looked at
 * sideways.
 *
 * The rotation the core reports is recorded and passed to nv_set_mode() so the
 * modeline layer can act on it once that exists; nv_present ignores it today.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "nv_present.h"

static nv_format     cur_fmt       = NV_FMT_0RGB1555; /* libretro's documented
                                                       * default until the core
                                                       * calls SET_PIXEL_FORMAT */
static double        cur_refresh   = 60.0;
static unsigned      cur_rotation  = 0;
static unsigned      cur_w         = 0;
static unsigned      cur_h         = 0;
static int           mode_valid    = 0;
static unsigned long frames_shown  = 0;
static unsigned long frames_duped  = 0;

/* The core picks its format per driver in mame2003_video_init_conversion()
 * (src/mame2003/video.c): depth 16 without VIDEO_NEEDS_6BITS_PER_GUN becomes
 * RGB565 -- the common case, and already the DDR format, so it goes straight
 * out with no staging copy. Six-bits-per-gun and depth 32 become XRGB8888;
 * depth 15 becomes 0RGB1555. Both of those pay a convert mame4all never did,
 * so they are the first thing to check when a driver benches slow. */
static nv_format host_fmt_from_retro(unsigned retro_fmt)
{
    switch (retro_fmt) {
    case RETRO_PIXEL_FORMAT_RGB565:   return NV_FMT_RGB565;
    case RETRO_PIXEL_FORMAT_0RGB1555: return NV_FMT_0RGB1555;
    case RETRO_PIXEL_FORMAT_XRGB8888: return NV_FMT_XRGB8888;
    default:                          return NV_FMT_RGB565;
    }
}

static const char *fmt_name(nv_format f)
{
    switch (f) {
    case NV_FMT_RGB565:   return "RGB565";
    case NV_FMT_0RGB1555: return "0RGB1555";
    case NV_FMT_XRGB8888: return "XRGB8888";
    case NV_FMT_PAL8:     return "PAL8";
    default:              return "unknown";
    }
}

void host_set_pixel_format(unsigned fmt)
{
    nv_format f = host_fmt_from_retro(fmt);
    if (f != cur_fmt) {
        cur_fmt = f;
        mode_valid = 0;      /* nv_set_mode carries the format; re-publish */
    }
    fprintf(stderr, "MISTER-HOST: pixel format %s\n", fmt_name(cur_fmt));
}

void host_set_rotation(unsigned rot)
{
    /* libretro's 0..3 is 0/90/180/270 counter-clockwise. Recorded, not acted
     * on: see the file header. */
    if (rot != cur_rotation) {
        cur_rotation = rot;
        mode_valid = 0;          /* republish; rot is an nv_set_mode argument */
    }
    fprintf(stderr, "MISTER-HOST: rotation %u (%u deg CCW), presented "
                    "unrotated\n", rot, rot * 90);
}

void host_geometry_changed(const struct retro_game_geometry *geom)
{
    /* Advisory only. The presented geometry is taken from the width/height that
     * arrive with each frame, because those are the dimensions of the bitmap
     * actually being handed over. This callback's numbers disagree with them
     * for rotated drivers: mame2003_video_get_geometry() applies
     * video_hw_transpose to vis_w/vis_h that mame2003_video_update_visible_area()
     * has ALREADY swapped for video_swap_xy, so a vertical game reports its
     * dimensions transposed twice. Trusting it would republish the modeline
     * with width and height exchanged. */
    fprintf(stderr, "MISTER-HOST: SET_GEOMETRY %ux%u (advisory)\n",
            geom->base_width, geom->base_height);
}

void host_video_set_refresh(double hz)
{
    if (hz > 0.0 && hz != cur_refresh) {
        cur_refresh = hz;
        mode_valid = 0;
    }
}

/* MISTER_SRC_STATS=N: at frame N, describe the frame the CORE handed over,
 * before it reaches DDR. This is the boundary that separates "the emulator did
 * not draw it" from "the present path lost it" -- and for RGB565 the present
 * path is a per-row memcpy, so if a layer is missing here it was never drawn. */
static unsigned long src_stats_at;
static int           src_stats_read;

static void host_src_stats(const void *data, unsigned w, unsigned h,
                           size_t pitch, nv_format fmt)
{
    const uint8_t *p = (const uint8_t *)data;
    unsigned x, y, nonzero = 0, distinct = 0;
    unsigned char seen[8192];
    memset(seen, 0, sizeof seen);

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            uint32_t v;
            if (fmt == NV_FMT_XRGB8888)
                v = ((const uint32_t *)(p + (size_t)y * pitch))[x];
            else
                v = ((const uint16_t *)(p + (size_t)y * pitch))[x];
            if (v) nonzero++;
            v &= 0xFFFF;
            if (!(seen[v >> 3] & (1u << (v & 7)))) {
                seen[v >> 3] |= (uint8_t)(1u << (v & 7));
                distinct++;
            }
        }
    }
    fprintf(stderr, "MISTER-SRCSTATS frame=%lu %ux%u pitch=%u fmt=%d "
                    "nonzero=%u/%u (%.1f%%) distinct=%u\n",
            src_stats_at, w, h, (unsigned)pitch, (int)fmt,
            nonzero, w * h, 100.0 * nonzero / (double)(w * h), distinct);
    fflush(stderr);
}

void host_video_refresh(const void *data, unsigned width, unsigned height,
                        size_t pitch)
{
    /* A NULL frame means "show the previous one again". The counter must still
     * advance or the reader's stale-frame watchdog blanks the screen. */
    if (!data) { frames_duped++; host_present_repeat(); return; }

    if (!mode_valid || width != cur_w || height != cur_h) {
        cur_w = width;
        cur_h = height;
        mode_valid = 1;
        host_present_mode((int)width, (int)height, cur_refresh,
                          (int)(cur_rotation * 90), cur_fmt);
    }

    /* pitch is a BYTE stride and libretro cores do not guarantee it equals
     * width * bpp -- mame2003-plus's bypass path passes the game bitmap's own
     * rowpixels (video.c:459), which is the driver's padded stride, not the
     * visible width. Passing it through is why nv_frame takes pitch_bytes. */
    if (!src_stats_read) {
        const char *e = getenv("MISTER_SRC_STATS");
        src_stats_at = e ? strtoul(e, NULL, 10) : 0;
        src_stats_read = 1;
    }
    if (src_stats_at && frames_shown + 1 == src_stats_at)
        host_src_stats(data, width, height, pitch, cur_fmt);

    /* host_present_frame(), not nv_frame(): with MISTER_THREADED_PRESENT the
     * worker does the DDR write and `data` cannot be handed over as-is, because
     * frame_convert() rewrites video_buffer every frame and the depth-15/32
     * bypass (video.c:459) passes the game bitmap the driver is still drawing
     * into. host_present.c copies it. */
    host_present_frame(data, (int)pitch, (int)width, (int)height);
    frames_shown++;
}

unsigned long host_video_shown(void) { return frames_shown; }
unsigned long host_video_duped(void) { return frames_duped; }

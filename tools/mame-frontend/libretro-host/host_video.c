/* host_video.c -- libretro frames into the MiSTer DDR present path.
 *
 * Everything that knows about DDR lives in nv_present.c and is shared with
 * mame4all. This file is only the libretro-shaped adapter in front of it:
 * translate the pixel format, notice a geometry change, hand over the frame.
 *
 * Rotation is NOT handled here, on purpose. See host_env.c's SET_ROTATION case:
 * refusing that call makes the core rotate its own bitmap
 * (mame2003_video_init_orientation -> video_swap_xy -> frame_convert), which is
 * exactly what mame4all does. The alternative -- accepting SET_ROTATION and
 * transposing here -- would add a full cache-hostile pass over every frame of
 * every vertical game to do work the emulator is already set up to do inside a
 * loop it is already running.
 */

#include <stdio.h>

#include "host.h"
#include "nv_present.h"

static nv_format     cur_fmt       = NV_FMT_0RGB1555; /* libretro's documented
                                                       * default until the core
                                                       * calls SET_PIXEL_FORMAT */
static double        cur_refresh   = 60.0;
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
    /* Unreachable while host_env.c refuses SET_ROTATION, which is the point --
     * if this ever fires, the refusal was lost and vertical games are about to
     * come out sideways. */
    fprintf(stderr, "MISTER-HOST: WARNING SET_ROTATION(%u) accepted; the core "
                    "has stopped rotating its own bitmap\n", rot);
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

void host_video_refresh(const void *data, unsigned width, unsigned height,
                        size_t pitch)
{
    /* A NULL frame means "show the previous one again". The counter must still
     * advance or the reader's stale-frame watchdog blanks the screen. */
    if (!data) { frames_duped++; nv_frame_repeat(); return; }

    if (!mode_valid || width != cur_w || height != cur_h) {
        cur_w = width;
        cur_h = height;
        mode_valid = 1;
        nv_set_mode((int)width, (int)height, cur_refresh, 0, cur_fmt);
    }

    /* pitch is a BYTE stride and libretro cores do not guarantee it equals
     * width * bpp -- mame2003-plus's bypass path passes the game bitmap's own
     * rowpixels (video.c:459), which is the driver's padded stride, not the
     * visible width. Passing it through is why nv_frame takes pitch_bytes. */
    nv_frame(data, (int)pitch, (int)width, (int)height);
    frames_shown++;
}

unsigned long host_video_shown(void) { return frames_shown; }
unsigned long host_video_duped(void) { return frames_duped; }

/*
 * nv_modeline.h — per-game video timing for the MAMESTer native-video path.
 *
 * Shared by the mame present backend (mister_video.cpp) and the standalone
 * fabric test writer (tools/mister/test_frame_writer.c) so both publish the
 * exact same register block. Header-only, C and C++ clean.
 *
 * The FPGA pixel clock is a fractional divider off a fixed 53.693182 MHz
 * CLK_VIDEO (fpga/rtl/mame_ce_pixel.sv), so any pixel rate is reachable and
 * the H rate can be held near 15.7 kHz across widths — which is what keeps the
 * analog output CRT-legal when the game's width changes. ascal scales HDMI.
 *
 * Register block at 0x3A0E0000 (see fpga/rtl/openbor_video_reader.sv):
 *
 *   +0x00  [31:0] magic 'TIM1'   (written LAST; 0 = ignore the block)
 *   +0x08  [15:0] h_active  [31:16] h_fp  [47:32] h_sync  [63:48] h_total
 *   +0x10  [15:0] v_active  [31:16] v_fp  [47:32] v_sync  [63:48] v_total
 *   +0x18  [23:0] ce_inc
 *
 * Back porches are implied (total - active - fp - sync). h_active is also the
 * DDR line stride, so lines must be padded to a multiple of 4 pixels.
 */
#ifndef NV_MODELINE_H
#define NV_MODELINE_H

#include <stdint.h>
#include <string.h>

#define NV_TIMING_OFFSET 0x000E0000u
#define NV_TIMING_MAGIC  0x54494D31u   /* "TIM1" */

#define NV_CLK_VIDEO_HZ  53693182.0
#define NV_TARGET_H_RATE 15700.0       /* 15 kHz CRT line rate */

struct nv_modeline {
    int h_active, h_fp, h_sync, h_total;
    int v_active, v_fp, v_sync, v_total;
    uint32_t ce_inc;
    double   pixclk, h_rate, refresh;
};

/* Build a modeline for a padded width (= line stride) and height at fps. */
static void nv_make_modeline(int pitch, int height, double fps,
                             struct nv_modeline *m)
{
    int h_total, v_total, hb, vb;

    if (!(fps > 40.0 && fps < 90.0)) fps = 60.0;   /* NaN-safe */

    /* ~24% horizontal blanking (320 -> 420 total, the Genesis H40 ratio),
     * kept a multiple of 4 so the stride stays word-aligned. */
    h_total = ((pitch * 21 / 16) + 3) & ~3;
    if (h_total < pitch + 40) h_total = pitch + 40;

    /* Lines per frame from the target H rate. A frame taller than that allows
     * (a pre-rotated vertical game) wins: the H rate then rises above 15.7 kHz,
     * so analog needs a multisync monitor, but HDMI is unaffected. */
    v_total = (int)(NV_TARGET_H_RATE / fps + 0.5);
    if (v_total < height + 16) v_total = height + 16;

    hb = h_total - pitch;
    m->h_fp   = hb * 17 / 100; if (m->h_fp   < 4) m->h_fp   = 4;
    m->h_sync = hb * 38 / 100; if (m->h_sync < 8) m->h_sync = 8;
    if (m->h_fp + m->h_sync > hb - 4) {            /* keep a back porch */
        m->h_fp   = (hb - 4) / 3;
        m->h_sync = (hb - 4) - 2 * ((hb - 4) / 3);
    }

    vb = v_total - height;
    m->v_sync = 3;
    m->v_fp   = vb / 8; if (m->v_fp < 2) m->v_fp = 2;
    if (m->v_fp + m->v_sync > vb - 1) m->v_fp = (vb > 5) ? (vb - 4) : 1;

    m->h_active = pitch;
    m->h_total  = h_total;
    m->v_active = height;
    m->v_total  = v_total;

    m->pixclk  = (double)h_total * (double)v_total * fps;
    m->h_rate  = (double)v_total * fps;
    m->refresh = fps;
    m->ce_inc  = (uint32_t)((m->pixclk / NV_CLK_VIDEO_HZ) * 16777216.0 + 0.5);
    if (m->ce_inc == 0) m->ce_inc = 1;
}

/*
 * Publish a modeline. `nv_base` is the mapping of 0x3A000000. The magic word is
 * cleared first and written last so the reader never validates a half-written
 * block; the reader also requires two identical reads before applying one.
 */
static void nv_publish_timing(volatile uint8_t *nv_base,
                              const struct nv_modeline *m)
{
    volatile uint32_t *t = (volatile uint32_t *)(nv_base + NV_TIMING_OFFSET);
    t[0] = 0;
    __sync_synchronize();
    t[1] = 0;
    t[2] = (uint32_t)m->h_active | ((uint32_t)m->h_fp    << 16);
    t[3] = (uint32_t)m->h_sync   | ((uint32_t)m->h_total << 16);
    t[4] = (uint32_t)m->v_active | ((uint32_t)m->v_fp    << 16);
    t[5] = (uint32_t)m->v_sync   | ((uint32_t)m->v_total << 16);
    t[6] = m->ce_inc;
    t[7] = 0;
    __sync_synchronize();
    t[0] = NV_TIMING_MAGIC;
    __sync_synchronize();
}

#endif /* NV_MODELINE_H */

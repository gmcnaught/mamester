/*
 * modeline_report.c — run nv_modeline.h over every driver geometry and report
 * what the fabric would be asked to do. Host tool (build and run on the dev
 * machine, not the device).
 *
 * The geometry list comes from the emulator itself:
 *
 *   docker run --rm --platform linux/arm/v7 -v "$PWD:/src" -w /src \
 *       mamester-armhf-build ./mame -listinfo |
 *     awk '/^\tname /{n=$2} /video \( screen raster/{
 *            match($0,/x ([0-9]+) y ([0-9]+)/,a); match($0,/freq ([0-9.]+)/,f);
 *            print n","a[1]","a[2]","f[1]}'
 *
 *   cc -O2 -o modeline_report tools/mister/modeline_report.c
 *   ./modeline_report geom.csv           # name,orientation,x,y,freq per line
 *
 * mame4all rounds the surface width up to a multiple of 32 (src/rpi/video.cpp,
 * select_display_mode), and hands vertical games their rotated (portrait)
 * dimensions, so this applies both before building the modeline.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../mame-frontend/mister-backend/nv_modeline.h"

#define MAX_W 512
#define MAX_H 512
#define BUF_BYTES 0x40000

int main(int argc, char **argv)
{
    FILE *f = fopen(argc > 1 ? argv[1] : "geom.csv", "r");
    char line[256];
    int total = 0, too_wide = 0, too_tall = 0, too_big = 0, native = 0;
    int crt_ok = 0, crt_high = 0, crt_low = 0, ce_bad = 0, bp_bad = 0;
    double max_pix = 0.0, max_h_rate = 0.0;
    char worst_pix[64] = "", worst_h[64] = "";

    if (!f) { perror("open"); return 1; }

    while (fgets(line, sizeof line, f)) {
        char name[64], orient[16];
        int x, y, pitch;
        double fps;
        struct nv_modeline m;

        if (sscanf(line, "%63[^,],%15[^,],%d,%d,%lf", name, orient, &x, &y, &fps) != 5)
            continue;
        total++;

        pitch = ((x + 31) / 32) * 32;          /* mame4all's surface width */

        if (pitch > MAX_W) { too_wide++; continue; }
        if (y     > MAX_H) { too_tall++; continue; }
        if ((size_t)pitch * y * 2 > BUF_BYTES) { too_big++; continue; }
        native++;

        nv_make_modeline(pitch, y, fps, &m);

        /* invariants the fabric relies on */
        if (m.h_total - m.h_active - m.h_fp - m.h_sync < 1 ||
            m.v_total - m.v_active - m.v_fp - m.v_sync < 1) {
            printf("BAD PORCH %-10s %dx%d -> h %d/%d/%d/%d  v %d/%d/%d/%d\n",
                   name, pitch, y, m.h_active, m.h_fp, m.h_sync, m.h_total,
                   m.v_active, m.v_fp, m.v_sync, m.v_total);
            bp_bad++;
        }
        /* ce_inc >= 2^23 would let the divider fire on consecutive clocks */
        if (m.ce_inc >= (1u << 23)) {
            printf("BAD CE    %-10s %dx%d fps %.2f -> %.3f MHz (inc %u)\n",
                   name, pitch, y, fps, m.pixclk / 1e6, m.ce_inc);
            ce_bad++;
        }

        if      (m.h_rate > 16500.0) crt_high++;
        else if (m.h_rate < 14800.0) crt_low++;
        else                         crt_ok++;

        if (m.pixclk > max_pix) { max_pix = m.pixclk; snprintf(worst_pix, sizeof worst_pix, "%s %dx%d", name, pitch, y); }
        if (m.h_rate > max_h_rate) { max_h_rate = m.h_rate; snprintf(worst_h, sizeof worst_h, "%s %dx%d@%.2f", name, pitch, y, fps); }
    }
    fclose(f);

    printf("\n%d drivers\n", total);
    printf("  native present      %5d (%.1f%%)\n", native, 100.0 * native / total);
    printf("  fallback: too wide  %5d   too tall %d   over 256 KB %d\n",
           too_wide, too_tall, too_big);
    printf("  H rate 14.8-16.5kHz %5d (analog CRT ok)\n", crt_ok);
    printf("  H rate high / low   %5d / %d (HDMI or multisync only)\n", crt_high, crt_low);
    printf("  bad porch / bad ce  %5d / %d\n", bp_bad, ce_bad);
    printf("  max pixel clock     %.3f MHz  (%s)\n", max_pix / 1e6, worst_pix);
    printf("  max H rate          %.2f kHz  (%s)\n", max_h_rate / 1e3, worst_h);
    return (bp_bad || ce_bad) ? 1 : 0;
}

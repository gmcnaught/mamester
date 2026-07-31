/*
 * test_frame_writer.c — standalone RGB565 framebuffer + timing test-writer.
 *
 * Proves the MAME core's DDR present path end-to-end with NO emulator: writes an
 * RGB565 pattern into the 0x3A000000 double-buffer and rings the control-word
 * doorbell that openbor_video_reader.sv scans out. DDR contract (from
 * MiSTer_OpenBOR src/native_video_writer.c):
 *
 *   0x3A000000 + 0x000   control word = (frame_counter<<2) | active_buf[1:0]
 *   0x3A000000 + 0x040   buffer 0   (RGB565, 1 MB slot)
 *   0x3A100040           buffer 1
 *   0x3A300000           per-game timing registers (Stage 3, see nv_modeline.h)
 *
 * RGB565: R=[15:11] G=[10:5] B=[4:0]. mame4all outputs this natively (no BGR
 * swap, unlike OpenBOR's SDL surfaces), so color bars here also verify the
 * reader's channel order (a red bar must read red).
 *
 * Usage:
 *   test_frame_writer bars              static SMPTE-ish color bars (default)
 *   test_frame_writer solid RRGGBB      one solid 24-bit color (converted to 565)
 *   test_frame_writer animate           alternate bars/inverted every ~0.5s
 *   test_frame_writer grid              1-px border + 16-px grid — geometry check
 *   test_frame_writer sweep [seconds]   cycle the arcade geometries Stage 3
 *                                       targets (256x224, 320x240, 384x224,
 *                                       400x254, tate), framed bars in each
 *
 * Any mode takes an optional geometry argument, which publishes a modeline and
 * draws at that size (default 320x240@59.92, the core's fallback mode):
 *
 *   test_frame_writer grid 384x224
 *   test_frame_writer animate 256x224@60
 *   test_frame_writer solid FF0000 400x254@54.7
 *
 * A border that is cut off, or a non-square grid, means the published timing
 * and the reader's line fetch disagree.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#include "../mame-frontend/mister-backend/nv_modeline.h"

#define NV_BASE     0x3A000000u
#define NV_REGION   0x00400000u   /* 4 MB */
#define NV_CTRL     0x00000000u
#define NV_BUF0     0x00000040u
#define NV_BUF1     0x00100040u
#define NV_BUF_BYTES 0x00100000u
#define NV_MAX_W    512
#define NV_MAX_H    512

/* Geometry in use — set by parse_geometry(). */
static int W = 320, H = 240, PITCH = 320;
static double FPS = 59.92;

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static void fill_bars(uint16_t *buf, int inverted)
{
    /* 8 vertical bars across the active width. */
    static const uint16_t bar[8] = {
        0xF800, /* red     */ 0x07E0, /* green   */ 0x001F, /* blue    */
        0xFFE0, /* yellow  */ 0x07FF, /* cyan    */ 0xF81F, /* magenta */
        0xFFFF, /* white   */ 0x8410  /* gray    */
    };
    int y, x;
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            int b = x / (W / 8);
            if (b > 7) b = 7;
            buf[y * PITCH + x] = inverted ? (uint16_t)~bar[b] : bar[b];
        }
}

static void fill_solid(uint16_t *buf, uint16_t c)
{
    int y, x;
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) buf[y * PITCH + x] = c;
}

/* Color bars inside a 1-px white border with red corner pixels: checks channel
 * order and edge clipping in one frame. */
static void fill_bars_framed(uint16_t *buf, int inverted)
{
    int y, x;
    fill_bars(buf, inverted);
    for (x = 0; x < W; x++) {
        buf[x]                         = 0xFFFF;
        buf[(size_t)(H - 1) * PITCH+x] = 0xFFFF;
    }
    for (y = 0; y < H; y++) {
        buf[(size_t)y * PITCH]         = 0xFFFF;
        buf[(size_t)y * PITCH + W - 1] = 0xFFFF;
    }
    buf[0]                               = 0xF800;
    buf[W - 1]                           = 0xF800;
    buf[(size_t)(H - 1) * PITCH]         = 0xF800;
    buf[(size_t)(H - 1) * PITCH + W - 1] = 0xF800;
}

/* 1-pixel white border, 16-px green grid, red corner pixels. Any missing edge
 * means the active area and the published h_active/v_active disagree. */
static void fill_grid(uint16_t *buf)
{
    int y, x;
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            uint16_t c = 0x0000;
            if (x == 0 || y == 0 || x == W - 1 || y == H - 1) c = 0xFFFF;
            else if ((x % 16) == 0 || (y % 16) == 0)          c = 0x07E0;
            buf[y * PITCH + x] = c;
        }
    buf[0]                             = 0xF800;
    buf[W - 1]                         = 0xF800;
    buf[(size_t)(H - 1) * PITCH]       = 0xF800;
    buf[(size_t)(H - 1) * PITCH + W-1] = 0xF800;
}

/* "384x224" or "256x224@60" */
static int parse_geometry(const char *s)
{
    int w = 0, h = 0;
    double fps = 0.0;
    const char *at;

    if (sscanf(s, "%dx%d", &w, &h) != 2) return 0;
    at = strchr(s, '@');
    if (at) fps = strtod(at + 1, 0);

    if (w < 64 || h < 64 || w > NV_MAX_W || h > NV_MAX_H) {
        fprintf(stderr, "geometry %dx%d out of range (64..%d x 64..%d)\n",
                w, h, NV_MAX_W, NV_MAX_H);
        return -1;
    }
    W = w; H = h; PITCH = (w + 3) & ~3;
    if (fps > 0.0) FPS = fps;
    if ((size_t)PITCH * H * 2 > NV_BUF_BYTES) {
        fprintf(stderr, "%dx%d needs %zu bytes, buffer is %u\n",
                W, H, (size_t)PITCH * H * 2, NV_BUF_BYTES);
        return -1;
    }
    return 1;
}

int main(int argc, char **argv)
{
    const char *mode = (argc > 1) ? argv[1] : "bars";
    const char *geom = NULL;
    struct nv_modeline m;
    int fd, i;
    volatile uint8_t *base;
    volatile uint32_t *ctrl;
    uint16_t *buf0, *buf1;

    /* The geometry argument is the last one that parses as WxH[@fps]. */
    for (i = 2; i < argc; i++) {
        int pw = 0, ph = 0;
        if (sscanf(argv[i], "%dx%d", &pw, &ph) == 2 && pw > 0 && ph > 0)
            geom = argv[i];
    }
    if (geom) {
        int r = parse_geometry(geom);
        if (r < 0) return 1;
    }

    fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { perror("open /dev/mem"); return 1; }
    base = mmap(NULL, NV_REGION, PROT_READ | PROT_WRITE, MAP_SHARED, fd, NV_BASE);
    if (base == MAP_FAILED) { perror("mmap"); return 1; }

    ctrl = (volatile uint32_t *)(base + NV_CTRL);
    buf0 = (uint16_t *)(base + NV_BUF0);
    buf1 = (uint16_t *)(base + NV_BUF1);

    nv_make_modeline(PITCH, H, FPS, &m);
    nv_publish_timing(base, &m);
    printf("timing: %dx%d active (pitch %d), %dx%d total, %.3f MHz pix, "
           "%.2f kHz H, %.2f Hz V, ce_inc=%u\n",
           W, H, PITCH, m.h_total, m.v_total, m.pixclk / 1e6,
           m.h_rate / 1e3, m.refresh, m.ce_inc);

    memset(buf0, 0, (size_t)PITCH * H * 2);
    memset(buf1, 0, (size_t)PITCH * H * 2);

    if (!strcmp(mode, "solid")) {
        unsigned v = (argc > 2) ? (unsigned)strtoul(argv[2], 0, 16) : 0xFFFFFF;
        uint16_t c = rgb565((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
        fill_solid(buf0, c);
        *ctrl = (1u << 2) | 0u;          /* frame 1, display buffer 0 */
        printf("solid #%06X -> RGB565 0x%04X, buffer 0 active\n", v & 0xFFFFFF, c);
        return 0;
    }

    if (!strcmp(mode, "grid")) {
        fill_grid(buf0);
        *ctrl = (1u << 2) | 0u;
        printf("grid written, buffer 0 active — the 1-px white border must be "
               "fully visible\n");
        return 0;
    }

    if (!strcmp(mode, "sweep")) {
        /* Cycle the arcade geometries Stage 3 has to handle, holding each for
         * a few seconds, so one run checks them all on a real display. */
        static const struct { int w, h; double fps; const char *what; } modes[] = {
            { 256, 224, 59.63, "gng / cps-ish"   },
            { 320, 240, 59.92, "core default"    },
            { 384, 224, 59.64, "sf2 / cps"       },
            { 400, 254, 54.71, "mk / t-unit"     },
            { 224, 288, 60.00, "tate (portrait)" },
        };
        int nmodes = (int)(sizeof(modes) / sizeof(modes[0]));
        int hold = 5, mi, tick;
        uint32_t frame = 0;
        int active = 0;

        if (argc > 2) { int v = atoi(argv[2]); if (v > 0) hold = v; }
        printf("sweep: %d modes, %d s each (Ctrl-C to stop)\n", nmodes, hold);

        for (mi = 0; ; mi = (mi + 1) % nmodes) {
            W = modes[mi].w; H = modes[mi].h;
            PITCH = (W + 3) & ~3; FPS = modes[mi].fps;
            nv_make_modeline(PITCH, H, FPS, &m);

            /* Draw at the new pitch first, then publish the geometry, so the
             * reader never fetches old-pitch data with new timing. */
            memset(buf0, 0, (size_t)PITCH * H * 2);
            memset(buf1, 0, (size_t)PITCH * H * 2);
            fill_bars_framed(buf0, 0);
            fill_bars_framed(buf1, 0);
            nv_publish_timing(base, &m);
            printf("  %3dx%-3d @%.2f Hz  %-16s %dx%d total, %.2f kHz H, "
                   "%.3f MHz pix\n",
                   W, H, FPS, modes[mi].what, m.h_total, m.v_total,
                   m.h_rate / 1e3, m.pixclk / 1e6);
            fflush(stdout);

            /* Keep the doorbell moving — the reader blanks on a stale frame
             * counter — and blink the bars so a frozen image is obvious. */
            for (tick = 0; tick < hold * 4; tick++) {
                uint16_t *dst = active ? buf1 : buf0;
                fill_bars_framed(dst, tick & 1);
                __sync_synchronize();
                frame++;
                *ctrl = (frame << 2) | (uint32_t)active;
                active ^= 1;
                usleep(250000);
            }
        }
    }

    if (!strcmp(mode, "animate")) {
        uint32_t frame = 0;
        int active = 0;
        printf("animate: alternating bars/inverted every 0.5s (Ctrl-C to stop)\n");
        for (;;) {
            uint16_t *dst = active ? buf1 : buf0;
            fill_bars(dst, (int)(frame & 1));
            __sync_synchronize();
            frame++;
            *ctrl = (frame << 2) | (uint32_t)active;
            active ^= 1;
            usleep(500000);
        }
    }

    /* default: static bars in buffer 0 */
    fill_bars(buf0, 0);
    *ctrl = (1u << 2) | 0u;
    printf("color bars written, buffer 0 active (ctrl=0x%08X)\n", (1u << 2) | 0u);
    return 0;
}

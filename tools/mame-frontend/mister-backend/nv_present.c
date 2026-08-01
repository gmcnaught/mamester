/* nv_present.c -- the MiSTer DDR present path, shared by both engines.
 *
 * Extracted verbatim from mister_video.cpp (Stage 9 Task 3). Behaviour is
 * unchanged; only the emulator-specific globals became parameters. Two
 * behaviours here were measured rather than chosen and must not be "tidied":
 *
 *   1. Converted output goes into a CACHED staging frame and crosses into DDR
 *      with one memcpy. Converting pixel-by-pixel straight into the uncached
 *      mapping spent 65% of the whole process's CPU on atarisy2 (512x384):
 *      16-bit stores into that window run at 24.8 MB/s (15.1 ms/frame) against
 *      89 MB/s for a memcpy. See tools/mister/ddr-write-bench.c.
 *   2. RGB565 sources write DDR DIRECTLY, with no staging copy. Staging a frame
 *      that is already in the destination format is pure loss without a worker
 *      thread to overlap it -- it cost mk 74.1 -> 72.3 fps.
 */

#include "nv_present.h"
#include "nv_modeline.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef NV_HOST_TEST
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

/* ---- DDR map -------------------------------------------------------------
 * Contract from MiSTer_OpenBOR native_video_writer.c, proven by
 * tools/mister/test_frame_writer.c. The upper 512 MB belongs to the FPGA, so
 * the buffer slots are 1 MB apart (Stage 3 follow-up); the reader's largest
 * frame, 512x512, is half of one slot. */
#define NV_BASE        0x3A000000u
#define NV_REGION      0x00400000u   /* 4 MB */
#define NV_BUF0        0x00000040u
#define NV_BUF1        0x00100040u
#define NV_BUF_BYTES   0x00100000u
#define NV_MAX_W       512
#define NV_MAX_H       512
#define NV_DEF_W       320
#define NV_DEF_H       240

/* Joystick writeback words, one per player. */
static const size_t NV_JOY_OFF[4] = { 0x08, 0x18, 0x20, 0x28 };

static int                 nv_fd      = -1;
static volatile uint8_t   *nv_base    = 0;
static volatile uint32_t  *nv_ctrl    = 0;
static uint16_t           *nv_buf[2]  = { 0, 0 };
static unsigned long       nv_frames  = 0;
static int                 nv_active  = 0;
static int                 nv_last_pub = 0;  /* buffer index of the last publish,
                                              * for nv_frame_repeat()           */
static int                 nv_enabled = 0;

/* Presented geometry. nv_pitch is the DDR line stride in pixels: the view width
 * padded to a multiple of 4, because the reader fetches whole 64-bit words. */
static int                 nv_view_w  = NV_DEF_W;
static int                 nv_view_h  = NV_DEF_H;
static int                 nv_pitch   = NV_DEF_W;
static nv_format           nv_fmt     = NV_FMT_RGB565;

static uint16_t           *nv_stage       = 0;
static size_t              nv_stage_bytes = 0;

static const volatile uint16_t *nv_pal     = 0;
static int                      nv_pal_len = 0;

static unsigned long       nv_hash_at   = 0;
static int                 nv_joy_debug = 0;

/* ---- pixel conversion ---------------------------------------------------- */

void nv_convert_1555_to_565(uint16_t *dst, const uint16_t *src, size_t px)
{
    size_t i;
    for (i = 0; i < px; i++) {
        const uint16_t s = src[i];
        const uint16_t r = (uint16_t)((s >> 10) & 0x1F);
        const uint16_t g = (uint16_t)((s >>  5) & 0x1F);
        const uint16_t b = (uint16_t)( s        & 0x1F);
        /* Green gains a bit: replicate the top bit into the new low bit so full
         * green maps to 0x3F rather than landing one step short at 0x3E. */
        dst[i] = (uint16_t)((r << 11) | ((((g << 1) | (g >> 4)) & 0x3F) << 5) | b);
    }
}

void nv_convert_8888_to_565(uint16_t *dst, const uint32_t *src, size_t px)
{
    size_t i;
    for (i = 0; i < px; i++) {
        const uint32_t s = src[i];
        dst[i] = (uint16_t)(((s >> 8) & 0xF800) |
                            ((s >> 5) & 0x07E0) |
                            ((s >> 3) & 0x001F));
    }
}

void nv_convert_pal8_to_565(uint16_t *dst, const uint8_t *src, size_t px,
                            const uint16_t *pal565)
{
    size_t i;
    for (i = 0; i < px; i++) dst[i] = pal565[src[i]];
}

/* Two pixels per 32-bit store when the destination is word-aligned, which it is
 * for every non-clipped frame (nv_pitch is a multiple of 4 and dx is 0). */
static void nv_convert_row_pal8(uint16_t *drow, const uint8_t *srow,
                                int cw, const uint16_t *pal)
{
    int x = 0;
    if ((((uintptr_t)drow) & 3) == 0) {
        uint32_t *d32 = (uint32_t *)drow;
        for (; x + 1 < cw; x += 2)
            *d32++ = (uint32_t)pal[srow[x]] | ((uint32_t)pal[srow[x + 1]] << 16);
    }
    for (; x < cw; x++) drow[x] = pal[srow[x]];
}

/* ---- frame checksum ------------------------------------------------------ */

uint64_t nv_hash_pixels(const uint16_t *buf, size_t px)
{
    uint64_t hash = 1469598103934665603ULL;   /* FNV-1a 64 offset basis */
    size_t i;
    for (i = 0; i < px; i++) {
        hash ^= (uint64_t)buf[i];
        hash *= 1099511628211ULL;             /* FNV-1a 64 prime */
    }
    return hash;
}

/* ---- lifecycle ----------------------------------------------------------- */

int nv_is_enabled(void)          { return nv_enabled; }
unsigned long nv_frame_count(void) { return nv_frames; }

int nv_open(void)
{
    const char *e;
    if (nv_enabled) return 1;

    e = getenv("MISTER_FRAME_HASH");
    nv_hash_at = e ? strtoul(e, 0, 10) : 0;
    nv_joy_debug = getenv("MISTER_JOY_DEBUG") != 0;

    if (getenv("MISTER_NO_NATIVE")) return 0;

#ifndef NV_HOST_TEST
    nv_fd = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
    if (nv_fd < 0) return 0;
    {
        void *m = mmap(NULL, NV_REGION, PROT_READ | PROT_WRITE, MAP_SHARED,
                       nv_fd, (off_t)NV_BASE);
        if (m == MAP_FAILED) { close(nv_fd); nv_fd = -1; return 0; }
        nv_base = (volatile uint8_t *)m;
    }
    nv_ctrl   = (volatile uint32_t *)(nv_base);
    nv_buf[0] = (uint16_t *)(nv_base + NV_BUF0);
    nv_buf[1] = (uint16_t *)(nv_base + NV_BUF1);
    *nv_ctrl  = 0;
    nv_frames = 0;
    nv_active = 0;
    nv_enabled = 1;
    fprintf(stderr, "nv_present: native video DDR @ 0x%08lx\n",
            (unsigned long)NV_BASE);
#endif
    return nv_enabled;
}

void nv_close(void)
{
#ifndef NV_HOST_TEST
    if (nv_base) munmap((void *)nv_base, NV_REGION);
    if (nv_fd >= 0) close(nv_fd);
#endif
    nv_base = 0; nv_fd = -1; nv_enabled = 0;
    free(nv_stage); nv_stage = 0; nv_stage_bytes = 0;
}

void nv_set_palette(const volatile uint16_t *pal565, int entries)
{
    nv_pal = pal565;
    nv_pal_len = entries;
}

void nv_set_mode(int width, int height, double refresh_hz, int rot, nv_format fmt)
{
    struct nv_modeline m;
    size_t frame_bytes;
    int pitch;
    int fits;

    nv_fmt = fmt;
    (void)rot;   /* the reader takes orientation from the published aspect */

    if (!nv_enabled) return;

    pitch = (width + 3) & ~3;
    fits = (width  > 0 && width  <= NV_MAX_W &&
            height > 0 && height <= NV_MAX_H &&
            (size_t)pitch * height * 2 <= NV_BUF_BYTES);

    if (fits) {
        nv_view_w = width;
        nv_view_h = height;
        nv_pitch  = pitch;
    } else {
        nv_view_w = NV_DEF_W;
        nv_view_h = NV_DEF_H;
        nv_pitch  = NV_DEF_W;
        fprintf(stderr, "nv_present: %dx%d exceeds the DDR buffer — "
                        "falling back to %dx%d center-clip\n",
                width, height, NV_DEF_W, NV_DEF_H);
    }

    if (!(refresh_hz > 0.0)) refresh_hz = 60.0;
    nv_make_modeline(nv_pitch, nv_view_h, refresh_hz, &m);
    nv_publish_timing(nv_base, &m);

    /* Clear both buffers once; the per-frame path writes only the live area, so
     * padding and borders stay black without a memset per frame. */
    frame_bytes = (size_t)nv_pitch * nv_view_h * 2;
    memset((void *)nv_buf[0], 0, frame_bytes);
    memset((void *)nv_buf[1], 0, frame_bytes);

    if (nv_stage_bytes < frame_bytes) {
        free(nv_stage);
        nv_stage = (uint16_t *)malloc(frame_bytes);
        nv_stage_bytes = nv_stage ? frame_bytes : 0;
    }
    if (nv_stage) memset(nv_stage, 0, nv_stage_bytes);

    fprintf(stderr,
            "nv_present: present %dx%d (pitch %d) — %dx%d total, "
            "%.3f MHz pix, %.2f kHz H, %.2f Hz V, ce_inc=%u, aspect %d:%d\n",
            nv_view_w, nv_view_h, nv_pitch, m.h_total, m.v_total,
            m.pixclk / 1e6, m.h_rate / 1e3, m.refresh, m.ce_inc, m.arx, m.ary);
    if (m.h_rate > 16500.0)
        fprintf(stderr, "nv_present: WARNING H rate %.2f kHz is above 15 kHz "
                        "CRT range (HDMI/multisync only)\n", m.h_rate / 1e3);
}

void nv_frame(const void *src, int pitch_bytes, int src_w, int src_h)
{
    int cw, ch, dx, dy, sx, sy, y;
    uint16_t *dst;

    if (!nv_enabled || !src) return;

    cw = src_w < nv_view_w ? src_w : nv_view_w;
    ch = src_h < nv_view_h ? src_h : nv_view_h;
    dx = (nv_view_w - cw) / 2;
    dy = (nv_view_h - ch) / 2;
    sx = (src_w - cw) / 2;
    sy = (src_h - ch) / 2;
    dst = nv_buf[nv_active];

    if (nv_fmt == NV_FMT_RGB565) {
        /* Already the destination format: straight to DDR, no staging. */
        const uint8_t *s = (const uint8_t *)src + (size_t)sy * pitch_bytes
                         + (size_t)sx * 2;
        if (cw == nv_pitch && (size_t)pitch_bytes == (size_t)cw * 2) {
            memcpy(dst + (size_t)dy * nv_pitch, s, (size_t)nv_pitch * ch * 2);
        } else {
            for (y = 0; y < ch; y++)
                memcpy(dst + (size_t)(dy + y) * nv_pitch + dx,
                       s + (size_t)y * pitch_bytes, (size_t)cw * 2);
        }
    } else if (nv_stage) {
        /* Everything else converts into the cached staging frame first, then
         * crosses in one memcpy. Stage and DDR share a layout, so this is a
         * single copy even in the clipped fallback -- the padding it also
         * carries is the zeroes nv_set_mode() put there. */
        if (nv_fmt == NV_FMT_PAL8) {
            uint16_t pal[512];
            int i, n = nv_pal_len > 512 ? 512 : nv_pal_len;
            if (!nv_pal) return;
            /* The palette is volatile -- the OSD layer rewrites it between
             * frames -- so take a plain copy the convert loop can keep in
             * registers. */
            for (i = 0; i < n; i++) pal[i] = nv_pal[i];
            for (; i < 512; i++)    pal[i] = 0;
            for (y = 0; y < ch; y++)
                nv_convert_row_pal8(nv_stage + (size_t)(dy + y) * nv_pitch + dx,
                                    (const uint8_t *)src
                                        + (size_t)(sy + y) * pitch_bytes + sx,
                                    cw, pal);
        } else if (nv_fmt == NV_FMT_0RGB1555) {
            for (y = 0; y < ch; y++)
                nv_convert_1555_to_565(nv_stage + (size_t)(dy + y) * nv_pitch + dx,
                                       (const uint16_t *)((const uint8_t *)src
                                           + (size_t)(sy + y) * pitch_bytes) + sx,
                                       (size_t)cw);
        } else { /* NV_FMT_XRGB8888 */
            for (y = 0; y < ch; y++)
                nv_convert_8888_to_565(nv_stage + (size_t)(dy + y) * nv_pitch + dx,
                                       (const uint32_t *)((const uint8_t *)src
                                           + (size_t)(sy + y) * pitch_bytes) + sx,
                                       (size_t)cw);
        }
        memcpy(dst, nv_stage, (size_t)nv_pitch * nv_view_h * 2);
    } else {
        return;
    }

    __sync_synchronize();
    nv_frames++;
    *nv_ctrl = ((uint32_t)nv_frames << 2) | (uint32_t)nv_active;
    nv_last_pub = nv_active;

    /* After the doorbell on purpose: the buffer just published is not reused
     * until two frames from now, so hashing it here costs the present nothing. */
    /* Hashed at N and again at 2N so ONE run can tell a driver that is running
     * from a driver that is merely advancing its frame counter. klax publishes
     * 600 frames at full speed with byte-identical DDR content -- an fps figure
     * alone calls that a pass. */
    if (nv_hash_at && (nv_frames == nv_hash_at || nv_frames == nv_hash_at * 2)) {
        uint64_t h = nv_hash_pixels(dst, (size_t)nv_pitch * nv_view_h);
        fprintf(stderr, "MISTER-FRAMEHASH frame=%lu hash=%016llx w=%d h=%d\n",
                nv_frames, (unsigned long long)h, nv_view_w, nv_view_h);
        fflush(stderr);
    }

    nv_active ^= 1;
}

void nv_frame_repeat(void)
{
    if (!nv_enabled) return;

    /* No barrier and no pixel traffic: the buffer's contents were already
     * published and fenced by the nv_frame() that filled it. Only the counter
     * moves, which is all the watchdog is watching. */
    nv_frames++;
    *nv_ctrl = ((uint32_t)nv_frames << 2) | (uint32_t)nv_last_pub;
}

uint32_t nv_pads(int player)
{
    uint32_t w;
    if (!nv_enabled || player < 0 || player > 3) return 0;
    w = *(volatile uint32_t *)(nv_base + NV_JOY_OFF[player]);
    if (nv_joy_debug) {
        static uint32_t prev[4];
        if (w != prev[player]) {
            prev[player] = w;
            fprintf(stderr, "nv_present: pad%d = 0x%08x\n", player + 1, w);
        }
    }
    return w;
}

/*
 * test_frame_writer.c — Stage 2 standalone RGB565 framebuffer test-writer.
 *
 * Proves the MAME core's DDR present path end-to-end with NO emulator: writes an
 * RGB565 pattern into the 0x3A000000 double-buffer and rings the control-word
 * doorbell that openbor_video_reader.sv scans out. DDR contract (from
 * MiSTer_OpenBOR src/native_video_writer.c):
 *
 *   0x3A000000 + 0x000  control word = (frame_counter<<2) | active_buf[1:0]
 *   0x3A000000 + 0x040  buffer 0   (320*240*2 = 153600 B, RGB565)
 *   0x3A000000 + 0x40040 buffer 1
 *
 * RGB565: R=[15:11] G=[10:5] B=[4:0]. mame4all outputs this natively (no BGR
 * swap, unlike OpenBOR's SDL surfaces), so color bars here also verify the
 * reader's channel order (a red bar must read red).
 *
 * Usage:
 *   test_frame_writer bars        static SMPTE-ish color bars (default)
 *   test_frame_writer solid RRGGBB one solid 24-bit color (converted to 565)
 *   test_frame_writer animate      alternate bars/inverted every ~0.5s (double-buffer + tear test)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#define NV_BASE     0x3A000000u
#define NV_REGION   0x00100000u   /* 1 MB */
#define NV_CTRL     0x00000000u
#define NV_BUF0     0x00000040u
#define NV_BUF1     0x00040040u
#define W 320
#define H 240
#define FRAME_PX (W * H)

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static void fill_bars(uint16_t *buf, int inverted)
{
    /* 8 vertical bars, 40 px each. */
    static const uint16_t bar[8] = {
        0xF800, /* red     */ 0x07E0, /* green   */ 0x001F, /* blue    */
        0xFFE0, /* yellow  */ 0x07FF, /* cyan    */ 0xF81F, /* magenta */
        0xFFFF, /* white   */ 0x8410  /* gray    */
    };
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int b = x / (W / 8);
            if (b > 7) b = 7;
            buf[y * W + x] = inverted ? (uint16_t)~bar[b] : bar[b];
        }
}

static void fill_solid(uint16_t *buf, uint16_t c)
{
    for (int i = 0; i < FRAME_PX; i++) buf[i] = c;
}

int main(int argc, char **argv)
{
    const char *mode = (argc > 1) ? argv[1] : "bars";

    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { perror("open /dev/mem"); return 1; }
    volatile uint8_t *base = mmap(NULL, NV_REGION, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, fd, NV_BASE);
    if (base == MAP_FAILED) { perror("mmap"); return 1; }

    volatile uint32_t *ctrl = (volatile uint32_t *)(base + NV_CTRL);
    uint16_t *buf0 = (uint16_t *)(base + NV_BUF0);
    uint16_t *buf1 = (uint16_t *)(base + NV_BUF1);

    if (!strcmp(mode, "solid")) {
        unsigned v = (argc > 2) ? (unsigned)strtoul(argv[2], 0, 16) : 0xFFFFFF;
        uint16_t c = rgb565((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
        fill_solid(buf0, c);
        *ctrl = (1u << 2) | 0u;          /* frame 1, display buffer 0 */
        printf("solid #%06X -> RGB565 0x%04X, buffer 0 active\n", v & 0xFFFFFF, c);
        return 0;
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

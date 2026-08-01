/*
 * ddr-write-bench.c — how fast can the HPS write the native-video DDR buffer?
 *
 * The present path is one frame-sized write into the 0x3A000000 window per
 * frame, and on atarisy2 (512x384 RGB565 = 384 KB) that write was 37% of the
 * whole process's CPU. This measures the ceiling directly: the same number of
 * bytes, through different store widths, under both /dev/mem mapping modes.
 *
 * Build (armhf container):
 *   gcc -O2 -marm -mfpu=neon -mfloat-abi=hard -o ddr-write-bench ddr-write-bench.c
 * Run on the device as root. It writes only into the DDR frame buffers, i.e.
 * the same bytes the emulator writes, so a loaded core just shows noise.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>
#include <arm_neon.h>

#define NV_BASE   0x3A000000u
#define NV_REGION 0x00400000u
#define NV_BUF0   0x00000040u
#define FRAME     (512u * 384u * 2u)      /* atarisy2: 384 KB */
#define ITERS     200

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void report(const char *what, const char *how, double dt)
{
    double mb = (double)FRAME * ITERS / (1024.0 * 1024.0);
    printf("  %-22s %-18s %7.1f MB/s   %6.2f ms/frame\n",
           what, how, mb / dt, dt * 1000.0 / ITERS);
}

/* Straight 32-bit stores: what the old per-pixel convert path amounted to. */
static void store32(volatile uint32_t *d, const uint32_t *s, size_t words)
{
    for (size_t i = 0; i < words; i++) d[i] = s[i];
}

/* 16-bit stores: exactly the old drow[x] = pal[srow[x]] pattern. */
static void store16(volatile uint16_t *d, const uint16_t *s, size_t halfwords)
{
    for (size_t i = 0; i < halfwords; i++) d[i] = s[i];
}

/* 128-bit NEON stores, 64 bytes per iteration. */
static void store_neon(uint8_t *d, const uint8_t *s, size_t bytes)
{
    for (size_t i = 0; i < bytes; i += 64) {
        uint8x16_t a = vld1q_u8(s + i);
        uint8x16_t b = vld1q_u8(s + i + 16);
        uint8x16_t c = vld1q_u8(s + i + 32);
        uint8x16_t e = vld1q_u8(s + i + 48);
        vst1q_u8(d + i,      a);
        vst1q_u8(d + i + 16, b);
        vst1q_u8(d + i + 32, c);
        vst1q_u8(d + i + 48, e);
    }
}

static void bench_mapping(int use_osync, const uint8_t *src)
{
    const char *how = use_osync ? "O_SYNC" : "no O_SYNC";
    int fd = open("/dev/mem", O_RDWR | O_CLOEXEC | (use_osync ? O_SYNC : 0));
    if (fd < 0) { perror("open /dev/mem"); return; }
    void *m = mmap(NULL, NV_REGION, PROT_READ | PROT_WRITE, MAP_SHARED, fd, NV_BASE);
    if (m == MAP_FAILED) { perror("mmap"); close(fd); return; }
    uint8_t *dst = (uint8_t*)m + NV_BUF0;

    double t;
    t = now_s();
    for (int i = 0; i < ITERS; i++) memcpy(dst, src, FRAME);
    report("memcpy", how, now_s() - t);

    t = now_s();
    for (int i = 0; i < ITERS; i++) store_neon(dst, src, FRAME);
    report("neon 128-bit stores", how, now_s() - t);

    t = now_s();
    for (int i = 0; i < ITERS; i++)
        store32((volatile uint32_t*)dst, (const uint32_t*)src, FRAME / 4);
    report("32-bit stores", how, now_s() - t);

    t = now_s();
    for (int i = 0; i < ITERS; i++)
        store16((volatile uint16_t*)dst, (const uint16_t*)src, FRAME / 2);
    report("16-bit stores", how, now_s() - t);

    munmap(m, NV_REGION);
    close(fd);
}

int main(void)
{
    uint8_t *src = 0;
    if (posix_memalign((void**)&src, 64, FRAME)) { perror("alloc"); return 1; }
    memset(src, 0x5a, FRAME);

    printf("frame=%u bytes, %d iterations per case\n", FRAME, ITERS);

    /* Cached RAM baseline: the same bytes into ordinary memory. */
    uint8_t *ram = 0;
    if (posix_memalign((void**)&ram, 64, FRAME) == 0) {
        double t = now_s();
        for (int i = 0; i < ITERS; i++) memcpy(ram, src, FRAME);
        report("memcpy", "cached RAM", now_s() - t);
        free(ram);
    }

    bench_mapping(1, src);
    bench_mapping(0, src);
    free(src);
    return 0;
}

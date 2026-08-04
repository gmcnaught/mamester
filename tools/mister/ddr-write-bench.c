/*
 * ddr-write-bench.c — how fast can the HPS write the native-video DDR buffer?
 *
 * The present path is one frame-sized write into the 0x3A000000 window per
 * frame, and on atarisy2 (512x384 RGB565 = 384 KB) that write was 37% of the
 * whole process's CPU. This measures the ceiling directly: the same number of
 * bytes, through different store widths, under every mapping we can obtain.
 *
 * Four transports, and they are NOT equivalent (issue #4, and
 * docs/dreamster-ddr-channel-review.md §5.2):
 *
 *   cached RAM    the upper bound. Not a transport -- the FPGA cannot see it.
 *   /dev/mem      the baseline, and what ships without the module. ARM's
 *                 phys_mem_access_prot() gives this Strongly-Ordered whatever
 *                 O_SYNC says, because pfn_valid() is false for a fabric
 *                 address -- which is why the O_SYNC A/B measures a null.
 *   /dev/mem_wc   CANDIDATE TRANSPORT. tools/mister/mem_wc/ maps the same
 *                 physical pages Normal Non-Cacheable, so stores merge. Maps
 *                 OUR window, so a win here is directly bankable: the reader
 *                 RTL, the buffer layout and the doorbell contract all stay.
 *   /dev/fb0      PROBE ONLY. fbdev conventionally maps write-combined, so a
 *                 fast result is independent evidence that WC works on this
 *                 silicon and kernel -- but fbdev maps only its OWN smem
 *                 region, so we cannot write our framebuffer through it
 *                 without moving the whole present path onto MISTER_FB.
 *
 * Build (armhf container):
 *   gcc -O2 -marm -mfpu=neon -mfloat-abi=hard -o ddr-write-bench ddr-write-bench.c
 * Run on the device as root. The /dev/mem arms write only into the DDR frame
 * buffers, i.e. the same bytes the emulator writes, so a loaded core just shows
 * noise. The /dev/fb0 arm scribbles on the framebuffer console.
 *
 * For the arm that matters:
 *   insmod mem_wc.ko phys_base=0x3A000000 phys_size=0x00400000
 * (unrestricted also works; the restricted form is what _handler.sh ships, so
 * benching it exercises the allowlist path too).
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
#define DOORBELLS 10000

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static double report(const char *what, const char *how, double dt)
{
    double mb = (double)FRAME * ITERS / (1024.0 * 1024.0);
    double rate = mb / dt;
    printf("  %-22s %-18s %7.1f MB/s   %6.2f ms/frame\n",
           what, how, rate, dt * 1000.0 / ITERS);
    return rate;
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

/* Run the four store forms against one destination.
 *
 * `neon_vs_memcpy` receives NEON's rate divided by memcpy's. This ratio is the
 * result that cannot be faked (see verdict() below), so every arm reports it. */
static void bench_dst(const char *how, uint8_t *dst, const uint8_t *src,
                      double *neon_vs_memcpy)
{
    double t, memcpy_rate, neon_rate;

    t = now_s();
    for (int i = 0; i < ITERS; i++) memcpy(dst, src, FRAME);
    memcpy_rate = report("memcpy", how, now_s() - t);

    t = now_s();
    for (int i = 0; i < ITERS; i++) store_neon(dst, src, FRAME);
    neon_rate = report("neon 128-bit stores", how, now_s() - t);

    t = now_s();
    for (int i = 0; i < ITERS; i++)
        store32((volatile uint32_t*)dst, (const uint32_t*)src, FRAME / 4);
    report("32-bit stores", how, now_s() - t);

    t = now_s();
    for (int i = 0; i < ITERS; i++)
        store16((volatile uint16_t*)dst, (const uint16_t*)src, FRAME / 2);
    report("16-bit stores", how, now_s() - t);

    if (neon_vs_memcpy)
        *neon_vs_memcpy = memcpy_rate > 0 ? neon_rate / memcpy_rate : 0;
}

/* Map the DDR window through `node` and bench the buffer-0 slot. */
static int bench_mem(const char *node, int use_osync, const char *how,
                     const uint8_t *src, double *neon_vs_memcpy)
{
    int fd = open(node, O_RDWR | O_CLOEXEC | (use_osync ? O_SYNC : 0));
    if (fd < 0) {
        printf("  %-22s %-18s unavailable (%s)\n", "--", how, node);
        return 0;
    }
    void *m = mmap(NULL, NV_REGION, PROT_READ | PROT_WRITE, MAP_SHARED, fd, NV_BASE);
    if (m == MAP_FAILED) {
        printf("  %-22s %-18s mmap failed (allowlist too narrow?)\n", "--", how);
        close(fd);
        return 0;
    }

    bench_dst(how, (uint8_t*)m + NV_BUF0, src, neon_vs_memcpy);

    munmap(m, NV_REGION);
    close(fd);
    return 1;
}

/* The doorbell, timed on its own: one 32-bit store plus the full-system drain
 * that must precede it once the pixel pages are write-combined. Sizes whether
 * keeping the control word on the strongly-ordered mapping (what nv_present.c
 * does) costs anything worth caring about. */
static void bench_doorbell(void)
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
    if (fd < 0) return;
    void *m = mmap(NULL, NV_REGION, PROT_READ | PROT_WRITE, MAP_SHARED, fd, NV_BASE);
    if (m == MAP_FAILED) { close(fd); return; }

    volatile uint32_t *ctrl = (volatile uint32_t *)m;
    uint32_t saved = *ctrl;

    double t = now_s();
    for (int i = 0; i < DOORBELLS; i++) {
        __asm__ __volatile__("dsb sy" ::: "memory");
        *ctrl = ((uint32_t)i << 2) | (uint32_t)(i & 1);
    }
    double dt = now_s() - t;

    *ctrl = saved;
    munmap(m, NV_REGION);
    close(fd);

    printf("  %-22s %-18s %7.3f us each\n", "dsb sy + doorbell",
           "strongly-ordered", dt * 1e6 / DOORBELLS);
}

/* Probe: is a write-combined mapping of FPGA-side DDR obtainable at all on this
 * kernel? fbdev's own smem, which we cannot present through, but which maps
 * WC by convention. */
static void bench_fb0(const uint8_t *src)
{
    int fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        printf("  %-22s %-18s unavailable (/dev/fb0)\n", "--", "fb0 (probe)");
        return;
    }
    /* Map exactly one frame; the console framebuffer is at least this big on
     * any mode MiSTer sets, and a short mapping keeps the scribble bounded. */
    void *m = mmap(NULL, FRAME, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) {
        printf("  %-22s %-18s mmap failed\n", "--", "fb0 (probe)");
        close(fd);
        return;
    }
    double ratio = 0;
    bench_dst("fb0 (probe)", (uint8_t*)m, src, &ratio);
    printf("      NEON/memcpy = %.2f%s\n", ratio,
           ratio > 1.0 ? "  (write-combined)" : "  (not write-combined)");
    munmap(m, FRAME);
    close(fd);
}

/* Under a Strongly-Ordered mapping every store is its own bus transaction, so
 * the path is transaction-latency bound and hand-written NEON measures SLOWER
 * than glibc memcpy (79.4 vs 89.5 MB/s, docs/bench-results.md). Write-combining
 * removes that bound and 128-bit stores merge into full bursts, so the ranking
 * must invert. That inversion is the check the throughput number cannot fake:
 * a fast MB/s figure with NEON still behind memcpy means something else changed
 * (cached alias, wrong region, module not actually loaded), not the memory
 * type. */
static void verdict(double so_ratio, double wc_ratio, int wc_ran)
{
    printf("\nverdict\n");
    printf("  NEON/memcpy, strongly-ordered : %.2f%s\n", so_ratio,
           so_ratio < 1.0 ? "  (expected: transaction-latency bound)"
                          : "  (UNEXPECTED — re-check the baseline)");
    if (!wc_ran) {
        printf("  /dev/mem_wc did not run — build and insmod "
               "tools/mister/mem_wc/ to measure the candidate transport\n");
        return;
    }
    printf("  NEON/memcpy, /dev/mem_wc      : %.2f\n", wc_ratio);
    if (wc_ratio > 1.0 && wc_ratio > so_ratio)
        printf("  => write-combining CONFIRMED: the ranking inverted.\n");
    else
        printf("  => write-combining NOT confirmed: NEON did not overtake "
               "memcpy. Whatever the MB/s says, the mapping did not change — "
               "check `dmesg | grep mem_wc` and that phys_base/phys_size cover "
               "0x%08x+0x%x.\n", NV_BASE, NV_REGION);
}

int main(void)
{
    uint8_t *src = 0;
    double so_ratio = 0, wc_ratio = 0;
    int wc_ran;

    if (posix_memalign((void**)&src, 64, FRAME)) { perror("alloc"); return 1; }
    memset(src, 0x5a, FRAME);

    printf("frame=%u bytes, %d iterations per case\n\n", FRAME, ITERS);

    /* Cached RAM baseline: the same bytes into ordinary memory. */
    uint8_t *ram = 0;
    if (posix_memalign((void**)&ram, 64, FRAME) == 0) {
        double t = now_s();
        for (int i = 0; i < ITERS; i++) memcpy(ram, src, FRAME);
        report("memcpy", "cached RAM", now_s() - t);
        free(ram);
    }

    printf("\n/dev/mem — baseline (strongly-ordered; O_SYNC cannot change it)\n");
    bench_mem("/dev/mem", 1, "O_SYNC", src, &so_ratio);
    bench_mem("/dev/mem", 0, "no O_SYNC", src, NULL);

    printf("\n/dev/mem_wc — candidate transport\n");
    wc_ran = bench_mem("/dev/mem_wc", 0, "write-combined", src, &wc_ratio);

    printf("\n/dev/fb0 — probe only (cannot carry our framebuffer)\n");
    bench_fb0(src);

    printf("\ndoorbell\n");
    bench_doorbell();

    verdict(so_ratio, wc_ratio, wc_ran);

    free(src);
    return 0;
}

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
 * Run on the device as root. The four store-form arms write only into the DDR
 * frame buffers, i.e. the same bytes the emulator writes, so a loaded core just
 * shows noise. Two arms reach further and are worth knowing about before you
 * run this against something you care about: the doorbell arm drives the
 * control word for 10000 iterations, so a loaded core's reader follows it
 * through 10000 buffer flips and then sees the counter jump backwards when the
 * saved value is restored; and the /dev/fb0 arm scribbles on the framebuffer
 * console.
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
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <arm_neon.h>

#define NV_BASE   0x3A000000u
#define NV_REGION 0x00400000u
#define NV_BUF0   0x00000040u
#define FRAME     (512u * 384u * 2u)      /* atarisy2: 384 KB */
#define ITERS     200
#define DOORBELLS 10000

/* How much faster than the strongly-ordered baseline a mapping has to measure
 * before this bench will call it write-combined. See verdict(). */
#define WC_MIN_SPEEDUP 3.0

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
 * `memcpy_rate_out` receives the memcpy figure in MB/s. memcpy is the form the
 * present path actually uses, and its rate against the strongly-ordered
 * baseline is the verdict (see verdict() below), so every arm reports it. */
static void bench_dst(const char *how, uint8_t *dst, const uint8_t *src,
                      double *memcpy_rate_out)
{
    double t, memcpy_rate;

    t = now_s();
    for (int i = 0; i < ITERS; i++) memcpy(dst, src, FRAME);
    memcpy_rate = report("memcpy", how, now_s() - t);

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

    if (memcpy_rate_out)
        *memcpy_rate_out = memcpy_rate;
}

/* Map the DDR window through `node` and bench the buffer-0 slot. */
static int bench_mem(const char *node, int use_osync, const char *how,
                     const uint8_t *src, double *memcpy_rate_out)
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

    bench_dst(how, (uint8_t*)m + NV_BUF0, src, memcpy_rate_out);

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
static void bench_fb0(const uint8_t *src, double so_rate)
{
    int fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        printf("  %-22s %-18s unavailable (/dev/fb0)\n", "--", "fb0 (probe)");
        return;
    }
    /* Ask fbdev how much there actually is, rather than assuming the console
     * framebuffer is at least FRAME on any mode MiSTer sets. mmap() of /dev/fb0
     * succeeds for any length -- it maps the smem region and leaves the rest of
     * the vma to fault -- so a framebuffer smaller than FRAME would not fail
     * here, it would SIGBUS partway through the first memcpy and kill the bench
     * on the machine you are trying to diagnose.
     *
     * Measured on 5.15.1-MiSTer: smem_len 1228800, comfortably over FRAME, so
     * the assumption happens to hold on this device in the mode it was in. It
     * is not something fbdev guarantees across modes or kernels, and the cost
     * of checking is one ioctl once. */
    struct fb_fix_screeninfo fix;
    if (ioctl(fd, FBIOGET_FSCREENINFO, &fix) != 0) {
        printf("  %-22s %-18s FBIOGET_FSCREENINFO failed\n", "--", "fb0 (probe)");
        close(fd);
        return;
    }
    if (fix.smem_len < FRAME) {
        printf("  %-22s %-18s skipped: smem_len %lu < frame %u\n", "--",
               "fb0 (probe)", (unsigned long)fix.smem_len, FRAME);
        close(fd);
        return;
    }
    void *m = mmap(NULL, FRAME, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) {
        printf("  %-22s %-18s mmap failed\n", "--", "fb0 (probe)");
        close(fd);
        return;
    }
    double fb_rate = 0;
    bench_dst("fb0 (probe)", (uint8_t*)m, src, &fb_rate);
    if (so_rate > 0)
        printf("      memcpy vs strongly-ordered = %.1fx%s\n",
               fb_rate / so_rate,
               fb_rate >= so_rate * WC_MIN_SPEEDUP
                   ? "  (write-combining is reachable on this kernel)"
                   : "  (no faster than /dev/mem — fbdev is not write-combined"
                     " here, so this probe says nothing)");
    munmap(m, FRAME);
    close(fd);
}

/* The verdict is the memcpy rate through /dev/mem_wc against the memcpy rate
 * through /dev/mem, into the SAME physical bytes with the same instructions.
 * Only the page attribute differs between the two arms, so a large ratio has
 * nowhere to come from except the memory type.
 *
 * Strongly-Ordered stores cannot merge, so each is its own bus transaction and
 * the path is transaction-latency bound at ~89 MB/s. Write-combining removes
 * that bound; measured on 5.15.1-MiSTer, 858.5 vs 89.0 MB/s.
 *
 * A RATIO rather than an absolute rate, because the absolute rate moves with
 * whatever else the A9 is doing: across four runs, with and without a game on
 * the other core, the strongly-ordered arm measured 44.7 to 89.0 MB/s -- but the
 * ratio stayed 7.8x to 9.6x, because both arms are slowed by the same load. A
 * 3x threshold sits far under the low end of that and far over anything the
 * strongly-ordered mapping can reach.
 *
 * An EARLIER VERSION of this check looked for hand-written NEON to overtake
 * glibc memcpy, on the theory that the transaction-latency bound was what held
 * 128-bit stores back. That is wrong and it reported failure on a working
 * module: glibc's ARM memcpy is itself NEON with prefetch and better alignment
 * handling than store_neon(), so it wins under BOTH memory types (measured
 * NEON/memcpy = 0.95 strongly-ordered, 0.66 write-combined). Do not reinstate
 * it. The per-form rates are still printed because the SHAPE of the four is
 * informative -- under WC the 16- and 32-bit forms close most of the gap to
 * memcpy (24 -> 315, 46 -> 331 MB/s) -- but no verdict is drawn from them. */
static void verdict(double so_rate, double wc_rate, int wc_ran)
{
    printf("\nverdict\n");
    printf("  memcpy, /dev/mem (strongly-ordered) : %7.1f MB/s\n", so_rate);
    if (!wc_ran) {
        printf("  /dev/mem_wc did not run — build and insmod "
               "tools/mister/mem_wc/ to measure the candidate transport\n");
        return;
    }
    printf("  memcpy, /dev/mem_wc                 : %7.1f MB/s\n", wc_rate);
    if (so_rate <= 0) {
        printf("  => no baseline to compare against — the /dev/mem arm did "
               "not run.\n");
        return;
    }
    printf("  speedup                             : %7.1fx (need %.1fx)\n",
           wc_rate / so_rate, WC_MIN_SPEEDUP);
    if (wc_rate >= so_rate * WC_MIN_SPEEDUP)
        printf("  => write-combining CONFIRMED: same bytes, same stores, "
               "only the page attribute differs.\n");
    else
        printf("  => write-combining NOT confirmed: /dev/mem_wc is no faster "
               "than /dev/mem, so its mmap did not give Normal Non-Cacheable "
               "pages — check `dmesg | grep mem_wc` and that phys_base/"
               "phys_size cover 0x%08x+0x%x.\n", NV_BASE, NV_REGION);
}

int main(void)
{
    uint8_t *src = 0;
    double so_rate = 0, wc_rate = 0;   /* memcpy MB/s, the verdict's inputs */
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
    bench_mem("/dev/mem", 1, "O_SYNC", src, &so_rate);
    bench_mem("/dev/mem", 0, "no O_SYNC", src, NULL);

    printf("\n/dev/mem_wc — candidate transport\n");
    wc_ran = bench_mem("/dev/mem_wc", 0, "write-combined", src, &wc_rate);

    printf("\n/dev/fb0 — probe only (cannot carry our framebuffer)\n");
    bench_fb0(src, so_rate);

    printf("\ndoorbell\n");
    bench_doorbell();

    verdict(so_rate, wc_rate, wc_ran);

    free(src);
    return 0;
}

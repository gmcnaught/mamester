/*
 * mister_video.cpp — MiSTer bench backend for mame4all-pi.
 *
 * Replaces the Raspberry Pi VideoCore present path (src/rpi/minimal.cpp +
 * gles2.cpp), which links bcm_host/EGL/GLES and does not exist on the MiSTer
 * Cortex-A9 HPS. It provides the same gp2x_* surface (see src/rpi/minimal.h) the
 * osd layer calls, but the present is a plain software path:
 *
 *   - v0 (this file): count frames and report achieved fps — the CPU-budget
 *     bench (docs/feasibility.md §6, step 1). Optionally blit the RGB565 frame
 *     to /dev/fb0 when present, so it can be eyeballed on device; never required.
 *   - later: replace the /dev/fb0 blit with the DDR double-buffer present shim
 *     (tools/mame-frontend/) once a driver set is confirmed to run at speed.
 *
 * SDL input is kept (works if a device is attached); it is not needed for the
 * headless bench. No emulation code is touched — only the present seam.
 *
 * Env knobs:
 *   MISTER_BENCH_FRAMES=N   exit after N presented frames (for fixed-length runs)
 *   MISTER_FB=1             blit 16bpp frames to /dev/fb0 when available
 *   MISTER_JOY_DEBUG=1      log each MiSTer joystick word as it changes
 *   MISTER_FRAME_HASH=N     print an FNV-1a 64 of the published frame at frame N,
 *                           for comparing two builds that should be pixel-identical
 *
 * Two traps when using MISTER_FRAME_HASH:
 *
 *   1. Run the binary from its own directory. MAME chdir()s to realpath(argv[0])'s
 *      directory at startup (src/rpi/rpi.cpp), so `/tmp/mame-x <game> -rompath roms`
 *      looks in /tmp/roms and reports every ROM missing — which looks exactly like
 *      a romset-version mismatch and is not one.
 *   2. Pick a frame where the driver is actually animating. Attract modes hold
 *      still for long stretches (gng frames 899-902 are byte-identical), so a
 *      matching hash there is weak evidence. Confirm frames N and N+1 differ under
 *      one build before comparing two. Vector drivers never hold still.
 */

#include "minimal.h"
#include "driver.h"

#include "nv_modeline.h"
#include "mister_profile.h"

#include <SDL.h>
#include <time.h>
#include <stdint.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

// ---- data globals the osd layer references (see minimal.h) ----------------
unsigned long            gp2x_dev[3];
unsigned char           *gp2x_screen8   = 0;
unsigned short          *gp2x_screen15  = 0;
void                    *rpi_screen     = 0;
volatile unsigned short  gp2x_palette[512];
int                      rotate_controls = 0;
void (*gles2_draw)(void *screen, int width, int height) = 0;  // unused here

static int surface_width  = 0;
static int surface_height = 0;

// ---- fps bench state ------------------------------------------------------
static unsigned long long bench_frames = 0;
static unsigned long      bench_t0_us  = 0;
static unsigned long      bench_last_us = 0;
static unsigned long      bench_limit  = 0;   // MISTER_BENCH_FRAMES (0 = unlimited)

// ---- optional /dev/fb0 target --------------------------------------------
static int       fb_fd     = -1;
static uint8_t  *fb_mem    = 0;
static size_t    fb_len    = 0;
static uint32_t  fb_w = 0, fb_h = 0, fb_stride = 0, fb_bpp = 0;
static bool      fb_enabled = false;

// ---- MiSTer native-video DDR present (the real core output) ---------------
// 0x3A000000 RGB565 double-buffer. Contract from MiSTer_OpenBOR
// native_video_writer.c, proven by tools/mister/test_frame_writer.c:
//   +0x000 control = (frame_counter<<2)|active_buf ; +0x040 BUF0 ; +0x100040 BUF1
// The openbor_video_reader stale-frame watchdog blanks on a stalled counter, so
// we bump the control word every frame (DisplayScreen is per-frame). mame4all's
// gp2x_screen15 is already RGB565 (gp2x_video_color15 macro) — no BGR swap.
// Stage 3 presents at the driver's NATIVE size: gp2x_set_video_mode publishes a
// modeline to the timing registers at +0x300000 and the frame is written at its
// own width/height (padded to a multiple of 4 pixels per line). Frames wider or
// taller than the reader supports fall back to the fixed 320x240 center-clip.
static const uintptr_t NV_BASE   = 0x3A000000u;
static const size_t    NV_REGION = 0x00400000u;   // 4 MB (upper 512 MB is the FPGA's)
static const size_t    NV_BUF0   = 0x00000040u;
static const size_t    NV_BUF1   = 0x00100040u;
// 1 MB per buffer slot; the reader's largest frame (512x512) is half of that.
static const size_t    NV_BUF_BYTES = 0x00100000u;
// Widest / tallest the reader supports (line FIFO and counter widths).
static const int       NV_MAX_W = 512, NV_MAX_H = 512;
// Fallback mode = the fixed Stage-4 geometry.
static const int       NV_DEF_W = 320, NV_DEF_H = 240;

static int               nv_fd    = -1;
static volatile uint8_t *nv_base  = 0;
static volatile uint32_t*nv_ctrl  = 0;
static uint16_t         *nv_buf[2] = {0, 0};
static uint32_t          nv_frame = 0;
static int               nv_active = 0;
static bool              nv_enabled = false;

// Geometry currently presented: nv_view_w/h is the displayed area, nv_pitch is
// the DDR line stride in pixels (the view width padded to a multiple of 4,
// because the reader fetches whole 64-bit words = 4 RGB565 pixels).
static int               nv_view_w = NV_DEF_W, nv_view_h = NV_DEF_H;
static int               nv_pitch  = NV_DEF_W;

// Staging frame for palettised (8bpp) drivers. The DDR mapping is uncached, so
// a store into it costs orders of magnitude more than a cached one: converting
// pixel-by-pixel straight into DDR spent 65% of the whole process's CPU on
// atarisy2 (512x384). The frame is converted here instead and then pushed
// across with one memcpy, which tools/mister/ddr-write-bench.c measures at
// 89 MB/s — 4.2 ms for that frame, and the same whatever the store width or
// mapping mode, i.e. a floor set by uncached transaction latency, not by the
// DDR3 or by codegen.
static uint16_t         *nv_stage = 0;
static size_t            nv_stage_bytes = 0;

// MISTER_FRAME_HASH=N — checksum the published frame at frame N. Declared here
// rather than beside nv_hash_frame() because init_SDL() reads the environment
// long before nv_present() is first called.
static unsigned long     nv_hash_at = 0;

static void nv_init(void);
static void nv_configure(int width, int height);
static void nv_present(void);

// MISTER_JOY_DEBUG=1 logs each MiSTer joystick word as it changes.
static bool nv_joy_debug = false;

// ---- per-game modeline ----------------------------------------------------
// Computation and the register layout are shared with the standalone fabric
// test writer — see nv_modeline.h.

// SDL input event handlers live in src/rpi/input.cpp.
extern void keyprocess(SDLKey inkey, SDL_bool pressed);
extern void joyprocess(Uint8 button, SDL_bool pressed, Uint8 njoy);
extern void mouse_motion_process(int x, int y);
extern void mouse_button_process(Uint8 button, SDL_bool pressed);

static SDL_Surface* sdlscreen = NULL;
SDL_Joystick* myjoy[4] = {0, 0, 0, 0};   // global: src/rpi/input.cpp references it

// ---- timing (verbatim from minimal.cpp) -----------------------------------
unsigned long gp2x_timer_read(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC_RAW, &now);
    return (unsigned long)((unsigned long long)now.tv_sec * 1000000ULL +
                           (now.tv_nsec / 1000ULL));
}

void gp2x_timer_delay(unsigned long ticks)
{
    unsigned long ini = gp2x_timer_read();
    while (gp2x_timer_read() - ini < ticks) { /* busy wait, as upstream */ }
}

// ---- optional fb0 open ----------------------------------------------------
static void fb_open_if_requested(void)
{
    const char* want = getenv("MISTER_FB");
    if (!want || want[0] == '0') return;
    fb_fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if (fb_fd < 0) return;
    fb_var_screeninfo var = {};
    fb_fix_screeninfo fix = {};
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &var) < 0 ||
        ioctl(fb_fd, FBIOGET_FSCREENINFO, &fix) < 0) {
        close(fb_fd); fb_fd = -1; return;
    }
    fb_w = var.xres; fb_h = var.yres; fb_bpp = var.bits_per_pixel;
    fb_stride = fix.line_length;
    fb_len = fix.smem_len ? fix.smem_len : (size_t)fb_stride * fb_h;
    void* m = mmap(0, fb_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (m == MAP_FAILED) { close(fb_fd); fb_fd = -1; return; }
    fb_mem = (uint8_t*)m;
    fb_enabled = (fb_bpp == 16);   // v0 blits RGB565 only
    fprintf(stderr, "mister_video: fb0 %ux%u %ubpp stride=%u blit=%d\n",
            fb_w, fb_h, fb_bpp, fb_stride, (int)fb_enabled);
}

// ---- lifecycle ------------------------------------------------------------
int init_SDL(void)
{
    if (SDL_Init(SDL_INIT_JOYSTICK) < 0) {
        fprintf(stderr, "mister_video: SDL joystick init failed: %s\n", SDL_GetError());
        // Not fatal for the bench — carry on without input.
    }
    // A video surface is needed for SDL 1.2 keyboard events; harmless under the
    // dummy driver (SDL_VIDEODRIVER=dummy, set by the launch handler). Failure
    // is non-fatal for a headless bench.
    sdlscreen = SDL_SetVideoMode(0, 0, 16, SDL_SWSURFACE);

    if (SDL_NumJoysticks()) {
        SDL_JoystickEventState(SDL_ENABLE);
        for (int i = 0; i < SDL_NumJoysticks() && i < 4; i++)
            myjoy[i] = SDL_JoystickOpen(i);
    }
    SDL_EventState(SDL_ACTIVEEVENT, SDL_IGNORE);
    SDL_EventState(SDL_SYSWMEVENT, SDL_IGNORE);
    SDL_EventState(SDL_VIDEORESIZE, SDL_IGNORE);
    SDL_EventState(SDL_USEREVENT, SDL_IGNORE);
    SDL_ShowCursor(SDL_DISABLE);

    const char* lim = getenv("MISTER_BENCH_FRAMES");
    bench_limit = lim ? strtoul(lim, 0, 10) : 0;
    nv_joy_debug = getenv("MISTER_JOY_DEBUG") != 0;
    const char* fh = getenv("MISTER_FRAME_HASH");
    nv_hash_at = fh ? strtoul(fh, 0, 10) : 0;
    fb_open_if_requested();
    mister_profile_init();      // no-op unless MISTER_PROFILE is set
    return 1;
}

void deinit_SDL(void)
{
    if (sdlscreen) { SDL_FreeSurface(sdlscreen); sdlscreen = NULL; }
    SDL_Quit();
}

void gp2x_deinit(void)
{
    if (nv_base) { munmap((void*)nv_base, NV_REGION); nv_base = 0; nv_enabled = false; }
    if (nv_fd >= 0) { close(nv_fd); nv_fd = -1; }
    if (fb_mem) { munmap(fb_mem, fb_len); fb_mem = 0; }
    if (fb_fd >= 0) { close(fb_fd); fb_fd = -1; }
    if (nv_stage) { free(nv_stage); nv_stage = 0; nv_stage_bytes = 0; }
    if (gp2x_screen8)  free(gp2x_screen8);
    if (gp2x_screen15) free(gp2x_screen15);
    gp2x_screen8 = 0; gp2x_screen15 = 0; rpi_screen = 0;
}

void gp2x_set_video_mode(struct osd_bitmap *bitmap, int bpp, int width, int height)
{
    surface_width  = width;
    surface_height = height;
    size_t px = (size_t)width * height;

    gp2x_screen8 = 0; gp2x_screen15 = 0;
    if (bitmap->depth == 8) {
        gp2x_screen8 = (unsigned char*)calloc(1, px);
        rpi_screen = gp2x_screen8;
    } else {
        gp2x_screen15 = (unsigned short*)calloc(1, px * 2);
        rpi_screen = gp2x_screen15;
    }
    fprintf(stderr, "mister_video: set_video_mode %dx%d depth=%d\n",
            width, height, bitmap->depth);
    nv_init();
    nv_configure(width, height);
}

// ---- the present seam -----------------------------------------------------
static void bench_tick(void)
{
    unsigned long now = gp2x_timer_read();
    if (bench_frames == 0) { bench_t0_us = now; bench_last_us = now; }
    bench_frames++;

    if (now - bench_last_us >= 2000000UL) {   // report every ~2 s
        double dt = (now - bench_t0_us) / 1e6;
        fprintf(stderr, "MISTER-BENCH fps=%.2f frames=%llu elapsed=%.1fs\n",
                dt > 0 ? bench_frames / dt : 0.0,
                (unsigned long long)bench_frames, dt);
        bench_last_us = now;
    }
    if (bench_limit && bench_frames >= bench_limit) {
        double dt = (now - bench_t0_us) / 1e6;
        fprintf(stderr, "MISTER-BENCH DONE fps=%.2f frames=%llu elapsed=%.1fs\n",
                dt > 0 ? bench_frames / dt : 0.0,
                (unsigned long long)bench_frames, dt);
        gp2x_deinit();
        deinit_SDL();
        exit(0);
    }
}

static void fb_blit16(void)
{
    if (!fb_enabled || !gp2x_screen15) return;
    uint32_t w = surface_width  < (int)fb_w ? surface_width  : fb_w;
    uint32_t h = surface_height < (int)fb_h ? surface_height : fb_h;
    for (uint32_t y = 0; y < h; y++)
        memcpy(fb_mem + (size_t)y * fb_stride,
               gp2x_screen15 + (size_t)y * surface_width,
               (size_t)w * 2);
}

// Map the native-video DDR region. Fails gracefully off-device (no /dev/mem or
// not root) so the bench mode still works. Idempotent.
static void nv_init(void)
{
    if (nv_enabled || getenv("MISTER_NO_NATIVE")) return;
    nv_fd = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
    if (nv_fd < 0) return;
    void* m = mmap(NULL, NV_REGION, PROT_READ | PROT_WRITE, MAP_SHARED, nv_fd,
                   (off_t)NV_BASE);
    if (m == MAP_FAILED) { close(nv_fd); nv_fd = -1; return; }
    nv_base = (volatile uint8_t*)m;
    nv_ctrl = (volatile uint32_t*)(nv_base);
    nv_buf[0] = (uint16_t*)(nv_base + NV_BUF0);
    nv_buf[1] = (uint16_t*)(nv_base + NV_BUF1);
    *nv_ctrl = 0;
    nv_frame = 0; nv_active = 0; nv_enabled = true;
    fprintf(stderr, "mister_video: native video DDR @ 0x%08lx\n",
            (unsigned long)NV_BASE);
}

// Pick the presented geometry for this driver and publish its modeline. Called
// from gp2x_set_video_mode, before the first present.
static void nv_configure(int width, int height)
{
    if (!nv_enabled) return;

    int pitch = (width + 3) & ~3;
    bool fits = (width  > 0 && width  <= NV_MAX_W &&
                 height > 0 && height <= NV_MAX_H &&
                 (size_t)pitch * height * 2 <= NV_BUF_BYTES);

    if (fits) {
        nv_view_w = width;
        nv_view_h = height;
        nv_pitch  = pitch;
    } else {
        // Too large for a buffer (or for the reader) — fall back to the fixed
        // mode and center-clip, as Stage 4 did for every game.
        nv_view_w = NV_DEF_W;
        nv_view_h = NV_DEF_H;
        nv_pitch  = NV_DEF_W;
        fprintf(stderr, "mister_video: %dx%d exceeds the DDR buffer — "
                        "falling back to %dx%d center-clip\n",
                width, height, NV_DEF_W, NV_DEF_H);
    }

    double fps = 60.0;
    if (Machine && Machine->drv && Machine->drv->frames_per_second > 0.0f)
        fps = (double)Machine->drv->frames_per_second;

    struct nv_modeline m;
    nv_make_modeline(nv_pitch, nv_view_h, fps, &m);
    nv_publish_timing(nv_base, &m);

    // Clear both buffers once; the per-frame path only writes the live area,
    // so any padding/border stays black without a memset per frame.
    size_t frame_bytes = (size_t)nv_pitch * nv_view_h * 2;
    memset((void*)nv_buf[0], 0, frame_bytes);
    memset((void*)nv_buf[1], 0, frame_bytes);

    // Staging frame (see nv_present). Zeroed once for the same reason as the
    // DDR buffers: only the live area is rewritten.
    if (nv_stage_bytes < frame_bytes) {
        free(nv_stage);
        nv_stage = (uint16_t*)malloc(frame_bytes);
        nv_stage_bytes = nv_stage ? frame_bytes : 0;
    }
    if (nv_stage) memset(nv_stage, 0, nv_stage_bytes);

    fprintf(stderr,
            "mister_video: present %dx%d (pitch %d) — %dx%d total, "
            "%.3f MHz pix, %.2f kHz H, %.2f Hz V, ce_inc=%u, aspect %d:%d\n",
            nv_view_w, nv_view_h, nv_pitch, m.h_total, m.v_total,
            m.pixclk / 1e6, m.h_rate / 1e3, m.refresh, m.ce_inc, m.arx, m.ary);
    if (m.h_rate > 16500.0)
        fprintf(stderr, "mister_video: WARNING H rate %.2f kHz is above 15 kHz "
                        "CRT range (HDMI/multisync only)\n", m.h_rate / 1e3);
}

// Convert one 8bpp row to RGB565 in cached memory. Two pixels per 32-bit store
// when the destination is word-aligned, which it is for every non-clipped
// frame (nv_pitch is a multiple of 4 and dx is 0).
static inline void nv_convert_row(uint16_t* drow, const unsigned char* srow,
                                  int cw, const uint16_t* pal)
{
    int x = 0;
    if ((((uintptr_t)drow) & 3) == 0) {
        uint32_t* d32 = (uint32_t*)drow;
        for (; x + 1 < cw; x += 2)
            *d32++ = (uint32_t)pal[srow[x]] | ((uint32_t)pal[srow[x + 1]] << 16);
    }
    for (; x < cw; x++) drow[x] = pal[srow[x]];
}

// Write the current mame frame into the inactive DDR buffer and ring the
// doorbell.
//
// The rule here is that nothing crosses into the uncached mapping a pixel at a
// time. A 16bpp driver has already produced RGB565, so its rows go straight to
// --- deterministic frame checksum ------------------------------------------
// MISTER_FRAME_HASH=N prints a 64-bit FNV-1a of the published frame at frame N.
//
// Emulation is deterministic given a fixed frame count and no input, so two
// builds that differ only in codegen MUST produce identical hashes. A mismatch
// is a real pixel difference with an exact frame to reproduce it at. This is the
// one test that catches wrong output which does not crash -- a crash/hang sweep
// passes such a bug silently, and -ffast-math with NEON is exactly where
// reassociation can move a vector coordinate by an LSB.
//
// Hashes the DDR buffer itself rather than the staging frame, so it covers BOTH
// the 8bpp staged path and the 16bpp direct-to-DDR carve-out. That read comes
// from the uncached window at roughly 89 MB/s, which is why it is one-shot: it
// runs on a single frame and after the doorbell, so it never delays a present.
//
// It does NOT cover audio. Nothing here does, and fast-math is most likely to
// change audio in a way nobody notices. Treat that as a known hole.
static void nv_hash_frame(const uint16_t* buf, size_t px, unsigned long frame,
                          int w, int h)
{
    uint64_t hash = 1469598103934665603ULL;        // FNV-1a 64 offset basis
    for (size_t i = 0; i < px; i++) {
        hash ^= (uint64_t)buf[i];
        hash *= 1099511628211ULL;                  // FNV-1a 64 prime
    }
    fprintf(stderr, "MISTER-FRAMEHASH frame=%lu hash=%016llx w=%d h=%d\n",
            frame, (unsigned long long)hash, w, h);
    fflush(stderr);
}

// DDR by memcpy. A palettised one is converted into a cached staging frame
// first and then crosses in one memcpy — converting directly into DDR cost 65%
// of process CPU on atarisy2 (512x384), and the same tax applied to every 8bpp
// driver, which is most of the 0.37b5 set. Staging the 16bpp case as well was
// tried and reverted: without a worker thread to overlap the DDR write, the
// extra cached copy is pure loss (mk 74.1 -> 72.3 fps).
static void nv_present(void)
{
    if (!nv_enabled) return;

    const int cw = surface_width  < nv_view_w ? surface_width  : nv_view_w;
    const int ch = surface_height < nv_view_h ? surface_height : nv_view_h;
    const int dx = (nv_view_w - cw) / 2, dy = (nv_view_h - ch) / 2;
    const int sx = (surface_width  - cw) / 2, sy = (surface_height - ch) / 2;
    uint16_t* dst = nv_buf[nv_active];

    if (gp2x_screen15) {
        const uint16_t* src = gp2x_screen15 + (size_t)sy * surface_width + sx;
        if (cw == nv_pitch && cw == surface_width) {
            memcpy(dst + (size_t)dy * nv_pitch, src, (size_t)nv_pitch * ch * 2);
        } else {
            for (int y = 0; y < ch; y++)
                memcpy(dst + (size_t)(dy + y) * nv_pitch + dx,
                       src + (size_t)y * surface_width, (size_t)cw * 2);
        }
    } else if (gp2x_screen8 && nv_stage) {
        // gp2x_palette is volatile (the osd layer rewrites it); take a plain
        // copy so the convert loop can keep entries in registers.
        uint16_t pal[512];
        for (int i = 0; i < 512; i++) pal[i] = gp2x_palette[i];

        for (int y = 0; y < ch; y++)
            nv_convert_row(nv_stage + (size_t)(dy + y) * nv_pitch + dx,
                           gp2x_screen8 + (size_t)(sy + y) * surface_width + sx,
                           cw, pal);

        // Stage and DDR buffer share a layout (same pitch, same centring), so
        // this is one memcpy even in the clipped fallback — the padding it also
        // copies is the zeroes nv_configure put there.
        memcpy(dst, nv_stage, (size_t)nv_pitch * nv_view_h * 2);
    } else {
        return;
    }

    __sync_synchronize();
    nv_frame++;
    *nv_ctrl = (nv_frame << 2) | (uint32_t)nv_active;

    // After the doorbell on purpose: the buffer just published is not reused
    // until two frames from now, so hashing it here costs the present nothing.
    if (nv_hash_at && nv_frame == nv_hash_at)
        nv_hash_frame(dst, (size_t)nv_pitch * nv_view_h, nv_frame,
                      nv_view_w, nv_view_h);

    nv_active ^= 1;
}

void DisplayScreen(void)
{
    nv_present();
    fb_blit16();
    bench_tick();
}

void gp2x_video_flip(void)
{
    DisplayScreen();
}

// ---- no-op / trivial stubs (palette, gles, throttle, frontend, printf) ----
void gp2x_video_setpalette(void) {}
void gp2x_sound_volume(int, int) {}
void update_throttle(void) {}

void gles2_create(int, int, int, int, int) {}
void gles2_destroy(void) {}
void gles2_palette_changed(void) {}

// ---- MiSTer controllers ---------------------------------------------------
// The FPGA writes each player's joystick word into DDR once per frame
// (openbor_video_reader, ST_WRITE_JOY0..3). The bit map is the core's CONF_STR
// J1 line and is identical for all four players — MAMESTer follows the
// multi-game arcade convention (NeoGeo_MiSTer) of Start and Coin on each
// player's own pad, rather than the Start 1P / Start 2P of a single-game core:
//
//   [0] right  [1] left  [2] down  [3] up
//   [4..9] Fire 1..Fire 6   [10] Start   [11] Coin   [12] Pause
//
// Each bit is replayed as MAME's own default binding for that input
// (src/inptport.cpp), so a pad works with every driver out of the box and the
// TAB menu can still remap. MAME 0.37b5 has no keyboard defaults at all for
// P4 movement or buttons (only Start 4 / Coin 4), and only three buttons for
// P3, so a full four-player setup needs bindings at the joystick layer — see
// the ledger.
static const size_t NV_JOY_OFF[4] = { 0x08, 0x18, 0x20, 0x28 };

enum {
    NV_BIT_RIGHT = 0, NV_BIT_LEFT = 1, NV_BIT_DOWN = 2, NV_BIT_UP = 3,
    NV_BIT_FIRE1 = 4, NV_BIT_START = 10, NV_BIT_COIN = 11, NV_BIT_PAUSE = 12
};

struct nv_padmap {
    SDLKey dir[4];      // right, left, down, up — bit order
    SDLKey fire[6];
    SDLKey start, coin;
};

static const nv_padmap nv_pad[4] = {
    // P1: arrows, LCtrl/LAlt/Space/LShift/Z/X, start 1, coin 5
    { { SDLK_RIGHT, SDLK_LEFT, SDLK_DOWN, SDLK_UP },
      { SDLK_LCTRL, SDLK_LALT, SDLK_SPACE, SDLK_LSHIFT, SDLK_z, SDLK_x },
      SDLK_1, SDLK_5 },
    // P2: G/D/F/R, A/S/Q/W, start 2, coin 6
    { { SDLK_g, SDLK_d, SDLK_f, SDLK_r },
      { SDLK_a, SDLK_s, SDLK_q, SDLK_w, SDLK_UNKNOWN, SDLK_UNKNOWN },
      SDLK_2, SDLK_6 },
    // P3: L/J/K/I, RCtrl/RShift/Enter, start 3, coin 7
    { { SDLK_l, SDLK_j, SDLK_k, SDLK_i },
      { SDLK_RCTRL, SDLK_RSHIFT, SDLK_RETURN, SDLK_UNKNOWN, SDLK_UNKNOWN, SDLK_UNKNOWN },
      SDLK_3, SDLK_7 },
    // P4: no movement/button defaults exist in 0.37b5 — start/coin only
    { { SDLK_UNKNOWN, SDLK_UNKNOWN, SDLK_UNKNOWN, SDLK_UNKNOWN },
      { SDLK_UNKNOWN, SDLK_UNKNOWN, SDLK_UNKNOWN, SDLK_UNKNOWN, SDLK_UNKNOWN, SDLK_UNKNOWN },
      SDLK_4, SDLK_8 },
};

static uint32_t nv_joy_prev[4];

static void nv_key_edge(uint32_t changed, uint32_t state, int bit, SDLKey k)
{
    if (k == SDLK_UNKNOWN || !(changed & (1u << bit))) return;
    keyprocess(k, (state & (1u << bit)) ? SDL_TRUE : SDL_FALSE);
}

// Replay pad edges as key presses. Called once per frame from
// gp2x_joystick_read(), which MAME calls (via osd_poll_joysticks) immediately
// before it reads the input ports.
static void nv_poll_pads(void)
{
    if (!nv_enabled) return;

    for (int p = 0; p < 4; p++) {
        uint32_t w = *(volatile uint32_t*)(nv_base + NV_JOY_OFF[p]);
        uint32_t changed = w ^ nv_joy_prev[p];
        if (!changed) continue;
        nv_joy_prev[p] = w;

        if (nv_joy_debug)
            fprintf(stderr, "mister_video: pad%d = 0x%08x\n", p + 1, w);

        for (int i = 0; i < 4; i++)
            nv_key_edge(changed, w, i, nv_pad[p].dir[i]);
        for (int i = 0; i < 6; i++)
            nv_key_edge(changed, w, NV_BIT_FIRE1 + i, nv_pad[p].fire[i]);
        nv_key_edge(changed, w, NV_BIT_START, nv_pad[p].start);
        nv_key_edge(changed, w, NV_BIT_COIN,  nv_pad[p].coin);
        if (p == 0)
            nv_key_edge(changed, w, NV_BIT_PAUSE, SDLK_p);   // MAME's UI pause
    }
}

unsigned long gp2x_joystick_read(void)
{
    SDL_Event event;
    nv_poll_pads();          // MiSTer controllers, via the FPGA joystick words
    mouse_motion_process(0, 0);
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_KEYDOWN:        keyprocess(event.key.keysym.sym, SDL_TRUE);  break;
        case SDL_KEYUP:          keyprocess(event.key.keysym.sym, SDL_FALSE); break;
        case SDL_JOYBUTTONDOWN:  joyprocess(event.jbutton.button, SDL_TRUE,  event.jbutton.which); break;
        case SDL_JOYBUTTONUP:    joyprocess(event.jbutton.button, SDL_FALSE, event.jbutton.which); break;
        case SDL_MOUSEMOTION:    mouse_motion_process(event.motion.xrel, event.motion.yrel); break;
        case SDL_MOUSEBUTTONDOWN: mouse_button_process(event.button.button, SDL_TRUE);  break;
        case SDL_MOUSEBUTTONUP:   mouse_button_process(event.button.button, SDL_FALSE); break;
        default: break;
        }
    }
    return 0;
}

// Frontend/menu — the bench launches a game directly on the command line, so
// these interactive-menu helpers are stubs.
void gp2x_frontend_init(void) {}
void gp2x_frontend_deinit(void) {}
void FE_DisplayScreen(void) {}
void gp2x_gamelist_text_out(int, int, char*, int) {}
void gp2x_gamelist_text_out_fmt(int, int, char*, ...) {}

void gp2x_printf_init(void) {}
void gp2x_printf(char* fmt, ...)
{
    va_list ap; va_start(ap, fmt); vfprintf(stdout, fmt, ap); va_end(ap);
}

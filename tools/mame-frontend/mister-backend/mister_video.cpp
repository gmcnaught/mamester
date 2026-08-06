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

// ---- MiSTer native-video DDR present --------------------------------------
// The DDR contract, the modeline publish, the staging frame, the format
// converters, the frame checksum and the joystick words all live in
// nv_present.c now, shared with tools/mame-frontend/libretro-host/. What is
// left in this file is mame4all's gp2x_* OSD glue and nothing else.
#include "nv_present.h"


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
    if (fb_mem) { munmap(fb_mem, fb_len); fb_mem = 0; }
    if (fb_fd >= 0) { close(fb_fd); fb_fd = -1; }
    nv_close();
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
    nv_open();
    nv_set_palette(gp2x_palette, 512);
    // mame4all rotates its own bitmap, so rotation is always 0 here. The
    // refresh is the driver's own -- mk is 53.2 Hz, not 60.
    {
        double hz = (Machine && Machine->drv && Machine->drv->frames_per_second > 0.0f)
                  ? (double)Machine->drv->frames_per_second : 60.0;
        nv_set_mode(width, height, hz, 0,
                    bitmap->depth == 8 ? NV_FMT_PAL8 : NV_FMT_RGB565);
    }
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
        /* present= records which DDR mapping the run measured; a WC/SO A/B is
         * unreadable without it. The trailing field is safe for
         * tools/gap-triage.sh, which greps `fps=\([0-9.]*\)`. */
        fprintf(stderr,
                "MISTER-BENCH DONE fps=%.2f frames=%llu elapsed=%.1fs present=%s\n",
                dt > 0 ? bench_frames / dt : 0.0,
                (unsigned long long)bench_frames, dt,
                nv_is_write_combined() ? "write-combined" : "strongly-ordered");
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

void DisplayScreen(void)
{
    // The source is whichever surface gp2x_set_video_mode allocated; its stride
    // is surface_width, and nv_frame() centre-clips if the driver is larger than
    // the presented geometry.
    if (gp2x_screen15)
        nv_frame(gp2x_screen15, surface_width * 2, surface_width, surface_height);
    else if (gp2x_screen8)
        nv_frame(gp2x_screen8, surface_width, surface_width, surface_height);
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
    if (!nv_is_enabled()) return;

    for (int p = 0; p < 4; p++) {
        uint32_t w = nv_pads(p);
        uint32_t changed = w ^ nv_joy_prev[p];
        if (!changed) continue;
        nv_joy_prev[p] = w;

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

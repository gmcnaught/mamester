/* _GNU_SOURCE before any header: cpu_set_t and pthread_setaffinity_np are GNU
 * extensions and glibc hides them otherwise. */
#define _GNU_SOURCE

/* host_present.c -- MISTER_THREADED_PRESENT: the DDR present on the second core.
 *
 * The HPS is a dual-core Cortex-A9 and MAME 0.78 is single-threaded, so one core
 * is idle while the other pays for the present. It is worth moving: galaga
 * measured 191.3 fps with no present against 149.0 with it
 * (docs/bench-results-2003plus.md, Task 7) -- 22% of frame time spent writing
 * DDR that the emulator could have spent emulating. The uncached window is the
 * reason it costs that much: 16-bit stores into it run at 24.8 MB/s and a memcpy
 * at 89 MB/s (nv_present.c's header), against several hundred MB/s for the
 * cached-to-cached copy this file pays instead.
 *
 * DEFAULT OFF, and off means NOTHING IN THIS FILE RUNS. host_video.c branches on
 * host_present_on and calls nv_present directly, so the disabled path has no
 * staging allocation, no copy, no mutex and no thread in it -- not even a call
 * into this translation unit. That is deliberate twice over: threading is meant
 * to be a per-driver carve-out for the drivers that cannot reach 60 fps any
 * other way (galaga throttled measures identical either way, 0 present-dropped),
 * so OFF is the common case; and it keeps every fps figure already measured
 * reproducible against a binary that also contains this file.
 *
 * The shape:
 *
 *   emulation thread            worker thread
 *   ----------------            -------------
 *   copy src -> slot[N]   \
 *   publish job (1 deep)   >--> nv_set_mode / nv_frame / nv_frame_repeat
 *   return to retro_run() /     (the ONLY thread that ever calls them)
 *
 * Three rules the rest of the file exists to keep:
 *
 *   1. The worker owns nv_present exclusively. Its globals -- nv_active,
 *      nv_frames, nv_stage, the DDR pointers -- have no locking of their own and
 *      are not going to grow any, because mame4all links the same file
 *      single-threaded.
 *   2. The source buffer cannot be handed over. frame_convert()
 *      (vendor/mame2003-plus/src/mame2003/video.c) rewrites video_buffer every
 *      frame, and the depth-15/32 bypass path (video.c:459) hands over the game
 *      bitmap itself, which the driver keeps drawing into. So the frame is
 *      copied into one of two staging slots the worker owns for the duration of
 *      the job.
 *   3. The emulation thread never waits for the worker except at a mode change
 *      and at shutdown. A queue that blocks when full would pace retro_run() to
 *      the present, which is the same mistake a blocking snd_pcm_writei made in
 *      Task 7 -- galaga read 60.7 fps instead of 136.1 because the benchmark was
 *      measuring ALSA (see host_audio.c). When the worker is still busy the
 *      OLDER frame is dropped, never the newer one, and the drops are counted
 *      and reported at exit.
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "nv_present.h"

enum { JOB_NONE = 0, JOB_FRAME, JOB_REPEAT, JOB_MODE };

struct pres_job {
    int       kind;
    int       slot;        /* JOB_FRAME: staging slot; -1 otherwise          */
    int       pitch;       /* JOB_FRAME: slot stride in bytes (compacted)    */
    int       w, h;        /* JOB_FRAME: source dimensions as nv_frame wants */
    double    hz;          /* JOB_MODE                                       */
    int       rot;         /* JOB_MODE                                       */
    nv_format fmt;         /* JOB_MODE                                       */
};

/* One mutex covers `job`, `busy_slot`, `in_flight`, `quit` and `drops`; nothing
 * here is hot enough to want anything finer. The frame copy itself is done
 * OUTSIDE it -- holding the lock across a 150 KB memcpy would serialise the two
 * threads and give back exactly what the thread was added to win. */
static pthread_mutex_t  lock  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   wake  = PTHREAD_COND_INITIALIZER;  /* emu -> worker    */
static pthread_cond_t   idle  = PTHREAD_COND_INITIALIZER;  /* worker -> emu    */
static pthread_t        worker;

static struct pres_job  job;              /* queue depth 1                    */
static int              busy_slot = -1;   /* slot the worker holds, -1 if none */
static int              in_flight;        /* worker is inside an nv_* call     */
static int              quit;
static unsigned long    drops;

/* Declared in host.h. Written once by host_present_init() before retro_init(),
 * read-only afterwards, so host_video.c's per-frame read needs no barrier. */
int                     host_present_on;
static nv_format        cur_fmt = NV_FMT_RGB565;  /* emulation thread only     */

/* Two slots: the emulator fills N+1 while the worker presents N. A third would
 * only add latency -- a frame queued behind another is a frame the display is
 * already too late for, which is why the queue drops instead of growing. */
static uint8_t         *slot_buf[2];
static size_t           slot_bytes[2];

static int fmt_bytes(nv_format f)
{
    switch (f) {
    case NV_FMT_PAL8:     return 1;
    case NV_FMT_XRGB8888: return 4;
    default:              return 2;   /* RGB565, 0RGB1555 */
    }
}

/* Caller holds the lock. */
static void wait_idle_locked(void)
{
    while (job.kind != JOB_NONE || in_flight)
        pthread_cond_wait(&idle, &lock);
}

static void *pres_worker(void *arg)
{
    (void)arg;

    pthread_mutex_lock(&lock);
    for (;;) {
        struct pres_job j;

        while (job.kind == JOB_NONE && !quit)
            pthread_cond_wait(&wake, &lock);
        if (job.kind == JOB_NONE)     /* quit, and the queue already drained */
            break;

        j = job;
        job.kind  = JOB_NONE;
        busy_slot = (j.kind == JOB_FRAME) ? j.slot : -1;
        in_flight = 1;
        pthread_mutex_unlock(&lock);

        /* Unlocked on purpose: this is the whole point of the thread. The
         * mutex release/acquire pair around it is also what orders the
         * emulation thread's writes into slot_buf[j.slot] before these reads. */
        switch (j.kind) {
        case JOB_FRAME:
            nv_frame(slot_buf[j.slot], j.pitch, j.w, j.h);
            break;
        case JOB_REPEAT:
            nv_frame_repeat();
            break;
        case JOB_MODE:
            nv_set_mode(j.w, j.h, j.hz, j.rot, j.fmt);
            break;
        default:
            break;
        }

        pthread_mutex_lock(&lock);
        busy_slot = -1;
        in_flight = 0;
        pthread_cond_signal(&idle);   /* only the emulation thread ever waits */
    }
    pthread_mutex_unlock(&lock);
    return NULL;
}

/* --- lifecycle ------------------------------------------------------------ */

void host_present_init(void)
{
    const char *e = getenv("MISTER_THREADED_PRESENT");

    /* Logged in BOTH directions, always. A run that hangs on the device is
     * diagnosed from its log, and the first question is which arm it was; a
     * silent default answers that with an absence, which is not an answer. */
    if (!e || strtol(e, NULL, 10) == 0) {
        fprintf(stderr, "MISTER-HOST: present on the emulation thread "
                        "(MISTER_THREADED_PRESENT off)\n");
        return;
    }

    /* With the present disabled (-nopresent / MISTER_NO_NATIVE) every nv_ call
     * returns immediately, so a worker would only add a staging copy to a run
     * whose entire purpose is to have no present cost in it. */
    if (!nv_is_enabled()) {
        fprintf(stderr, "MISTER-HOST: MISTER_THREADED_PRESENT ignored, "
                        "the present is disabled\n");
        return;
    }

    if (pthread_create(&worker, NULL, pres_worker, NULL) != 0) {
        fprintf(stderr, "MISTER-HOST: present worker failed to start, "
                        "presenting on the emulation thread\n");
        return;
    }

    /* Pin the two threads to opposite cores rather than leaving it to the
     * scheduler. The whole point of this path is that the DDR write happens on
     * the OTHER A9 while the emulator runs; if Linux puts them on the same
     * core the code is correct and buys nothing, and the difference is
     * invisible in the fps number. It is also not observable after the fact:
     * tick-based accounting undercounts a worker that runs in ~2 ms bursts and
     * sleeps between them, so /proc reported it using ~0 CPU while it was
     * demonstrably publishing 1200 frames. Pinning removes the question.
     *
     * Failures are non-fatal and reported: an unpinned run still presents
     * correctly, it just stops being a controlled measurement, and a benchmark
     * that silently lost its premise is worse than one that says so.
     * MISTER_PRESENT_AFFINITY=0 disables pinning. */
    if (!getenv("MISTER_PRESENT_AFFINITY") ||
        getenv("MISTER_PRESENT_AFFINITY")[0] != '0') {
        cpu_set_t emu, wrk;
        int rc_e, rc_w;
        CPU_ZERO(&emu); CPU_SET(0, &emu);
        CPU_ZERO(&wrk); CPU_SET(1, &wrk);
        rc_e = pthread_setaffinity_np(pthread_self(), sizeof emu, &emu);
        rc_w = pthread_setaffinity_np(worker,         sizeof wrk, &wrk);
        if (rc_e || rc_w)
            fprintf(stderr, "MISTER-HOST: WARNING affinity not set "
                            "(emu=%d worker=%d); the two threads may share a "
                            "core and the threaded arm means nothing\n",
                    rc_e, rc_w);
        else
            fprintf(stderr, "MISTER-HOST: emulation pinned to cpu0, "
                            "present worker to cpu1\n");
    }

    host_present_on = 1;
    fprintf(stderr, "MISTER-HOST: threaded present (worker owns nv_present)\n");
}

void host_present_drain(void)
{
    if (!host_present_on) return;
    pthread_mutex_lock(&lock);
    wait_idle_locked();
    pthread_mutex_unlock(&lock);
}

/* Must run before nv_close() unmaps /dev/mem: the worker dereferences that
 * mapping inside nv_frame() and an unmap under it is a segfault at exit, not a
 * hang. Clearing host_present_on after the join is what makes a late frame --
 * retro_unload_game() is entitled to present one -- go straight to nv_present
 * from host_video.c rather than into a queue with no worker behind it. */
void host_present_stop(void)
{
    if (!host_present_on) return;

    pthread_mutex_lock(&lock);
    quit = 1;
    pthread_cond_signal(&wake);
    pthread_mutex_unlock(&lock);
    pthread_join(worker, NULL);
    host_present_on = 0;

    free(slot_buf[0]); slot_buf[0] = NULL; slot_bytes[0] = 0;
    free(slot_buf[1]); slot_buf[1] = NULL; slot_bytes[1] = 0;
}

unsigned long host_present_drops(void) { return drops; }

/* --- submission ----------------------------------------------------------- */

/* nv_set_mode() reallocates nv_stage and clears both DDR buffers, so a frame
 * queued against the old geometry must not be presented after it. This is one
 * of the two places the emulation thread is allowed to block: a mode change
 * happens once at load and then only when a driver switches resolution, so the
 * stall does not show up in an fps figure the way a per-frame one would. */
void host_present_mode(int width, int height, double refresh_hz, int rot,
                       nv_format fmt)
{
    cur_fmt = fmt;      /* emulation thread only: it sizes the staging copy */

    pthread_mutex_lock(&lock);
    wait_idle_locked();
    job.kind = JOB_MODE;
    job.slot = -1;
    job.w    = width;
    job.h    = height;
    job.hz   = refresh_hz;
    job.rot  = rot;
    job.fmt  = fmt;
    pthread_cond_signal(&wake);
    wait_idle_locked();   /* the modeline must be live before the next frame */
    pthread_mutex_unlock(&lock);
}

void host_present_frame(const void *src, int pitch_bytes, int src_w, int src_h)
{
    int    slot, y;
    size_t row, need, rowcopy;

    if (!src || src_w <= 0 || src_h <= 0 || pitch_bytes <= 0) return;

    /* The slot is compacted to width*bpp rather than carrying the source's own
     * stride: the copy has to touch every visible pixel anyway, and a compacted
     * slot is what keeps nv_frame()'s single-memcpy RGB565 fast path
     * (nv_present.c:265, pitch_bytes == cw*2) reachable for the bypass drivers
     * that hand over a padded stride -- crospang reports pitch 1088 for a
     * 320-wide frame. */
    row  = (size_t)src_w * fmt_bytes(cur_fmt);
    need = row * (size_t)src_h;

    pthread_mutex_lock(&lock);
    if (job.kind == JOB_FRAME) {
        /* The worker is still on the previous frame. Overwrite the queued one
         * in place -- it is older than what is being handed over now, and the
         * worker cannot be holding it (a queued slot is by construction not
         * busy_slot). Dropping beats blocking; see rule 3 in the header. */
        slot     = job.slot;
        job.kind = JOB_NONE;
        drops++;
    } else {
        if (job.kind == JOB_REPEAT) { job.kind = JOB_NONE; drops++; }
        /* Never busy_slot, and safe when it is -1 because then the worker holds
         * nothing. Only this thread picks slots, so the choice cannot be raced:
         * the slot stays private until the job below publishes it. */
        slot = (busy_slot == 0) ? 1 : 0;
    }
    pthread_mutex_unlock(&lock);

    if (slot_bytes[slot] < need) {
        /* Grow-only. The trigger is a geometry or format change, so this runs
         * once per mode rather than per frame; the contents are overwritten
         * immediately, so there is nothing to preserve across the realloc. */
        free(slot_buf[slot]);
        slot_buf[slot]   = (uint8_t *)malloc(need);
        slot_bytes[slot] = slot_buf[slot] ? need : 0;
        if (!slot_buf[slot]) { drops++; return; }
    }

    /* A core may report a stride shorter than width*bpp only if it is lying, but
     * believing it here would read off the end of its bitmap. */
    rowcopy = ((size_t)pitch_bytes < row) ? (size_t)pitch_bytes : row;
    if ((size_t)pitch_bytes == row) {
        memcpy(slot_buf[slot], src, need);
    } else {
        for (y = 0; y < src_h; y++)
            memcpy(slot_buf[slot] + (size_t)y * row,
                   (const uint8_t *)src + (size_t)y * pitch_bytes, rowcopy);
    }

    pthread_mutex_lock(&lock);
    job.kind  = JOB_FRAME;
    job.slot  = slot;
    job.pitch = (int)row;
    job.w     = src_w;
    job.h     = src_h;
    pthread_cond_signal(&wake);
    pthread_mutex_unlock(&lock);
}

void host_present_repeat(void)
{
    pthread_mutex_lock(&lock);
    if (job.kind == JOB_FRAME) {
        /* "Show the previous frame again" is older news than a real frame that
         * has not been presented yet, so the repeat is dropped instead of
         * replacing it. Order is never at risk: the queue is one deep and the
         * worker takes jobs in submission order, so a repeat can never overtake
         * a real frame and republish a stale buffer index. */
        drops++;
    } else {
        if (job.kind == JOB_REPEAT) drops++;   /* coalesced; the counter still
                                                * advances when it runs, which
                                                * is all the watchdog wants */
        job.kind = JOB_REPEAT;
        job.slot = -1;
        pthread_cond_signal(&wake);
    }
    pthread_mutex_unlock(&lock);
}

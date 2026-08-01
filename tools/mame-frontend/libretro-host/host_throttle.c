/* host_throttle.c -- pace the emulation loop at the driver's native rate.
 *
 * Unthrottled is a benchmark mode, not a play mode. The rate is the DRIVER's,
 * not 60: mk is 53.2 Hz, galaga 60.61, gng 59.59 (0.37b5) / 60.00 (0.78). The
 * modeline published by nv_set_mode() already carries that rate, so pacing to
 * anything else would beat against the display.
 *
 * Two device-specific traps, both already paid for elsewhere in this tree:
 *
 *   armhf `long` is 32 bits, so a nanosecond accumulator in a long wraps after
 *   2.1 seconds. Everything here is int64_t.
 *
 *   The deadline is advanced by a fixed period from the PREVIOUS deadline, not
 *   from "now". Sleeping period-from-now accumulates every scheduling overshoot
 *   permanently, so the emulator drifts slow and never catches up.
 *
 * TIMER_ABSTIME against CLOCK_MONOTONIC is used rather than a relative sleep:
 * it eliminates the read-then-sleep race, and it is what keeps the deadline
 * arithmetic honest when a frame runs long.
 */

#include <errno.h>
#include <stdio.h>
#include <time.h>

#include "host.h"

static int64_t period_ns;
static int64_t deadline_ns;
static int     armed;
static unsigned long late_frames;

static int64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

void host_throttle_start(double fps)
{
    if (!(fps > 1.0)) fps = 60.0;
    period_ns   = (int64_t)(1e9 / fps + 0.5);
    deadline_ns = now_ns() + period_ns;
    armed       = 1;
    late_frames = 0;
    fprintf(stderr, "MISTER-HOST: throttle %.4f Hz (%lld us/frame)\n",
            fps, (long long)(period_ns / 1000));
}

void host_throttle_wait(void)
{
    struct timespec ts;
    int64_t now;

    if (!armed) return;

    now = now_ns();
    if (now >= deadline_ns) {
        /* The frame took longer than its period. Counting these is the cheap
         * way to tell "runs at native rate" from "runs as fast as it can and
         * that happens to be close". */
        late_frames++;
        /* Resynchronise rather than trying to make up an unbounded backlog: a
         * driver that is genuinely too slow would otherwise spend every
         * subsequent frame in a no-op sleep with the deadline receding. */
        if (now - deadline_ns > 4 * period_ns)
            deadline_ns = now;
        deadline_ns += period_ns;
        return;
    }

    ts.tv_sec  = (time_t)(deadline_ns / 1000000000LL);
    ts.tv_nsec = (long)  (deadline_ns % 1000000000LL);
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR)
        ;
    deadline_ns += period_ns;
}

unsigned long host_throttle_late(void) { return late_frames; }

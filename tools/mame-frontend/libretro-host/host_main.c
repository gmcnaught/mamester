/* host_main.c -- argv, libretro lifecycle, and the benchmark counter.
 *
 *   mame2003 <setname> [-rompath DIR] [-frames N]
 *                     [-nopresent] [-nothrottle] [-throttle] [-nosound]
 *
 * Video goes out through the MiSTer DDR present path (host_video.c), pads come
 * back from the FPGA reader (host_input.c), audio goes to MiSTer's ALSA chain
 * (host_audio.c) and the loop is paced at the driver's native rate
 * (host_throttle.c).
 *
 * -frames means benchmark, and a benchmark is UNPACED by default -- pacing it
 * would measure the throttle. -throttle overrides that when the point is to
 * check a played run holds its rate. -nosound silences the OUTPUT only; the
 * sound chips stay emulated, because that CPU cost is part of what is measured.
 *
 * -nopresent sets MISTER_NO_NATIVE, which makes nv_open() a no-op present. The
 * gap between a run with and without it is the present cost; a -nopresent
 * figure is comparable to mame4all run the same way and to nothing else.
 *
 * MISTER_THREADED_PRESENT=1 moves that cost to the second A9 core
 * (host_present.c). Off by default, and off means host_video.c calls nv_present
 * directly with nothing in between; the exit line's `present-dropped` count is
 * how much of the threaded arm's speed came from not presenting at all.
 *
 * MISTER_LOOP_TRACE=N prints a flushed heartbeat every N frames, for localising
 * a hang that only reproduces on the device.
 *
 * The fps line format (`MISTER-BENCH fps=%.1f`) matches mame4all's so that
 * gap-triage.sh's parser works unchanged for both engines.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "host.h"
#include "nv_present.h"

/* --- helpers ------------------------------------------------------------- */

/* armhf: `long` is 32 bits, so a nanosecond accumulator in a long overflows
 * after 2.1 seconds. Everything time-related here is int64_t. */
static int64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s <setname> [-rompath DIR] [-frames N]\n"
        "                    [-nopresent] [-nothrottle] [-throttle] [-nosound]\n"
        "  -rompath DIR   where <setname>.zip lives (default: roms2003)\n"
        "  -frames N      stop after N emulated frames (default: MISTER_BENCH_FRAMES,\n"
        "                 else run forever). Implies unthrottled.\n"
        "  -nopresent     bypass the DDR present (sets MISTER_NO_NATIVE)\n"
        "  -nothrottle    run flat out even without -frames\n"
        "  -throttle      pace even WITH -frames, to measure a played run\n"
        "  -nosound       no ALSA output; the sound chips are still emulated\n",
        argv0);
}

int main(int argc, char **argv)
{
    const char *setname = NULL;
    const char *rompath = "roms2003";
    const char *env_frames;
    unsigned long frame_limit = 0;
    unsigned long trace_every = 0;
    char rom_path[1024];
    struct retro_system_info sysinfo;
    struct retro_system_av_info av;
    unsigned long f;
    int64_t t0, t1;
    double secs, fps;
    int i;
    int no_audio = 0, no_throttle = 0, force_throttle = 0, throttle = 0;
    int audio_ok = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-rompath") == 0 && i + 1 < argc) {
            rompath = argv[++i];
        } else if (strcmp(argv[i], "-frames") == 0 && i + 1 < argc) {
            frame_limit = strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "-nopresent") == 0) {
            /* nv_open() reads this; setting the variable rather than passing a
             * flag keeps the knob identical for both engines. */
            setenv("MISTER_NO_NATIVE", "1", 1);
        } else if (strcmp(argv[i], "-nothrottle") == 0) {
            no_throttle = 1;
        } else if (strcmp(argv[i], "-throttle") == 0) {
            /* Pace even with -frames. Without this there is no way to measure
             * a throttled run at all: -frames means benchmark, and a benchmark
             * is unpaced by default. */
            force_throttle = 1;
        } else if (strcmp(argv[i], "-nosound") == 0) {
            /* Silences the OUTPUT only. The sound chips stay emulated, because
             * that CPU cost is part of what is being measured -- disabling
             * them would benchmark a different emulator. */
            no_audio = 1;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "MISTER-HOST: unknown option %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        } else if (!setname) {
            setname = argv[i];
        } else {
            fprintf(stderr, "MISTER-HOST: unexpected argument %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    if (!setname) { usage(argv[0]); return 1; }

    /* MISTER_BENCH_FRAMES is what the existing triage scripts set. -frames
     * wins when both are given. */
    if (!frame_limit && (env_frames = getenv("MISTER_BENCH_FRAMES")) != NULL)
        frame_limit = strtoul(env_frames, NULL, 10);

    /* MISTER_LOOP_TRACE=N: a flushed heartbeat every N frames. A run that hangs
     * inside the loop only does it on the device, so the log has to be able to
     * say where it stopped: the last MISTER-TRACE line names the frame the loop
     * did not get past, and the counters beside it say whether that frame had
     * already reached the present. One compare per frame when unset. */
    if ((env_frames = getenv("MISTER_LOOP_TRACE")) != NULL)
        trace_every = strtoul(env_frames, NULL, 10);

    snprintf(rom_path, sizeof rom_path, "%s/%s.zip", rompath, setname);

    /* The core derives the setname from the path basename minus extension and
     * requires the file to exist (mame2003.c:250 path_is_valid). */
    retro_get_system_info(&sysinfo);
    if (!sysinfo.need_fullpath) {
        fprintf(stderr, "MISTER-HOST: core wants the ROM in memory, not a path "
                        "-- this host only passes paths\n");
        return 2;
    }
    fprintf(stderr, "MISTER-HOST: %s %s, content %s\n",
            sysinfo.library_name,
            sysinfo.library_version && *sysinfo.library_version
                ? sysinfo.library_version : "(no git version)",
            rom_path);

    /* System and save directories: the emulator chdir()s nowhere, so these are
     * relative to the cwd the launcher used -- /media/fat/games/mame on the
     * device. Left as "." rather than an absolute path so a bench run out of a
     * scratch directory keeps its own nvram. */
    host_env_init(".", ".");

    /* Before retro_init: the core calls SET_PIXEL_FORMAT and can present its
     * first frame from inside retro_load_game, so the DDR mapping has to be
     * live by then. nv_open() fails gracefully off-device and honours
     * MISTER_NO_NATIVE, so this is safe everywhere. */
    if (!nv_open())
        fprintf(stderr, "MISTER-HOST: present disabled "
                        "(MISTER_NO_NATIVE, or /dev/mem unavailable)\n");

    /* MISTER_THREADED_PRESENT=1 moves the DDR write to the idle second A9 core.
     * Off by default, and it must be started here: the core is allowed to
     * present its first frame from inside retro_load_game(). */
    host_present_init();

    retro_set_environment(host_environment);
    retro_set_video_refresh(host_video_refresh);
    retro_set_audio_sample_batch(host_audio_batch);
    retro_set_audio_sample(host_audio_sample);
    retro_set_input_poll(host_input_poll);
    retro_set_input_state(host_input_state);

    retro_init();

    {
        struct retro_game_info game;
        memset(&game, 0, sizeof game);
        game.path = rom_path;
        if (!retro_load_game(&game)) {
            fprintf(stderr, "MISTER-HOST: retro_load_game failed for %s\n",
                    rom_path);
            retro_deinit();
            return 2;
        }
    }

    retro_get_system_av_info(&av);
    /* The driver's native rate -- mk is 53.2, not 60 -- which the modeline
     * needs. It is not knowable before the game loads, so the first frame
     * after this publishes the timing. */
    host_video_set_refresh(av.timing.fps);
    fprintf(stderr, "MISTER-HOST: %ux%u @ %.4f Hz, %.0f Hz audio, present=%s\n",
            av.geometry.base_width, av.geometry.base_height,
            av.timing.fps, av.timing.sample_rate,
            nv_is_enabled() ? "DDR" : "off");

    /* All four ports, unconditionally: the core only runs its control setup on
     * the LAST port it is told about (mame2003.c:757 run_update), so telling it
     * about fewer ports than the driver supports leaves the rest undescribed.
     * nv_pads() serves four words regardless. */
    for (i = 0; i < 4; i++)
        retro_set_controller_port_device(i, RETRO_DEVICE_JOYPAD);

    /* A frame limit means benchmark, and a benchmark must not be paced -- the
     * whole point is the ceiling. -nothrottle forces it off for an unlimited
     * run too. Decided BEFORE the audio open, because it also decides whether
     * the ALSA writes block, and a blocking write would pace an unthrottled
     * benchmark to the audio clock. */
    throttle = force_throttle || (!frame_limit && !no_throttle);

    audio_ok = !no_audio && host_audio_open((unsigned)av.timing.sample_rate,
                                            av.timing.fps, !throttle) == 0;
    if (!no_audio && !audio_ok)
        fprintf(stderr, "MISTER-HOST: continuing without audio output "
                        "(the sound chips are still emulated)\n");

    /* EXACTLY ONE CLOCK. In a throttled run the ALSA write is blocking, so
     * audio already paces the loop; running the timer as well means two clocks
     * fighting over the same loop. Measured: galaga throttled for 10800 frames
     * held 60.57 fps with 0 underruns and 0 dropped, yet the timer called 8117
     * of those frames "late" -- it was reporting the sound card running 0.07%
     * slower than the nominal 60.6061 Hz, not a fault. The timer only takes
     * over when there is no audio to pace against. */
    if (throttle && audio_ok) {
        throttle = 0;
        fprintf(stderr, "MISTER-HOST: paced by the audio clock\n");
    }
    if (throttle) host_throttle_start(av.timing.fps);
    fflush(stderr);

    t0 = now_ns();
    for (f = 0; !frame_limit || f < frame_limit; f++) {
        if (trace_every && f % trace_every == 0) {
            /* nv_frame_count() is the worker's counter when threading is on, so
             * this read is unsynchronised. It is a diagnostic line: a stale
             * value costs nothing, and taking the present mutex here would put
             * the emulation thread behind the worker, which is the one thing
             * the design refuses to do. */
            fprintf(stderr, "MISTER-TRACE frame=%lu presented=%lu duped=%lu "
                            "published=%lu audio=%lu\n",
                    f, host_video_shown(), host_video_duped(),
                    nv_frame_count(), host_audio_frames());
            fflush(stderr);
        }
        retro_run();
        if (throttle) host_throttle_wait();
    }
    t1 = now_ns();

    /* AFTER the clock stops. The figure wanted is the rate the emulator ran at,
     * and with a threaded present the worker is allowed to still be finishing
     * the last frame or two -- charging that tail to the loop would report the
     * present's rate instead. Draining here also makes nv_frame_count() below a
     * settled number rather than a read racing the worker. */
    host_present_drain();

    secs = (double)(t1 - t0) / 1e9;
    fps  = secs > 0.0 ? (double)f / secs : 0.0;

    /* Emulated frames, not presented frames: the core only calls video_cb when
     * something changed (video.c:430), so counting presents would undercount a
     * static screen and report a speed the emulator is not running at. */
    printf("MISTER-BENCH fps=%.1f\n", fps);
    fprintf(stderr,
            "MISTER-HOST: %lu frames in %.2fs, %lu presented, %lu duped, "
            "%lu published, %lu present-dropped, %lu audio frames, "
            "%lu underruns, %lu dropped, %lu late, present=%s\n",
            f, secs, host_video_shown(), host_video_duped(), nv_frame_count(),
            host_present_drops(), host_audio_frames(), host_audio_underruns(),
            host_audio_dropped(), host_throttle_late(),
            /* Which DDR mapping this run measured. A/B tables that do not
             * record this are unreadable a week later. */
            nv_is_write_combined() ? "write-combined" : "strongly-ordered");
    fflush(stdout);

    retro_unload_game();
    retro_deinit();
    /* Before nv_close(): the worker dereferences the /dev/mem mapping. */
    host_present_stop();
    host_audio_close();
    nv_close();
    return 0;
}

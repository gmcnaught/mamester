/* host_main.c -- argv, libretro lifecycle, and the benchmark counter.
 *
 *   mame2003 <setname> [-rompath DIR] [-frames N] [-nopresent] [-nothrottle]
 *
 * This build deliberately has no present, no input and no audio output: it is
 * the emulator and nothing else, so the fps it reports is 0.78's raw cost on
 * this silicon. Compare it only against mame4all run the same way
 * (MISTER_NO_NATIVE=1), never against the Stage 8 bands, which were measured
 * with a core loaded and the DDR present live.
 *
 * The sound CHIPS are still emulated -- only the ALSA write is missing, worth
 * 1-3%. Turning sound off would measure a different emulator.
 *
 * The video/audio/input stubs below are replaced in Tasks 6 and 7; the fps line
 * format (`MISTER-BENCH fps=%.1f`) matches mame4all's so that gap-triage.sh's
 * parser works unchanged for both engines.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "host.h"

/* --- state the video hooks record, for the startup summary --------------- */
static unsigned      cur_fmt        = RETRO_PIXEL_FORMAT_0RGB1555; /* libretro's
                                      * documented default when a core never
                                      * calls SET_PIXEL_FORMAT */
static unsigned      cur_rotation   = 0;
static unsigned      cur_width      = 0;
static unsigned      cur_height     = 0;
static unsigned long frames_shown   = 0;   /* video_cb calls with a bitmap   */
static unsigned long frames_duped   = 0;   /* video_cb calls with NULL       */
static unsigned long audio_frames   = 0;   /* stereo sample pairs            */

static const char *fmt_name(unsigned f)
{
    switch (f) {
    case RETRO_PIXEL_FORMAT_0RGB1555: return "0RGB1555";
    case RETRO_PIXEL_FORMAT_XRGB8888: return "XRGB8888";
    case RETRO_PIXEL_FORMAT_RGB565:   return "RGB565";
    default:                          return "unknown";
    }
}

/* --- video stubs (Task 6 replaces these with host_video.c) --------------- */

void host_set_pixel_format(unsigned fmt) { cur_fmt = fmt; }
void host_set_rotation(unsigned rot)     { cur_rotation = rot; }

void host_geometry_changed(const struct retro_game_geometry *geom)
{
    cur_width  = geom->base_width;
    cur_height = geom->base_height;
}

void host_video_refresh(const void *data, unsigned width, unsigned height,
                        size_t pitch)
{
    (void)pitch;
    /* A NULL frame means "repeat the last one". It is counted separately
     * because Task 6 must still ring the DDR doorbell for it -- the reader's
     * stale-frame watchdog blanks the screen if the counter stops advancing. */
    if (!data) { frames_duped++; return; }
    cur_width  = width;
    cur_height = height;
    frames_shown++;
}

/* --- audio and input stubs (Task 7) -------------------------------------- */

size_t host_audio_batch(const int16_t *data, size_t frames)
{
    (void)data;
    audio_frames += frames;
    return frames;
}

void host_audio_sample(int16_t left, int16_t right)
{
    (void)left; (void)right;
    audio_frames++;
}

void host_input_poll(void) { }

int16_t host_input_state(unsigned port, unsigned device, unsigned index,
                         unsigned id)
{
    (void)port; (void)device; (void)index; (void)id;
    return 0;
}

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
        "usage: %s <setname> [-rompath DIR] [-frames N] [-nopresent] [-nothrottle]\n"
        "  -rompath DIR   where <setname>.zip lives (default: roms2003)\n"
        "  -frames N      stop after N emulated frames (default: MISTER_BENCH_FRAMES,\n"
        "                 else run forever)\n"
        "  -nopresent     accepted; this build has no present path to disable\n"
        "  -nothrottle    accepted; this build never throttles\n",
        argv0);
}

int main(int argc, char **argv)
{
    const char *setname = NULL;
    const char *rompath = "roms2003";
    const char *env_frames;
    unsigned long frame_limit = 0;
    char rom_path[1024];
    struct retro_system_info sysinfo;
    struct retro_system_av_info av;
    unsigned long f;
    int64_t t0, t1;
    double secs, fps;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-rompath") == 0 && i + 1 < argc) {
            rompath = argv[++i];
        } else if (strcmp(argv[i], "-frames") == 0 && i + 1 < argc) {
            frame_limit = strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "-nopresent") == 0 ||
                   strcmp(argv[i], "-nothrottle") == 0) {
            /* accepted for harness compatibility; see usage() */
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
    fprintf(stderr, "MISTER-HOST: %ux%u @ %.4f Hz, %.0f Hz audio, %s, rot=%u\n",
            av.geometry.base_width, av.geometry.base_height,
            av.timing.fps, av.timing.sample_rate,
            fmt_name(cur_fmt), cur_rotation);
    fprintf(stderr, "MISTER-HOST: present=stub (Task 5 build: emulator only)\n");
    fflush(stderr);

    t0 = now_ns();
    for (f = 0; !frame_limit || f < frame_limit; f++)
        retro_run();
    t1 = now_ns();

    secs = (double)(t1 - t0) / 1e9;
    fps  = secs > 0.0 ? (double)f / secs : 0.0;

    /* Emulated frames, not presented frames: the core only calls video_cb when
     * something changed (video.c:430), so counting presents would undercount a
     * static screen and report a speed the emulator is not running at. */
    printf("MISTER-BENCH fps=%.1f\n", fps);
    fprintf(stderr,
            "MISTER-HOST: %lu frames in %.2fs, %lu presented, %lu duped, "
            "%lu audio frames (%.0f Hz effective)\n",
            f, secs, frames_shown, frames_duped, audio_frames,
            secs > 0.0 ? (double)audio_frames / secs : 0.0);
    fflush(stdout);

    retro_unload_game();
    retro_deinit();
    return 0;
}

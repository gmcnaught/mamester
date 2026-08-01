/* host_env.c -- the RETRO_ENVIRONMENT callback for MAMESTer's libretro host.
 *
 * Every command handled here was found by grepping the core, not assumed:
 *
 *   grep -rho 'environ_cb *( *RETRO_ENVIRONMENT_[A-Z_0-9]*' \
 *       vendor/mame2003-plus/src/mame2003/
 *
 * That is 25 distinct commands as of submodule d6bf36f6. Anything else lands in
 * default: and is refused -- and logged when MISTER_HOST_DEBUG=1, because a core
 * that starts issuing a new command should not do it invisibly.
 *
 * Two responses are load-bearing rather than boilerplate:
 *
 *   GET_LOG_INTERFACE   mame2003.c:185 stores log_cb from it, and
 *                       retro_load_game() calls log_cb() BEFORE any other
 *                       guard. Refuse this and log_cb stays NULL, so a bad
 *                       romset path is a segfault instead of a message.
 *   GET_CORE_OPTIONS_VERSION  answering 0 sends the core down the legacy
 *                       SET_VARIABLES path (core_options.c:1539), which means
 *                       the v1/v2 option structures never have to be parsed
 *                       here. GET_VARIABLE below is what actually sets options.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"

static const char *host_system_dir = NULL;
static const char *host_save_dir   = NULL;
static int         host_debug      = 0;

/* Pinned so that every benchmark arm measures the same emulator. Keys and legal
 * values were read out of vendor/mame2003-plus/src/mame2003/core_options.c
 * (APPNAME is "mame2003-plus", mame2003.h:62) -- an unrecognised key or an
 * illegal value falls through to the core's own default SILENTLY, so a table
 * written from naming convention looks pinned and is not.
 *
 * Two entries are judgement calls, recorded here and in the results doc:
 *
 *   sample_rate = 44100, against the 48000 default. mame4all's bench runs at
 *   44100 (Stage 6: period = sample_rate/refresh, 735 at 44100/60). Leaving
 *   this at 48000 would have the two engines doing measurably different amounts
 *   of sound work, and Stage 8 measured sound at a 1.07-1.31x CPU cost -- large
 *   enough to swamp a real engine difference. MiSTer's only ALSA card is 48 kHz
 *   stereo and the plug layer resamples either way.
 *
 *   use_samples = disabled, against the enabled default. The reference
 *   collection ships 72 sample sets; loading them changes both CPU cost and
 *   what is audible, and mame4all's bench did not use them.
 *
 * Verified core defaults, for the record: frameskip=disabled, sample_rate=48000,
 * cpu_clock_scale=default, skip_disclaimer=disabled, skip_warnings=disabled,
 * use_samples=enabled, autosave_hiscore=default, nvram_bootstraps=enabled. */
static const struct { const char *key; const char *value; } host_options[] = {
    { "mame2003-plus_frameskip",        "disabled" }, /* "0" is NOT legal; the
                                                       * set is disabled,1..11,
                                                       * auto,auto_aggressive,
                                                       * auto_max            */
    { "mame2003-plus_sample_rate",      "44100"    },
    { "mame2003-plus_cpu_clock_scale",  "default"  },
    { "mame2003-plus_skip_disclaimer",  "enabled"  },
    { "mame2003-plus_skip_warnings",    "enabled"  },
    { "mame2003-plus_use_samples",      "disabled" },
    { "mame2003-plus_autosave_hiscore", "disabled" },
    { "mame2003-plus_nvram_bootstraps", "disabled" },
    { NULL, NULL }
};

static void host_log(enum retro_log_level level, const char *fmt, ...)
{
    static const char *const tag[] = { "DBG", "INF", "WRN", "ERR" };
    va_list ap;

    if (level == RETRO_LOG_DEBUG && !host_debug)
        return;

    fprintf(stderr, "[%s] ", (unsigned)level < 4 ? tag[level] : "???");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fflush(stderr);
}

void host_env_init(const char *system_dir, const char *save_dir)
{
    const char *dbg = getenv("MISTER_HOST_DEBUG");

    host_system_dir = system_dir;
    host_save_dir   = save_dir;
    host_debug      = (dbg && *dbg == '1');
}

bool host_environment(unsigned cmd, void *data)
{
    switch (cmd) {

    /* --- must be answered ------------------------------------------------ */

    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        ((struct retro_log_callback *)data)->log = host_log;
        return true;

    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
        *(unsigned *)data = 0;
        return true;

    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable *var = (struct retro_variable *)data;
        int i;
        var->value = NULL;
        for (i = 0; host_options[i].key; i++) {
            if (strcmp(var->key, host_options[i].key) == 0) {
                var->value = host_options[i].value;
                return true;
            }
        }
        /* Not pinned: the core keeps its own default. Visible under
         * MISTER_HOST_DEBUG=1 so the unpinned set can be audited. */
        host_log(RETRO_LOG_DEBUG, "env: unpinned option %s\n", var->key);
        return false;
    }

    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool *)data = false;
        return true;

    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        *(const char **)data = host_system_dir;
        return host_system_dir != NULL;

    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char **)data = host_save_dir;
        return host_save_dir != NULL;

    /* --- video: recorded now, presented in Task 6 ------------------------ */

    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        host_set_pixel_format(*(const enum retro_pixel_format *)data);
        return true;

    /* REFUSED ON PURPOSE, and this is the single most consequential answer in
     * this file after the log interface.
     *
     * mame2003_video_init_orientation() (src/mame2003/video.c:127) tests this
     * call. Accept it and the core hands over an UNROTATED bitmap, expecting
     * the frontend to rotate -- which the DDR reader cannot do, so every
     * vertical game would come out sideways. Refuse it and the core takes the
     * `Mame will rotate internally` branch: video_swap_xy is set and
     * frame_convert() transposes inside the pixel loop it is already running.
     * That is also exactly what mame4all does (nv_present.h: "mame4all rotates
     * its own bitmap and passes 0"), so both engines present pre-rotated
     * frames and the comparison stays honest.
     *
     * The alternative -- transpose in host_video.c -- would add a full
     * cache-hostile pass over every frame of every vertical game to redo work
     * the emulator is already positioned to do for free. */
    case RETRO_ENVIRONMENT_SET_ROTATION:
        return false;

    case RETRO_ENVIRONMENT_SET_GEOMETRY:
        host_geometry_changed((const struct retro_game_geometry *)data);
        return true;

    /* --- accepted and ignored -------------------------------------------- */

    case RETRO_ENVIRONMENT_SET_MESSAGE:
        host_log(RETRO_LOG_INFO, "core message: %s\n",
                 ((const struct retro_message *)data)->msg);
        return true;

    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
    case RETRO_ENVIRONMENT_SET_VARIABLES:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL:
        return true;

    /* --- optional interfaces this host does not provide ------------------- *
     * All four are guarded at the call site: the core presets the struct or
     * checks the return, so refusing them is the documented path rather than
     * a gamble. GET_VFS_INTERFACE refused means libretro-common's filestream
     * uses plain stdio, which is what is wanted on the device anyway.        */

    case RETRO_ENVIRONMENT_GET_PERF_INTERFACE:
    case RETRO_ENVIRONMENT_GET_LED_INTERFACE:
    case RETRO_ENVIRONMENT_GET_VFS_INTERFACE:
    case RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK:
    case RETRO_ENVIRONMENT_GET_INPUT_DEVICE_CAPABILITIES:
    case RETRO_ENVIRONMENT_GET_GAME_INFO_EXT:
    case RETRO_ENVIRONMENT_GET_LANGUAGE:
        return false;

    default:
        host_log(RETRO_LOG_DEBUG, "env: unhandled cmd %u\n", cmd);
        return false;
    }
}

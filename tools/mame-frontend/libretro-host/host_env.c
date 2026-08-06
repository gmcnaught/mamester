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

/* mame2003-plus_cyclone_mode selects the hand-written ARM CPU cores at RUNTIME
 * (core_options.c: default / disabled / Cyclone / DrZ80 / Cyclone+DrZ80 /
 * DrZ80(snd) / Cyclone+DrZ80(snd)). It is env-settable rather than pinned to a
 * constant because on/off is a benchmark arm -- and because mame4all runs with
 * BOTH cores disabled (0002-default-asm-cores-off.patch: Cyclone segfaults on
 * entry for every 68000 driver, DrZ80 crashes in DrZ80Run), so leaving 2003-plus
 * on its own default puts an untested difference inside the comparison. */
static char host_cyclone_mode[32] = "default";

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
#if defined(MAMESTER_ENGINE_LRMAME)
    /* Current MAME (0.289) namespaces its options `mame_*`, not
     * `mame2003-plus_*`, and pins NOTHING yet -- deliberately. An unrecognised
     * key falls through to the core's own default SILENTLY (the reason the
     * table below is annotated so heavily), so a table written from naming
     * convention would look pinned and not be. Nothing goes in here until the
     * gate build proves the engine runs and each key has been read out of
     * src/osd/libretro/libretro-internal/libretro_core_options.h.
     *
     * This is not a gap in the meantime: host_capture_defaults() snapshots the
     * core's own defaults from SET_VARIABLES, so GET_VARIABLE still answers for
     * every option. The engine simply runs at its own defaults. */
#else
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

    /* The next two are not preferences, they are statements about what this
     * host HAS. Left at their core defaults the core polls devices that cannot
     * exist here, once per input code per frame:
     *
     *   input_interface defaults to "simultaneous", which makes every
     *   osd_is_key_pressed() an input_cb(RETRO_DEVICE_KEYBOARD) round trip
     *   (mame2003.c:1467). "retropad" returns 0 before the call. There is no
     *   keyboard on the MiSTer pad path, so both answers are 0 and only the
     *   pinned one is free.
     *
     *   xy_device defaults to "mouse", so every joycode that is not a retropad
     *   code falls through to a "mouse" then a "lightgun" get_retro_code()
     *   lookup plus their input_cb calls (mame2003.c:1224-1246). nv_pads()
     *   serves twelve digital bits and nothing else; "disabled" skips the
     *   block. Crosshair drawing is untouched -- that is crosshair_enabled,
     *   which is deliberately NOT pinned because it changes what is on screen.
     */
    { "mame2003-plus_input_interface",  "retropad" },
    { "mame2003-plus_xy_device",        "disabled" },
#endif
    { NULL, NULL }
};

/* Defaults captured from the core's own SET_VARIABLES, so GET_VARIABLE can
 * answer for EVERY option rather than only the pinned ones.
 *
 * This is not an optimisation, it is the libretro contract, and getting it
 * wrong corrupts the picture rather than failing loudly. update_variables()
 * (core_options.c:977) is:
 *
 *     if (environ_cb(GET_VARIABLE, &var) && !string_is_empty(var.value))
 *         switch (index) { case OPT_BRIGHTNESS: options.brightness = ...;
 *                          palette_set_global_brightness(...); break; ... }
 *
 * There is NO else. Answer false and the corresponding options.* field is never
 * assigned and its palette_set_global_* call never happens -- so brightness and
 * gamma keep whatever the options struct held, and the palette is built wrong.
 * Symptom: frames that are almost entirely black with a handful of colours, and
 * wrong colours where anything does appear. RetroArch never hits this because a
 * frontend is expected to own every option value and always supply one.
 *
 * SET_VARIABLES sends "<description>; <default>|<alt>|<alt>" with the default
 * first (core_options.c:1663), and frees the buffer immediately afterwards, so
 * both key and value are copied here. */
#define HOST_MAX_OPTIONS 64
static struct { char *key; char *value; } host_defaults[HOST_MAX_OPTIONS];
static int host_defaults_count;

static void host_capture_defaults(const struct retro_variable *vars)
{
    host_defaults_count = 0;
    for (; vars && vars->key && host_defaults_count < HOST_MAX_OPTIONS; vars++) {
        const char *sep, *end;
        size_t n;
        if (!vars->value) continue;
        sep = strstr(vars->value, "; ");
        if (!sep) continue;
        sep += 2;
        end = strchr(sep, '|');
        n = end ? (size_t)(end - sep) : strlen(sep);

        host_defaults[host_defaults_count].key = strdup(vars->key);
        host_defaults[host_defaults_count].value = (char *)malloc(n + 1);
        if (!host_defaults[host_defaults_count].key ||
            !host_defaults[host_defaults_count].value)
            break;
        memcpy(host_defaults[host_defaults_count].value, sep, n);
        host_defaults[host_defaults_count].value[n] = '\0';
        host_defaults_count++;
    }
    fprintf(stderr, "MISTER-HOST: captured %d core-option defaults\n",
            host_defaults_count);
}

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

    {
        const char *cm = getenv("MISTER_CYCLONE_MODE");
        if (cm && *cm) {
            snprintf(host_cyclone_mode, sizeof host_cyclone_mode, "%s", cm);
            fprintf(stderr, "MISTER-HOST: cyclone_mode=%s\n", host_cyclone_mode);
        }
    }
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
        if (strcmp(var->key, "mame2003-plus_cyclone_mode") == 0) {
            var->value = host_cyclone_mode;
            return true;
        }
        for (i = 0; host_options[i].key; i++) {
            if (strcmp(var->key, host_options[i].key) == 0) {
                var->value = host_options[i].value;
                return true;
            }
        }
        /* Not pinned: serve the core's OWN default, captured from
         * SET_VARIABLES. Returning false here would leave the matching
         * options.* field unassigned -- see host_capture_defaults(). */
        for (i = 0; i < host_defaults_count; i++) {
            if (strcmp(var->key, host_defaults[i].key) == 0) {
                var->value = host_defaults[i].value;
                return true;
            }
        }
        host_log(RETRO_LOG_WARN, "env: no value for option %s\n", var->key);
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

    /* ACCEPTED, which means the core hands over the bitmap UNROTATED and the
     * frontend owns rotation (mame2003_video_init_orientation, video.c:127).
     *
     * Nothing rotates in software here. Rotation belongs in ascal, alongside a
     * [mamester_vertical] MiSTer.ini section in the shape of arcade_vertical;
     * until that exists, vertical games are presented sideways on purpose and
     * are meant to be looked at that way.
     *
     * Three things follow from accepting it, all wanted:
     *   - frame_convert() stops transposing, so every ROT90 driver drops a
     *     cache-hostile pass per frame;
     *   - video_flip_x/y and video_swap_xy all end up 0, which re-enables the
     *     core's zero-copy bypass for depth 15 and 32 drivers (video.c:240
     *     requires exactly that);
     *   - a vertical game presents landscape (galaga 288x224, not 224x288), so
     *     its line count drops back to ~262 and the H rate to ~15.7 kHz, which
     *     is what makes it syncable on a 15 kHz CRT in the meantime.
     *
     * The core calls this TWICE: once with a probe value to test whether the
     * frontend supports rotation at all, then again with the real 0..3. Both
     * must be accepted or it takes the "Mame will rotate internally" branch. */
    case RETRO_ENVIRONMENT_SET_ROTATION:
        host_set_rotation(*(const unsigned *)data);
        return true;

    case RETRO_ENVIRONMENT_SET_GEOMETRY:
        host_geometry_changed((const struct retro_game_geometry *)data);
        return true;

    /* Current MAME sends this; mame2003-plus never did (its only geometry
     * notification is SET_GEOMETRY, which cannot carry a refresh rate). It is
     * the ONE new command that is load-bearing rather than ceremonial: the
     * payload is a full retro_system_av_info, so it can change `timing.fps`,
     * and the modeline published to the FPGA is derived from that rate. Ignore
     * it and a driver that revises its refresh after load keeps presenting at
     * the rate reported by retro_get_system_av_info() -- the raster stays right
     * and the timing quietly does not.
     *
     * The core also uses it when the geometry outgrows the maximum it first
     * reported (libretro.cpp:533), which nv_set_mode() has to see. */
#ifdef RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
        host_av_info_changed((const struct retro_system_av_info *)data);
        return true;
#endif

    /* --- accepted and ignored -------------------------------------------- */

    case RETRO_ENVIRONMENT_SET_MESSAGE:
        host_log(RETRO_LOG_INFO, "core message: %s\n",
                 ((const struct retro_message *)data)->msg);
        return true;

    case RETRO_ENVIRONMENT_SET_VARIABLES:
        host_capture_defaults((const struct retro_variable *)data);
        return true;

    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL:
        return true;

    /* --- current MAME also issues these -----------------------------------
     * Guarded on the constant rather than on the engine: the two engines vendor
     * different vintages of libretro.h, and this file is compiled against
     * whichever one belongs to the engine being built. An #ifdef on the command
     * keeps one shared source compiling against both. */
#ifdef RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK
    case RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK:
#endif
#ifdef RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS
    case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
#endif
#ifdef RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
#endif
        return true;

    /* The content directory. Refusing it is not neutral: the core falls back to
     * the directory the ROM came from (libretro.cpp:788), which scatters state
     * into the romset directory -- the same failure GET_SAVE_DIRECTORY exists
     * to avoid. Served from the system directory, which is where the launcher
     * puts the emulator's data. */
#ifdef RETRO_ENVIRONMENT_GET_CONTENT_DIRECTORY
    case RETRO_ENVIRONMENT_GET_CONTENT_DIRECTORY:
        *(const char **)data = host_system_dir;
        return host_system_dir != NULL;
#endif

    /* Refused deliberately.
     *
     * GET_INPUT_BITMASKS would let the core read a whole port in one call
     * instead of one call per button. Refusing it costs a handful of calls per
     * frame and keeps host_input.c's single code path serving both engines;
     * accepting it is an optimisation to make with a profile in hand, not
     * ahead of one.
     *
     * The fast-forward pair is refused because the host owns pacing
     * (host_throttle.c, or the audio clock -- see host_main.c's "EXACTLY ONE
     * CLOCK"). Letting the core believe it may fast-forward would put a second
     * opinion on frame timing into a loop whose whole design is that there is
     * only one. */
#ifdef RETRO_ENVIRONMENT_GET_INPUT_BITMASKS
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
#endif
#ifdef RETRO_ENVIRONMENT_GET_FASTFORWARDING
    case RETRO_ENVIRONMENT_GET_FASTFORWARDING:
#endif
#ifdef RETRO_ENVIRONMENT_SET_FASTFORWARDING_OVERRIDE
    case RETRO_ENVIRONMENT_SET_FASTFORWARDING_OVERRIDE:
#endif
        return false;

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

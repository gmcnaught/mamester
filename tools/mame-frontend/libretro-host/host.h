/* host.h -- MAMESTer's libretro host: shared declarations.
 *
 * mame2003-plus is built as a static archive (tools/build-m2003p.sh) and linked
 * directly into this binary. There is no dlopen and no RetroArch: this file is
 * the whole frontend contract, and it exists so that both engines reach the
 * MiSTer present path (tools/mame-frontend/mister-backend/nv_present.h) through
 * the same code.
 *
 * Split of responsibility:
 *   host_env.c    the environment callback -- every RETRO_ENVIRONMENT_* the core
 *                 actually issues, and the pinned core-option table
 *   host_main.c   argv, lifecycle, the retro_run() loop and the bench counter
 *   host_video.c  (Task 6) the real implementations of the three video hooks
 *                 below; until then host_main.c carries counting stubs
 */
#ifndef MAMESTER_LIBRETRO_HOST_H
#define MAMESTER_LIBRETRO_HOST_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "libretro.h"
#include "nv_present.h"

/* --- environment (host_env.c) -------------------------------------------- */

/* Directories reported for GET_SYSTEM_DIRECTORY and GET_SAVE_DIRECTORY. The
 * core falls back to the *content* directory when these are NULL, which would
 * scatter nvram/hiscore files into the romset directory. Both strings are
 * retained, not copied. */
void host_env_init(const char *system_dir, const char *save_dir);

/* The callback handed to retro_set_environment(). */
bool host_environment(unsigned cmd, void *data);

/* --- video hooks, called by the environment callback --------------------- */

/* RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: one of the RETRO_PIXEL_FORMAT_* values.
 * mame2003-plus picks this per driver in mame2003_video_init_conversion(). */
void host_set_pixel_format(unsigned fmt);

/* RETRO_ENVIRONMENT_SET_ROTATION: 0..3 meaning 0/90/180/270 counter-clockwise.
 * host_env.c REFUSES this call, which makes the core rotate its own bitmap; see
 * the comment there. This hook therefore only ever fires if that refusal is
 * lost, and says so loudly. */
void host_set_rotation(unsigned rot);

/* RETRO_ENVIRONMENT_SET_GEOMETRY. Note the argument type: this core never sends
 * SET_SYSTEM_AV_INFO -- video.c:90 is the only geometry notification and it
 * passes a struct retro_game_geometry. Refresh rate therefore does not change
 * here and stays whatever retro_get_system_av_info() reported. */
void host_geometry_changed(const struct retro_game_geometry *geom);

/* RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO. Current MAME sends this; mame2003-plus
 * never does. It carries the timing block as well as the geometry, so it is the
 * only notification that can change the refresh rate after load -- and the
 * modeline published to the FPGA is derived from that rate. */
void host_av_info_changed(const struct retro_system_av_info *av);

/* --- video (host_video.c) ------------------------------------------------- */
void    host_video_refresh(const void *data, unsigned width, unsigned height,
                           size_t pitch);

/* The driver's native rate, from retro_get_system_av_info(). It is not known
 * until the game is loaded, so the modeline is re-published on the next frame
 * after this is set. */
void          host_video_set_refresh(double hz);
unsigned long host_video_shown(void);   /* frames presented                   */
unsigned long host_video_duped(void);   /* NULL frames re-published as dupes  */

/* --- present (host_present.c) --------------------------------------------- */

/* Nonzero when MISTER_THREADED_PRESENT put the present on a worker thread.
 *
 * READ IT AT THE CALL SITE and go straight to nv_present when it is 0. Threading
 * is meant to be a per-driver carve-out for drivers that cannot reach 60 fps any
 * other way -- the same shape as nv_present.c's "8bpp stages, 16bpp writes DDR
 * direct" split -- so OFF is the common case and has to be the cheap one:
 * measured, galaga throttled is identical either way with 0 present-dropped, so
 * threading correctly buys nothing where there is headroom. Nothing in
 * host_present.c may run in that case -- no staging allocation, no copy, no
 * mutex, no thread.
 *
 * Written once by host_present_init(), before retro_init(); read-only after,
 * including by the worker. */
extern int host_present_on;

/* Start the worker if the knob is set and the present is live. Must run after
 * nv_open() and before the first frame -- the core can present from inside
 * retro_load_game(). Leaves host_present_on at 0 and creates no thread when the
 * knob is unset, which is the default. */
void          host_present_init(void);
/* The three present entry points, valid ONLY when host_present_on is nonzero.
 * They are not a wrapper around nv_present for the OFF case: when the knob is
 * off, call nv_set_mode/nv_frame/nv_frame_repeat directly. */
void          host_present_mode(int width, int height, double refresh_hz,
                                int rot, nv_format fmt);
void          host_present_frame(const void *src, int pitch_bytes,
                                 int src_w, int src_h);
void          host_present_repeat(void);

/* Block until the worker has finished everything queued. Needed before reading
 * nv_frame_count() for the exit summary, and it is where the deferred present
 * cost lands rather than in the measured loop. */
void          host_present_drain(void);

/* Join the worker. MUST happen before nv_close() unmaps /dev/mem. */
void          host_present_stop(void);

/* Frames the queue discarded because the worker was still busy. Zero unless
 * MISTER_THREADED_PRESENT is on. */
unsigned long host_present_drops(void);

/* --- audio (host_audio.c) ------------------------------------------------- */

/* Open MiSTer's ALSA chain for this driver's rate and refresh. Returns 0, or
 * negative after reporting why; a failure is not fatal -- the run continues
 * silently, which is what a benchmark wants anyway.
 *
 * `nonblock` decides what the clock is; see the comment in host_audio.c. Pass 1
 * for an unthrottled benchmark, 0 for a played run. */
int           host_audio_open(unsigned rate, double fps, int nonblock);
void          host_audio_close(void);
size_t        host_audio_batch(const int16_t *data, size_t frames);
void          host_audio_sample(int16_t left, int16_t right);
unsigned long host_audio_underruns(void);
unsigned long host_audio_dropped(void);   /* periods dropped, non-blocking only */
unsigned long host_audio_frames(void);

/* --- input (host_input.c) ------------------------------------------------- */
void    host_input_poll(void);
int16_t host_input_state(unsigned port, unsigned device, unsigned index,
                         unsigned id);

/* --- throttle (host_throttle.c) ------------------------------------------ */
void          host_throttle_start(double fps);
void          host_throttle_wait(void);
unsigned long host_throttle_late(void);   /* frames that overran their period */

#endif /* MAMESTER_LIBRETRO_HOST_H */

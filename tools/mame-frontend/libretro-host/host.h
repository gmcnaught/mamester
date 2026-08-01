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

/* --- video (host_video.c) ------------------------------------------------- */
void    host_video_refresh(const void *data, unsigned width, unsigned height,
                           size_t pitch);

/* The driver's native rate, from retro_get_system_av_info(). It is not known
 * until the game is loaded, so the modeline is re-published on the next frame
 * after this is set. */
void          host_video_set_refresh(double hz);
unsigned long host_video_shown(void);   /* frames presented                   */
unsigned long host_video_duped(void);   /* NULL frames re-published as dupes  */

/* --- audio and input callbacks (host_main.c) ----------------------------- */
size_t  host_audio_batch(const int16_t *data, size_t frames);
void    host_audio_sample(int16_t left, int16_t right);
void    host_input_poll(void);
int16_t host_input_state(unsigned port, unsigned device, unsigned index,
                         unsigned id);

#endif /* MAMESTER_LIBRETRO_HOST_H */

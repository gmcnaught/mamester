/* Does the core reach a running machine with NO content? If it does, an
 * in-MAME differential harness needs no romset. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "libretro.h"
static bool env(unsigned cmd, void *data) {
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: return false;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:  return true;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_CONTENT_DIRECTORY: *(const char**)data="."; return true;
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:   return true;
    default: return false;
    }
}
static void vid(const void*d,unsigned w,unsigned h,size_t p){(void)d;(void)w;(void)h;(void)p;}
static void ipoll(void){}
static int16_t ist(unsigned a,unsigned b,unsigned c,unsigned d){(void)a;(void)b;(void)c;(void)d;return 0;}
static size_t ab(const int16_t*d,size_t f){(void)d;return f;}
static void as_(int16_t l,int16_t r){(void)l;(void)r;}
int main(void) {
    struct retro_system_info si; retro_get_system_info(&si);
    fprintf(stderr, "core: %s %s\n", si.library_name, si.library_version);
    retro_set_environment(env); retro_set_video_refresh(vid);
    retro_set_input_poll(ipoll); retro_set_input_state(ist);
    retro_set_audio_sample_batch(ab); retro_set_audio_sample(as_);
    retro_init();
    bool ok = retro_load_game(NULL);
    fprintf(stderr, "retro_load_game(NULL) = %s\n", ok ? "OK -- machine is running" : "FAILED");
    if (ok) { retro_run(); fprintf(stderr, "retro_run() survived\n"); }
    return ok ? 0 : 1;
}

/* host_audio.c -- libretro audio batches to MiSTer's ALSA chain.
 *
 * The configuration here is Stage 6's, reused rather than rediscovered, because
 * the failure mode is obscure and expensive to find again
 * (vendor/mame4all-pi/src/rpi/sound.cpp, tools/mister/patches/
 * 0001-alsa-explicit-hw-params.patch):
 *
 *   snd_pcm_set_params' LATENCY-derived buffer is REJECTED by MiSTer's PCM
 *   chain (plug -> rate -> file(/dev/MrAudio) -> hw:0) for many (rate, refresh)
 *   pairs -- including 44100 at 60 Hz, the common case, which fails with
 *   "Unable to get period size". Which pairs fail is rate-dependent and
 *   scattered. The very same configuration requested EXPLICITLY through
 *   hw_params is accepted. So ask for it explicitly: one period per emulated
 *   frame (sample_rate / fps, 735 at 44100/60) and four of them, which is what
 *   the latency form was trying to express anyway.
 *
 * The only card is `Dummy`, a model_MiSTer build that is 48 kHz stereo only;
 * rate_resample is on because the core runs at 44100 (pinned in host_env.c).
 *
 * Unlike mame4all this host does NOT feed the period size back into the
 * emulator. mame4all had to -- its mixer derives samples_per_frame from it and
 * crashed when they disagreed. libretro inverts the relationship: the core
 * computes its own samples-per-frame and hands over however many it produced,
 * so the period is only an ALSA-side buffering choice here.
 */

#include <stdio.h>
#include <string.h>

#include <alsa/asoundlib.h>

#include "host.h"

static snd_pcm_t *pcm;
static unsigned long underruns;
static unsigned long dropped;
static unsigned long frames_written;
static int           nonblocking;

/* Blocking or not is not a style choice, it decides WHAT THE CLOCK IS, and
 * getting it wrong silently invalidates every benchmark number.
 *
 * A blocking snd_pcm_writei stalls once the ALSA buffer is full, which paces
 * the emulation loop to the audio clock. Measured here: galaga unthrottled with
 * a blocking write reported 60.7 fps -- its native rate -- when the emulator's
 * actual unpaced ceiling for that driver is around 190. The benchmark was
 * measuring ALSA.
 *
 * So: NON-BLOCKING when benchmarking, where the fps figure has to be the
 * emulator's ceiling and a dropped period is irrelevant; BLOCKING when playing,
 * where the audio clock is the better master anyway (it absorbs the drift
 * between CLOCK_MONOTONIC and the sound card that a video-clock throttle would
 * eventually turn into an underrun or an overrun). host_main.c picks by whether
 * the run is throttled. */
int host_audio_open(unsigned rate, double fps, int nonblock)
{
    snd_pcm_hw_params_t *params = NULL;
    snd_pcm_uframes_t    period;
    snd_pcm_uframes_t    buffer_frames = 0, period_frames = 0;
    unsigned int         periods = 4;
    unsigned int         want_rate = rate;
    int                  err;

    period = (snd_pcm_uframes_t)((double)rate / (fps > 1.0 ? fps : 60.0) + 0.5);
    if (period < 64) period = 64;

#define TRY(call) do { \
        if ((err = (call)) < 0) { \
            fprintf(stderr, "MISTER-HOST: ALSA %s: %s\n", #call, snd_strerror(err)); \
            if (params) snd_pcm_hw_params_free(params); \
            if (pcm) { snd_pcm_close(pcm); pcm = NULL; } \
            return -1; \
        } \
    } while (0)

    TRY(snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0));
    TRY(snd_pcm_hw_params_malloc(&params));
    TRY(snd_pcm_hw_params_any(pcm, params));
    TRY(snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED));
    TRY(snd_pcm_hw_params_set_format(pcm, params, SND_PCM_FORMAT_S16));
    TRY(snd_pcm_hw_params_set_channels(pcm, params, 2));
    TRY(snd_pcm_hw_params_set_rate_resample(pcm, params, 1));
    TRY(snd_pcm_hw_params_set_rate_near(pcm, params, &want_rate, 0));
    TRY(snd_pcm_hw_params_set_period_size_near(pcm, params, &period, 0));
    TRY(snd_pcm_hw_params_set_periods_near(pcm, params, &periods, 0));
    TRY(snd_pcm_hw_params(pcm, params));
    snd_pcm_hw_params_free(params);
    params = NULL;
    TRY(snd_pcm_get_params(pcm, &buffer_frames, &period_frames));
    TRY(snd_pcm_prepare(pcm));
    TRY(snd_pcm_nonblock(pcm, nonblock ? 1 : 0));
#undef TRY

    nonblocking = nonblock;
    fprintf(stderr, "MISTER-HOST: ALSA %u Hz stereo, %.2f fps, "
                    "period %lu frames, buffer %lu frames, %s\n",
            want_rate, fps, (unsigned long)period_frames,
            (unsigned long)buffer_frames,
            nonblock ? "non-blocking (emulator is the clock)"
                     : "blocking (audio is the clock)");

    /* Prime with silence so the first real batch is not immediately an
     * underrun while the emulator is still finishing its first frame. */
    {
        size_t bytes = snd_pcm_frames_to_bytes(pcm, period_frames * 2);
        void *silence = calloc(1, bytes);
        if (silence) {
            snd_pcm_writei(pcm, silence, period_frames * 2);
            free(silence);
        }
    }
    return 0;
}

void host_audio_close(void)
{
    if (!pcm) return;
    snd_pcm_drop(pcm);
    snd_pcm_close(pcm);
    pcm = NULL;
}

/* libretro hands over interleaved stereo int16, which is exactly what
 * snd_pcm_writei wants -- no conversion, no repacking. */
size_t host_audio_batch(const int16_t *data, size_t frames)
{
    snd_pcm_sframes_t n;

    if (!pcm || !data || !frames) return frames;
    (void)nonblocking;

    n = snd_pcm_writei(pcm, data, (snd_pcm_uframes_t)frames);
    if (n == -EAGAIN) {
        /* Non-blocking only: the buffer is full because the emulator is ahead
         * of real time, which is the normal state of an unthrottled benchmark.
         * Dropping is the point -- blocking here would make the run measure
         * ALSA instead of the emulator. */
        dropped++;
    } else if (n == -EPIPE) {
        /* Underrun. Recover and carry on rather than stalling the emulation
         * loop: a dropped period is a click, a stall is a frame drop that then
         * causes the next underrun. Counted and reported at exit. */
        underruns++;
        snd_pcm_prepare(pcm);
    } else if (n < 0) {
        if (snd_pcm_recover(pcm, (int)n, 1) < 0)
            fprintf(stderr, "MISTER-HOST: ALSA write failed: %s\n",
                    snd_strerror((int)n));
    } else {
        frames_written += (unsigned long)n;
    }
    return frames;
}

/* The core never calls this -- retro_set_audio_sample is a no-op in
 * mame2003-plus (mame2003.c:658) -- but the callback must exist. */
void host_audio_sample(int16_t left, int16_t right)
{
    int16_t pair[2];
    pair[0] = left; pair[1] = right;
    host_audio_batch(pair, 1);
}

unsigned long host_audio_underruns(void) { return underruns; }
unsigned long host_audio_dropped(void)   { return dropped; }
unsigned long host_audio_frames(void)    { return frames_written; }

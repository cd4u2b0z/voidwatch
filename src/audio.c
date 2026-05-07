#include <alloca.h>
#include <alsa/asoundlib.h>
#include <fftw3.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "config.h"

static const float BAND_LO[BAND_COUNT] = {
    20.f, 60.f, 250.f, 500.f, 2000.f, 6000.f,
};
static const float BAND_HI[BAND_COUNT] = {
    60.f, 250.f, 500.f, 2000.f, 6000.f, 20000.f,
};

typedef struct {
    snd_pcm_t      *pcm;
    fftwf_plan      plan;
    float          *win;
    float          *time_buf;
    fftwf_complex  *freq_buf;
    float          *capture;
    int             channels;
    int             sample_rate;
    int             chunk;

    pthread_t       thread;
    pthread_mutex_t lock;
    atomic_int      running;

    float           band[BAND_COUNT];
    float           band_peak[BAND_COUNT];
    float           transient;
    float           prev_total;
    int             ready;
} AudioCtx;

static AudioCtx C;

static void make_hann(float *w, int n) {
    for (int i = 0; i < n; i++) {
        w[i] = 0.5f * (1.0f - cosf(6.2831853f * (float)i / (float)(n - 1)));
    }
}

static float band_mag(fftwf_complex *X, int lo, int hi) {
    if (hi <= lo) return 0.0f;
    double acc = 0.0;
    for (int k = lo; k < hi; k++) {
        float re = X[k][0], im = X[k][1];
        acc += sqrt((double)(re * re + im * im));
    }
    acc /= (double)(hi - lo);
    return logf(1.0f + (float)acc);
}

static void *capture_loop(void *arg) {
    (void)arg;
    AudioCtx *c = &C;

    while (atomic_load(&c->running)) {
        snd_pcm_sframes_t got = snd_pcm_readi(c->pcm, c->capture,
                                              (snd_pcm_uframes_t)c->chunk);
        if (got < 0) {
            if (got == -EPIPE) {
                snd_pcm_prepare(c->pcm);
                continue;
            }
            break;
        }

        /* Downmix to mono and apply Hann window. */
        for (snd_pcm_sframes_t i = 0; i < got; i++) {
            float s = 0.0f;
            for (int ch = 0; ch < c->channels; ch++) {
                s += c->capture[i * c->channels + ch];
            }
            s /= (float)c->channels;
            c->time_buf[i] = s * c->win[i];
        }
        for (snd_pcm_sframes_t i = got; i < c->chunk; i++) {
            c->time_buf[i] = 0.0f;
        }

        fftwf_execute(c->plan);

        const int half = c->chunk / 2;
        float new_band[BAND_COUNT];
        float total = 0.0f;
        for (int i = 0; i < BAND_COUNT; i++) {
            int lo = (int)(BAND_LO[i] * (float)c->chunk
                                      / (float)c->sample_rate);
            int hi = (int)(BAND_HI[i] * (float)c->chunk
                                      / (float)c->sample_rate);
            if (lo < 1)    lo = 1;
            if (hi > half) hi = half;
            new_band[i] = band_mag(c->freq_buf, lo, hi);
            total += new_band[i];
        }

        pthread_mutex_lock(&c->lock);
        for (int i = 0; i < BAND_COUNT; i++) {
            /* Per-band AGC: track running peak with slow decay. */
            if (new_band[i] > c->band_peak[i]) {
                c->band_peak[i] = new_band[i];
            } else {
                c->band_peak[i] *= AUDIO_AGC_DECAY;
            }
            if (c->band_peak[i] < AUDIO_AGC_FLOOR) {
                c->band_peak[i] = AUDIO_AGC_FLOOR;
            }
            float norm = new_band[i] / c->band_peak[i];
            if (norm > 1.0f) norm = 1.0f;

            /* Envelope follower: fast attack, slow release. */
            float a = (norm > c->band[i]) ? AUDIO_ATTACK : AUDIO_RELEASE;
            c->band[i] += (norm - c->band[i]) * a;
        }

        /* Transient: positive flux in total energy. */
        float delta = total - c->prev_total;
        c->prev_total = total;
        if (delta > 0.0f) {
            float t = delta / (total + 0.001f);
            if (t > c->transient) c->transient = t;
        }
        c->transient *= AUDIO_TRANSIENT_DECAY;
        c->ready = 1;
        pthread_mutex_unlock(&c->lock);
    }
    return NULL;
}

int audio_init(const char *device) {
    memset(&C, 0, sizeof C);
    C.chunk       = AUDIO_CHUNK;
    C.sample_rate = AUDIO_SAMPLE_RATE;
    pthread_mutex_init(&C.lock, NULL);

    const char *dev = device;
    if (!dev) dev = getenv("VOIDWATCH_AUDIO_DEVICE");
    if (!dev) dev = AUDIO_DEVICE_DEFAULT;

    if (snd_pcm_open(&C.pcm, dev, SND_PCM_STREAM_CAPTURE, 0) < 0) {
        return -1;
    }

    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);
    if (snd_pcm_hw_params_any(C.pcm, hw) < 0) goto fail;
    if (snd_pcm_hw_params_set_access(C.pcm, hw,
                                     SND_PCM_ACCESS_RW_INTERLEAVED) < 0) goto fail;
    if (snd_pcm_hw_params_set_format(C.pcm, hw,
                                     SND_PCM_FORMAT_FLOAT_LE) < 0) goto fail;

    unsigned int rate = (unsigned)C.sample_rate;
    snd_pcm_hw_params_set_rate_near(C.pcm, hw, &rate, NULL);
    C.sample_rate = (int)rate;

    if (snd_pcm_hw_params_set_channels(C.pcm, hw, 1) < 0) {
        if (snd_pcm_hw_params_set_channels(C.pcm, hw, 2) < 0) goto fail;
        C.channels = 2;
    } else {
        C.channels = 1;
    }

    snd_pcm_uframes_t chunk = (snd_pcm_uframes_t)C.chunk;
    snd_pcm_hw_params_set_period_size_near(C.pcm, hw, &chunk, NULL);
    C.chunk = (int)chunk;

    if (snd_pcm_hw_params(C.pcm, hw) < 0)  goto fail;
    if (snd_pcm_prepare(C.pcm) < 0)        goto fail;

    C.win      = fftwf_alloc_real(C.chunk);
    C.time_buf = fftwf_alloc_real(C.chunk);
    C.freq_buf = fftwf_alloc_complex(C.chunk / 2 + 1);
    C.capture  = malloc(sizeof(float) * (size_t)C.chunk
                                       * (size_t)C.channels);
    if (!C.win || !C.time_buf || !C.freq_buf || !C.capture) goto fail;

    make_hann(C.win, C.chunk);
    C.plan = fftwf_plan_dft_r2c_1d(C.chunk, C.time_buf, C.freq_buf,
                                   FFTW_MEASURE);
    if (!C.plan) goto fail;

    atomic_store(&C.running, 1);
    if (pthread_create(&C.thread, NULL, capture_loop, NULL) != 0) {
        atomic_store(&C.running, 0);
        goto fail;
    }
    return 0;

fail:
    audio_shutdown();
    return -1;
}

void audio_shutdown(void) {
    if (atomic_load(&C.running)) {
        atomic_store(&C.running, 0);
        pthread_join(C.thread, NULL);
    }
    if (C.plan)     fftwf_destroy_plan(C.plan);
    if (C.win)      fftwf_free(C.win);
    if (C.time_buf) fftwf_free(C.time_buf);
    if (C.freq_buf) fftwf_free(C.freq_buf);
    free(C.capture);
    if (C.pcm)      snd_pcm_close(C.pcm);
    pthread_mutex_destroy(&C.lock);
    memset(&C, 0, sizeof C);
}

void audio_list_devices(FILE *out) {
    void **hints = NULL;
    if (snd_device_name_hint(-1, "pcm", &hints) < 0 || !hints) {
        fprintf(out, "voidwatch: could not enumerate ALSA PCM devices.\n");
        return;
    }

    fprintf(out, "ALSA PCM capture sources:\n");
    int shown = 0;
    for (void **h = hints; *h; h++) {
        char *name = snd_device_name_get_hint(*h, "NAME");
        char *desc = snd_device_name_get_hint(*h, "DESC");
        char *ioid = snd_device_name_get_hint(*h, "IOID");

        /* IOID is "Input", "Output", or NULL (= both). Skip output-only. */
        if (!ioid || strcmp(ioid, "Input") == 0) {
            int monitor = 0;
            if (name && (strstr(name, "monitor") || strstr(name, "Monitor"))) monitor = 1;
            if (desc && (strstr(desc, "monitor") || strstr(desc, "Monitor"))) monitor = 1;

            fprintf(out, "  %-28s%s\n",
                    name ? name : "(unnamed)",
                    monitor ? "  (monitor)" : "");
            if (desc) {
                /* desc is multi-line: name\ndetails — show inline. */
                char *nl = strchr(desc, '\n');
                if (nl) *nl = ' ';
                fprintf(out, "      %s\n", desc);
            }
            shown++;
        }

        free(name);
        free(desc);
        free(ioid);
    }
    snd_device_name_free_hint(hints);

    if (!shown) {
        fprintf(out, "  (none found)\n");
    }
    fprintf(out,
        "\nUse --device <name> or set $VOIDWATCH_AUDIO_DEVICE.\n"
        "For desktop audio on PipeWire/PulseAudio, pick a 'monitor' source\n"
        "or run with device=pulse and route in pavucontrol's Recording tab.\n");
}

AudioSnapshot audio_snapshot(void) {
    AudioSnapshot s;
    pthread_mutex_lock(&C.lock);
    for (int i = 0; i < BAND_COUNT; i++) s.bands[i] = C.band[i];
    s.transient = C.transient;
    s.active    = C.ready;
    pthread_mutex_unlock(&C.lock);
    return s;
}

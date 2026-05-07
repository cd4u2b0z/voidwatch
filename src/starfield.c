#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "config.h"
#include "palette.h"
#include "starfield.h"

/* Effective brightness threshold below which a spike glyph is suppressed.
 * Twinkle dips force the spike to flicker on and off — exactly the
 * "diamond winks" Hubble-y read we want. */
#define SPIKE_FIRE_THRESHOLD 0.78f
/* Floor on the rendered glyph colour so a near-threshold flare still reads
 * crisp instead of fading into the framebuffer haze. */
#define SPIKE_MIN_BRIGHT     0.65f

static const float STAR_PARALLAX[STAR_LAYERS]   = { 0.10f, 0.40f, 1.00f };
static const float STAR_BRIGHTNESS[STAR_LAYERS] = { 0.35f, 0.65f, 1.00f };

static float frand(void) { return (float)rand() / (float)RAND_MAX; }

static Color pick_spectral(void) {
    /* Loose stellar mix: cool reds most common, then sunlike, then hot blues. */
    float r = frand();
    if (r < 0.55f) return g_palette.star_m;
    if (r < 0.90f) return g_palette.star_g;
    return g_palette.star_b;
}

static int pick_layer(void) {
    float r = frand();
    if (r < 0.55f) return 0; /* far     */
    if (r < 0.85f) return 1; /* mid     */
    return 2;                /* near    */
}

int starfield_init(Starfield *sf, int cols, int rows) {
    sf->world_w = (float)(cols * 2);
    sf->world_h = (float)(rows * 4);

    int target = (int)((float)cols * (float)rows * STAR_DENSITY);
    if (target < 32)              target = 32;
    if (target > STAR_COUNT_MAX)  target = STAR_COUNT_MAX;

    sf->stars = calloc((size_t)target, sizeof(Star));
    if (!sf->stars) return -1;
    sf->count = target;

    for (int i = 0; i < target; i++) {
        Star *s            = &sf->stars[i];
        s->wx              = frand() * sf->world_w;
        s->wy              = frand() * sf->world_h;
        s->layer           = pick_layer();
        s->color           = pick_spectral();
        s->base_bright     = 0.45f + 0.55f * frand();
        s->twinkle_phase   = frand() * 6.2831853f;
        s->twinkle_rate    = 0.3f + 2.5f * frand();

        /* Spike-eligible: only the near layer's brighter half. Yields ~7%
         * of the field, in line with the "brightest 10%" target. */
        s->spike = (s->layer == 2 && s->base_bright > 0.80f);
    }
    return 0;
}

void starfield_free(Starfield *sf) {
    free(sf->stars);
    sf->stars = NULL;
    sf->count = 0;
}

static inline float wrapf(float v, float m) {
    v = fmodf(v, m);
    if (v < 0.0f) v += m;
    return v;
}

void starfield_draw(const Starfield *sf, Framebuffer *fb,
                    float cam_x, float cam_y, double t,
                    const AudioSnapshot *snap) {
    /* Treble adds depth to the twinkle envelope. Base envelope is [0.7, 1.0]
     * at silence; full treble pushes it to ~[0.0, 1.0] for sharp shimmer. */
    float amp      = 0.15f + snap->bands[BAND_TREBLE] * 0.15f * MOD_TREBLE_TWINKLE;
    float bass_lift = 1.0f + snap->bands[BAND_BASS] * 0.30f;

    for (int i = 0; i < sf->count; i++) {
        const Star *s = &sf->stars[i];
        float par = STAR_PARALLAX[s->layer];

        float sx = wrapf(s->wx - cam_x * par, sf->world_w);
        float sy = wrapf(s->wy - cam_y * par, sf->world_h);

        float twinkle = 1.0f - amp + amp * sinf(s->twinkle_phase
                                                + (float)t * s->twinkle_rate);
        if (twinkle < 0.0f) twinkle = 0.0f;
        float bright  = s->base_bright * STAR_BRIGHTNESS[s->layer]
                        * twinkle * bass_lift;

        /* fb_max so the starfield doesn't accumulate over the decay buffer
         * — stars hold their brightness while bodies trail through. */
        fb_max(fb, (int)sx, (int)sy,
               s->color.r * bright,
               s->color.g * bright,
               s->color.b * bright);
    }
}

void starfield_spikes(const Starfield *sf, FILE *out,
                      int cols, int rows,
                      float cam_x, float cam_y, double t,
                      const AudioSnapshot *snap) {
    /* Same twinkle envelope as starfield_draw — pulled out so spikes
     * fire in lockstep with the framebuffer glow. */
    float amp       = 0.15f + snap->bands[BAND_TREBLE] * 0.15f * MOD_TREBLE_TWINKLE;
    float bass_lift = 1.0f + snap->bands[BAND_BASS] * 0.30f;

    float world_w = sf->world_w;
    float world_h = sf->world_h;

    for (int i = 0; i < sf->count; i++) {
        const Star *s = &sf->stars[i];
        if (!s->spike) continue;
        float par = STAR_PARALLAX[s->layer];

        float sx = wrapf(s->wx - cam_x * par, world_w);
        float sy = wrapf(s->wy - cam_y * par, world_h);

        float twinkle = 1.0f - amp + amp * sinf(s->twinkle_phase
                                                + (float)t * s->twinkle_rate);
        if (twinkle < 0.0f) twinkle = 0.0f;
        float bright = s->base_bright * STAR_BRIGHTNESS[s->layer]
                       * twinkle * bass_lift;
        if (bright < SPIKE_FIRE_THRESHOLD) continue;

        /* Sub-pixel → cell. Skip if off-screen. */
        int cx = (int)(sx / 2.0f);
        int cy = (int)(sy / 4.0f);
        if (cx < 0 || cy < 0 || cx >= cols || cy >= rows) continue;

        /* Boost bright into [SPIKE_MIN_BRIGHT, 1.0] so a barely-firing
         * flare still punches through the decay buffer behind it. */
        float k = bright;
        if (k > 1.0f)             k = 1.0f;
        if (k < SPIKE_MIN_BRIGHT) k = SPIKE_MIN_BRIGHT;
        int r = (int)(s->color.r * k * 255.0f);
        int g = (int)(s->color.g * k * 255.0f);
        int b = (int)(s->color.b * k * 255.0f);
        if (r > 255) r = 255;
        if (g > 255) g = 255;
        if (b > 255) b = 255;

        /* ANSI rows/cols are 1-indexed. */
        fprintf(out, "\x1b[%d;%dH\x1b[38;2;%d;%d;%dm*",
                cy + 1, cx + 1, r, g, b);
    }
    fputs("\x1b[0m", out);
}

#include <math.h>

#define STB_PERLIN_IMPLEMENTATION
#include "stb_perlin.h"

#include "config.h"
#include "nebula.h"
#include "palette.h"

void nebula_draw(Framebuffer *fb, float cam_x, float cam_y, double t,
                 const AudioSnapshot *snap) {
    float t1 = (float)t * NEBULA_DRIFT_BIG;
    float t2 = (float)t * NEBULA_DRIFT_SML;
    float pcx = cam_x * NEBULA_PARALLAX;
    float pcy = cam_y * NEBULA_PARALLAX;

    const float inv_thresh = 1.0f / (1.0f - NEBULA_THRESHOLD);
    const float bass_gain  = 1.0f + snap->bands[BAND_BASS] * MOD_BASS_NEBULA;

    for (int cy = 0; cy < fb->rows; cy++) {
        for (int cx = 0; cx < fb->cols; cx++) {
            float wx = (float)(cx * 2 + 1) + pcx;
            float wy = (float)(cy * 4 + 2) + pcy;

            float n1 = stb_perlin_noise3(wx * NEBULA_SCALE_BIG,
                                         wy * NEBULA_SCALE_BIG,
                                         t1, 0, 0, 0);
            float n2 = stb_perlin_noise3(wx * NEBULA_SCALE_SML,
                                         wy * NEBULA_SCALE_SML,
                                         t2, 0, 0, 0);

            float n = 0.65f * n1 + 0.35f * n2;
            n = (n + 1.0f) * 0.5f;          /* -1..1 -> 0..1 */
            n -= NEBULA_THRESHOLD;
            if (n <= 0.0f) continue;
            n  = powf(n * inv_thresh, NEBULA_SHAPE);
            float k = n * NEBULA_INTENSITY * bass_gain;

            float mix = (n2 + 1.0f) * 0.5f;
            const Color v = g_palette.nebula_violet;
            const Color cr = g_palette.nebula_crimson;
            float r = (v.r  * (1.0f - mix) + cr.r * mix) * k;
            float g = (v.g  * (1.0f - mix) + cr.g * mix) * k;
            float b = (v.b  * (1.0f - mix) + cr.b * mix) * k;

            int sx0 = cx * 2;
            int sy0 = cy * 4;
            for (int dy = 0; dy < 4; dy++) {
                for (int dx = 0; dx < 2; dx++) {
                    fb_max(fb, sx0 + dx, sy0 + dy, r, g, b);
                }
            }
        }
    }
}

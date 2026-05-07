#include <math.h>
#include <stdio.h>

#include "config.h"
#include "palette.h"
#include "render.h"

/* Display-side colour grading. Slight gamma > 1 deepens the blacks (more
 * void), and a subtle blue lift in shadows nudges everything toward
 * "deep-space" rather than flat dark grey. */
#define GRADE_GAMMA       1.18f
#define GRADE_SHADOW_BLUE 0.04f

/*
 * Braille dot bit indices for a 2x4 sub-pixel grid.
 *   col 0   col 1
 *    0       3
 *    1       4
 *    2       5
 *    6       7
 * Codepoint = 0x2800 | (1 << bit_index for each lit dot).
 */
static const int braille_bits[4][2] = {
    { 0, 3 },
    { 1, 4 },
    { 2, 5 },
    { 6, 7 },
};

static inline int clamp_byte(float v) {
    if (v <= 0.0f) return 0;
    if (v >= 1.0f) return 255;
    v = powf(v, GRADE_GAMMA);
    return (int)(v * 255.0f + 0.5f);
}

static inline void grade_pixel(float *r, float *g, float *b) {
    /* Lift blue in shadows — quadratic falloff so highlights are untouched. */
    float lum = 0.299f * *r + 0.587f * *g + 0.114f * *b;
    float shadow = 1.0f - lum;
    if (shadow < 0.0f) shadow = 0.0f;
    *b += (1.0f - *b) * GRADE_SHADOW_BLUE * shadow * shadow;
}

void render_flush(const Framebuffer *fb, FILE *out) {
    fputs("\x1b[H", out);
    fprintf(out, "\x1b[48;2;%d;%d;%dm",
            clamp_byte(g_palette.void_bg.r),
            clamp_byte(g_palette.void_bg.g),
            clamp_byte(g_palette.void_bg.b));

    int last_r = -1, last_g = -1, last_b = -1;

    for (int cy = 0; cy < fb->rows; cy++) {
        for (int cx = 0; cx < fb->cols; cx++) {
            int   code = 0x2800;
            float sr = 0, sg = 0, sb = 0;
            int   lit = 0;

            for (int dy = 0; dy < 4; dy++) {
                for (int dx = 0; dx < 2; dx++) {
                    int sx = cx * 2 + dx;
                    int sy = cy * 4 + dy;
                    const float *p = &fb->data[(sy * fb->sub_w + sx) * 3];
                    float lum = 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2];
                    if (lum > LUM_THRESHOLD) {
                        code |= (1 << braille_bits[dy][dx]);
                        sr += p[0];
                        sg += p[1];
                        sb += p[2];
                        lit++;
                    }
                }
            }

            if (!lit) {
                fputc(' ', out);
                continue;
            }

            float inv = 1.0f / (float)lit;
            float fr = sr * inv, fg = sg * inv, fb_ = sb * inv;
            grade_pixel(&fr, &fg, &fb_);
            int r = clamp_byte(fr);
            int g = clamp_byte(fg);
            int b = clamp_byte(fb_);
            if (r != last_r || g != last_g || b != last_b) {
                fprintf(out, "\x1b[38;2;%d;%d;%dm", r, g, b);
                last_r = r; last_g = g; last_b = b;
            }

            unsigned u = (unsigned)code;
            fputc((int)(0xE0 | (u >> 12)),         out);
            fputc((int)(0x80 | ((u >> 6) & 0x3F)), out);
            fputc((int)(0x80 | (u & 0x3F)),        out);
        }
        if (cy < fb->rows - 1) fputc('\n', out);
    }

    fputs("\x1b[0m", out);
    fflush(out);
}

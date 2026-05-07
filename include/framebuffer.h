#ifndef VOIDWATCH_FRAMEBUFFER_H
#define VOIDWATCH_FRAMEBUFFER_H

/*
 * Float RGB framebuffer at sub-cell resolution.
 * Each terminal cell maps to a 2x4 grid of sub-pixels (Braille dots),
 * so sub_w = cols*2 and sub_h = rows*4.
 *
 * Storage is interleaved RGB triplets: data[(y*sub_w + x)*3 + {0,1,2}].
 * Additive blending is the only write path — composite passes accumulate.
 */
typedef struct {
    int    cols, rows;   /* terminal cells */
    int    sub_w, sub_h; /* cols*2, rows*4 */
    float *data;         /* sub_w * sub_h * 3 */
} Framebuffer;

int  fb_init(Framebuffer *fb, int cols, int rows);
void fb_free(Framebuffer *fb);
void fb_clear(Framebuffer *fb);
void fb_decay(Framebuffer *fb, float factor);

static inline void fb_add(Framebuffer *fb, int x, int y,
                          float r, float g, float b) {
    if ((unsigned)x >= (unsigned)fb->sub_w) return;
    if ((unsigned)y >= (unsigned)fb->sub_h) return;
    float *p = &fb->data[(y * fb->sub_w + x) * 3];
    p[0] += r; p[1] += g; p[2] += b;
}

/* Take per-channel max — used by the starfield so its brightness stays
 * constant across decay ticks instead of accumulating. */
static inline void fb_max(Framebuffer *fb, int x, int y,
                          float r, float g, float b) {
    if ((unsigned)x >= (unsigned)fb->sub_w) return;
    if ((unsigned)y >= (unsigned)fb->sub_h) return;
    float *p = &fb->data[(y * fb->sub_w + x) * 3];
    if (r > p[0]) p[0] = r;
    if (g > p[1]) p[1] = g;
    if (b > p[2]) p[2] = b;
}

#endif

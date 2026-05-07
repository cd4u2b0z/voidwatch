#include <stdlib.h>
#include <string.h>

#include "framebuffer.h"

int fb_init(Framebuffer *fb, int cols, int rows) {
    fb->cols  = cols;
    fb->rows  = rows;
    fb->sub_w = cols * 2;
    fb->sub_h = rows * 4;
    size_t n  = (size_t)fb->sub_w * (size_t)fb->sub_h * 3u;
    fb->data  = calloc(n, sizeof(float));
    return fb->data ? 0 : -1;
}

void fb_free(Framebuffer *fb) {
    free(fb->data);
    fb->data = NULL;
}

void fb_clear(Framebuffer *fb) {
    size_t n = (size_t)fb->sub_w * (size_t)fb->sub_h * 3u;
    memset(fb->data, 0, n * sizeof(float));
}

void fb_decay(Framebuffer *fb, float factor) {
    size_t n = (size_t)fb->sub_w * (size_t)fb->sub_h * 3u;
    for (size_t i = 0; i < n; i++) fb->data[i] *= factor;
}

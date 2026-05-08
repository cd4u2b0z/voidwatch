/* tests/test_framebuffer.c — framebuffer numeric correctness.
 *
 * Verifies fb_init / fb_clear / fb_decay / fb_add / fb_max behave as the
 * pipeline expects:
 *   - sub-pixel dimensions are cols*2 × rows*4
 *   - clear zeroes; decay multiplies; add accumulates; max pegs without
 *     accumulating
 *   - bounds checks reject negative and out-of-range coords
 *
 * Plain C asserts. Exit 0 on pass, abort on fail.
 */

#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "framebuffer.h"

#define EPS 1e-6f

static int feq(float a, float b) { return fabsf(a - b) < EPS; }

static const float *pixel(const Framebuffer *fb, int x, int y) {
    return &fb->data[(y * fb->sub_w + x) * 3];
}

static void test_init_dims(void) {
    Framebuffer fb = {0};
    assert(fb_init(&fb, 80, 24) == 0);
    assert(fb.cols  == 80);
    assert(fb.rows  == 24);
    assert(fb.sub_w == 160);   /* cols * 2 */
    assert(fb.sub_h == 96);    /* rows * 4 */
    assert(fb.data  != NULL);

    /* fresh init must zero the buffer (calloc semantics) */
    for (int i = 0; i < fb.sub_w * fb.sub_h * 3; i++) {
        assert(fb.data[i] == 0.0f);
    }
    fb_free(&fb);
    assert(fb.data == NULL);
}

static void test_add_accumulates(void) {
    Framebuffer fb = {0};
    assert(fb_init(&fb, 4, 2) == 0);

    fb_add(&fb, 3, 5, 0.10f, 0.20f, 0.30f);
    fb_add(&fb, 3, 5, 0.05f, 0.10f, 0.15f);

    const float *p = pixel(&fb, 3, 5);
    assert(feq(p[0], 0.15f));
    assert(feq(p[1], 0.30f));
    assert(feq(p[2], 0.45f));

    /* untouched pixel stays zero */
    const float *q = pixel(&fb, 0, 0);
    assert(q[0] == 0.0f && q[1] == 0.0f && q[2] == 0.0f);

    fb_free(&fb);
}

static void test_max_pegs(void) {
    Framebuffer fb = {0};
    assert(fb_init(&fb, 4, 2) == 0);

    fb_max(&fb, 1, 1, 0.50f, 0.20f, 0.80f);
    /* lower r/g should not lower the stored value; higher b should not lift
     * the lower channels */
    fb_max(&fb, 1, 1, 0.30f, 0.40f, 0.60f);

    const float *p = pixel(&fb, 1, 1);
    assert(feq(p[0], 0.50f));   /* held at original (higher) */
    assert(feq(p[1], 0.40f));   /* lifted to higher new */
    assert(feq(p[2], 0.80f));   /* held at original */

    /* repeated max with same value = no-op */
    fb_max(&fb, 1, 1, 0.50f, 0.40f, 0.80f);
    p = pixel(&fb, 1, 1);
    assert(feq(p[0], 0.50f));
    assert(feq(p[1], 0.40f));
    assert(feq(p[2], 0.80f));

    fb_free(&fb);
}

static void test_decay(void) {
    Framebuffer fb = {0};
    assert(fb_init(&fb, 2, 1) == 0);

    fb_add(&fb, 0, 0, 1.0f, 1.0f, 1.0f);
    fb_add(&fb, 3, 3, 0.4f, 0.5f, 0.6f);

    fb_decay(&fb, 0.5f);

    const float *a = pixel(&fb, 0, 0);
    assert(feq(a[0], 0.5f));
    assert(feq(a[1], 0.5f));
    assert(feq(a[2], 0.5f));

    const float *b = pixel(&fb, 3, 3);
    assert(feq(b[0], 0.20f));
    assert(feq(b[1], 0.25f));
    assert(feq(b[2], 0.30f));

    /* repeated decay compounds */
    fb_decay(&fb, 0.5f);
    a = pixel(&fb, 0, 0);
    assert(feq(a[0], 0.25f));

    fb_free(&fb);
}

static void test_clear(void) {
    Framebuffer fb = {0};
    assert(fb_init(&fb, 4, 2) == 0);

    fb_add(&fb, 0, 0, 1.0f, 1.0f, 1.0f);
    fb_add(&fb, 7, 7, 1.0f, 1.0f, 1.0f);
    fb_clear(&fb);

    for (int i = 0; i < fb.sub_w * fb.sub_h * 3; i++) {
        assert(fb.data[i] == 0.0f);
    }
    fb_free(&fb);
}

static void test_bounds_reject(void) {
    Framebuffer fb = {0};
    assert(fb_init(&fb, 4, 2) == 0);

    /* negative coords must be rejected silently */
    fb_add(&fb, -1,  0, 9.0f, 9.0f, 9.0f);
    fb_add(&fb,  0, -1, 9.0f, 9.0f, 9.0f);
    fb_max(&fb, -1, -1, 9.0f, 9.0f, 9.0f);

    /* out-of-range high coords must be rejected silently */
    fb_add(&fb, fb.sub_w,     0, 9.0f, 9.0f, 9.0f);
    fb_add(&fb, 0,        fb.sub_h, 9.0f, 9.0f, 9.0f);
    fb_max(&fb, fb.sub_w + 100, fb.sub_h + 100, 9.0f, 9.0f, 9.0f);

    /* nothing should have been written anywhere */
    for (int i = 0; i < fb.sub_w * fb.sub_h * 3; i++) {
        assert(fb.data[i] == 0.0f);
    }
    fb_free(&fb);
}

static void test_corners_in_range(void) {
    Framebuffer fb = {0};
    assert(fb_init(&fb, 4, 2) == 0);

    /* extreme valid coords work */
    fb_add(&fb, 0, 0, 0.1f, 0.0f, 0.0f);
    fb_add(&fb, fb.sub_w - 1, fb.sub_h - 1, 0.0f, 0.2f, 0.0f);
    fb_max(&fb, fb.sub_w - 1, 0, 0.0f, 0.0f, 0.3f);

    assert(feq(pixel(&fb, 0, 0)[0], 0.1f));
    assert(feq(pixel(&fb, fb.sub_w - 1, fb.sub_h - 1)[1], 0.2f));
    assert(feq(pixel(&fb, fb.sub_w - 1, 0)[2], 0.3f));

    fb_free(&fb);
}

int main(void) {
    test_init_dims();
    test_add_accumulates();
    test_max_pegs();
    test_decay();
    test_clear();
    test_bounds_reject();
    test_corners_in_range();
    printf("framebuffer: PASS (7 cases)\n");
    return 0;
}

#ifndef VOIDWATCH_RENDER_H
#define VOIDWATCH_RENDER_H

#include <stdio.h>
#include "framebuffer.h"

/*
 * Composite the framebuffer to the terminal:
 *   - Each cell scans its 2x4 sub-pixels, builds a Braille glyph from any
 *     sub-pixel whose luminance exceeds LUM_THRESHOLD.
 *   - Cell foreground colour is the mean RGB of lit sub-pixels.
 *   - Background is set once per frame to the void colour; empty cells emit
 *     a single space.
 */
void render_flush(const Framebuffer *fb, FILE *out);

#endif

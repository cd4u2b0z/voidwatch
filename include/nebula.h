#ifndef VOIDWATCH_NEBULA_H
#define VOIDWATCH_NEBULA_H

#include "audio.h"
#include "framebuffer.h"

/*
 * Two-octave Perlin wash, sampled once per terminal cell. Stamps every
 * sub-pixel of a cell with the same colour via fb_max — Braille resolves
 * adjacent cells smoothly, so per-sub-pixel sampling would just burn CPU.
 *
 * The slow large-scale layer carries the wash; the faster detail layer
 * also drives the violet <-> crimson colour blend so the field doesn't
 * read as one flat tint.
 */
void nebula_draw(Framebuffer *fb, float cam_x, float cam_y, double t,
                 const AudioSnapshot *snap);

#endif

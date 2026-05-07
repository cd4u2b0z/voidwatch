#ifndef VOIDWATCH_STARFIELD_H
#define VOIDWATCH_STARFIELD_H

#include <stdio.h>

#include "audio.h"
#include "config.h"
#include "framebuffer.h"

typedef struct {
    float wx, wy;          /* world position in sub-pixels */
    float twinkle_phase;
    float twinkle_rate;
    float base_bright;
    Color color;
    int   layer;           /* 0..STAR_LAYERS-1 */
    int   spike;           /* eligible for direct-render flare glyph */
} Star;

typedef struct {
    Star *stars;
    int   count;
    float world_w, world_h;
} Starfield;

int  starfield_init(Starfield *sf, int cols, int rows);
void starfield_free(Starfield *sf);
void starfield_draw(const Starfield *sf, Framebuffer *fb,
                    float cam_x, float cam_y, double t,
                    const AudioSnapshot *snap);

/*
 * Direct-render diffraction spikes for the brightest stars. Cursor-positioned
 * ANSI glyphs emitted *after* render_flush — bypasses the framebuffer so the
 * spike snaps in cleanly instead of bleeding through the decay/bloom path.
 * Call before hud_draw so HUD widgets clip on top.
 */
void starfield_spikes(const Starfield *sf, FILE *out,
                      int cols, int rows,
                      float cam_x, float cam_y, double t,
                      const AudioSnapshot *snap);

#endif

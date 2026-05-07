#ifndef VOIDWATCH_HUD_H
#define VOIDWATCH_HUD_H

#include <stdio.h>

#include "audio.h"
#include "body.h"

/*
 * HUD overlay drawn after the framebuffer flush:
 *   - top-left:     sector coords + uptime
 *   - upper-left:   event log (transient onsets)
 *   - top-right:    passive scan readout (rotates through bodies)
 *   - over field:   body labels (faded by distance from screen centre)
 *   - bottom-right: 6-band level meter, transient pip
 *
 * Direct cursor-positioned output to `out`. Skips silently on terminals
 * too small to host the widgets.
 */
/*
 * In astro mode, the sector header + body labels + scan readout are owned
 * by `astro_hud` instead — set `astro_mode` to skip them here. The audio
 * meter and event log still draw.
 */
void hud_draw(FILE *out, int cols, int rows,
              const BodySystem *bs,
              const AudioSnapshot *snap,
              float cam_x, float cam_y, double t,
              int astro_mode);

/*
 * Push a one-line event to the HUD log from outside the HUD. Subject to
 * the same rate-limit + TTL as the internal transient log. Pass the same
 * `t` the render loop is using.
 */
void hud_log_event(double t, const char *text);

/*
 * Centered help overlay listing keys. Cursor-positioned ANSI; designed to
 * draw on top of everything (call last, after hud_draw). No-op on terms
 * too small to host the box.
 */
void hud_help_overlay(FILE *out, int cols, int rows);

#endif

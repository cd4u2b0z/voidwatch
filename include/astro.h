#ifndef VOIDWATCH_ASTRO_H
#define VOIDWATCH_ASTRO_H

#include <stdio.h>
#include <time.h>

#include "audio.h"
#include "ephem.h"
#include "framebuffer.h"

/*
 * Real-ephemeris mode. Composites Sun + Moon + 8 planets over the existing
 * starfield/nebula backdrop using an azimuthal-equidistant projection
 * centered on the zenith (all-sky horizon view, north up).
 *
 * The ephemeris is recomputed each frame off `time(NULL)`. Cheap — Meeus
 * low-precision is a few hundred mults per body.
 */

/* Body trail ring buffer. Stores true RA/Dec — we re-project every frame
 * so trails show motion *against the stars* (incl. retrograde) rather
 * than diurnal arcs. New samples push when JD has advanced enough that
 * the apparent motion is at least ~1 sub-pixel — cheap and self-tuning
 * across time-scrub speeds. */
#define ASTRO_TRAIL_LEN 256

typedef struct {
    double ra_rad;
    double dec_rad;
    int    valid;
} TrailSample;

typedef struct {
    Observer       observer;
    EphemPosition  pos[EPHEM_COUNT];
    double         jd;          /* current Julian Day */
    double         lst_hours;   /* local sidereal time */

    /* Moon phase derived state. `elongation` is signed: 0 = new, π/2 =
     * first quarter, π = full, 3π/2 = last quarter. `illum` is the
     * illuminated fraction of the visible disc, [0, 1]. */
    double         moon_illum;
    double         moon_elongation;

    /* Toggles */
    int            show_grid;       /* alt-az grid lines (key: g)        */
    int            show_trails;     /* planet RA/Dec trails (key: t)     */
    int            cursor_active;   /* object-pick cursor on (key: c)    */

    /* Trail ring buffers per body. */
    TrailSample    trails[EPHEM_COUNT][ASTRO_TRAIL_LEN];
    int            trail_head[EPHEM_COUNT];
    double         trail_last_jd;

    /* Cursor screen position in *cell* coords; nudged by hjkl. The
     * nearest above-horizon body is reported in the scan panel. */
    int            cursor_col, cursor_row;
    int            cursor_locked_body;   /* EphemBody index, -1 = none   */
} AstroState;

/* Compute every body's geocentric + topocentric position from `now`. */
void astro_update(AstroState *st, time_t now);

/* Stamp body discs/labels into the framebuffer. Called between
 * `body_draw` and `render_flush` — same composite slot. */
void astro_draw(const AstroState *st, Framebuffer *fb,
                int cols, int rows, const AudioSnapshot *snap);

/* Cursor-positioned label overlay drawn after render_flush, before HUD.
 * Names appear next to each above-horizon body, faded by altitude. */
void astro_labels(const AstroState *st, FILE *out,
                  int cols, int rows);

/* Astro-mode HUD: replaces the normal sector header + scan readout with
 * lat/lon + LST and a rotating planet info panel. `speed` is the time
 * multiplier (1.0 = real-time), `offset_s` the scrub offset in seconds. */
void astro_hud(const AstroState *st, FILE *out,
               int cols, int rows, double t,
               double speed, double offset_s);

/* Phase name from elongation, e.g. "Waxing Crescent". */
const char *astro_moon_phase_name(double elongation_rad);

/* Lunar age in days since last new moon (synodic month = 29.530589). */
double astro_moon_age_days(double elongation_rad);

#endif

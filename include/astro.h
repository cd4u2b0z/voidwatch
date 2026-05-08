#ifndef VOIDWATCH_ASTRO_H
#define VOIDWATCH_ASTRO_H

#include <stdio.h>
#include <time.h>

#include "asteroid.h"
#include "audio.h"
#include "comet.h"
#include "dso.h"
#include "ephem.h"
#include "framebuffer.h"
#include "satellite.h"

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
    int            show_grid;            /* alt-az grid lines (key: g)        */
    int            show_constellations;  /* constellation stick figures (l)   */
    int            show_trails;          /* planet RA/Dec trails (key: t)     */
    int            show_dso;             /* deep-sky objects (key: d)         */
    int            show_aurora;          /* aurora effect (key: a)            */
    int            cursor_active;        /* object-pick cursor on (key: c)    */

    /* Perspective. 0 = geocentric (default — observer's all-sky view),
     * 1 = heliocentric (top-down "looking at the solar system from above
     * the ecliptic"). Toggle with `m`. */
    int            view_mode;

    /* Decorative parallax star backdrop (the sandbox starfield pressed
     * into double duty). Not real positions — a lively layer that sits
     * *behind* the real HYG sky in geo mode and is the only star layer
     * in helio mode. Toggle with `s`, default on. */
    int            show_star_backdrop;

    /* Trail ring buffers per body. */
    TrailSample    trails[EPHEM_COUNT][ASTRO_TRAIL_LEN];
    int            trail_head[EPHEM_COUNT];
    double         trail_last_jd;

    /* Cursor screen position in *cell* coords; nudged by hjkl. The
     * nearest above-horizon body is reported in the scan panel. */
    int            cursor_col, cursor_row;
    int            cursor_locked_body;   /* EphemBody index, -1 = none   */

    /* Track mode. When active, the cursor follows the snapshot-tracked
     * body each frame instead of staying still while the sky rotates.
     * `track_kind` is a small enum
     *   0=none, 1=planet, 2=comet, 3=asteroid, 4=DSO, 5=satellite
     * — keeping it int so astro.h doesn't need to expose the PickKind
     * enum from astro.c. `track_idx` indexes the appropriate array. */
    int            track_active;
    int            track_kind;
    int            track_idx;

    /* In-program search prompt. When `search_active` is non-zero, the
     * HUD shows a prompt at the bottom and keystrokes are appended to
     * `search_buf`. Enter triggers a body-name lookup + time scrub to
     * the next rise. Esc cancels. */
    int            search_active;
    int            search_len;
    char           search_buf[32];

    /* Bundled comets: state recomputed each frame in astro_update. */
    CometState     comets[COMET_COUNT];

    /* Bundled asteroids: same pattern. */
    AsteroidState  asteroids[ASTEROID_COUNT];

    /* Bundled satellites (Phase 6): per-frame look angles. The static
     * SGP4 model cache lives in src/satellite.c; this array is just the
     * per-tick output. */
    SatelliteState satellites[SATELLITE_COUNT];
    int            show_satellites;       /* toggle key: i           */

    /* Per-frame brightness compensation for fb_add stamps. main.c sets
     * fb_decay between 0.92 (1× scrub) and 0.50 (≥1000× scrub) to
     * suppress trails of fast-moving bodies. The drop in steady-state
     * accumulation is up to 6.25× — without compensation, planets get
     * dim at high scrub speed. main.c sets this each frame so stamps
     * can multiply their per-frame intensity to keep the *steady-state*
     * brightness constant across the scrub range.
     *
     * Default 1.0 means "no boost" (1× scrub baseline). */
    float          bright_boost;
} AstroState;

/* Compute every body's geocentric + topocentric position from `now`. */
void astro_update(AstroState *st, time_t now);

/* Push HUD event-log entries when astro state transitions: meteor shower
 * goes active/inactive, solar/lunar eclipse begins/ends. Idempotent —
 * only fires on edges, not every frame. `t_mono` is the same monotonic
 * timestamp threaded through hud_draw / hud_log_event. */
void astro_surface_events(const AstroState *st, double t_mono);

/* Arm track mode: snapshots whichever body is currently nearest the
 * cursor (planet / comet / asteroid) into the track_* fields. If
 * nothing's near enough, no-op. Called when the user presses `T`. */
void astro_track_arm(AstroState *st, int cols, int rows);

/* Per-frame track update: if track_active, look up the tracked body's
 * current screen-cell position and slide the cursor there. Untracks if
 * the body drops below the horizon. Called from main.c right after
 * astro_update so the cursor sticks for the rest of the frame. */
void astro_track_tick(AstroState *st, int cols, int rows);

/* In-program search jump. Look up a body by name (planets / comets /
 * asteroids exactly; DSOs by substring — case-insensitive), find when
 * it next crosses the horizon going up. Writes seconds-from-now offset
 * to *out_seconds, the display name to display_out, and the resolved
 * (kind, idx) tag to *out_kind / *out_idx so the caller can arm track
 * mode (kind values: 1=planet, 2=comet, 3=asteroid, 4=DSO).
 *
 * Returns 0 on hit, -1 if name doesn't match, -2 if no rise within
 * 30 days. */
int astro_search_body(const AstroState *st, const char *name,
                      double *out_seconds, char *display_out, size_t cap,
                      int *out_kind, int *out_idx);

/* Find the next "interesting" astronomical event after `from_jd`.
 * Walks forward in 1-day steps up to `max_days`, returning the JD of
 * the first eclipse / planet-planet conjunction / shower peak / solstice
 * / equinox encountered. Writes a short label (≤ 28 chars) into
 * `label_out` (caller-allocated). Returns 0 on hit, -1 if nothing
 * within the window. Called by the `J` key handler in main.c. */
int astro_find_next_event(const AstroState *st, double from_jd,
                          int max_days,
                          double *out_jd, char *label_out, size_t label_cap);

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

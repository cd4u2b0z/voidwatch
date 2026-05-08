#ifndef VOIDWATCH_HEADLESS_H
#define VOIDWATCH_HEADLESS_H

#include <stdio.h>
#include <time.h>

#include "ephem.h"

/*
 * Headless / one-shot output. None of these enter the render loop or
 * touch the terminal — they compute, print, and exit. Designed for shell
 * scripting + status-bar integration.
 *
 * Each function returns 0 on success, non-zero on failure (e.g. unknown
 * body name passed to headless_next).
 */

/* `voidwatch --tonight` — human-readable summary of what's worth looking
 * at tonight: Sun/Moon, planets visible during the night, active meteor
 * shower, comets/asteroids brighter than the configured cutoffs. */
int headless_tonight(const Observer *obs, time_t now, FILE *out);

/* `voidwatch --print-state` — full state dump for piping into other
 * tools. Plain text columns when `json=0`; structured JSON when `json=1`. */
int headless_print_state(const Observer *obs, time_t now, FILE *out, int json);

/* `voidwatch --next <body>` — when does this body next rise above the
 * horizon? Searches up to 30 days forward; prints time + alt-az.
 * Returns non-zero if the body name doesn't match any planet/comet/asteroid. */
int headless_next_rise(const Observer *obs, time_t now,
                       const char *name, FILE *out);

/* `voidwatch --year <year>` — annual almanac for the given Gregorian
 * year. Walks the whole year computing eclipses, planet-planet
 * conjunctions, shower peak dates, equinoxes/solstices. Prints a
 * sorted human-readable list. */
int headless_year(const Observer *obs, int year, FILE *out);

/* `voidwatch --validate` — internal sanity tests against known JPL /
 * Meeus reference values (Sun J2000, Moon at known full moons, comet
 * positions for a known epoch, etc.). Prints PASS/FAIL with delta in
 * arcseconds for each check. Returns 0 if everything passed, 1 if any
 * test failed. */
int headless_validate(FILE *out);

/* `voidwatch --snapshot [cols rows]` — render a single astro frame to
 * stdout (ANSI truecolor + cursor positioning + braille glyphs) and
 * exit. No alt-screen, no raw mode, no audio. Useful for piping into
 * `cat`, saving with `> file.ans`, or feeding a status-bar refresher.
 * Default 80×40 if dimensions aren't passed. */
int headless_snapshot(const Observer *obs, time_t now,
                      int cols, int rows, FILE *out);

#endif

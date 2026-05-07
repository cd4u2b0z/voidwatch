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

#endif
